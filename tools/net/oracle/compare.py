#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Compare two RFCAP1 protocol captures.

The oracle is local-only: captures are generated against a locally owned
vanilla server and are never committed. This tool compares the capture streams
by state/packet-id sequence, and optionally by payload bytes.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from nbt import NbtError, first_difference, read_root

MAGIC = b"RFCAP1\n"


@dataclass(frozen=True)
class Packet:
    state: int
    packet_id: int
    body: bytes


def read_capture(path: Path) -> list[Packet]:
    data = path.read_bytes()
    if not data.startswith(MAGIC):
        raise ValueError(f"{path}: missing {MAGIC!r} magic")
    packets: list[Packet] = []
    offset = len(MAGIC)
    while offset < len(data):
        if offset + 9 > len(data):
            raise ValueError(f"{path}: truncated packet header at {offset}")
        state = data[offset]
        packet_id, length = struct.unpack_from("<II", data, offset + 1)
        offset += 9
        if offset + length > len(data):
            raise ValueError(f"{path}: truncated payload at {offset}")
        packets.append(Packet(state, packet_id, data[offset : offset + length]))
        offset += length
    return packets


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated VarInt")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, offset


def read_string(data: bytes, offset: int) -> tuple[str, int]:
    length, offset = read_varint(data, offset)
    end = offset + length
    if end > len(data):
        raise ValueError("truncated string")
    return data[offset:end].decode("utf-8"), end


def read_i64(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 8 > len(data):
        raise ValueError("truncated i64")
    return struct.unpack_from("<q", data, offset)[0], offset + 8


def skip_nbt(data: bytes, offset: int) -> int:
    # Network NBT in packed registry entries has an unnamed root tag.
    tag = data[offset]
    offset += 1
    if tag == 0:
        return offset
    return skip_nbt_payload(data, offset, tag, 0)


def skip_nbt_payload(data: bytes, offset: int, tag: int, depth: int) -> int:
    if depth > 512:
        raise ValueError("nbt nesting is too deep")
    if tag == 1:
        return offset + 1
    if tag == 2:
        return offset + 2
    if tag in (3, 5):
        return offset + 4
    if tag in (4, 6):
        return offset + 8
    if tag == 7:
        length = int.from_bytes(data[offset : offset + 4], "big", signed=True)
        return offset + 4 + max(length, 0)
    if tag == 8:
        length = int.from_bytes(data[offset : offset + 2], "big")
        return offset + 2 + length
    if tag == 9:
        element = data[offset]
        count = int.from_bytes(data[offset + 1 : offset + 5], "big", signed=True)
        offset += 5
        for _ in range(max(count, 0)):
            offset = skip_nbt_payload(data, offset, element, depth + 1)
        return offset
    if tag == 10:
        while True:
            child = data[offset]
            offset += 1
            if child == 0:
                return offset
            name_length = int.from_bytes(data[offset : offset + 2], "big")
            offset += 2 + name_length
            offset = skip_nbt_payload(data, offset, child, depth + 1)
    if tag == 11:
        length = int.from_bytes(data[offset : offset + 4], "big", signed=True)
        return offset + 4 + 4 * max(length, 0)
    if tag == 12:
        length = int.from_bytes(data[offset : offset + 4], "big", signed=True)
        return offset + 4 + 8 * max(length, 0)
    raise ValueError(f"unknown NBT tag {tag}")


def registry_nbt(entries: list[Packet]) -> dict[str, dict[str, tuple[int, object]]]:
    result: dict[str, dict[str, tuple[int, object]]] = {}
    for packet in entries:
        if packet.state != 1 or packet.packet_id != 7:
            continue
        _, offset = read_varint(packet.body, 0)
        registry, offset = read_string(packet.body, offset)
        count, offset = read_varint(packet.body, offset)
        values: dict[str, tuple[int, object]] = {}
        for _ in range(count):
            name, offset = read_string(packet.body, offset)
            present = packet.body[offset]
            offset += 1
            if present:
                try:
                    value, offset = read_root(packet.body, offset)
                except NbtError as error:
                    raise ValueError(f"{registry}/{name}: {error}") from error
                values[name] = value
        result[registry] = values
    return result


def registry_entries(entries: list[Packet]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for packet in entries:
        if packet.state != 1 or packet.packet_id != 7:
            continue
        _, offset = read_varint(packet.body, 0)
        registry, offset = read_string(packet.body, offset)
        count, offset = read_varint(packet.body, offset)
        names = []
        for _ in range(count):
            name, offset = read_string(packet.body, offset)
            names.append(name)
            present = packet.body[offset]
            offset += 1
            if present:
                offset = skip_nbt(packet.body, offset)
        result[registry] = names
    return result


def registry_summary(entries: list[Packet]) -> list[tuple[str, int]]:
    """Registry id and declared entry count for each configuration registry."""
    summary = []
    for packet in entries:
        if packet.state != 1 or packet.packet_id != 7:
            continue
        _, offset = read_varint(packet.body, 0)
        registry, offset = read_string(packet.body, offset)
        count, _ = read_varint(packet.body, offset)
        summary.append((registry, count))
    return summary


def login_fields(body: bytes) -> dict[str, object]:
    _, offset = read_varint(body, 0)  # packet id
    fields: dict[str, object] = {}
    fields["player_id"] = struct.unpack_from("<i", body, offset)[0]
    offset += 4
    fields["hardcore"] = bool(body[offset])
    offset += 1
    count, offset = read_varint(body, offset)
    levels = []
    for _ in range(count):
        level, offset = read_string(body, offset)
        levels.append(level)
    fields["levels"] = levels
    fields["max_players"], offset = read_varint(body, offset)
    fields["chunk_radius"], offset = read_varint(body, offset)
    fields["simulation_distance"], offset = read_varint(body, offset)
    fields["reduced_debug_info"] = bool(body[offset])
    offset += 1
    fields["show_death_screen"] = bool(body[offset])
    offset += 1
    fields["limited_crafting"] = bool(body[offset])
    offset += 1
    fields["dimension_type"], offset = read_varint(body, offset)
    fields["dimension"], offset = read_string(body, offset)
    fields["seed"], offset = read_i64(body, offset)
    fields["game_type"], offset = read_varint(body, offset)
    previous, offset = read_varint(body, offset)
    fields["previous_game_type"] = None if previous == 0 else previous - 1
    fields["debug"] = bool(body[offset])
    offset += 1
    fields["flat"] = bool(body[offset])
    offset += 1
    has_death = bool(body[offset])
    offset += 1
    if has_death:
        death, offset = read_i64(body, offset)
        fields["last_death"] = death
    else:
        fields["last_death"] = None
    fields["portal_cooldown"], offset = read_varint(body, offset)
    fields["sea_level"], offset = read_varint(body, offset)
    fields["online_mode"] = bool(body[offset])
    offset += 1
    fields["enforces_secure_chat"] = bool(body[offset])
    return fields


def first_payload_mismatch(left: bytes, right: bytes) -> int | None:
    for index, (a, b) in enumerate(zip(left, right, strict=False)):
        if a != b:
            return index
    if len(left) != len(right):
        return min(len(left), len(right))
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument(
        "--login-ignore",
        default="player_id,seed",
        help="comma-separated login fields normalized away",
    )
    parser.add_argument(
        "--registry-nbt",
        action="store_true",
        help="compare registry entry NBT for the selected registries",
    )
    parser.add_argument(
        "--nbt-registries",
        default="minecraft:worldgen/biome,minecraft:dimension_type,minecraft:damage_type",
        help="comma-separated registry ids for --registry-nbt",
    )
    parser.add_argument(
        "--registry-entries",
        action="store_true",
        help="compare configuration registry entry ids",
    )
    parser.add_argument(
        "--registries",
        action="store_true",
        help="compare configuration registry ids and entry counts",
    )
    parser.add_argument(
        "--login",
        action="store_true",
        help="compare decoded play login fields instead of raw bytes",
    )
    parser.add_argument(
        "--payload",
        action="store_true",
        help="compare packet payload bytes as well as ids",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="compare only the first N packets (0 = all)",
    )
    parser.add_argument(
        "--states",
        default="",
        help="comma-separated states to compare (for example 2)",
    )
    parser.add_argument(
        "--ignore",
        default="",
        help="comma-separated state:id pairs to skip, for example 2:0,2:46",
    )
    args = parser.parse_args()

    ignored = {
        tuple(int(part, 0) for part in item.split(":"))
        for item in args.ignore.split(",")
        if item
    }
    left_all = read_capture(args.left)
    right_all = read_capture(args.right)
    states = {int(item, 0) for item in args.states.split(",") if item}
    left = [
        packet
        for packet in left_all
        if (packet.state, packet.packet_id) not in ignored
        and (not states or packet.state in states)
    ]
    right = [
        packet
        for packet in right_all
        if (packet.state, packet.packet_id) not in ignored
        and (not states or packet.state in states)
    ]
    if args.limit > 0:
        left = left[: args.limit]
        right = right[: args.limit]
    print(
        f"oracle: left {len(left_all)} packet(s), right {len(right_all)} packet(s); "
        f"comparing {len(left)} vs {len(right)}"
    )
    if args.registry_nbt:
        selected = {
            item.strip() for item in args.nbt_registries.split(",") if item.strip()
        }
        left_nbt = registry_nbt(left_all)
        right_nbt = registry_nbt(right_all)
        failures = 0
        for registry in sorted(selected):
            left_entries = left_nbt.get(registry, {})
            right_entries = right_nbt.get(registry, {})
            if set(left_entries) != set(right_entries):
                print(f"oracle: registry '{registry}' has different entry ids")
                failures += 1
                continue
            for name in left_entries:
                difference = first_difference(
                    left_entries[name], right_entries[name], f"{registry}/{name}"
                )
                if difference is not None:
                    print(f"oracle: registry NBT differs: {difference}")
                    failures += 1
                    break
        if failures == 0:
            print(
                f"oracle: {len(selected)} registry entry sets match "
                "for the selected registries"
            )
            return 0
        return 1

    if args.registry_entries:
        left_entries = registry_entries(left_all)
        right_entries = registry_entries(right_all)
        failures = 0
        for registry in sorted(set(left_entries) | set(right_entries)):
            left_names = left_entries.get(registry)
            right_names = right_entries.get(registry)
            if left_names != right_names:
                if left_names is None or right_names is None:
                    print(f"oracle: registry '{registry}' is missing on one side")
                else:
                    for index, (left_name, right_name) in enumerate(
                        zip(left_names, right_names, strict=False)
                    ):
                        if left_name != right_name:
                            print(
                                f"oracle: registry '{registry}' entry[{index}] "
                                f"differs: left={left_name!r} right={right_name!r}"
                            )
                            break
                    if len(left_names) != len(right_names):
                        print(
                            f"oracle: registry '{registry}' entry count differs "
                            f"({len(left_names)} vs {len(right_names)})"
                        )
                failures += 1
        if failures == 0:
            print(
                f"oracle: {len(left_entries)} registry entry lists match "
                "(ids and order)"
            )
            return 0
        return 1

    if args.registries:
        left_registries = registry_summary(left_all)
        right_registries = registry_summary(right_all)
        failures = 0
        for index in range(max(len(left_registries), len(right_registries))):
            left_entry = (
                left_registries[index] if index < len(left_registries) else None
            )
            right_entry = (
                right_registries[index]
                if index < len(right_registries)
                else None
            )
            if left_entry != right_entry:
                print(
                    f"oracle: registry[{index}] differs: "
                    f"left={left_entry!r} right={right_entry!r}"
                )
                failures += 1
        if failures == 0:
            print(
                f"oracle: {len(left_registries)} registry headers match "
                "(ids and entry counts)"
            )
            return 0
        return 1

    if args.login:
        left_login = next(
            (packet for packet in left_all if packet.state == 2 and packet.packet_id == 50),
            None,
        )
        right_login = next(
            (packet for packet in right_all if packet.state == 2 and packet.packet_id == 50),
            None,
        )
        if left_login is None or right_login is None:
            print("oracle: login packet is missing")
            return 1
        left_fields = login_fields(left_login.body)
        right_fields = login_fields(right_login.body)
        ignored_fields = {
            field.strip()
            for field in args.login_ignore.split(",")
            if field.strip()
        }
        failures = 0
        for key, value in left_fields.items():
            if key in ignored_fields:
                continue
            if key == "levels":
                # M4 opens only the overworld; vanilla still lists every
                # registered level key even when the nether is disabled.
                value = [level for level in value if level == "minecraft:overworld"]
                right_levels = [
                    level
                    for level in right_fields.get("levels", [])
                    if level == "minecraft:overworld"
                ]
                if value != right_levels:
                    print(
                        f"oracle: login field 'levels' differs: "
                        f"left={value!r} right={right_levels!r}"
                    )
                    failures += 1
                continue
            if value != right_fields.get(key):
                print(
                    f"oracle: login field '{key}' differs: "
                    f"left={value!r} right={right_fields.get(key)!r}"
                )
                failures += 1
        if failures == 0:
            print("oracle: decoded login fields match (except player entity id)")
            return 0
        return 1

    mismatches = 0
    for index, (a, b) in enumerate(zip(left, right, strict=False)):
        if (a.state, a.packet_id) != (b.state, b.packet_id):
            print(
                f"oracle: mismatch at {index}: "
                f"left state={a.state} id={a.packet_id}, "
                f"right state={b.state} id={b.packet_id}"
            )
            mismatches += 1
            break
        if args.payload:
            offset = first_payload_mismatch(a.body, b.body)
            if offset is not None:
                window = 8
                print(
                    f"oracle: payload mismatch at {index} "
                    f"(state={a.state} id={a.packet_id}) offset={offset}"
                )
                print(f"  left : {a.body[offset:offset + window].hex()}")
                print(f"  right: {b.body[offset:offset + window].hex()}")
                mismatches += 1
                break

    if len(left) != len(right):
        print(f"oracle: packet count differs ({len(left)} vs {len(right)})")
        mismatches += 1

    left_ids = {(packet.state, packet.packet_id) for packet in left}
    right_ids = {(packet.state, packet.packet_id) for packet in right}
    print(
        f"oracle: unique ids left={len(left_ids)} right={len(right_ids)} "
        f"only-left={len(left_ids - right_ids)} "
        f"only-right={len(right_ids - left_ids)}"
    )
    if mismatches == 0:
        print("oracle: captures match")
        return 0
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"oracle: {error}", file=sys.stderr)
        raise SystemExit(2) from error
