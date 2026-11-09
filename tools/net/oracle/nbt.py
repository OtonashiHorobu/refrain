# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Minimal network-NBT reader for oracle comparisons.

The packed registry entries use an unnamed root tag (type byte first); nested
compounds and lists follow the normal NBT grammar. Values keep their tag type so
an int/long or float/double mismatch is visible.
"""

from __future__ import annotations

import struct


class NbtError(ValueError):
    pass


def _need(data: bytes, offset: int, count: int) -> None:
    if offset + count > len(data):
        raise NbtError(f"truncated NBT at {offset}")


def read_tag(data: bytes, offset: int, depth: int = 0) -> tuple[tuple[int, object], int]:
    if depth > 512:
        raise NbtError("NBT nesting is too deep")
    _need(data, offset, 1)
    tag = data[offset]
    offset += 1
    if tag == 0:
        return (0, None), offset
    value, offset = read_payload(data, offset, tag, depth)
    return (tag, value), offset


def read_payload(
    data: bytes, offset: int, tag: int, depth: int
) -> tuple[object, int]:
    if tag == 1:
        _need(data, offset, 1)
        return struct.unpack_from(">b", data, offset)[0], offset + 1
    if tag == 2:
        _need(data, offset, 2)
        return struct.unpack_from(">h", data, offset)[0], offset + 2
    if tag == 3:
        _need(data, offset, 4)
        return struct.unpack_from(">i", data, offset)[0], offset + 4
    if tag == 4:
        _need(data, offset, 8)
        return struct.unpack_from(">q", data, offset)[0], offset + 8
    if tag == 5:
        _need(data, offset, 4)
        return struct.unpack_from(">f", data, offset)[0], offset + 4
    if tag == 6:
        _need(data, offset, 8)
        return struct.unpack_from(">d", data, offset)[0], offset + 8
    if tag == 7:
        _need(data, offset, 4)
        length = struct.unpack_from(">i", data, offset)[0]
        offset += 4
        _need(data, offset, max(length, 0))
        return data[offset : offset + length], offset + max(length, 0)
    if tag == 8:
        _need(data, offset, 2)
        length = struct.unpack_from(">H", data, offset)[0]
        offset += 2
        _need(data, offset, length)
        return data[offset : offset + length].decode("utf-8", "replace"), offset + length
    if tag == 9:
        _need(data, offset, 5)
        element = data[offset]
        count = struct.unpack_from(">i", data, offset + 1)[0]
        offset += 5
        values = []
        for _ in range(max(count, 0)):
            value, offset = read_payload(data, offset, element, depth + 1)
            values.append((element, value))
        return values, offset
    if tag == 10:
        values: dict[str, tuple[int, object]] = {}
        while True:
            _need(data, offset, 1)
            child = data[offset]
            offset += 1
            if child == 0:
                return values, offset
            _need(data, offset, 2)
            name_length = struct.unpack_from(">H", data, offset)[0]
            offset += 2
            _need(data, offset, name_length)
            name = data[offset : offset + name_length].decode("utf-8", "replace")
            offset += name_length
            value, offset = read_payload(data, offset, child, depth + 1)
            values[name] = (child, value)
    if tag == 11:
        _need(data, offset, 4)
        length = struct.unpack_from(">i", data, offset)[0]
        offset += 4
        _need(data, offset, 4 * max(length, 0))
        values = [
            struct.unpack_from(">i", data, offset + 4 * index)[0]
            for index in range(max(length, 0))
        ]
        return values, offset + 4 * max(length, 0)
    if tag == 12:
        _need(data, offset, 4)
        length = struct.unpack_from(">i", data, offset)[0]
        offset += 4
        _need(data, offset, 8 * max(length, 0))
        values = [
            struct.unpack_from(">q", data, offset + 8 * index)[0]
            for index in range(max(length, 0))
        ]
        return values, offset + 8 * max(length, 0)
    raise NbtError(f"unknown NBT tag {tag}")


def read_root(data: bytes, offset: int) -> tuple[tuple[int, object], int]:
    """Read the unnamed root used by packed registry entries."""
    return read_tag(data, offset)


def first_difference(
    left: tuple[int, object], right: tuple[int, object], path: str = ""
) -> str | None:
    if left[0] != right[0]:
        return f"{path}: tag {left[0]} != {right[0]}"
    a, b = left[1], right[1]
    if left[0] == 10:
        left_keys = list(a)
        right_keys = list(b)
        for key in left_keys:
            if key not in b:
                return f"{path}/{key}: missing on right"
            difference = first_difference(a[key], b[key], f"{path}/{key}")
            if difference is not None:
                return difference
        for key in right_keys:
            if key not in a:
                return f"{path}/{key}: missing on left"
        return None
    if left[0] == 9:
        if len(a) != len(b):
            return f"{path}: list length {len(a)} != {len(b)}"
        for index, (left_item, right_item) in enumerate(zip(a, b, strict=True)):
            difference = first_difference(
                left_item, right_item, f"{path}[{index}]"
            )
            if difference is not None:
                return difference
        return None
    if a != b:
        return f"{path}: {a!r} != {b!r}"
    return None
