#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Decode `level_chunk_with_light` bodies from an RFCAP1 capture.

The decoder is intentionally independent of Refrain's C++ codec: it follows
the wire layout from the protocol specification so a field-level comparison
catches both encoder bugs and accidental assumptions shared with the server.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from nbt import NbtError, read_root

CHUNK_PACKET_ID = 46
SECTION_ENTRIES = 4096
BIOME_ENTRIES = 64
LIGHT_BYTES = 2048


class ChunkError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes, offset: int = 0) -> None:
        self.data = data
        self.offset = offset

    def remaining(self) -> int:
        return len(self.data) - self.offset

    def take(self, count: int) -> bytes:
        end = self.offset + count
        if count < 0 or end > len(self.data):
            raise ChunkError(f"truncated payload at {self.offset}")
        value = self.data[self.offset : end]
        self.offset = end
        return value

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def i16(self) -> int:
        return struct.unpack(">h", self.take(2))[0]

    def i32(self) -> int:
        return struct.unpack(">i", self.take(4))[0]

    def i64(self) -> int:
        return struct.unpack(">q", self.take(8))[0]

    def varint(self) -> int:
        value = 0
        shift = 0
        while True:
            byte = self.u8()
            value |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return value
            shift += 7
            if shift > 35:
                raise ChunkError("VarInt is longer than five bytes")

    def bool(self) -> bool:
        value = self.u8()
        if value not in (0, 1):
            raise ChunkError(f"invalid boolean {value}")
        return bool(value)

    def string(self) -> str:
        length = self.varint()
        return self.take(length).decode("utf-8")

    def bitset(self) -> bytes:
        length = self.varint()
        if length > self.remaining():
            raise ChunkError("bitset declares more bytes than the payload holds")
        return self.take(length)


@dataclass(frozen=True)
class Section:
    y: int
    non_empty: int
    fluids: int
    block_bits: int
    block_palette: tuple[int, ...]
    block_values: tuple[int, ...]
    biome_bits: int
    biome_palette: tuple[int, ...]
    biome_values: tuple[int, ...]


@dataclass(frozen=True)
class BlockEntity:
    packed_xz: int
    y: int
    type_id: int
    tag: object | None


@dataclass(frozen=True)
class Light:
    sky_mask: bytes
    block_mask: bytes
    empty_sky_mask: bytes
    empty_block_mask: bytes
    sky_updates: tuple[bytes, ...]
    block_updates: tuple[bytes, ...]


@dataclass(frozen=True)
class Chunk:
    x: int
    z: int
    heightmaps: dict[int, tuple[int, ...]]
    sections: tuple[Section, ...]
    block_entities: tuple[BlockEntity, ...]
    light: Light
    body_size: int


def unpack_bits(words: tuple[int, ...], count: int, bits: int) -> tuple[int, ...]:
    if bits == 0:
        if count != 0:
            raise ChunkError("zero-bit storage declares entries")
        return ()
    if bits > 32:
        raise ChunkError(f"unsupported palette width {bits}")
    per_word = 64 // bits
    needed = (count + per_word - 1) // per_word
    if len(words) < needed:
        raise ChunkError(
            f"palette storage has {len(words)} words, expected {needed}"
        )
    mask = (1 << bits) - 1
    values = []
    for index in range(count):
        word = words[index // per_word]
        shift = (index % per_word) * bits
        values.append((word >> shift) & mask)
    return tuple(values)


def read_palette(reader: Reader, entries: int, global_bits: int) -> tuple[
    int, tuple[int, ...], tuple[int, ...]
]:
    bits = reader.u8()
    if bits == 0:
        palette = (reader.varint(),)
        values = palette * entries
        return bits, palette, values
    if bits <= global_bits:
        size = reader.varint()
        if size == 0:
            raise ChunkError("empty palette")
        if size > 1 << bits:
            raise ChunkError(
                f"palette has {size} entries for {bits}-bit storage"
            )
        palette = tuple(reader.varint() for _ in range(size))
        expected_words = (
            entries + (64 // bits) - 1
        ) // (64 // bits)
        words = tuple(reader.i64() for _ in range(expected_words))
        indices = unpack_bits(words, entries, bits)
        values = tuple(palette[index] for index in indices)
        return bits, palette, values
    # Global palette: the bit width itself is the storage width and no
    # palette follows.
    per_word = 64 // bits
    word_count = (entries + per_word - 1) // per_word
    words = tuple(reader.i64() for _ in range(word_count))
    return bits, (), unpack_bits(words, entries, bits)


def read_section(reader: Reader, y: int) -> Section:
    non_empty = reader.u16()
    fluids = reader.u16()
    block_bits, block_palette, block_values = read_palette(
        reader, SECTION_ENTRIES, 8
    )
    biome_bits, biome_palette, biome_values = read_palette(
        reader, BIOME_ENTRIES, 3
    )
    return Section(
        y=y,
        non_empty=non_empty,
        fluids=fluids,
        block_bits=block_bits,
        block_palette=block_palette,
        block_values=block_values,
        biome_bits=biome_bits,
        biome_palette=biome_palette,
        biome_values=biome_values,
    )


def decode_chunk(body: bytes) -> Chunk:
    reader = Reader(body)
    packet_id = reader.varint()
    if packet_id != CHUNK_PACKET_ID:
        raise ChunkError(f"not a level_chunk_with_light packet: {packet_id}")
    x = reader.i32()
    z = reader.i32()

    heightmap_count = reader.varint()
    if heightmap_count > 8:
        raise ChunkError(f"heightmap count {heightmap_count} is too large")
    heightmaps: dict[int, tuple[int, ...]] = {}
    for _ in range(heightmap_count):
        type_id = reader.varint()
        word_count = reader.varint()
        if type_id in heightmaps:
            raise ChunkError(f"duplicate heightmap type {type_id}")
        heightmaps[type_id] = tuple(reader.i64() for _ in range(word_count))

    section_size = reader.varint()
    section_bytes = reader.take(section_size)
    sections_reader = Reader(section_bytes)
    sections: list[Section] = []
    y = -4
    while not sections_reader.remaining() == 0:
        if y > 19:
            raise ChunkError("section buffer has more than 24 sections")
        sections.append(read_section(sections_reader, y))
        y += 1
    if not sections:
        raise ChunkError("section buffer is empty")

    block_entity_count = reader.varint()
    if block_entity_count > 1024:
        raise ChunkError(f"block entity count {block_entity_count} is too large")
    block_entities: list[BlockEntity] = []
    for _ in range(block_entity_count):
        packed_xz = reader.u8()
        entity_y = reader.i16()
        type_id = reader.varint()
        tag = None
        if reader.bool():
            try:
                tag, reader.offset = read_root(reader.data, reader.offset)
            except NbtError as error:
                raise ChunkError(f"block entity NBT: {error}") from error
        block_entities.append(
            BlockEntity(
                packed_xz=packed_xz,
                y=entity_y,
                type_id=type_id,
                tag=tag,
            )
        )

    sky_mask = reader.bitset()
    block_mask = reader.bitset()
    empty_sky_mask = reader.bitset()
    empty_block_mask = reader.bitset()

    def read_updates() -> tuple[bytes, ...]:
        count = reader.varint()
        if count > 64:
            raise ChunkError(f"light update count {count} is too large")
        updates = []
        for _ in range(count):
            length = reader.varint()
            if length != LIGHT_BYTES:
                raise ChunkError(
                    f"light update has {length} bytes, expected {LIGHT_BYTES}"
                )
            updates.append(reader.take(length))
        return tuple(updates)

    sky_updates = read_updates()
    block_updates = read_updates()
    if reader.remaining() != 0:
        raise ChunkError(f"{reader.remaining()} trailing bytes after light data")
    return Chunk(
        x=x,
        z=z,
        heightmaps=heightmaps,
        sections=tuple(sections),
        block_entities=tuple(block_entities),
        light=Light(
            sky_mask=sky_mask,
            block_mask=block_mask,
            empty_sky_mask=empty_sky_mask,
            empty_block_mask=empty_block_mask,
            sky_updates=sky_updates,
            block_updates=block_updates,
        ),
        body_size=len(body),
    )


def first_difference(left: Chunk, right: Chunk) -> str | None:
    if (left.x, left.z) != (right.x, right.z):
        return f"position left=({left.x},{left.z}) right=({right.x},{right.z})"
    if left.heightmaps.keys() != right.heightmaps.keys():
        return (
            "heightmap types left="
            f"{sorted(left.heightmaps)} right={sorted(right.heightmaps)}"
        )
    for type_id in sorted(left.heightmaps):
        if left.heightmaps[type_id] != right.heightmaps[type_id]:
            return f"heightmap {type_id} differs"
    if len(left.sections) != len(right.sections):
        return (
            f"section count left={len(left.sections)} right={len(right.sections)}"
        )
    for index, (a, b) in enumerate(zip(left.sections, right.sections, strict=True)):
        if a.y != b.y:
            return f"section[{index}] y left={a.y} right={b.y}"
        if a.non_empty != b.non_empty:
            return (
                f"section[{index}] non_empty left={a.non_empty} "
                f"right={b.non_empty}"
            )
        if a.fluids != b.fluids:
            return f"section[{index}] fluids left={a.fluids} right={b.fluids}"
        if a.block_values != b.block_values:
            for value_index, (left_value, right_value) in enumerate(
                zip(a.block_values, b.block_values, strict=False)
            ):
                if left_value != right_value:
                    return (
                        f"section[{index}] block[{value_index}] "
                        f"left={left_value} right={right_value}"
                    )
            return f"section[{index}] block storage length differs"
        if a.biome_values != b.biome_values:
            for value_index, (left_value, right_value) in enumerate(
                zip(a.biome_values, b.biome_values, strict=False)
            ):
                if left_value != right_value:
                    return (
                        f"section[{index}] biome[{value_index}] "
                        f"left={left_value} right={right_value}"
                    )
            return f"section[{index}] biome storage length differs"
    if len(left.block_entities) != len(right.block_entities):
        return (
            "block entity count left="
            f"{len(left.block_entities)} right={len(right.block_entities)}"
        )
    for index, (a, b) in enumerate(
        zip(left.block_entities, right.block_entities, strict=True)
    ):
        if a != b:
            return f"block entity[{index}] differs"
    if left.light.sky_mask != right.light.sky_mask:
        return "sky light mask differs"
    if left.light.block_mask != right.light.block_mask:
        return "block light mask differs"
    if left.light.empty_sky_mask != right.light.empty_sky_mask:
        return "empty sky light mask differs"
    if left.light.empty_block_mask != right.light.empty_block_mask:
        return "empty block light mask differs"
    if left.light.sky_updates != right.light.sky_updates:
        return "sky light updates differ"
    if left.light.block_updates != right.light.block_updates:
        return "block light updates differ"
    return None


def compare_chunks(
    left: list[bytes], right: list[bytes], strict: bool = False
) -> int:
    left_chunks = {}
    right_chunks = {}
    for body in left:
        chunk = decode_chunk(body)
        left_chunks[(chunk.x, chunk.z)] = chunk
    for body in right:
        chunk = decode_chunk(body)
        right_chunks[(chunk.x, chunk.z)] = chunk
    failures = 0
    missing = 0
    compared = 0
    for key in sorted(set(left_chunks) | set(right_chunks)):
        left_chunk = left_chunks.get(key)
        right_chunk = right_chunks.get(key)
        if left_chunk is None or right_chunk is None:
            missing += 1
            if strict:
                print(f"oracle: chunk {key} is missing on one side")
                failures += 1
            continue
        compared += 1
        difference = first_difference(left_chunk, right_chunk)
        if difference is not None:
            print(f"oracle: chunk {key} differs: {difference}")
            failures += 1
            if failures >= 10:
                print("oracle: stopping after 10 chunk differences")
                break
    if missing:
        print(f"oracle: {missing} chunk(s) are only present on one side")
    if failures == 0:
        print(
            "oracle: "
            f"{compared} chunk(s) match "
            "(sections, heightmaps, light, block entities)"
        )
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit("chunk.py is imported by compare.py")
