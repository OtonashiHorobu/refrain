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
        "--payload",
        action="store_true",
        help="compare packet payload bytes as well as ids",
    )
    args = parser.parse_args()

    left = read_capture(args.left)
    right = read_capture(args.right)
    print(f"oracle: left {len(left)} packet(s), right {len(right)} packet(s)")

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
