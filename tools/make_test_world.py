#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Generate and independently validate a synthetic vanilla Anvil test world.

This is an independent oracle for Refrain's world import/export: it is
not derived from the C++ implementation.  All data is synthetic; no Mojang
code, jar contents or assets are used.

``gen`` writes a byte-deterministic world (gzip NBT ``level.dat``, zlib region/
entities/poi files, one player, a tiny data pack).  NBT compounds are sorted by
key, gzip mtime is zero and region sectors use first-fit allocation over chunks
sorted by ``(x, z)``.  Strings are Java modified UTF-8 (CESU-8 surrogate pairs,
U+0000 -> C0 80); ``level.dat`` contains U+1F600 to exercise surrogate handling.

``verify`` parses with its own NBT/Anvil reader and accepts worlds from other
valid writers: sector layout, palette order, gzip mtime, ``block_count``,
``isLightOn`` and ``blending_data`` are not required.

CLI::

    python3 tools/make_test_world.py gen --out DIR [--seed N] [--chunks N] [--force]
    python3 tools/make_test_world.py verify DIR [--json] [--expect-seed N]
    python3 tools/make_test_world.py selftest
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import shutil
import struct
import sys
import tempfile
import zlib
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

FORMAT_ID = "refrain.anvil-fixture/1"
DATA_VERSION = 5023
LEVEL_NAME = "Refrain Fixture \U0001F600"
DEFAULT_SEED = 0
MAX_CHUNKS = 256
SECTIONS_PER_CHUNK = 24
MIN_SECTION_Y = -4
HEIGHTMAP_WORDS = 37
MAX_CHUNK_BYTES = 1024 * 1024
MAX_CHUNK_NBT_BYTES = 16 * 1024 * 1024
MAX_LEVEL_NBT_BYTES = 64 * 1024 * 1024
MAX_PLAYER_NBT_BYTES = 16 * 1024 * 1024
MAX_COMPRESSED_BYTES = 128 * 1024 * 1024
MAX_JSON_BYTES = 1024 * 1024
MAX_NBT_DEPTH = 512
MAX_NBT_COLLECTION = 1 << 20
MAX_NBT_TAGS = 1 << 22
U64_MASK = (1 << 64) - 1
I64_MIN, I64_MAX = -(1 << 63), (1 << 63) - 1
I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
PLAYER_UUID_INTS = (0x9F8E7D6C, 0x5B4A4938, 0xA2710F1E, 0x2D3C4B5A)
PACK_FORMAT = 71

class Tag:
    """Java Edition binary NBT tag ids (numeric values are the wire format)."""

    END, BYTE, SHORT, INT, LONG = 0, 1, 2, 3, 4
    FLOAT, DOUBLE, BYTE_ARRAY, STRING = 5, 6, 7, 8
    LIST, COMPOUND, INT_ARRAY, LONG_ARRAY = 9, 10, 11, 12

VALID_TAGS = frozenset(range(Tag.BYTE, Tag.LONG_ARRAY + 1))
SCALAR_FORMATS = {
    Tag.BYTE: (">b", 1),
    Tag.SHORT: (">h", 2),
    Tag.INT: (">i", 4),
    Tag.LONG: (">q", 8),
    Tag.FLOAT: (">f", 4),
    Tag.DOUBLE: (">d", 8),
}
INTEGER_LIMITS = {
    Tag.BYTE: (-128, 127),
    Tag.SHORT: (-32768, 32767),
    Tag.INT: (I32_MIN, I32_MAX),
    Tag.LONG: (I64_MIN, I64_MAX),
}

class FixtureError(Exception):
    """A user-visible generation or validation failure."""

@dataclass(frozen=True, slots=True)
class Scalar:
    tag: int
    value: int | float

@dataclass(frozen=True, slots=True)
class Array:
    tag: int
    value: bytes | list[int]

@dataclass(frozen=True, slots=True)
class NBTList:
    element_type: int
    items: list[Any]

@dataclass(frozen=True, slots=True)
class _RegionRecord:
    x: int
    z: int
    nbt: dict[str, Any]

def Byte(value: int) -> Scalar: return Scalar(Tag.BYTE, value)
def Short(value: int) -> Scalar: return Scalar(Tag.SHORT, value)
def Int(value: int) -> Scalar: return Scalar(Tag.INT, value)
def Long(value: int) -> Scalar: return Scalar(Tag.LONG, value)
def Float(value: float) -> Scalar: return Scalar(Tag.FLOAT, value)
def Double(value: float) -> Scalar: return Scalar(Tag.DOUBLE, value)
def ByteArray(value: bytes) -> Array: return Array(Tag.BYTE_ARRAY, value)
def IntArray(value: list[int]) -> Array: return Array(Tag.INT_ARRAY, value)
def LongArray(value: list[int]) -> Array: return Array(Tag.LONG_ARRAY, value)

# --- NBT writing
def _tag_id(value: Any) -> int:
    if isinstance(value, (Scalar, Array)):
        return value.tag
    if isinstance(value, str):
        return Tag.STRING
    if isinstance(value, NBTList):
        return Tag.LIST
    if isinstance(value, dict):
        return Tag.COMPOUND
    raise FixtureError(f"unsupported NBT value type: {type(value).__name__}")

def _encode_modified_utf8(text: str) -> bytes:
    out = bytearray()
    for ch in text:
        cp = ord(ch)
        if cp == 0:
            out += b"\xc0\x80"
        elif cp <= 0x7F:
            out.append(cp)
        elif cp <= 0x7FF:
            out += bytes((0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)))
        elif cp <= 0xFFFF:
            out += bytes((0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F)))
            out.append(0x80 | (cp & 0x3F))
        else:
            cp -= 0x10000
            for unit in (0xD800 | (cp >> 10), 0xDC00 | (cp & 0x3FF)):
                out += bytes((0xE0 | (unit >> 12), 0x80 | ((unit >> 6) & 0x3F)))
                out.append(0x80 | (unit & 0x3F))
    if len(out) > 0xFFFF:
        raise FixtureError("NBT string exceeds the 65535-byte modified UTF-8 limit")
    return bytes(out)

def _write_string(out: bytearray, text: str) -> None:
    encoded = _encode_modified_utf8(text)
    out += struct.pack(">H", len(encoded)) + encoded

def _write_payload(out: bytearray, value: Any) -> None:
    if isinstance(value, Scalar):
        if value.tag in INTEGER_LIMITS:
            low, high = INTEGER_LIMITS[value.tag]
            if not low <= value.value <= high:
                raise FixtureError(f"NBT integer out of range: {value.value}")
        out += struct.pack(SCALAR_FORMATS[value.tag][0], value.value)
    elif isinstance(value, Array):
        out += struct.pack(">i", len(value.value))
        if value.tag == Tag.BYTE_ARRAY:
            out += value.value
        else:
            fmt = ">i" if value.tag == Tag.INT_ARRAY else ">q"
            low, high = INTEGER_LIMITS[
                Tag.INT if value.tag == Tag.INT_ARRAY else Tag.LONG
            ]
            for item in value.value:
                if not low <= item <= high:
                    raise FixtureError(f"NBT array element out of range: {item}")
                out += struct.pack(fmt, item)
    elif isinstance(value, str):
        _write_string(out, value)
    elif isinstance(value, NBTList):
        if (value.element_type == Tag.END and value.items) or (
            value.element_type != Tag.END and value.element_type not in VALID_TAGS
        ):
            raise FixtureError("invalid TAG_List element type or non-empty TAG_End list")
        out.append(value.element_type)
        out += struct.pack(">i", len(value.items))
        for item in value.items:
            if _tag_id(item) != value.element_type:
                raise FixtureError("TAG_List contains a heterogeneous element")
            _write_payload(out, item)
    elif isinstance(value, dict):
        for key in sorted(value):
            if not isinstance(key, str):
                raise FixtureError("NBT compound key is not a string")
            out.append(_tag_id(value[key]))
            _write_string(out, key)
            _write_payload(out, value[key])
        out.append(Tag.END)
    else:
        raise FixtureError(f"unsupported NBT value type: {type(value).__name__}")

def _write_nbt(root: dict[str, Any]) -> bytes:
    out = bytearray((Tag.COMPOUND,))
    _write_string(out, "")
    _write_payload(out, root)
    return bytes(out)

# --- NBT reading (independent parser)
def _decode_modified_utf8(data: bytes, source: str) -> str:
    units: list[int] = []
    i, size = 0, len(data)
    while i < size:
        first = data[i]
        if first < 0x80:
            units.append(first)
            i += 1
        elif first & 0xE0 == 0xC0:
            if i + 1 >= size or data[i + 1] & 0xC0 != 0x80:
                raise FixtureError(f"{source}: malformed modified UTF-8 (2-byte)")
            cp = ((first & 0x1F) << 6) | (data[i + 1] & 0x3F)
            if cp < 0x80 and not (cp == 0 and first == 0xC0):
                raise FixtureError(f"{source}: overlong modified UTF-8 sequence")
            units.append(cp)
            i += 2
        elif first & 0xF0 == 0xE0:
            if i + 2 >= size or (data[i + 1] | data[i + 2]) & 0xC0 != 0x80:
                raise FixtureError(f"{source}: malformed modified UTF-8 (3-byte)")
            cp = ((first & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6)
            cp |= data[i + 2] & 0x3F
            if cp < 0x800:
                raise FixtureError(f"{source}: overlong modified UTF-8 sequence")
            units.append(cp)
            i += 3
        elif first & 0xF8 == 0xF0:
            if i + 3 >= size or (data[i + 1] | data[i + 2] | data[i + 3]) & 0xC0 != 0x80:
                raise FixtureError(f"{source}: malformed UTF-8 (4-byte)")
            cp = (
                ((first & 0x07) << 18)
                | ((data[i + 1] & 0x3F) << 12)
                | ((data[i + 2] & 0x3F) << 6)
                | (data[i + 3] & 0x3F)
            )
            if not 0x10000 <= cp <= 0x10FFFF:
                raise FixtureError(f"{source}: invalid 4-byte UTF-8 code point")
            units.append(cp)
            i += 4
        else:
            raise FixtureError(f"{source}: malformed modified UTF-8 leading byte")
    chars: list[str] = []
    i = 0
    while i < len(units):
        unit = units[i]
        if 0xD800 <= unit <= 0xDBFF:
            if i + 1 >= len(units) or not 0xDC00 <= units[i + 1] <= 0xDFFF:
                raise FixtureError(f"{source}: unpaired UTF-16 high surrogate")
            low = units[i + 1]
            chars.append(chr(0x10000 + ((unit - 0xD800) << 10) + low - 0xDC00))
            i += 2
        elif 0xDC00 <= unit <= 0xDFFF:
            raise FixtureError(f"{source}: unpaired UTF-16 low surrogate")
        else:
            chars.append(chr(unit))
            i += 1
    return "".join(chars)

class _NBTReader:
    def __init__(self, data: bytes, source: str) -> None:
        self.data, self.source, self.pos, self.tags = data, source, 0, 0

    def _fail(self, message: str) -> None:
        raise FixtureError(f"{self.source}: {message}")

    def _need(self, count: int) -> None:
        if count < 0 or self.pos + count > len(self.data):
            self._fail("truncated NBT payload")

    def _unpack(self, fmt: str, size: int) -> Any:
        self._need(size)
        value = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += size
        return value

    def _string(self) -> str:
        length = self._unpack(">H", 2)
        self._need(length)
        raw = self.data[self.pos : self.pos + length]
        self.pos += length
        return _decode_modified_utf8(raw, self.source)

    def _count(self) -> int:
        count = self._unpack(">i", 4)
        if count < 0 or count > MAX_NBT_COLLECTION:
            self._fail(f"invalid NBT collection length {count}")
        return count

    def _count_tag(self, depth: int) -> None:
        if depth > MAX_NBT_DEPTH:
            self._fail("NBT nesting too deep")
        self.tags += 1
        if self.tags > MAX_NBT_TAGS:
            self._fail("NBT tag count exceeds the safety limit")

    def read_root(self) -> dict[str, Any]:
        if self._unpack(">B", 1) != Tag.COMPOUND:
            self._fail("root tag is not TAG_Compound")
        self._string()
        self._count_tag(1)
        root = self._compound_body(1)
        if self.pos != len(self.data):
            self._fail("trailing bytes after the NBT root compound")
        return root

    def _compound_body(self, depth: int) -> dict[str, Any]:
        result: dict[str, Any] = {}
        while True:
            tag = self._unpack(">B", 1)
            if tag == Tag.END:
                return result
            if tag not in VALID_TAGS:
                self._fail(f"unknown NBT tag id {tag}")
            name = self._string()
            if name in result:
                self._fail(f"duplicate compound key {name!r}")
            self._count_tag(depth + 1)
            result[name] = self._payload(tag, depth + 1)

    def _payload(self, tag: int, depth: int) -> Any:
        if tag in SCALAR_FORMATS:
            fmt, size = SCALAR_FORMATS[tag]
            return self._unpack(fmt, size)
        if tag == Tag.STRING:
            return self._string()
        if tag == Tag.BYTE_ARRAY:
            length = self._count()
            self._need(length)
            value = self.data[self.pos : self.pos + length]
            self.pos += length
            return value
        if tag in (Tag.INT_ARRAY, Tag.LONG_ARRAY):
            length = self._count()
            size, code = (4, "i") if tag == Tag.INT_ARRAY else (8, "q")
            self._need(length * size)
            values = list(struct.unpack_from(f">{length}{code}", self.data, self.pos))
            self.pos += length * size
            return values
        if tag == Tag.LIST:
            element_type, length = self._unpack(">B", 1), self._count()
            if (element_type == Tag.END and length) or (
                element_type not in VALID_TAGS and element_type != Tag.END
            ):
                self._fail(f"invalid TAG_List element type {element_type}")
            items = []
            for _ in range(length):
                self._count_tag(depth + 1)
                items.append(self._payload(element_type, depth + 1))
            return NBTList(element_type, items)
        if tag == Tag.COMPOUND:
            return self._compound_body(depth)
        self._fail(f"unknown NBT tag id {tag}")
        raise AssertionError("unreachable")

def _parse_nbt(data: bytes, source: str) -> dict[str, Any]:
    return _NBTReader(data, source).read_root()

# --- compression / file helpers
def _gzip_compress(payload: bytes) -> bytes:
    buffer = io.BytesIO()
    with gzip.GzipFile(
        fileobj=buffer, mode="wb", mtime=0, filename="", compresslevel=6
    ) as handle:
        handle.write(payload)
    return buffer.getvalue()

def _gzip_decompress(data: bytes, limit: int, source: str) -> bytes:
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(data), mode="rb") as handle:
            result = handle.read(limit + 1)
    except (OSError, EOFError, ValueError) as exc:
        raise FixtureError(f"{source}: invalid gzip stream: {exc}") from exc
    if len(result) > limit:
        raise FixtureError(f"{source}: gzip payload exceeds the {limit}-byte limit")
    return result

def _zlib_decompress(data: bytes, limit: int, source: str) -> bytes:
    decompressor, output, pending = zlib.decompressobj(), bytearray(), data
    while True:
        room = limit + 1 - len(output)
        if room <= 0:
            raise FixtureError(f"{source}: zlib payload exceeds the {limit}-byte limit")
        try:
            output += decompressor.decompress(pending, room)
        except zlib.error as exc:
            raise FixtureError(f"{source}: invalid zlib stream: {exc}") from exc
        if len(output) > limit:
            raise FixtureError(f"{source}: zlib payload exceeds the {limit}-byte limit")
        pending = decompressor.unconsumed_tail
        if decompressor.eof:
            break
        if not pending:
            raise FixtureError(f"{source}: truncated zlib stream")
    if decompressor.unused_data:
        raise FixtureError(f"{source}: trailing bytes after the zlib stream")
    return bytes(output)

def _read_file_limited(path: Path, limit: int) -> bytes:
    try:
        if path.stat().st_size > limit:
            raise FixtureError(f"{path}: file exceeds the {limit}-byte limit")
        return path.read_bytes()
    except OSError as exc:
        raise FixtureError(f"{path}: cannot read file: {exc}") from exc

def _read_gzip_nbt(path: Path, limit: int) -> dict[str, Any]:
    compressed = _read_file_limited(path, MAX_COMPRESSED_BYTES)
    return _parse_nbt(_gzip_decompress(compressed, limit, str(path)), str(path))

def _write_gzip_nbt(path: Path, root: dict[str, Any]) -> None:
    path.write_bytes(_gzip_compress(_write_nbt(root)))

# --- synthetic world generation
def _signed(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value

def _packed_words(values: Sequence[int], bits: int) -> list[int]:
    per_word = 64 // bits
    words = [0] * ((len(values) + per_word - 1) // per_word)
    for index, value in enumerate(values):
        if not 0 <= value < (1 << bits):
            raise FixtureError(f"packed value {value} does not fit in {bits} bits")
        words[index // per_word] |= value << ((index % per_word) * bits)
    return [_signed(word, 64) for word in words]

def _long_array(values: Sequence[int], bits: int) -> Array:
    return LongArray(_packed_words(values, bits))

def _uuid_text(ints: Sequence[int]) -> str:
    if len(ints) != 4:
        raise FixtureError("UUID must contain exactly four 32-bit ints")
    words = [value & 0xFFFFFFFF for value in ints]
    return (f"{words[0]:08x}-{words[1] >> 16:04x}-{words[1] & 0xFFFF:04x}-"
            f"{words[2] >> 16:04x}-{words[2] & 0xFFFF:04x}{words[3]:08x}")

def _mix64(value: int) -> int:
    value = (value + 0x9E3779B97F4A7C15) & U64_MASK
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & U64_MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & U64_MASK
    return (value ^ (value >> 31)) & U64_MASK

def _entity_uuid_ints(cx: int, cz: int, kind: int) -> list[int]:
    seed = ((cx & 0xFFFFFFFF) << 32) ^ (cz & 0xFFFFFFFF) ^ ((kind + 1) * 0x9E3779B9)
    high, low = _mix64(seed ^ 0x5DEECE66D), _mix64(seed ^ 0xD1B54A32D192ED03)
    words = [high >> 32, high & 0xFFFFFFFF, low >> 32, low & 0xFFFFFFFF]
    return [_signed(word, 32) for word in words]

def _pack_block_pos(x: int, y: int, z: int) -> int:
    return _signed(((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF), 64)

def _heightmap_values(kind: int) -> list[int]:
    return [70 + ((x * 5 + z * 11 + kind * 3) % 20) for z in range(16) for x in range(16)]

def _biome_container() -> dict[str, Any]:
    # Biomes use a 4x4x4 grid: 64 entries per section.
    return {"palette": NBTList(Tag.STRING, ["minecraft:plains", "minecraft:desert"]),
            "data": _long_array([index & 1 for index in range(64)], 1)}

def _palette_entry(entry: str | dict[str, Any], compound_form: bool) -> Any:
    """26.3 ``BlockState.CODEC``: bare id string or ``{"": name}``/``{id, props}``."""
    if isinstance(entry, str):
        name, properties = entry, None
    else:
        name = _string(entry.get("Name"), "palette entry Name")
        properties = entry.get("Properties")
    if not compound_form:
        return name
    if not properties:
        return {"": name}
    return {"id": name, "properties": dict(properties)}

def _block_states(palette: list[str | dict[str, Any]],
                  data: Array | None = None) -> dict[str, Any]:
    compound_form = any(isinstance(entry, dict) and entry.get("Properties")
                        for entry in palette)
    return {
        "palette": NBTList(Tag.COMPOUND if compound_form else Tag.STRING,
                           [_palette_entry(entry, compound_form)
                            for entry in palette]),
        **({"data": data} if data is not None else {}),
    }

def _build_sections() -> list[dict[str, Any]]:
    # 26.3 writes only non-empty sections (an empty section compound is not
    # added to `sections`); every entry carries its absolute section Y.
    biomes = _biome_container()
    return [
        {
            "Y": Byte(0),
            "block_states": _block_states(
                ["minecraft:air", "minecraft:stone",
                 {"Name": "minecraft:oak_log", "Properties": {"axis": "y"}}],
                _long_array([1, 2] + [0] * 4094, 2)),
            "biomes": biomes,
            "SkyLight": ByteArray(bytes([0x00, 0xFF]) * 1024),
        },
        {
            "Y": Byte(1),
            "block_states": _block_states(["minecraft:bedrock"]),
            "biomes": biomes,
        },
        {
            "Y": Byte(2),
            "block_states": _block_states(
                [f"minecraft:test_block_{index}" for index in range(17)],
                _long_array([index % 17 for index in range(4096)], 5)),
            "biomes": biomes,
            "SkyLight": ByteArray(b"\xff" * 2048),
        },
        {
            "Y": Byte(-4),
            "block_states": _block_states(["minecraft:air"]),
            "biomes": biomes,
        },
    ]

def _build_post_processing() -> NBTList:
    entries = [NBTList(Tag.SHORT, []) for _ in range(SECTIONS_PER_CHUNK)]
    entries[2] = NBTList(Tag.SHORT, [Short(0), Short(15), Short(15)])
    return NBTList(Tag.LIST, entries)

def _tick(block_id: str, priority: int, x: int, y: int, z: int, delay: int) -> dict:
    return {"i": block_id, "p": Int(priority), "x": Int(x), "y": Int(y),
            "z": Int(z), "t": Int(delay), "priority": Int(0)}

def _build_chunk(cx: int, cz: int, ordinal: int) -> dict[str, Any]:
    return {
        "DataVersion": Int(DATA_VERSION), "xPos": Int(cx), "zPos": Int(cz),
        "yPos": Int(MIN_SECTION_Y), "Status": "minecraft:full",
        "LastUpdate": Long(1000 + ordinal), "InhabitedTime": Long(250 + ordinal),
        "isLightOn": Byte(1),
        "sections": NBTList(Tag.COMPOUND, _build_sections()),
        "Heightmaps": {name: _long_array(_heightmap_values(i), 9)
                       for i, name in enumerate(("MOTION_BLOCKING", "WORLD_SURFACE",
                                                 "OCEAN_FLOOR"))},
        "block_ticks": NBTList(Tag.COMPOUND, [
            _tick("minecraft:redstone_wire", 3, 0, 70, 0, 20),
            _tick("minecraft:water", 0, 1, 64, 1, 5),
        ]),
        "fluid_ticks": NBTList(Tag.COMPOUND, [_tick("minecraft:water", 0, 1, 64, 1, 5)]),
        "block_entities": NBTList(Tag.COMPOUND, [
            {"id": "minecraft:chest", "x": Int(0), "y": Int(65), "z": Int(0),
             "KeepPacked": Byte(0), "Items": NBTList(Tag.COMPOUND, [])},
            {"id": "minecraft:sign", "x": Int(1), "y": Int(66), "z": Int(0),
             "front_text": {"messages": NBTList(Tag.STRING, ["", "", "", ""])}},
        ]),
        "structures": {"starts": {}, "References": {}},
        "PostProcessing": _build_post_processing(),
        "blending_data": {"min_section": Int(0), "max_section": Int(0)},
    }

def _build_entities(cx: int, cz: int) -> dict[str, Any]:
    base_x, base_z = cx * 16, cz * 16
    def entity(kind: int, entity_id: str, x: float, y: float, z: float) -> dict:
        return {"id": entity_id, "Pos": NBTList(Tag.DOUBLE, [Double(x), Double(y), Double(z)]),
                "UUID": NBTList(Tag.INT, [Int(w) for w in _entity_uuid_ints(cx, cz, kind)]),
                "Rotation": NBTList(Tag.FLOAT, [Float(0.0), Float(0.0)]),
                "Motion": NBTList(Tag.DOUBLE, [Double(0.0), Double(0.0), Double(0.0)])}
    pig = entity(0, "minecraft:pig", base_x + 8.5, 70.0, base_z + 8.5)
    item = entity(1, "minecraft:item", base_x + 2.5, 70.0, base_z + 2.5)
    item["Item"] = {"id": "minecraft:stone", "count": Byte(1)}
    return {"DataVersion": Int(DATA_VERSION), "Position": {"x": Int(cx), "z": Int(cz)},
            "Entities": NBTList(Tag.COMPOUND, [pig, item])}

def _build_poi(cx: int, cz: int) -> dict[str, Any]:
    return {"DataVersion": Int(DATA_VERSION), "Sections": NBTList(Tag.COMPOUND, [{
        "Y": Byte(0), "valid": Byte(1),
        "Records": NBTList(Tag.COMPOUND, [{
            "type": "minecraft:home", "pos": Long(_pack_block_pos(cx * 16, 64, cz * 16)),
            "free_tickets": Int(0)}]),
    }])}

def _build_world_gen_settings(seed: int) -> dict[str, Any]:
    return {
        "DataVersion": Int(DATA_VERSION),
        "data": {
            "seed": Long(seed),
            "generate_structures": Byte(1),
            "bonus_chest": Byte(0),
        },
    }

def _build_level(seed: int) -> dict[str, Any]:
    return {"Data": {
        "DataVersion": Int(DATA_VERSION),
        "Version": {"Name": "26.3", "Id": Int(DATA_VERSION), "Series": "main",
                    "Snapshot": Byte(0)},
        "LevelName": LEVEL_NAME, "LastPlayed": Long(0), "GameType": Int(1),
        "difficulty_settings": {"difficulty": "easy", "hardcore": Byte(0),
                                "locked": Byte(0)},
        "initialized": Byte(1), "allowCommands": Byte(1),
        "Time": Long(24000),
        "spawn": {"pos": IntArray([8, 72, 8]),
                  "dimension": "minecraft:overworld",
                  "pitch": Float(0.0), "yaw": Float(0.0)},
        "DataPacks": {"Enabled": NBTList(Tag.STRING, ["vanilla"]),
                      "Disabled": NBTList(Tag.STRING, [])},
    }}

def _build_player() -> dict[str, Any]:
    return {"DataVersion": Int(DATA_VERSION),
            "UUID": IntArray([_signed(value, 32) for value in PLAYER_UUID_INTS]),
            "Pos": NBTList(Tag.DOUBLE, [Double(8.5), Double(72.0), Double(8.5)]),
            "Rotation": NBTList(Tag.FLOAT, [Float(0.0), Float(0.0)]),
            "Health": Float(20.0), "foodLevel": Int(20),
            "Inventory": NBTList(Tag.COMPOUND, [
                {"Slot": Byte(0), "id": "minecraft:stone", "count": Byte(64)}]),
            "XpLevel": Int(5), "SelectedItemSlot": Int(0), "playerGameType": Int(1)}

def _write_region_file(path: Path, chunks: dict[tuple[int, int], dict[str, Any]]) -> None:
    payloads: list[tuple[int, int, bytes, int]] = []
    for cx, cz in sorted(chunks):
        payload = bytes([2]) + zlib.compress(_write_nbt(chunks[(cx, cz)]), 6)
        length = len(payload)
        sector_count = (4 + length + 4095) // 4096
        if length > MAX_CHUNK_BYTES or sector_count > 255:
            raise FixtureError(f"chunk ({cx}, {cz}) is too large for an Anvil region")
        payloads.append((cx, cz, payload, sector_count))

    occupied, placements = [True, True], []
    for cx, cz, payload, count in payloads:
        offset = 2
        while True:
            end = offset + count
            if end > len(occupied):
                occupied.extend([False] * (end - len(occupied)))
                break
            if not any(occupied[offset:end]):
                break
            offset += 1
        occupied[offset : offset + count] = [True] * count
        placements.append((offset, count, cx, cz, payload))

    image = bytearray(len(occupied) * 4096)
    for offset, count, cx, cz, payload in placements:
        index = (cx & 31) + ((cz & 31) << 5)
        struct.pack_into(">I", image, index * 4, (offset << 8) | count)
        struct.pack_into(">I", image, offset * 4096, len(payload))
        image[offset * 4096 + 4 : offset * 4096 + 4 + len(payload)] = payload
    path.write_bytes(bytes(image))

def _prepare_out_dir(path: Path, force: bool) -> None:
    if path.is_symlink() or path.resolve() == Path(path.anchor):
        raise FixtureError(f"{path}: refusing symlink or filesystem root as --out")
    if path.exists() and not path.is_dir():
        raise FixtureError(f"{path}: exists and is not a directory")
    if path.is_dir() and any(path.iterdir()):
        if not force:
            raise FixtureError(f"{path}: directory is not empty (pass --force)")
        try:
            shutil.rmtree(path)
        except OSError as exc:
            raise FixtureError(f"{path}: cannot remove: {exc}") from exc
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise FixtureError(f"{path}: cannot create directory: {exc}") from exc

def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
def _write_datapack(root: Path) -> None:
    datapack = root / "datapacks" / "refrain_fixture"
    (datapack / "data" / "refrain_fixture" / "loot_table").mkdir(parents=True)
    _write_json(datapack / "pack.mcmeta", {"pack": {
        "pack_format": PACK_FORMAT, "description": "Refrain synthetic test fixture"}})
    _write_json(datapack / "data" / "refrain_fixture" / "loot_table" / "synthetic.json",
                {"type": "minecraft:chest", "pools": []})

def generate_world(out: Path, *, seed: int = DEFAULT_SEED, chunks: int | None = None,
                   force: bool = False) -> int:
    """Generate the synthetic world at ``out`` and return the chunk count."""

    if not I32_MIN <= seed <= I32_MAX:
        raise FixtureError(f"seed {seed} does not fit in the level.dat TAG_Int field")
    if chunks is not None and not 0 <= chunks <= MAX_CHUNKS:
        raise FixtureError(f"--chunks must be between 0 and {MAX_CHUNKS}")
    _prepare_out_dir(out, force)
    overworld = ([(0, 0), (0, 1), (1, 0)] if chunks is None
                 else [(index % 8, index // 8) for index in range(chunks)])
    region_chunks = {(cx, cz): _build_chunk(cx, cz, ordinal)
                     for ordinal, (cx, cz) in enumerate(sorted(overworld))}
    entities_chunks = {(cx, cz): _build_entities(cx, cz) for cx, cz in sorted(overworld)}
    poi_chunks = {(cx, cz): _build_poi(cx, cz) for cx, cz in sorted(overworld)}
    negative_region = {(-1, 0): _build_chunk(-1, 0, 0)}
    overworld = out / "dimensions" / "minecraft" / "overworld"
    (overworld / "region").mkdir(parents=True)
    (overworld / "entities").mkdir()
    (overworld / "poi").mkdir()
    (out / "players" / "data").mkdir(parents=True)
    (out / "data" / "minecraft").mkdir(parents=True)
    _write_gzip_nbt(out / "level.dat", _build_level(seed))
    _write_gzip_nbt(out / "data" / "minecraft" / "world_gen_settings.dat",
                    _build_world_gen_settings(seed))
    _write_region_file(overworld / "region" / "r.0.0.mca", region_chunks)
    _write_region_file(overworld / "region" / "r.-1.0.mca", negative_region)
    _write_region_file(overworld / "entities" / "r.0.0.mca", entities_chunks)
    _write_region_file(overworld / "poi" / "r.0.0.mca", poi_chunks)
    _write_gzip_nbt(out / "players" / "data" /
                    f"{_uuid_text(PLAYER_UUID_INTS)}.dat", _build_player())
    _write_datapack(out)
    return len(region_chunks) + len(negative_region)

# --- region reading
def _region_coords(path: Path) -> tuple[int, int]:
    name = path.name
    if not name.startswith("r.") or not name.endswith(".mca"):
        raise FixtureError(f"{path}: not a region file name (r.<x>.<z>.mca)")
    parts = name[2:-4].split(".")
    if len(parts) != 2:
        raise FixtureError(f"{path}: not a region file name (r.<x>.<z>.mca)")
    try:
        return int(parts[0]), int(parts[1])
    except ValueError as exc:
        raise FixtureError(f"{path}: invalid region coordinates") from exc

def _read_region_file(path: Path) -> list[_RegionRecord]:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise FixtureError(f"{path}: cannot stat region file: {exc}") from exc
    if size < 8192 or size % 4096:
        raise FixtureError(f"{path}: region size {size} is not a multiple of 4096")
    total_sectors, rx, rz = size // 4096, *_region_coords(path)
    records: list[_RegionRecord] = []
    occupied: list[tuple[int, int]] = []
    try:
        with path.open("rb") as handle:
            header = handle.read(8192)
            if len(header) != 8192:
                raise FixtureError(f"{path}: truncated region header")
            for index in range(1024):
                raw = struct.unpack_from(">I", header, index * 4)[0]
                if raw == 0:
                    continue
                offset, count = raw >> 8, raw & 0xFF
                if offset < 2 or count == 0:
                    raise FixtureError(f"{path}: slot {index}: invalid location {raw:#x}")
                if offset + count > total_sectors:
                    raise FixtureError(
                        f"{path}: slot {index}: sectors {offset}+{count} exceed file"
                    )
                occupied.append((offset, offset + count))
                handle.seek(offset * 4096)
                length_raw = handle.read(4)
                if len(length_raw) != 4:
                    raise FixtureError(f"{path}: slot {index}: truncated chunk length")
                length = struct.unpack(">I", length_raw)[0]
                if not 1 <= length <= MAX_CHUNK_BYTES:
                    raise FixtureError(f"{path}: slot {index}: invalid length {length}")
                if 4 + length > count * 4096:
                    raise FixtureError(
                        f"{path}: slot {index}: chunk spills outside its sectors"
                    )
                payload = handle.read(length)
                if len(payload) != length:
                    raise FixtureError(f"{path}: slot {index}: truncated payload")
                source, compression = f"{path}: slot {index}", payload[0]
                compressed = payload[1:]
                if compression == 2:
                    raw_nbt = _zlib_decompress(compressed, MAX_CHUNK_NBT_BYTES, source)
                elif compression == 1:
                    raw_nbt = _gzip_decompress(compressed, MAX_CHUNK_NBT_BYTES, source)
                elif compression == 3:
                    if len(compressed) > MAX_CHUNK_NBT_BYTES:
                        raise FixtureError(f"{source}: uncompressed chunk is too large")
                    raw_nbt = compressed
                else:
                    raise FixtureError(f"{source}: unsupported compression id {compression}")
                records.append(
                    _RegionRecord(
                        rx * 32 + index % 32,
                        rz * 32 + index // 32,
                        _parse_nbt(raw_nbt, source),
                    )
                )
    except OSError as exc:
        raise FixtureError(f"{path}: cannot read region file: {exc}") from exc
    occupied.sort()
    for (start, end), (next_start, _) in zip(occupied, occupied[1:]):
        if next_start < end:
            raise FixtureError(
                f"{path}: overlapping chunk sectors at {start} and {next_start}"
            )
    return records

# --- structural validation helpers
def _as(value: Any, kind: type, label: str, where: str) -> Any:
    if not isinstance(value, kind):
        raise FixtureError(f"{where}: expected {label}")
    return value

def _compound(value: Any, where: str) -> dict[str, Any]:
    return _as(value, dict, "TAG_Compound", where)

def _list(value: Any, where: str) -> NBTList: return _as(value, NBTList, "TAG_List", where)
def _int(value: Any, where: str) -> int: return _as(value, int, "an integer tag", where)
def _string(value: Any, where: str) -> str: return _as(value, str, "TAG_String", where)
def _bytes(value: Any, where: str) -> bytes: return _as(value, bytes, "TAG_Byte_Array", where)
def _words(value: Any, where: str) -> list[int]:
    return _as(value, list, "TAG_Long_Array", where)

def _palette_words_for_bits(bits: int, entries: int = 4096) -> int:
    return (entries + (64 // bits) - 1) // (64 // bits)

def _check_palette_words(count: int, words: list[int], where: str, block: bool) -> None:
    bits = max(1, (count - 1).bit_length())
    entries = 4096 if block else 64
    allowed = {_palette_words_for_bits(bits, entries)}
    if block:
        allowed.add(_palette_words_for_bits(max(4, bits), entries))
    if len(words) not in allowed:
        expected = ", ".join(str(value) for value in sorted(allowed))
        raise FixtureError(
            f"{where}: packed palette has {len(words)} words, expected one of {expected}"
        )

def _block_palette_name(entry: Any, where: str) -> str:
    if isinstance(entry, str):
        return entry
    item = _compound(entry, where)
    properties = item.get("properties")
    if "id" in item:
        name = _string(item.get("id"), f"{where}.id")
        if properties is None:
            return name
        props = _compound(properties, f"{where}.properties")
        parts = sorted(f"{key}={_string(value, f'{where}.properties.{key}')}"
                       for key, value in props.items())
        return f"{name}[{','.join(parts)}]"
    if len(item) == 1 and "" in item:
        return _string(item.get(""), f"{where}['']")
    raise FixtureError(f"{where}: expected {{'': name}} or {{id, properties}}")

def _section_palette(container: dict[str, Any], where: str, biome: bool) -> list[str]:
    palette = _list(container.get("palette"), f"{where}.palette")
    names: list[str] = []
    for index, entry in enumerate(palette.items):
        if biome:
            names.append(_string(entry, f"{where}.palette[{index}]"))
        else:
            names.append(_block_palette_name(entry, f"{where}.palette[{index}]"))
    if not names:
        raise FixtureError(f"{where}.palette: empty palette")
    if "data" in container:
        _check_palette_words(len(names), _words(container["data"], f"{where}.data"),
                             f"{where}.data", block=not biome)
    elif len(names) != 1:
        raise FixtureError(f"{where}: multi-entry palette without packed data")
    return names

def _validate_section(
    section: dict[str, Any], where: str
) -> tuple[list[str], list[str], bool]:
    if "Y" not in section:
        raise FixtureError(f"{where}.Y: missing")
    _int(section["Y"], f"{where}.Y")
    block_names: list[str] = []
    biome_names: list[str] = []
    if "block_states" in section:
        container = _compound(section["block_states"], f"{where}.block_states")
        block_names = _section_palette(container, f"{where}.block_states", False)
    if "biomes" in section:
        container = _compound(section["biomes"], f"{where}.biomes")
        biome_names = _section_palette(container, f"{where}.biomes", True)
    for key in ("SkyLight", "BlockLight"):
        if key in section and len(_bytes(section[key], f"{where}.{key}")) != 2048:
            raise FixtureError(f"{where}.{key}: expected 2048 bytes")
    return block_names, biome_names, any(n != "minecraft:air" for n in block_names)

def _validate_ticks(value: Any, where: str) -> int:
    ticks = _list(value, where)
    for index, entry in enumerate(ticks.items):
        _compound(entry, f"{where}[{index}]")
    return len(ticks.items)
def _validate_post_processing(value: Any, where: str) -> int:
    outer = _list(value, where)
    if len(outer.items) != SECTIONS_PER_CHUNK:
        raise FixtureError(f"{where}: expected {SECTIONS_PER_CHUNK} entries")
    for index, entry in enumerate(outer.items):
        inner = _list(entry, f"{where}[{index}]")
        if inner.element_type not in (Tag.END, Tag.SHORT):
            raise FixtureError(f"{where}[{index}]: expected TAG_List<TAG_Short>")
        for value_index, short in enumerate(inner.items):
            if not isinstance(short, int) or not -32768 <= short <= 32767:
                raise FixtureError(f"{where}[{index}][{value_index}]: invalid short")
    return len(outer.items)

def _summarize_chunk(record: _RegionRecord, path: Path) -> dict[str, Any]:
    where, nbt = f"{path}: chunk ({record.x}, {record.z})", record.nbt
    if _int(nbt.get("DataVersion"), f"{where}.DataVersion") != DATA_VERSION:
        raise FixtureError(f"{where}.DataVersion: expected {DATA_VERSION}")
    x = _int(nbt.get("xPos"), f"{where}.xPos")
    z = _int(nbt.get("zPos"), f"{where}.zPos")
    if (x, z) != (record.x, record.z):
        raise FixtureError(f"{where}: xPos/zPos ({x}, {z}) does not match its slot")
    status = _string(nbt.get("Status"), f"{where}.Status")
    sections = _list(nbt.get("sections"), f"{where}.sections")
    _int(nbt.get("yPos"), f"{where}.yPos")
    blocks: set[str] = set()
    biomes: set[str] = set()
    non_empty = 0
    for index, entry in enumerate(sections.items):
        section = _compound(entry, f"{where}.sections[{index}]")
        block_names, biome_names, populated = _validate_section(
            section, f"{where}.sections[{index}]"
        )
        blocks.update(block_names)
        biomes.update(biome_names)
        non_empty += int(populated)
    heightmaps = _compound(nbt.get("Heightmaps"), f"{where}.Heightmaps")
    for name in ("MOTION_BLOCKING", "WORLD_SURFACE", "OCEAN_FLOOR"):
        if name not in heightmaps:
            raise FixtureError(f"{where}.Heightmaps: missing {name}")
    for name in sorted(heightmaps):
        words = _words(heightmaps[name], f"{where}.Heightmaps.{name}")
        if len(words) != HEIGHTMAP_WORDS:
            raise FixtureError(f"{where}.Heightmaps.{name}: expected 37 words")
    block_ticks = _validate_ticks(nbt.get("block_ticks"), f"{where}.block_ticks")
    fluid_ticks = _validate_ticks(nbt.get("fluid_ticks"), f"{where}.fluid_ticks")
    block_entities = _list(nbt.get("block_entities"), f"{where}.block_entities")
    for index, entry in enumerate(block_entities.items):
        _string(
            _compound(entry, f"{where}.block_entities[{index}]").get("id"),
            f"{where}.block_entities[{index}].id",
        )
    _validate_post_processing(nbt.get("PostProcessing"), f"{where}.PostProcessing")
    return {"x": x, "z": z, "status": status, "sections": len(sections.items),
            "non_empty_sections": non_empty, "block_palette": sorted(blocks),
            "biome_palette": sorted(biomes), "heightmaps": sorted(heightmaps),
            "block_entities": len(block_entities.items),
            "block_ticks": block_ticks, "fluid_ticks": fluid_ticks}

def _validate_level(root: Path) -> dict[str, Any]:
    path = root / "level.dat"
    if not path.is_file():
        raise FixtureError(f"{path}: missing level.dat")
    data = _compound(
        _read_gzip_nbt(path, MAX_LEVEL_NBT_BYTES).get("Data"), "level.dat Data"
    )
    if _int(data.get("DataVersion"), "level.dat Data.DataVersion") != DATA_VERSION:
        raise FixtureError(f"level.dat Data.DataVersion: expected {DATA_VERSION}")
    name = _string(data.get("LevelName"), "level.dat Data.LevelName")
    if "\U0001F600" not in name:
        raise FixtureError("level.dat Data.LevelName: U+1F600 did not round-trip")
    spawn = _compound(data.get("spawn"), "level.dat Data.spawn")
    spawn_pos = spawn.get("pos")
    if isinstance(spawn_pos, Array):
        spawn_pos = spawn_pos.value
    if not isinstance(spawn_pos, list) or len(spawn_pos) != 3:
        raise FixtureError("level.dat Data.spawn.pos: expected an int array of 3")
    settings_path = root / "data" / "minecraft" / "world_gen_settings.dat"
    if not settings_path.is_file():
        raise FixtureError(f"{settings_path}: missing world_gen_settings.dat")
    settings = _compound(
        _read_gzip_nbt(settings_path, MAX_LEVEL_NBT_BYTES).get("data"),
        "world_gen_settings.dat data",
    )
    seed = _int(settings.get("seed"), "world_gen_settings.dat data.seed")
    return {"data_version": DATA_VERSION, "level_name": name, "seed": seed,
            "spawn": list(spawn_pos), "dimensions": "dimensions/minecraft"}

def _validate_entities(root: Path) -> dict[str, Any]:
    directory = root / "dimensions" / "minecraft" / "overworld" / "entities"
    paths = sorted(directory.glob("*.mca")) if directory.is_dir() else []
    if not paths:
        raise FixtureError(f"{directory}: no entities region files")
    files = chunks = total = 0
    by_type: dict[str, int] = {}
    for path in paths:
        files += 1
        for record in _read_region_file(path):
            where, nbt = f"{path}: chunk ({record.x}, {record.z})", record.nbt
            if _int(nbt.get("DataVersion"), f"{where}.DataVersion") != DATA_VERSION:
                raise FixtureError(f"{where}.DataVersion: expected {DATA_VERSION}")
            position = _compound(nbt.get("Position"), f"{where}.Position")
            coords = (_int(position.get("x"), f"{where}.Position.x"),
                      _int(position.get("z"), f"{where}.Position.z"))
            if coords != (record.x, record.z):
                raise FixtureError(f"{where}: Position does not match its slot")
            entities = _list(nbt.get("Entities"), f"{where}.Entities")
            for index, entry in enumerate(entities.items):
                entity = _compound(entry, f"{where}.Entities[{index}]")
                entity_id = _string(entity.get("id"), f"{where}.Entities[{index}].id")
                by_type[entity_id] = by_type.get(entity_id, 0) + 1
                total += 1
            chunks += 1
    return {"files": files, "chunks": chunks, "total": total,
            "by_type": dict(sorted(by_type.items()))}

def _validate_poi(root: Path) -> None:
    directory = root / "dimensions" / "minecraft" / "overworld" / "poi"
    paths = sorted(directory.glob("*.mca")) if directory.is_dir() else []
    if not paths:
        raise FixtureError(f"{directory}: no poi region files")
    for path in paths:
        for record in _read_region_file(path):
            where, nbt = f"{path}: chunk ({record.x}, {record.z})", record.nbt
            if _int(nbt.get("DataVersion"), f"{where}.DataVersion") != DATA_VERSION:
                raise FixtureError(f"{where}.DataVersion: expected {DATA_VERSION}")
            sections = _list(nbt.get("Sections"), f"{where}.Sections")
            for index, entry in enumerate(sections.items):
                section = _compound(entry, f"{where}.Sections[{index}]")
                if "valid" in section:
                    _int(section["valid"], f"{where}.Sections[{index}].valid")
                records = _list(section.get("Records"), f"{where}.Sections[{index}].Records")
                for record_index, record_entry in enumerate(records.items):
                    poi = _compound(record_entry, f"{where}.Records[{record_index}]")
                    _string(poi.get("type"), f"{where}.Records[{record_index}].type")
                    _int(poi.get("pos"), f"{where}.Records[{record_index}].pos")
                    if "free_tickets" in poi:
                        _int(poi["free_tickets"], "free_tickets")

def _validate_datapacks(root: Path) -> None:
    directory = root / "datapacks"
    metas = sorted(directory.glob("*/pack.mcmeta")) if directory.is_dir() else []
    if not metas:
        raise FixtureError(f"{directory}: no <pack>/pack.mcmeta found")
    for path in metas:
        try:
            document = json.loads(_read_file_limited(path, MAX_JSON_BYTES).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise FixtureError(f"{path}: invalid pack JSON: {exc}") from exc
        pack = _compound(document.get("pack"), f"{path} pack")
        if _int(pack.get("pack_format"), f"{path} pack.pack_format") <= 0:
            raise FixtureError(f"{path}: pack_format must be positive")

def _validate_playerdata(root: Path) -> list[dict[str, Any]]:
    directory = root / "players" / "data"
    paths = sorted(directory.glob("*.dat")) if directory.is_dir() else []
    if not paths:
        raise FixtureError(f"{directory}: no player .dat files")
    players: list[dict[str, Any]] = []
    seen: set[str] = set()
    for path in paths:
        document = _read_gzip_nbt(path, MAX_PLAYER_NBT_BYTES)
        uuid_ints = document.get("UUID")
        if not isinstance(uuid_ints, list) or len(uuid_ints) != 4:
            raise FixtureError(f"{path}: UUID must be TAG_Int_Array[4]")
        uuid = _uuid_text(uuid_ints)
        if path.stem.lower() != uuid: raise FixtureError(f"{path}: UUID mismatch: {uuid}")
        if uuid in seen: raise FixtureError(f"{path}: duplicate player UUID {uuid}")
        seen.add(uuid)
        xp_level = _int(document.get("XpLevel"), f"{path} XpLevel")
        players.append({"uuid": uuid, "xp_level": xp_level})
    players.sort(key=lambda item: item["uuid"])
    return players

def _canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)

def verify_world(root: Path, *, expect_seed: int | None = None) -> dict[str, Any]:
    """Validate the world at ``root`` and return the deterministic summary."""

    if not root.is_dir():
        raise FixtureError(f"{root}: not a directory")
    level = _validate_level(root)
    region_dir = root / "dimensions" / "minecraft" / "overworld" / "region"
    paths = sorted(region_dir.glob("*.mca")) if region_dir.is_dir() else []
    if not paths:
        raise FixtureError(f"{region_dir}: no region files")
    chunks: list[dict[str, Any]] = []
    seen: set[tuple[int, int]] = set()
    for path in paths:
        for record in _read_region_file(path):
            if (record.x, record.z) in seen:
                raise FixtureError(f"{path}: duplicate chunk ({record.x}, {record.z})")
            seen.add((record.x, record.z))
            chunks.append(_summarize_chunk(record, path))
    chunks.sort(key=lambda chunk: (chunk["x"], chunk["z"]))
    if expect_seed is not None and level["seed"] != expect_seed:
        raise FixtureError(f"seed mismatch: expected {expect_seed}, found {level['seed']}")
    summary: dict[str, Any] = {
        "format": FORMAT_ID,
        "level": level,
        "chunks": chunks,
        "entities": _validate_entities(root),
        "playerdata": _validate_playerdata(root),
    }
    _validate_poi(root)
    _validate_datapacks(root)
    summary["summary_sha256"] = hashlib.sha256(
        _canonical_json(summary).encode("utf-8")
    ).hexdigest()
    return summary

# --- CLI / selftest
def _format_human(summary: dict[str, Any]) -> str:
    level = summary["level"]
    lines = [
        f"format: {summary['format']}",
        f"level: data_version={level['data_version']} seed={level['seed']} "
        f"name={json.dumps(level['level_name'], ensure_ascii=False)}",
        f"chunks: {len(summary['chunks'])}",
    ]
    for c in summary["chunks"]:
        lines.append(
            f"  ({c['x']}, {c['z']}): {c['status']} sections={c['sections']} "
            f"non_empty={c['non_empty_sections']} block_entities={c['block_entities']} "
            f"block_ticks={c['block_ticks']} fluid_ticks={c['fluid_ticks']}"
        )
    e = summary["entities"]
    by_type = ", ".join(f"{k}={v}" for k, v in sorted(e["by_type"].items()))
    lines += [
        f"entities: files={e['files']} chunks={e['chunks']} total={e['total']} [{by_type}]",
        f"players: {len(summary['playerdata'])}",
    ]
    lines += [f"  {p['uuid']} xp_level={p['xp_level']}" for p in summary["playerdata"]]
    lines.append(f"summary_sha256: {summary['summary_sha256']}")
    return "\n".join(lines) + "\n"

def _load_tree(root: Path) -> dict[str, bytes]:
    return {str(path.relative_to(root)): path.read_bytes()
            for path in sorted(root.rglob("*")) if path.is_file()}

def _compare_trees(left: Path, right: Path) -> None:
    left_files, right_files = _load_tree(left), _load_tree(right)
    if left_files.keys() != right_files.keys():
        raise FixtureError(
            f"selftest: file set mismatch: {sorted(left_files.keys() ^ right_files.keys())}"
        )
    for name in sorted(left_files):
        if left_files[name] != right_files[name]:
            raise FixtureError(f"selftest: byte mismatch in {name}")

def selftest() -> int:
    """Generate twice, compare byte-for-byte, verify, and report the chunk count."""

    with tempfile.TemporaryDirectory(prefix="refrain-anvil-selftest-") as temporary:
        first, second = Path(temporary) / "first", Path(temporary) / "second"
        generated = generate_world(first, seed=1234)
        generate_world(second, seed=1234)
        _compare_trees(first, second)
        summary = verify_world(first, expect_seed=1234)
        if summary["level"]["level_name"] != LEVEL_NAME:
            raise FixtureError("selftest: level name did not round-trip")
        if len(summary["chunks"]) != generated:
            raise FixtureError(
                f"selftest: generated {generated} chunks, "
                f"verified {len(summary['chunks'])}"
            )
        print(f"selftest OK: {generated} chunks")
    return 0

def _chunks_argument(text: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if not 0 <= value <= MAX_CHUNKS:
        raise argparse.ArgumentTypeError(f"must be between 0 and {MAX_CHUNKS}")
    return value

def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="make_test_world.py",
        description=(
            "Generate and validate a synthetic vanilla-format Java Anvil world. "
            "Independent oracle for Refrain world import/export; no Mojang "
            "code or assets are used."
        ),
        epilog=(
            "examples:\n"
            "  make_test_world.py gen --out /tmp/fx --seed 1234\n"
            "  make_test_world.py gen --out /tmp/fx --chunks 8 --force\n"
            "  make_test_world.py verify /tmp/fx --json | head -30\n"
            "  make_test_world.py selftest\n\n"
            "gen layout: level.dat, data/minecraft/world_gen_settings.dat,\n"
            "dimensions/minecraft/overworld/{region,entities,poi}/*.mca,\n"
            "players/data/<uuid>.dat and\n"
            "datapacks/refrain_fixture/. Output is byte-deterministic."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate = subparsers.add_parser(
        "gen", help="generate a synthetic test world",
        description=(
            "Create a deterministic synthetic Anvil world. The destination must be "
            "absent or empty unless --force is given. The default fixture has three "
            "chunks in r.0.0 plus (-1, 0) in r.-1.0; --chunks replaces the r.0.0 set "
            "with (i % 8, i // 8) for i in 0..N-1."
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    generate.add_argument("--out", type=Path, required=True, help="destination directory")
    generate.add_argument("--seed", type=int, default=DEFAULT_SEED,
                          help="world seed (32-bit signed)")
    generate.add_argument("--chunks", type=_chunks_argument, default=None, metavar="N",
                          help=f"N chunks in r.0.0 (0..{MAX_CHUNKS}); default fixture if omitted")
    generate.add_argument("--force", action="store_true",
                          help="replace a non-empty destination directory")
    verify = subparsers.add_parser(
        "verify", help="validate a world and print a summary",
        description=(
            "Independently parse and structurally validate an Anvil world. Exit 0 on "
            "success; exit 1 with 'error: ...' on stderr otherwise. Sector layout, "
            "palette order, gzip mtime, block_count, isLightOn and blending_data are "
            "not required, so worlds from other valid writers are accepted."
        ),
    )
    verify.add_argument("dir", type=Path, help="world directory to validate")
    verify.add_argument("--json", action="store_true",
                        help="print a compact, sorted JSON summary")
    verify.add_argument("--expect-seed", type=int, default=None, metavar="N",
                        help="fail unless seed == N")
    subparsers.add_parser(
        "selftest", help="generate twice, compare bytes, and verify",
        description=(
            "Generate the default fixture twice with the same seed into temporary "
            "directories, assert the two trees are byte-identical, verify the world "
            "and print 'selftest OK: <N> chunks'."
        ),
    )
    return parser

def main(argv: Sequence[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "gen":
            generate_world(args.out, seed=args.seed, chunks=args.chunks, force=args.force)
        elif args.command == "verify":
            summary = verify_world(args.dir, expect_seed=args.expect_seed)
            sys.stdout.write(
                _canonical_json(summary) + "\n" if args.json else _format_human(summary)
            )
        else:
            return selftest()
    except (FixtureError, OSError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
