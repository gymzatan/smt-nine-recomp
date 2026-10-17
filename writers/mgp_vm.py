# -*- coding: utf-8 -*-
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple


FIXED_OPCODE_LENGTHS: Dict[int, int] = {
    0x00: 1,
    0x01: 7,
    0x02: 3,
    0x03: 3,
    0x04: 5,
    0x05: 7,
    0x06: 5,
    0x07: 25,
    0x08: 5,
    0x09: 27,
    0x0A: 5,
    0x0B: 27,
    0x0C: 27,
    0x0D: 3,
    0x0E: 3,
    0x0F: 1,
    0x10: 1,
    0x11: 3,
    0x12: 3,
    0x13: 5,
    0x14: 11,
    0x17: 1,
    0x18: 5,
    0x19: 1,
    0x1A: 5,
    0x1B: 1,
    0x1C: 1,
    0x1D: 5,
    0x1E: 3,
    0x1F: 1,
    0x20: 5,
    0x21: 3,
    0x22: 1,
    0x23: 1,
    0x24: 1,
    0x25: 1,
    0x26: 3,
    0x27: 1,
    0x28: 7,
    0x29: 1,
    0x2A: 9,
    0x2B: 3,
    0x2C: 3,
    0x2D: 9,
    0x2E: 3,
    0x2F: 3,
    0x30: 1,
    0x31: 11,
    0x32: 11,
    0x33: 3,
    0x34: 3,
    0x35: 5,
    0x36: 3,
    0x37: 5,
    0x38: 1,
    0x39: 1,
    0x3A: 1,
    0x3B: 3,
    0x3C: 3,
    0x3D: 15,
    0x3E: 5,
    0x3F: 3,
    0x40: 3,
    0x41: 21,
    0x42: 19,
    0x43: 19,
    0x44: 35,
    0x45: 11,
    0x46: 5,
    0x47: 26,
    0x48: 23,
    0x49: 6,
    0x4A: 3,
    0x4B: 9,
    0x4C: 7,
    0x4D: 7,
    0x4E: 34,
    0x4F: 3,
    0x50: 29,
    0x51: 5,
    0x52: 3,
    0x53: 3,
    0x54: 5,
    0x55: 3,
    0x56: 3,
    0x57: 5,
    0x58: 9,
    0x59: 9,
    0x5A: 5,
    0x5B: 1,
    0x5C: 5,
    0x5D: 3,
    0x5E: 1,
    0x5F: 3,
    0x60: 3,
    0x61: 5,
    0x62: 3,
    0x63: 1,
    0x64: 3,
    0x65: 1,
    0x66: 7,
    0x67: 23,
    0x68: 9,
    0x69: 3,
    0x6A: 5,
    0x6B: 7,
    0x6C: 3,
    0x6D: 3,
    0x6E: 9,
    0x6F: 3,
    0x70: 3,
    0x71: 3,
    0x72: 3,
    0x73: 3,
}

STOP_OPCODES = {
    0x00,
    0x03,
    0x04,
    0x05,
    0x1F,
    0x22,
    0x23,
    0x25,
    0x46,
    0x50,
    0x53,
    0x69,
    0x70,
}

TEXT_FAMILY_NAMES = {
    0x01: "main_top",
    0x02: "main_body",
    0x1A: "list_item_with_value",
    0x21: "prompt_body",
    0x26: "list_text",
    0x2F: "ui_text",
    0x55: "channel55_body",
    0x58: "list_weighted_text",
    0x72: "channel72_body",
}


@dataclass(frozen=True)
class MgpLocalTextRecord:
    local_idx: int
    absolute_offset: int
    raw: bytes


@dataclass(frozen=True)
class MgpOpcodeEvent:
    opcode: int
    rel_offset: int
    absolute_offset: int
    length: int
    raw: bytes
    stop_boundary: bool
    local_idx: Optional[int] = None
    slot_id: Optional[int] = None
    extra_value: Optional[int] = None
    text_family: Optional[str] = None


@dataclass(frozen=True)
class MgpScene:
    scene_idx: int
    scene_start: int
    scene_end: int
    scene_name: str
    scene_prefix: bytes
    nested_offset: int
    nested: bytes
    code_end: int
    meta_start: int
    meta_end: int
    text_start: int
    text_end: int
    child_table_start: int
    target_label_table_start: int
    label_names: List[str]
    label_offsets: List[int]
    local_texts: List[MgpLocalTextRecord]
    child_scene_names: List[str]
    target_label_names: List[str]
    entry_offset: int

    @property
    def code(self) -> bytes:
        return self.nested[:self.code_end]


def _dedupe_preserve_order(values: Sequence[int]) -> List[int]:
    out: List[int] = []
    seen: set[int] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        out.append(int(value))
    return out


def _collect_mgp_scene_header_info(data: bytes) -> Optional[Tuple[int, List[int], List[int]]]:
    if len(data) < 8:
        return None

    scene_count = struct.unpack_from("<H", data, 2)[0]
    header_end = 4 + scene_count * 4
    if scene_count <= 0 or header_end > len(data):
        return None

    raw_values = [
        struct.unpack_from("<I", data, 4 + idx * 4)[0]
        for idx in range(scene_count)
    ]
    return header_end, raw_values, list(range(scene_count))


def collect_mgp_scene_offset_slots(data: bytes) -> Optional[List[int]]:
    header_info = _collect_mgp_scene_header_info(data)
    if header_info is None:
        return None
    header_end, raw_values, raw_slots = header_info
    scene_offsets = [
        int(value)
        for value in raw_values
        if header_end <= int(value) < len(data)
    ]
    scene_offsets = _dedupe_preserve_order(scene_offsets)
    slot_values = []
    seen_offsets: set[int] = set()
    for slot_idx, value in zip(raw_slots, raw_values):
        value = int(value)
        if value < header_end or value >= len(data):
            continue
        if value in seen_offsets:
            continue
        seen_offsets.add(value)
        slot_values.append(int(slot_idx))
    if not scene_offsets or scene_offsets[0] != header_end:
        return None
    if any(scene_offsets[idx + 1] <= scene_offsets[idx] for idx in range(len(scene_offsets) - 1)):
        return None
    if len(slot_values) != len(scene_offsets):
        return None
    return slot_values


def collect_mgp_scene_offsets(data: bytes) -> Optional[List[int]]:
    header_info = _collect_mgp_scene_header_info(data)
    if header_info is None:
        return None
    header_end, raw_values, _ = header_info
    scene_offsets = [
        int(value)
        for value in raw_values
        if header_end <= int(value) < len(data)
    ]
    scene_offsets = _dedupe_preserve_order(scene_offsets)
    if not scene_offsets or scene_offsets[0] != header_end:
        return None
    if any(scene_offsets[idx + 1] <= scene_offsets[idx] for idx in range(len(scene_offsets) - 1)):
        return None
    return scene_offsets


def _parse_string_table(nested: bytes, start: int, end: int) -> List[str]:
    if start >= end or end > len(nested):
        return []
    block = nested[start:end]
    if len(block) < 2:
        return []
    count = struct.unpack_from("<H", block, 0)[0]
    pos = 2
    if len(block) < pos + count:
        return []
    lengths = list(block[pos:pos + count])
    pos += count + 2
    out: List[str] = []
    for length in lengths:
        if pos + length > len(block):
            return []
        raw = block[pos:pos + length]
        pos += length
        out.append(raw.rstrip(b"\x00").decode("cp932", errors="replace"))
    return out


def _parse_label_meta(nested: bytes, meta_start: int, meta_end: int) -> Optional[tuple[List[int], List[str]]]:
    meta = nested[meta_start:meta_end]
    if len(meta) < 4:
        return None

    label_count = struct.unpack_from("<H", meta, 0)[0]
    table_span = struct.unpack_from("<H", meta, 2)[0]
    if label_count <= 0 or table_span != label_count * 4:
        return None

    offsets = [
        struct.unpack_from("<I", meta, 4 + idx * 4)[0]
        for idx in range(label_count)
    ]
    lengths_start = 4 + table_span
    lengths_end = lengths_start + label_count
    if lengths_end + 2 > len(meta):
        return None
    lengths = list(meta[lengths_start:lengths_end])
    label_bytes = struct.unpack_from("<H", meta, lengths_end)[0]
    labels_start = lengths_end + 2
    if labels_start + label_bytes != len(meta):
        return None

    labels: List[str] = []
    pos = labels_start
    for length in lengths:
        if length <= 0 or pos + length > len(meta):
            return None
        raw = meta[pos:pos + length]
        pos += length
        labels.append(raw.rstrip(b"\x00").decode("cp932", errors="replace"))
    return offsets, labels


def _parse_local_texts(
    nested: bytes,
    text_start: int,
    text_end: int,
    absolute_base_offset: int,
) -> Optional[List[MgpLocalTextRecord]]:
    if text_start + 4 > text_end:
        return None
    text_count = struct.unpack_from("<H", nested, text_start)[0]
    lengths_start = text_start + 2
    lengths_end = lengths_start + text_count
    if lengths_end + 2 > text_end:
        return None

    total_bytes = struct.unpack_from("<H", nested, lengths_end)[0]
    records_start = lengths_end + 2
    if text_count == 0:
        return [] if records_start == text_end else None

    out: List[MgpLocalTextRecord] = []
    cursor = records_start
    computed_total = 0
    for local_idx in range(text_count):
        record_size = nested[lengths_start + local_idx]
        if record_size <= 0 or cursor + record_size > text_end:
            return None
        raw = bytes(nested[cursor:cursor + record_size - 1])
        out.append(MgpLocalTextRecord(
            local_idx=local_idx,
            absolute_offset=absolute_base_offset + cursor,
            raw=raw,
        ))
        cursor += record_size
        computed_total += record_size
    if cursor != text_end:
        return None
    if computed_total & 0xFFFF != total_bytes:
        return None
    return out


def parse_mgs_scene(data: bytes, scene_name: str = "") -> Optional[MgpScene]:
    if len(data) < 18 or data[:2] != b"\x7e\x00":
        return None

    code_end, text_start, child_table_start, target_label_table_start = struct.unpack_from("<4I", data, 2)
    meta_start = code_end
    meta_end = text_start
    text_end = child_table_start
    if not (0 < code_end <= meta_end <= text_start <= text_end <= target_label_table_start <= len(data)):
        return None

    label_meta = _parse_label_meta(data, meta_start, meta_end)
    if label_meta is None:
        return None
    label_offsets, label_names = label_meta
    entry_offset = int(label_offsets[0]) if label_offsets else 0
    if not (0 <= entry_offset < code_end):
        entry_offset = 0

    local_texts = _parse_local_texts(data, text_start, text_end, 0)
    if local_texts is None:
        return None

    child_scene_names = _parse_string_table(data, child_table_start, target_label_table_start)
    target_label_names = _parse_string_table(data, target_label_table_start, len(data))

    return MgpScene(
        scene_idx=0,
        scene_start=0,
        scene_end=len(data),
        scene_name=scene_name,
        scene_prefix=b"",
        nested_offset=0,
        nested=data,
        code_end=code_end,
        meta_start=meta_start,
        meta_end=meta_end,
        text_start=text_start,
        text_end=text_end,
        child_table_start=child_table_start,
        target_label_table_start=target_label_table_start,
        label_names=label_names,
        label_offsets=label_offsets,
        local_texts=local_texts,
        child_scene_names=child_scene_names,
        target_label_names=target_label_names,
        entry_offset=entry_offset,
    )


def parse_mgp_scene(raw_scene: bytes, scene_idx: int, scene_start: int, scene_end: int) -> Optional[MgpScene]:
    if not raw_scene:
        return None

    scene_name_span = raw_scene[0]
    nested_offset = 1 + scene_name_span
    if scene_name_span <= 0 or nested_offset + 0x12 > len(raw_scene):
        return None

    scene_name = raw_scene[1:nested_offset].rstrip(b"\x00").decode("cp932", errors="replace")
    nested = raw_scene[nested_offset:]
    if len(nested) < 18 or nested[:2] != b"\x7e\x00":
        return None

    code_end, text_start, child_table_start, target_label_table_start = struct.unpack_from("<4I", nested, 2)
    meta_start = code_end
    meta_end = text_start
    text_end = child_table_start
    if not (0 < code_end <= meta_end <= text_start <= text_end <= target_label_table_start <= len(nested)):
        return None

    label_meta = _parse_label_meta(nested, meta_start, meta_end)
    if label_meta is None:
        return None
    label_offsets, label_names = label_meta
    entry_offset = int(label_offsets[0]) if label_offsets else 0
    if not (0 <= entry_offset < code_end):
        entry_offset = 0

    local_texts = _parse_local_texts(nested, text_start, text_end, scene_start + nested_offset)
    if local_texts is None:
        return None

    child_scene_names = _parse_string_table(nested, child_table_start, target_label_table_start)
    target_label_names = _parse_string_table(nested, target_label_table_start, len(nested))

    return MgpScene(
        scene_idx=scene_idx,
        scene_start=scene_start,
        scene_end=scene_end,
        scene_name=scene_name,
        scene_prefix=bytes(raw_scene[:nested_offset]),
        nested_offset=nested_offset,
        nested=nested,
        code_end=code_end,
        meta_start=meta_start,
        meta_end=meta_end,
        text_start=text_start,
        text_end=text_end,
        child_table_start=child_table_start,
        target_label_table_start=target_label_table_start,
        label_names=label_names,
        label_offsets=label_offsets,
        local_texts=local_texts,
        child_scene_names=child_scene_names,
        target_label_names=target_label_names,
        entry_offset=entry_offset,
    )


def load_mgp(path: Path) -> List[MgpScene]:
    data = path.read_bytes()
    scene_offsets = collect_mgp_scene_offsets(data) or []
    scenes: List[MgpScene] = []
    for scene_idx, scene_start in enumerate(scene_offsets):
        scene_end = scene_offsets[scene_idx + 1] if scene_idx + 1 < len(scene_offsets) else len(data)
        parsed = parse_mgp_scene(data[scene_start:scene_end], scene_idx, scene_start, scene_end)
        if parsed is not None:
            scenes.append(parsed)
    return scenes


def load_mgs(path: Path) -> List[MgpScene]:
    parsed = parse_mgs_scene(path.read_bytes(), scene_name=path.stem)
    return [parsed] if parsed is not None else []


MGS_OPCODE_LENGTH_OVERRIDES: Dict[int, int] = {}


def _typed_clause_operand_length(type_tag: int) -> int:
    return 1 if int(type_tag) == 0 else 5


def opcode_length(code: bytes, pc: int, code_end: Optional[int] = None, *, mgs_mode: bool = False) -> Optional[int]:
    code_limit = len(code) if code_end is None else min(len(code), code_end)
    if not (0 <= pc < code_limit):
        return None
    op = code[pc]
    if op == 0x15:
        pos = pc + 5
        if pos > code_limit:
            return None
        while True:
            if pos >= code_limit:
                return None
            lhs_tag = int(code[pos])
            lhs_len = _typed_clause_operand_length(lhs_tag)
            if pos + lhs_len > code_limit:
                return None
            pos += lhs_len
            if pos >= code_limit:
                return None
            pos += 1  # relation code
            if pos >= code_limit:
                return None
            rhs_tag = int(code[pos])
            rhs_len = _typed_clause_operand_length(rhs_tag)
            if pos + rhs_len > code_limit:
                return None
            pos += rhs_len
            if pos >= code_limit:
                return None
            chain_code = int(code[pos])
            pos += 1
            if chain_code not in (2, 3):
                return pos - pc
    if op == 0x16:
        if pc + 2 > code_limit:
            return None
        return 6 if code[pc + 1] in (0x15, 0x18) else 1
    if mgs_mode and op in MGS_OPCODE_LENGTH_OVERRIDES:
        return MGS_OPCODE_LENGTH_OVERRIDES[op]
    return FIXED_OPCODE_LENGTHS.get(op)


def decode_opcode_event(scene: MgpScene, pc: int) -> Optional[MgpOpcodeEvent]:
    code = scene.code
    length = opcode_length(code, pc, scene.code_end)
    if length is None or pc + length > scene.code_end:
        return None

    op = code[pc]
    local_idx: Optional[int] = None
    slot_id: Optional[int] = None
    extra_value: Optional[int] = None
    text_family = TEXT_FAMILY_NAMES.get(op)

    if op == 0x01 and length >= 7:
        slot_id = struct.unpack_from("<h", code, pc + 1)[0]
        local_idx = struct.unpack_from("<H", code, pc + 3)[0]
        extra_value = struct.unpack_from("<h", code, pc + 5)[0]
    elif op in (0x02, 0x21, 0x26, 0x2F, 0x55, 0x72) and length >= 3:
        local_idx = struct.unpack_from("<H", code, pc + 1)[0]
    elif op == 0x1A and length >= 5:
        local_idx = struct.unpack_from("<H", code, pc + 1)[0]
        extra_value = struct.unpack_from("<h", code, pc + 3)[0]
    elif op in (0x03, 0x54, 0x71) and length >= 3:
        slot_id = struct.unpack_from("<h", code, pc + 1)[0]
    elif op == 0x04 and length >= 5:
        extra_value = pc + 1
    elif op == 0x58 and length >= 9:
        local_idx = struct.unpack_from("<H", code, pc + 5)[0]
        extra_value = struct.unpack_from("<h", code, pc + 7)[0]

    return MgpOpcodeEvent(
        opcode=op,
        rel_offset=pc,
        absolute_offset=scene.scene_start + scene.nested_offset + pc,
        length=length,
        raw=bytes(code[pc:pc + length]),
        stop_boundary=op in STOP_OPCODES,
        local_idx=local_idx,
        slot_id=slot_id,
        extra_value=extra_value,
        text_family=text_family,
    )


def iter_linear_opcode_events(scene: MgpScene, start_offset: Optional[int] = None) -> Iterator[MgpOpcodeEvent]:
    pc = scene.entry_offset if start_offset is None else int(start_offset)
    code = scene.code
    while pc < scene.code_end:
        event = decode_opcode_event(scene, pc)
        if event is None:
            break
        yield event
        if event.length <= 0:
            break
        pc += event.length


def load_mgp_from_bytes(data: bytes) -> List[MgpScene]:
    scene_offsets = collect_mgp_scene_offsets(data) or []
    scenes: List[MgpScene] = []
    for scene_idx, scene_start in enumerate(scene_offsets):
        scene_end = scene_offsets[scene_idx + 1] if scene_idx + 1 < len(scene_offsets) else len(data)
        parsed = parse_mgp_scene(data[scene_start:scene_end], scene_idx, scene_start, scene_end)
        if parsed is not None:
            scenes.append(parsed)
    return scenes


def load_mgs_from_bytes(data: bytes, *, scene_name: str = "") -> List[MgpScene]:
    parsed = parse_mgs_scene(data, scene_name=scene_name)
    return [parsed] if parsed is not None else []
