# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from collections import defaultdict, deque
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

from fullwidth_ascii import normalize_visible_ascii, to_fullwidth_char
from apply_cache import _split_rl_window_chunks
from mgp_vm import (
    collect_mgp_scene_offset_slots,
    collect_mgp_scene_offsets,
    decode_opcode_event,
    load_mgp_from_bytes,
    load_mgs_from_bytes,
    opcode_length,
    STOP_OPCODES,
)


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SCRIPT_DIR = Path(__file__).parent
DEFAULT_TRANSLATED_DIR = SCRIPT_DIR / "translated"
DEFAULT_OUTPUT_DIR = SCRIPT_DIR / "output"
DEFAULT_BACKUP_MEDIA_DIR = SCRIPT_DIR.parent / "backup" / "Media"
TBL_CANDIDATES = [
    SCRIPT_DIR.parent / "font_pipeline" / "final_game_f24_full.tbl",
    SCRIPT_DIR.parent / "font_pipeline" / "final_game_f24.tbl",
    SCRIPT_DIR.parent / "font_pipeline" / "sysfont_cn.tbl",
]
ORIGINAL_GLYPH_BYTES = {
    "∮": bytes.fromhex("8793"),
    "∟": bytes.fromhex("8798"),
    "⊿": bytes.fromhex("8799"),
}
SYMBOL_NORMALIZATION = {
    "·": "・",
    "－": "ー",
    "～": "〜",
    "~": "〜",
    "Ⅰ": "Ｉ",
    "Ⅱ": "ＩＩ",
    "Ⅲ": "ＩＩＩ",
    "Ⅳ": "ＩＶ",
    "é": "e",
    "ā": "a",
    "ū": "u",
    "호": "ho",
}

NONRETURNING_STOP_OPCODES = {0x00}
RESUMING_STOP_OPCODES = STOP_OPCODES - NONRETURNING_STOP_OPCODES - {0x1F}
ROOT_STACK_WARNING_DEPTH = 0x20
NONVISIBLE_PLACEHOLDER_CHARS = {"⊇", "⊂"}
LINE_FILLER_RE = re.compile(r"^\s*（补全第\d+行）\s*$")


class WritebackFailure(RuntimeError):
    pass


def fail(file_name: str, message: str, *, offset: Optional[int] = None) -> None:
    prefix = f"写回失败: {file_name}"
    if offset is not None:
        prefix += f" @0x{int(offset):X}"
    raise WritebackFailure(f"{prefix}\n{message}")


def resolve_tbl_file(tbl_path: Optional[Path] = None) -> Path:
    if tbl_path is not None:
        return tbl_path
    for candidate in TBL_CANDIDATES:
        if candidate.exists():
            return candidate
    return TBL_CANDIDATES[0]


def load_tbl(tbl_path: Path) -> Dict[str, bytes]:
    char_map: Dict[str, bytes] = {}
    with open(tbl_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or "=" not in line:
                continue
            hex_code, chars = line.split("=", 1)
            hex_code = hex_code.strip()
            if not hex_code:
                continue
            try:
                raw = bytes.fromhex(hex_code)
            except ValueError:
                continue
            for ch in chars:
                char_map.setdefault(ch, raw)
    for ch, raw in ORIGINAL_GLYPH_BYTES.items():
        char_map.setdefault(ch, raw)
    # Fallback aliases: map common chars missing from TBL to equivalent chars present in TBL
    _CHAR_ALIASES = {
        "\uff5e": "\u301c",  # ～ → 〜
        "\u2014": "\u2015",  # — → ―
        "\uff0d": "\u2015",  # － → ―
        "\u00b7": "\u30fb",  # · → ・
    }
    for src, dst in _CHAR_ALIASES.items():
        if src not in char_map and dst in char_map:
            char_map[src] = char_map[dst]
    return char_map


def sanitize_translation_text(text: str) -> str:
    cleaned = (text or "").replace("\r\n", "\n").replace("\r", "\n").strip(" \t")
    cleaned_lines = [
        line.rstrip(" \t")
        for line in cleaned.split("\n")
        if not LINE_FILLER_RE.match(line.strip())
    ]
    cleaned = "\n".join(cleaned_lines)
    cleaned = "".join(SYMBOL_NORMALIZATION.get(ch, ch) for ch in cleaned)
    return normalize_visible_ascii(cleaned, preserve_control_tags=True)


def sanitize_mgs_translation_text(text: str) -> str:
    """Preserve MGS-visible text bytes as much as possible.

    MGS round-trip must not normalize visible symbols such as U+FF5E `～`,
    and must not trim fullwidth indentation that belongs to the runtime text
    unit. Only remove obvious review-only filler and ASCII edge whitespace.
    """
    cleaned = (text or "").replace("\r\n", "\n").replace("\r", "\n").strip(" \t")
    cleaned_lines = [
        line.rstrip(" \t")
        for line in cleaned.split("\n")
        if not LINE_FILLER_RE.match(line.strip())
    ]
    return "\n".join(cleaned_lines)


def normalize_char(ch: str) -> str:
    return to_fullwidth_char(ch)


def tokenize_line(line: str) -> List[Tuple[str, object]]:
    tokens: List[Tuple[str, object]] = []
    i = 0
    while i < len(line):
        if line[i] == "<" and i + 3 < len(line) and line[i + 3] == ">":
            hex_part = line[i + 1:i + 3]
            if all(ch in "0123456789abcdefABCDEF" for ch in hex_part):
                tokens.append(("ctrl", int(hex_part, 16)))
                i += 4
                continue
        tokens.append(("char", line[i]))
        i += 1
    return tokens


def split_text_to_lines(text: str) -> List[str]:
    return [line.rstrip("\r") for line in sanitize_translation_text(text).split("\n")]


def split_mgs_text_to_lines(text: str) -> List[str]:
    return [line.rstrip("\r") for line in sanitize_mgs_translation_text(text).split("\n")]


def split_text_to_payloads(text: str) -> List[str]:
    cleaned = sanitize_translation_text(text)
    return [cleaned]


def resolve_storage_lines(
    text: str,
    part_lines: Optional[List[object]] = None,
) -> Tuple[Optional[List[str]], str]:
    if part_lines is None:
        return split_text_to_payloads(text), ""
    if not isinstance(part_lines, list):
        return None, "part_lines 必须是数组"
    lines: List[str] = []
    for idx, raw_line in enumerate(part_lines):
        if raw_line is None:
            lines.append("")
            continue
        line = sanitize_translation_text(str(raw_line))
        lines.append(line)
    return lines, ""


def _page_signature(page: Dict[str, object]) -> Tuple[Tuple[str, object], ...]:
    return tuple(
        sorted(
            (str(key), page[key])
            for key in page.keys()
            if key not in ("part_start", "part_count")
        )
    )


def normalize_original_pages(
    raw_pages: object,
    part_count: int,
    *,
    collapse_compatible_pages: bool = False,
) -> Optional[List[Dict[str, object]]]:
    if part_count <= 0:
        return []
    if not isinstance(raw_pages, list) or not raw_pages:
        return [{
            "part_start": 0,
            "part_count": int(part_count),
        }]

    pages: List[Dict[str, object]] = []
    for raw_page in raw_pages:
        if not isinstance(raw_page, dict):
            return None
        part_start = raw_page.get("part_start")
        part_count_value = raw_page.get("part_count")
        if (
            not isinstance(part_start, int)
            or isinstance(part_start, bool)
            or not isinstance(part_count_value, int)
            or isinstance(part_count_value, bool)
            or int(part_start) < 0
            or int(part_count_value) <= 0
            or int(part_start) + int(part_count_value) > int(part_count)
        ):
            return None
        page = dict(raw_page)
        page["part_start"] = int(part_start)
        page["part_count"] = int(part_count_value)
        pages.append(page)

    cursor = 0
    for page in pages:
        if int(page["part_start"]) != cursor:
            return None
        cursor += int(page["part_count"])
    if cursor != int(part_count):
        return None

    if not collapse_compatible_pages:
        return [dict(page) for page in pages]

    collapsed: List[Dict[str, object]] = []
    for page in pages:
        if (
            collapsed
            and int(collapsed[-1]["part_start"]) + int(collapsed[-1]["part_count"]) == int(page["part_start"])
            and _page_signature(collapsed[-1]) == _page_signature(page)
        ):
            collapsed[-1]["part_count"] = int(collapsed[-1]["part_count"]) + int(page["part_count"])
            continue
        collapsed.append(dict(page))
    return collapsed


def normalize_target_pages(
    raw_pages: object,
    *,
    expected_part_count: int,
) -> Tuple[Optional[List[Dict[str, object]]], str]:
    if not isinstance(raw_pages, list):
        return None, "target_pages 必须是数组"
    if not raw_pages:
        return None, "target_pages 不能为空"

    cursor = 0
    normalized: List[Dict[str, object]] = []
    for page_idx, raw_page in enumerate(raw_pages):
        if not isinstance(raw_page, dict):
            return None, f"target_pages[{page_idx}] 必须是对象"
        part_count = raw_page.get("part_count")
        if not isinstance(part_count, int) or isinstance(part_count, bool) or int(part_count) <= 0:
            return None, f"target_pages[{page_idx}].part_count 必须是正整数"
        page = {
            key: value
            for key, value in raw_page.items()
            if key not in ("part_start", "part_count")
        }
        page["part_start"] = int(cursor)
        page["part_count"] = int(part_count)
        normalized.append(page)
        cursor += int(part_count)

    if cursor != int(expected_part_count):
        return None, f"target_pages part_count 总和 {cursor} 与目标 payload 数量 {int(expected_part_count)} 不一致"
    return normalized, ""


LOCAL_IDX_PATCH_OFFSETS = {
    0x01: 3,
    0x02: 1,
    0x1A: 1,
    0x21: 1,
    0x26: 1,
    0x2F: 1,
    0x55: 1,
    0x58: 5,
    0x72: 1,
}


def _patch_opcode_local_idx(opcode_raw: bytes, new_local_idx: int) -> bytes:
    if new_local_idx < 0 or new_local_idx > 0xFFFF:
        raise WritebackFailure("local_idx 越界")
    if not opcode_raw:
        raise WritebackFailure("空 opcode 模板")
    opcode = int(opcode_raw[0])
    patch_offset = LOCAL_IDX_PATCH_OFFSETS.get(opcode)
    if patch_offset is None or patch_offset + 2 > len(opcode_raw):
        raise WritebackFailure(f"不支持重写 opcode 0x{opcode:02X}")
    patched = bytearray(opcode_raw)
    struct.pack_into("<H", patched, int(patch_offset), int(new_local_idx))
    return bytes(patched)


def _opcode_local_idx_mask(opcode_raw: bytes) -> bytes:
    if not opcode_raw:
        return opcode_raw
    opcode = int(opcode_raw[0])
    patch_offset = LOCAL_IDX_PATCH_OFFSETS.get(opcode)
    if patch_offset is None or patch_offset + 2 > len(opcode_raw):
        return opcode_raw
    masked = bytearray(opcode_raw)
    masked[patch_offset:patch_offset + 2] = b"\x00\x00"
    return bytes(masked)


def _part_opcode_raw(part: dict) -> bytes:
    raw_hex = str(part.get("bytecode_raw") or "")
    if not raw_hex:
        raise WritebackFailure("part 缺少 bytecode_raw")
    try:
        return bytes.fromhex(raw_hex)
    except ValueError as exc:
        raise WritebackFailure("part.bytecode_raw 非法") from exc


def _part_is_explicit_speaker(part: dict) -> bool:
    return str(part.get("bytecode_kind") or "") == "speaker"


def _part_bytecode_rel_start(part: dict, scene_abs_start: int) -> int:
    value = part.get("bytecode_offset")
    if not isinstance(value, int) or isinstance(value, bool):
        raise WritebackFailure("part 缺少 bytecode_offset")
    return int(value) - int(scene_abs_start)


def _part_bytecode_rel_end(part: dict, scene_abs_start: int) -> int:
    start = _part_bytecode_rel_start(part, scene_abs_start)
    length = part.get("bytecode_length")
    if not isinstance(length, int) or isinstance(length, bool) or int(length) <= 0:
        raise WritebackFailure("part 缺少 bytecode_length")
    return int(start) + int(length)


def normalize_part_line_map(
    part_line_map: object,
    parts: Optional[Sequence[dict]],
) -> Optional[List[int]]:
    if not isinstance(part_line_map, list):
        return None
    if not isinstance(parts, list) or not parts or len(part_line_map) != len(parts):
        return None
    normalized: List[int] = []
    for raw_value in part_line_map:
        if not isinstance(raw_value, int) or isinstance(raw_value, bool) or raw_value < 0:
            return None
        normalized.append(int(raw_value))
    if not normalized:
        return None
    max_index = max(normalized)
    if sorted(set(normalized)) != list(range(max_index + 1)):
        return None
    return normalized


def merge_original_control_tokens(
    original_text: str,
    translated_lines_tokens: List[List[Tuple[str, object]]],
    *,
    preserve_visible_placeholder_positions: bool = False,
) -> List[List[Tuple[str, object]]]:
    def _is_visible_placeholder_pair(
        tokens: Sequence[Tuple[str, object]],
        index: int,
    ) -> bool:
        if index + 1 >= len(tokens):
            return False
        kind, value = tokens[index]
        next_kind, next_value = tokens[index + 1]
        return (
            kind == "ctrl"
            and int(value) == 0xFF
            and next_kind == "ctrl"
            and 0x01 <= int(next_value) <= 0x0B
            and int(next_value) != 0x07
        )

    def _layout_stream(
        tokens: List[Tuple[str, object]],
    ) -> Tuple[List[List[Tuple[str, object]]], List[Tuple[int, int]]]:
        visible_units: List[List[Tuple[str, object]]] = []
        controls: List[Tuple[int, int]] = []
        has_visible_chars = any(kind != "ctrl" for kind, _ in tokens)
        cursor = 0
        while cursor < len(tokens):
            if (
                preserve_visible_placeholder_positions
                and has_visible_chars
                and _is_visible_placeholder_pair(tokens, cursor)
            ):
                visible_units.append([tokens[cursor], tokens[cursor + 1]])
                cursor += 2
                continue
            kind, value = tokens[cursor]
            if kind == "ctrl":
                controls.append((len(visible_units), int(value)))
            else:
                visible_units.append([tokens[cursor]])
            cursor += 1
        return visible_units, controls

    def _is_subsequence(expected: Sequence[int], observed: Sequence[int]) -> bool:
        if not observed:
            return True
        cursor = 0
        for value in expected:
            if value == observed[cursor]:
                cursor += 1
                if cursor >= len(observed):
                    return True
        return False

    original_lines = original_text.split("\n") if original_text else []
    merged: List[List[Tuple[str, object]]] = []

    for line_idx, translated_tokens in enumerate(translated_lines_tokens):
        if line_idx >= len(original_lines):
            merged.append(translated_tokens)
            continue

        original_tokens = tokenize_line(original_lines[line_idx])
        original_visible_units, original_positions = _layout_stream(original_tokens)
        translated_visible_units, translated_positions = _layout_stream(translated_tokens)
        original_signature = tuple(value for _, value in original_positions)
        translated_signature = tuple(value for _, value in translated_positions)
        if not original_signature or translated_signature == original_signature:
            merged.append(translated_tokens)
            continue

        translated_visible_len = len(translated_visible_units)
        merged_positions: List[Tuple[int, int, Tuple[str, object]]] = []
        extra_controls: List[Tuple[int, int, Tuple[str, object]]] = []
        translated_cursor = 0
        can_project = _is_subsequence(original_signature, translated_signature)
        for order_idx, (original_pos, original_value) in enumerate(original_positions):
            if (
                can_project
                and translated_cursor < len(translated_positions)
                and translated_positions[translated_cursor][1] == original_value
            ):
                translated_pos, translated_value = translated_positions[translated_cursor]
                merged_positions.append(
                    (min(translated_pos, translated_visible_len), order_idx, ("ctrl", translated_value))
                )
                translated_cursor += 1
            else:
                merged_positions.append(
                    (min(original_pos, translated_visible_len), order_idx, ("ctrl", original_value))
                )
        while translated_cursor < len(translated_positions):
            translated_pos, translated_value = translated_positions[translated_cursor]
            extra_controls.append(
                (min(translated_pos, translated_visible_len), len(original_positions) + translated_cursor, ("ctrl", translated_value))
            )
            translated_cursor += 1
        merged_positions.extend(extra_controls)
        merged_positions.sort(key=lambda item: (item[0], item[1]))

        merged_control_groups: Dict[int, List[Tuple[str, object]]] = {}
        for position, _, token in merged_positions:
            merged_control_groups.setdefault(position, []).append(token)
        rebuilt: List[Tuple[str, object]] = []
        for visible_index, unit in enumerate(translated_visible_units):
            rebuilt.extend(merged_control_groups.get(visible_index, []))
            rebuilt.extend(unit)
        rebuilt.extend(merged_control_groups.get(len(translated_visible_units), []))

        merged.append(rebuilt)

    return merged


def _control_signature(text: str) -> Tuple[int, ...]:
    return tuple(
        int(token[1])
        for token in tokenize_line(text)
        if token[0] == "ctrl"
    )


def _control_layout_signature(text: str) -> Tuple[Tuple[int, int], ...]:
    visible_count = 0
    layout: List[Tuple[int, int]] = []
    for kind, value in tokenize_line(text):
        if kind == "ctrl":
            layout.append((visible_count, int(value)))
        else:
            visible_count += 1
    return tuple(layout)


def _select_body_template_indices(
    *,
    body_templates: List[Tuple[bytes, str, int]],
    body_payloads: List[str],
) -> Optional[List[int]]:
    if len(body_payloads) > len(body_templates):
        return None

    template_signatures = [_control_signature(template_text) for _, template_text, _ in body_templates]
    payload_signatures = [_control_signature(payload) for payload in body_payloads]
    candidate_lists: List[List[int]] = []
    for payload_idx, payload_signature in enumerate(payload_signatures):
        candidates = [
            template_idx
            for template_idx, template_signature in enumerate(template_signatures)
            if template_signature == payload_signature
        ]
        if not candidates:
            return None
        candidates.sort(key=lambda template_idx: (abs(template_idx - payload_idx), template_idx))
        candidate_lists.append(candidates)

    assignment: List[Optional[int]] = [None] * len(body_payloads)
    used_templates: List[bool] = [False] * len(body_templates)
    search_order = sorted(range(len(body_payloads)), key=lambda idx: (len(candidate_lists[idx]), idx))

    def _dfs(depth: int) -> bool:
        if depth >= len(search_order):
            return True
        payload_idx = search_order[depth]
        for template_idx in candidate_lists[payload_idx]:
            if used_templates[template_idx]:
                continue
            used_templates[template_idx] = True
            assignment[payload_idx] = template_idx
            if _dfs(depth + 1):
                return True
            assignment[payload_idx] = None
            used_templates[template_idx] = False
        return False

    if not _dfs(0):
        return None
    return [int(value) for value in assignment if value is not None]


def _choose_body_templates_with_reuse(
    *,
    body_templates: List[Tuple[bytes, str, int]],
    body_payloads: List[str],
) -> Optional[List[Tuple[bytes, str, int]]]:
    template_indices = _select_body_template_indices(
        body_templates=body_templates,
        body_payloads=body_payloads[: min(len(body_payloads), len(body_templates))],
    )
    if template_indices is None:
        template_signatures = [_control_signature(template_text) for _, template_text, _ in body_templates]
        chosen_templates: List[Tuple[bytes, str, int]] = []
        for payload_idx, payload in enumerate(body_payloads):
            payload_signature = _control_signature(payload)
            compatible_indices = [
                template_idx
                for template_idx, template_signature in enumerate(template_signatures)
                if template_signature == payload_signature
            ]
            if not compatible_indices:
                return None
            compatible_indices.sort(key=lambda template_idx: (abs(template_idx - payload_idx), template_idx))
            chosen_templates.append(body_templates[compatible_indices[0]])
        return chosen_templates

    chosen_templates = [body_templates[idx] for idx in template_indices]
    if len(body_payloads) <= len(chosen_templates):
        return chosen_templates[: len(body_payloads)]

    template_signatures = [_control_signature(template_text) for _, template_text, _ in body_templates]
    for payload_idx in range(len(chosen_templates), len(body_payloads)):
        payload_signature = _control_signature(body_payloads[payload_idx])
        compatible_indices = [
            template_idx
            for template_idx, template_signature in enumerate(template_signatures)
            if template_signature == payload_signature
        ]
        if not compatible_indices:
            return None
        compatible_indices.sort(key=lambda template_idx: (abs(template_idx - payload_idx), template_idx))
        chosen_templates.append(body_templates[compatible_indices[0]])
    return chosen_templates


_LAYOUT_FAILURES: list = []


def encode_token(token: Tuple[str, object], char_map: Dict[str, bytes]) -> bytes:
    kind, value = token
    if kind == "ctrl":
        return bytes([int(value) & 0xFF])
    ch = normalize_char(str(value))
    encoded = char_map.get(ch, b"")
    if not encoded:
        raise KeyError(ch)
    return encoded


def encode_tokens(tokens: List[Tuple[str, object]], char_map: Dict[str, bytes]) -> bytes:
    out = bytearray()
    for token in tokens:
        out.extend(encode_token(token, char_map))
    return bytes(out)


def decode_script_bytes(data: bytes) -> str:
    out: List[str] = []
    i = 0
    while i < len(data):
        b1 = data[i]
        if b1 == 0x00:
            break
        if (0x81 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xFC):
            if i + 1 < len(data):
                pair = data[i:i + 2]
                try:
                    out.append(pair.decode("cp932"))
                except Exception:
                    out.append(f"[{pair.hex().upper()}]")
                i += 2
                continue
        if b1 == 0x0A:
            out.append("\n")
        elif 0x20 <= b1 <= 0x7E:
            out.append(chr(b1))
        else:
            out.append(f"<{b1:02X}>")
        i += 1
    return "".join(out)


def collect_json_files(translated_dir: Path, include_rl: bool) -> List[Path]:
    json_files = sorted(translated_dir.glob("*.json"))
    json_files = [
        path for path in json_files
        if not path.name.startswith(("xbe_", "script_overflow", "script_preserved_original"))
        and path.name != "_summary.json"
    ]
    if not include_rl:
        json_files = [path for path in json_files if path.name.upper() != "RL.MGP.JSON"]
    return json_files


def iter_side_entries(
    json_data: dict,
    is_scriptf: bool,
    *,
    include_empty: bool = False,
) -> List[Dict[str, object]]:
    side = "scriptf" if is_scriptf else "script"
    own_cat = "scriptf_only" if is_scriptf else "script_only"
    payloads: List[Dict[str, object]] = []

    def add_payload(item: dict, *, diff: bool, base_owner: str, category: str, shared_common: bool = False) -> None:
        def side_value(key: str, *, allow_base_fallback: bool = True) -> object:
            side_key = f"{side}_{key}"
            if side_key in item:
                return item.get(side_key)
            if allow_base_fallback and base_owner == side:
                return item.get(key)
            return None

        text_key = f"{side}_text"
        side_chinese_key = f"{side}_chinese"
        side_speaker_cn_key = f"{side}_speaker_cn"
        side_body_cn_key = f"{side}_body_cn"
        if shared_common:
            chinese_value = str(item.get(side_chinese_key) or item.get("chinese") or "")
            speaker_cn_value = str(item.get(side_speaker_cn_key) or item.get("speaker_cn") or "")
            body_cn_value = str(item.get(side_body_cn_key) or item.get("body_cn") or "")
        else:
            chinese_key = side_chinese_key if side_chinese_key in item else "chinese"
            chinese_value = str(item.get(chinese_key) or "")
            speaker_cn_value = str(
                item.get(side_speaker_cn_key) if side_speaker_cn_key in item else item.get("speaker_cn") or ""
            )
            body_cn_value = str(
                item.get(side_body_cn_key) if side_body_cn_key in item else item.get("body_cn") or ""
            )
        payload = {
            "offset": side_value("offset"),
            "length": side_value("length"),
            "text": item.get(text_key, item.get("text")),
            "chinese": chinese_value,
            "speaker_cn": speaker_cn_value,
            "body_cn": body_cn_value,
            "parts": side_value("parts") or [],
            "pages": side_value("pages") or [],
            "target_pages": side_value("target_pages"),
            "paragraph_pages": side_value("paragraph_pages"),
            "paragraph_segments": side_value("paragraph_segments"),
            "native_jp_lines": side_value("native_jp_lines"),
            "preserve_existing_records": side_value("preserve_existing_records"),
            "part_lines": side_value("part_lines"),
            "part_line_map": side_value("part_line_map"),
            "storage_jp_lines": side_value("storage_jp_lines"),
            "storage_line_roles": side_value("storage_line_roles"),
            "original_offset": side_value("original_offset"),
            "scene_idx": side_value("scene_idx"),
            "scene_entry_index": side_value("scene_entry_index"),
            "speaker": side_value("speaker"),
            "speaker_source": side_value("speaker_source"),
            "body_text": side_value("body_text"),
            "category": category,
        }
        if include_empty or payload["chinese"]:
            payloads.append(payload)

    for cat in ("common", own_cat):
        for item in json_data.get(cat, []):
            if isinstance(item, dict):
                add_payload(
                    item,
                    diff=False,
                    base_owner="script" if cat == "common" else side,
                    category=cat,
                    shared_common=(cat == "common"),
                )

    for item in json_data.get("different", []):
        if isinstance(item, dict):
            add_payload(item, diff=True, base_owner="script", category="different")

    return payloads


def ensure_parts(payload: Dict[str, object]) -> List[dict]:
    parts = payload.get("parts")
    if isinstance(parts, list) and parts:
        return [dict(part) for part in parts if isinstance(part, dict)]
    offset = payload.get("original_offset", payload.get("offset"))
    length = payload.get("length")
    if isinstance(offset, int) and isinstance(length, int):
        raw_hex = ""
        return [{"offset": int(offset), "length": int(length), "raw_hex": raw_hex}]
    return []


def resolve_original_part_lines(payload: Dict[str, object], parts: List[dict]) -> List[str]:
    storage_jp_lines = payload.get("storage_jp_lines")
    normalized_map = normalize_part_line_map(payload.get("part_line_map"), parts)
    if (
        isinstance(storage_jp_lines, list)
        and normalized_map is not None
        and max(normalized_map) < len(storage_jp_lines)
    ):
        return [str(storage_jp_lines[idx] or "") for idx in normalized_map]
    if isinstance(storage_jp_lines, list) and len(storage_jp_lines) == len(parts):
        return [str(line or "") for line in storage_jp_lines]

    decoded_parts: List[str] = []
    for part in parts:
        raw_hex = str(part.get("raw_hex") or "")
        if not raw_hex:
            decoded_parts = []
            break
        try:
            decoded_parts.append(decode_script_bytes(bytes.fromhex(raw_hex)))
        except ValueError:
            decoded_parts = []
            break
    if decoded_parts and len(decoded_parts) == len(parts):
        return decoded_parts

    text = str(payload.get("text") or "")
    body_text = payload.get("body_text")
    if len(parts) == 1:
        if str(payload.get("speaker_source") or "") == "inherited":
            if isinstance(body_text, str) and body_text:
                return [body_text]
            text_lines = text.split("\n")
            return ["\n".join(text_lines[1:]) if len(text_lines) > 1 else ""]
        return [text]

    return []


def is_nonvisible_storage_line(line: str) -> bool:
    normalized = str(line or "").replace("\r", "").replace("\n", "")
    stripped = normalized.strip()
    if not stripped:
        return True
    if stripped == "　":
        return True
    try:
        tokens = tokenize_line(normalized)
    except ValueError:
        return False
    # XBE/VM treats bare FF 01..0B pairs as visible text expansions.
    # Do not apply that rule to option-marker helper lines like <FF><07>⊂,
    # where the only visible payload is a structural placeholder glyph.
    has_visible_placeholder_pair = False
    for idx, (kind, value) in enumerate(tokens):
        if kind != "ctrl" or int(value) != 0xFF:
            continue
        if idx + 1 >= len(tokens):
            continue
        next_kind, next_value = tokens[idx + 1]
        if next_kind == "ctrl" and 0x01 <= int(next_value) <= 0x0B:
            has_visible_placeholder_pair = True
            break
    visible_chars = [str(value) for kind, value in tokens if kind != "ctrl"]
    if visible_chars and all(ch in NONVISIBLE_PLACEHOLDER_CHARS for ch in visible_chars):
        return True
    if has_visible_placeholder_pair:
        return False
    if all(kind == "ctrl" for kind, _ in tokens):
        return True
    return False


def _filter_mgs_mapping_parts(
    payload: Dict[str, object],
) -> Tuple[List[dict], Optional[List[str]]]:
    parts = payload.get("parts")
    if not isinstance(parts, list) or not parts:
        return [], None
    normalized_parts = [dict(part) for part in parts if isinstance(part, dict)]
    if not normalized_parts:
        return [], None

    original_part_lines = resolve_original_part_lines(payload, normalized_parts)
    if len(original_part_lines) != len(normalized_parts):
        raw_part_lines = payload.get("part_lines")
        if isinstance(raw_part_lines, list) and len(raw_part_lines) == len(normalized_parts):
            return normalized_parts, [str(line or "") for line in raw_part_lines]
        return normalized_parts, None

    raw_part_lines = payload.get("part_lines")
    can_filter_part_lines = isinstance(raw_part_lines, list) and len(raw_part_lines) == len(normalized_parts)
    if not can_filter_part_lines:
        return normalized_parts, None

    filtered_parts: List[dict] = []
    filtered_part_lines: List[str] = []
    for idx, (part, original_line) in enumerate(zip(normalized_parts, original_part_lines)):
        if is_nonvisible_storage_line(original_line):
            continue
        filtered_parts.append(part)
        if can_filter_part_lines:
            filtered_part_lines.append(str(raw_part_lines[idx] or ""))

    if not filtered_parts:
        if can_filter_part_lines:
            return normalized_parts, [str(line or "") for line in raw_part_lines]
        return normalized_parts, None
    if can_filter_part_lines:
        return filtered_parts, filtered_part_lines
    return filtered_parts, None


def _collapse_mgs_placeholder_prefix_lines(lines: List[str]) -> List[str]:
    collapsed = [str(line or "") for line in lines]
    changed = True
    while changed:
        changed = False
        next_lines: List[str] = []
        idx = 0
        while idx < len(collapsed):
            current = collapsed[idx]
            if idx + 1 < len(collapsed):
                following = collapsed[idx + 1]
                if is_nonvisible_storage_line(current) and following.startswith(current):
                    changed = True
                    idx += 1
                    continue
            next_lines.append(current)
            idx += 1
        collapsed = next_lines
    return collapsed


def expand_visible_lines_to_parts(
    payload: Dict[str, object],
    parts: List[dict],
    translated_visible_lines: List[str],
    original_lines: List[str],
) -> Optional[List[str]]:
    storage_lines = payload.get("storage_jp_lines")
    if not isinstance(storage_lines, list) or len(storage_lines) != len(parts):
        return None
    passthrough_indices: List[int] = []
    visible_indices: List[int] = []
    for idx, raw_line in enumerate(storage_lines):
        line = str(raw_line or "")
        if is_nonvisible_storage_line(line):
            passthrough_indices.append(idx)
        else:
            visible_indices.append(idx)
    if not passthrough_indices or len(translated_visible_lines) != len(visible_indices):
        return None
    expanded: List[str] = []
    cursor = 0
    passthrough_index_set = set(passthrough_indices)
    for idx in range(len(parts)):
        if idx in passthrough_index_set:
            expanded.append(original_lines[idx])
            continue
        expanded.append(translated_visible_lines[cursor])
        cursor += 1
    return expanded


def expand_single_repeated_visible_line_to_parts(
    translated_visible_lines: List[str],
    original_lines: List[str],
) -> Optional[List[str]]:
    if len(translated_visible_lines) != 1:
        return None
    visible_original_lines = [
        line for line in original_lines
        if not is_nonvisible_storage_line(line)
    ]
    if not visible_original_lines:
        return None
    first_visible = visible_original_lines[0]
    if any(line != first_visible for line in visible_original_lines):
        return None
    expanded: List[str] = []
    translated_line = translated_visible_lines[0]
    for original_line in original_lines:
        if is_nonvisible_storage_line(original_line):
            expanded.append(original_line)
        else:
            expanded.append(translated_line)
    return expanded


def expand_payloads_to_all_original_parts_for_residue(
    parts: List[dict],
    original_lines: List[str],
    translated_payloads: List[str],
) -> Optional[List[str]]:
    if not translated_payloads or len(original_lines) != len(parts):
        return None

    visible_indices = [
        idx for idx, original_line in enumerate(original_lines)
        if not is_nonvisible_storage_line(original_line)
    ]
    if not visible_indices:
        return list(original_lines)

    if len(translated_payloads) == len(visible_indices):
        expanded: List[str] = []
        cursor = 0
        visible_index_set = set(visible_indices)
        for idx, original_line in enumerate(original_lines):
            if idx not in visible_index_set:
                expanded.append(original_line)
                continue
            expanded.append(translated_payloads[cursor])
            cursor += 1
        return expanded

    repeated = expand_single_repeated_visible_line_to_parts(translated_payloads, original_lines)
    if repeated is not None:
        return repeated

    if len(translated_payloads) > len(visible_indices):
        return None

    expanded = list(original_lines)
    visible_count = len(visible_indices)
    payload_count = len(translated_payloads)
    for visible_pos, part_idx in enumerate(visible_indices):
        payload_idx = ((visible_pos + 1) * payload_count - 1) // visible_count
        expanded[part_idx] = translated_payloads[payload_idx]
    return expanded


def resolve_translated_visible_lines(payload: Dict[str, object]) -> List[str]:
    translated_text = str(payload.get("chinese") or "")
    speaker_cn = str(payload.get("speaker_cn") or "")
    body_cn = str(payload.get("body_cn") or "")
    if speaker_cn and str(payload.get("speaker_source") or "") != "inherited":
        text_lines = split_text_to_lines(translated_text)
        if text_lines and text_lines[0] == speaker_cn:
            return text_lines
        body_reference = body_cn if body_cn else str(payload.get("body_text") or "")
        body_lines = split_text_to_lines(body_reference)
        if body_lines and text_lines == body_lines:
            return [speaker_cn, *text_lines]
    if str(payload.get("speaker_source") or "") == "inherited":
        text_lines = split_text_to_lines(translated_text)
        body_reference = body_cn if body_cn else str(payload.get("body_text") or "")
        body_lines = split_text_to_lines(body_reference)
        if speaker_cn and text_lines and text_lines[0] == speaker_cn:
            translated_text = "\n".join(text_lines[1:])
        elif body_lines and len(text_lines) == len(body_lines) + 1:
            translated_text = "\n".join(text_lines[1:])
    return split_text_to_lines(translated_text)


def resolve_mgs_translated_visible_lines(payload: Dict[str, object]) -> List[str]:
    translated_text = str(payload.get("chinese") or "")
    speaker_cn = str(payload.get("speaker_cn") or "")
    body_cn = str(payload.get("body_cn") or "")
    if speaker_cn and str(payload.get("speaker_source") or "") != "inherited":
        text_lines = split_mgs_text_to_lines(translated_text)
        if text_lines and text_lines[0] == speaker_cn:
            return text_lines
        body_reference = body_cn if body_cn else str(payload.get("body_text") or "")
        body_lines = split_mgs_text_to_lines(body_reference)
        if body_lines and text_lines == body_lines:
            return [speaker_cn, *text_lines]
    if str(payload.get("speaker_source") or "") == "inherited":
        text_lines = split_mgs_text_to_lines(translated_text)
        body_reference = body_cn if body_cn else str(payload.get("body_text") or "")
        body_lines = split_mgs_text_to_lines(body_reference)
        if (
            speaker_cn
            and text_lines
            and text_lines[0] == speaker_cn
            and body_lines
            and text_lines[1:] == body_lines
        ):
            translated_text = "\n".join(text_lines[1:])
        elif body_lines and len(text_lines) == len(body_lines) + 1:
            translated_text = "\n".join(text_lines[1:])
    return split_mgs_text_to_lines(translated_text)


def group_translated_lines_by_template_payloads(
    translated_lines: Sequence[str],
    template_lines: Sequence[str],
) -> Optional[List[str]]:
    if not template_lines:
        return [] if not translated_lines else None

    segment_lengths: List[int] = []
    for template_line in template_lines:
        sublines = split_text_to_lines(str(template_line))
        segment_lengths.append(max(1, len(sublines)))

    if sum(segment_lengths) != len(translated_lines):
        return None

    grouped: List[str] = []
    cursor = 0
    for segment_length in segment_lengths:
        grouped.append("\n".join(translated_lines[cursor:cursor + segment_length]))
        cursor += segment_length
    return grouped


def resolve_translated_part_lines(
    payload: Dict[str, object],
    parts: List[dict],
    original_lines: List[str],
) -> Tuple[Optional[List[str]], str]:
    part_lines = payload.get("part_lines")
    raw_translated_text = str(payload.get("chinese") or "")
    raw_translated_lines = split_text_to_lines(str(payload.get("chinese") or ""))
    translated_visible_lines = resolve_translated_visible_lines(payload)
    translated_text = "\n".join(translated_visible_lines)

    if isinstance(part_lines, list):
        lines, error = resolve_storage_lines(raw_translated_text, part_lines)
        if lines is None:
            lines, error = resolve_storage_lines(translated_text, part_lines)
    elif len(parts) == 1:
        lines, error = resolve_storage_lines(raw_translated_text, None)
        if lines is None:
            lines, error = resolve_storage_lines(translated_text, None)
    else:
        raw_grouped = group_translated_lines_by_template_payloads(
            raw_translated_lines,
            original_lines,
        )
        if raw_grouped is not None:
            lines, error = raw_grouped, ""
        else:
            grouped = group_translated_lines_by_template_payloads(
                translated_visible_lines,
                original_lines,
            )
            if grouped is not None:
                lines, error = grouped, ""
            else:
                lines, error = translated_visible_lines, ""
    if lines is None:
        return None, error
    if len(lines) != len(parts):
        expanded = expand_visible_lines_to_parts(payload, parts, lines, original_lines)
        if expanded is not None:
            return expanded, ""
        repeated = expand_single_repeated_visible_line_to_parts(lines, original_lines)
        if repeated is not None:
            return repeated, ""
        return None, f"storage line 数量 {len(lines)} 与 parts 数量 {len(parts)} 不一致"
    return lines, ""


def infer_implicit_layout_rewrite(
    *,
    file_name: str,
    payload: Dict[str, object],
    parts: List[dict],
    original_lines: List[str],
    original_pages: List[Dict[str, object]],
) -> Tuple[Optional[List[str]], Optional[List[Dict[str, object]]], str]:
    if isinstance(payload.get("part_lines"), list):
        return None, None, ""

    translated_visible_lines = resolve_translated_visible_lines(payload)
    if not translated_visible_lines:
        return None, None, ""

    if str(file_name).upper() == "RL.MGP":
        original_chunks = _split_rl_window_chunks("\n".join(original_lines))
        translated_chunks = _split_rl_window_chunks("\n".join(translated_visible_lines))
        if original_chunks and len(original_chunks) == len(translated_chunks):
            translated_payloads: List[str] = []
            target_page_specs: List[Dict[str, object]] = []
            total_pages = len(original_pages)
            line_to_page_index: List[int] = []
            for page_idx, page in enumerate(original_pages):
                part_start = int(page["part_start"])
                part_count = int(page["part_count"])
                while len(line_to_page_index) < int(part_start):
                    line_to_page_index.append(-1)
                line_to_page_index.extend([int(page_idx)] * int(part_count))
            if len(line_to_page_index) != len(original_lines):
                line_to_page_index = []
            rl_chunk_plan_ok = True
            line_cursor = 0
            for chunk_idx, (original_chunk, translated_chunk) in enumerate(     
                zip(original_chunks, translated_chunks)
            ):
                original_chunk_lines = split_text_to_lines(str(original_chunk.get("raw") or ""))
                translated_chunk_lines = split_text_to_lines(str(translated_chunk.get("raw") or ""))
                if not original_chunk_lines:
                    continue
                if not translated_chunk_lines:
                    rl_chunk_plan_ok = False
                    break
                chunk_line_start = int(line_cursor)
                chunk_line_end = int(line_cursor) + len(original_chunk_lines)
                if chunk_line_end > len(original_lines):
                    rl_chunk_plan_ok = False
                    break
                if not line_to_page_index:
                    rl_chunk_plan_ok = False
                    break
                grouped_page_indices: List[int] = []
                grouped_page_capacities: List[int] = []
                current_page_index: Optional[int] = None
                current_capacity = 0
                for chunk_line_index in range(int(chunk_line_start), int(chunk_line_end)):
                    page_index = int(line_to_page_index[chunk_line_index])
                    if page_index < 0 or page_index >= total_pages:
                        rl_chunk_plan_ok = False
                        break
                    if current_page_index is None:
                        current_page_index = int(page_index)
                        current_capacity = 1
                        continue
                    if int(page_index) == int(current_page_index):
                        current_capacity += 1
                        continue
                    grouped_page_indices.append(int(current_page_index))
                    grouped_page_capacities.append(int(current_capacity))
                    current_page_index = int(page_index)
                    current_capacity = 1
                if not rl_chunk_plan_ok:
                    break
                if current_page_index is not None:
                    grouped_page_indices.append(int(current_page_index))
                    grouped_page_capacities.append(int(current_capacity))
                if sum(grouped_page_capacities) != len(original_chunk_lines):
                    rl_chunk_plan_ok = False
                    break
                # v1-compatible: merge extra CN lines into last record, pad if fewer
                if len(translated_chunk_lines) > len(original_chunk_lines):
                    merged_last = "\n".join(translated_chunk_lines[len(original_chunk_lines) - 1:])
                    translated_chunk_lines = translated_chunk_lines[:len(original_chunk_lines) - 1] + [merged_last]
                while len(translated_chunk_lines) < len(original_chunk_lines):
                    translated_chunk_lines.append("\u3000")
                if len(translated_chunk_lines) < len(grouped_page_indices):
                    rl_chunk_plan_ok = False
                    break
                translated_payloads.extend(translated_chunk_lines)
                remaining_lines = len(translated_chunk_lines)
                remaining_pages = len(grouped_page_indices)
                for grouped_index, template_page_index in enumerate(grouped_page_indices):
                    original_page = original_pages[int(template_page_index)]
                    page_capacity = int(grouped_page_capacities[grouped_index])
                    min_reserved_for_rest = remaining_pages - 1
                    assigned_count = min(page_capacity, remaining_lines - min_reserved_for_rest)
                    if assigned_count <= 0:
                        rl_chunk_plan_ok = False
                        break
                    page_spec = {
                        key: value
                        for key, value in original_page.items()
                        if key not in ("part_start", "part_count")
                    }
                    page_spec["template_page_index"] = int(template_page_index)
                    page_spec["part_count"] = int(assigned_count)
                    target_page_specs.append(page_spec)
                    remaining_lines -= int(assigned_count)
                    remaining_pages -= 1
                if not rl_chunk_plan_ok:
                    break
                if remaining_lines != 0:
                    rl_chunk_plan_ok = False
                    break
                line_cursor = int(chunk_line_end)
            if rl_chunk_plan_ok and line_cursor == len(original_lines):
                target_pages, error = normalize_target_pages(
                    target_page_specs,
                    expected_part_count=len(translated_payloads),
                )
                if translated_payloads and target_pages is not None:
                    return translated_payloads, target_pages, ""
                if target_pages is None:
                    return None, None, error

    speaker_slot_capacities: List[int] = []
    speaker_visible_capacities: List[int] = []
    speaker_original_lines_by_page: List[List[str]] = []
    speaker_passthrough_lines_by_page: List[List[str]] = []
    body_capacities: List[int] = []
    body_template_lines_by_page: List[List[str]] = []
    slot_kinds_by_page: List[List[str]] = []
    total_required_speakers = 0

    for page_idx, original_page in enumerate(original_pages):
        original_start = int(original_page["part_start"])
        original_count = int(original_page["part_count"])
        page_parts = parts[original_start:original_start + original_count]
        page_lines = original_lines[original_start:original_start + original_count]
        slot_kinds: List[str] = []
        speaker_slot_capacity = 0
        body_capacity = 0
        speaker_original_lines: List[str] = []
        body_template_lines: List[str] = []
        for part, original_line in zip(page_parts, page_lines):
            explicit_speaker = _part_is_explicit_speaker(part)
            if explicit_speaker:
                if is_nonvisible_storage_line(original_line):
                    slot_kinds.append("control")
                else:
                    slot_kinds.append("speaker")
                    speaker_slot_capacity += 1
                    speaker_original_lines.append(original_line)
            else:
                slot_kinds.append("body")
                body_capacity += 1
                body_template_lines.append(original_line)
        speaker_visible_capacity = 0
        speaker_passthrough_lines: List[str] = []
        if speaker_original_lines:
            all_same_speaker_line = all(line == speaker_original_lines[0] for line in speaker_original_lines)
            all_nonvisible_speaker_lines = all(
                is_nonvisible_storage_line(line)
                for line in speaker_original_lines
            )
            if all_nonvisible_speaker_lines:
                speaker_passthrough_lines = list(speaker_original_lines)
            else:
                speaker_visible_capacity = (
                    1
                    if all_same_speaker_line
                    else len(speaker_original_lines)
                )
        speaker_slot_capacities.append(speaker_slot_capacity)
        speaker_visible_capacities.append(speaker_visible_capacity)
        speaker_original_lines_by_page.append(speaker_original_lines)
        speaker_passthrough_lines_by_page.append(speaker_passthrough_lines)
        body_capacities.append(body_capacity)
        body_template_lines_by_page.append(body_template_lines)
        slot_kinds_by_page.append(slot_kinds)
        total_required_speakers += speaker_visible_capacity

    if total_required_speakers > 0 and len(translated_visible_lines) < total_required_speakers:
        return None, None, "中文行数少于必需的 speaker 槽位，无法隐式重排"

    if len(translated_visible_lines) <= 0:
        return None, None, ""

    translated_body_visible_lines = translated_visible_lines[total_required_speakers:]
    flattened_body_template_lines = [
        line
        for page_lines in body_template_lines_by_page
        for line in page_lines
    ]
    grouped_body_payloads = group_translated_lines_by_template_payloads(
        translated_body_visible_lines,
        flattened_body_template_lines,
    )
    body_lines = (
        grouped_body_payloads
        if grouped_body_payloads is not None
        else list(translated_body_visible_lines)
    )
    insert_template_index: Optional[int] = None
    extra_page_count = 0
    insert_page_capacity = 0
    total_body_capacity = sum(int(value) for value in body_capacities)
    if len(body_lines) > total_body_capacity:
        fallback_insert_template_index: Optional[int] = None
        for idx in range(len(original_pages) - 1, -1, -1):
            if int(speaker_slot_capacities[idx]) > 0:
                continue
            if int(body_capacities[idx]) <= 0:
                continue
            if all(not _control_signature(line) for line in body_template_lines_by_page[idx]):
                insert_template_index = idx
                break
            if fallback_insert_template_index is None:
                fallback_insert_template_index = idx
        if insert_template_index is None:
            insert_template_index = fallback_insert_template_index
        if insert_template_index is None:
            return None, None, "中文行数超出原始页面容量，且不存在可复用的无 speaker 页面模板，无法隐式重排"
        insert_page_capacity = int(body_capacities[insert_template_index])
        extra_body_lines = len(body_lines) - total_body_capacity
        extra_page_count = (int(extra_body_lines) + int(insert_page_capacity) - 1) // int(insert_page_capacity)

    remaining_body_capacity_from = [0] * (len(original_pages) + 1)
    remaining_speaker_capacity_from = [0] * (len(original_pages) + 1)
    remaining_body_page_count_from = [0] * (len(original_pages) + 1)
    for idx in range(len(original_pages) - 1, -1, -1):
        remaining_body_capacity_from[idx] = remaining_body_capacity_from[idx + 1] + int(body_capacities[idx])
        remaining_speaker_capacity_from[idx] = (
            remaining_speaker_capacity_from[idx + 1] + int(speaker_visible_capacities[idx])
        )
        remaining_body_page_count_from[idx] = (
            remaining_body_page_count_from[idx + 1] + (1 if int(body_capacities[idx]) > 0 else 0)
        )

    @lru_cache(maxsize=None)
    def _search_layout(
        page_idx: int,
        speaker_cursor: int,
        body_cursor: int,
        inserted_pages_used: int,
    ) -> Optional[Tuple[Tuple[int, int, int], ...]]:
        if speaker_cursor == total_required_speakers and body_cursor == len(body_lines):
            return tuple()
        if page_idx >= len(original_pages):
            return None
        if total_required_speakers - speaker_cursor > remaining_speaker_capacity_from[page_idx]:
            return None
        remaining_insert_capacity = int(max(0, extra_page_count - inserted_pages_used) * int(insert_page_capacity))
        if len(body_lines) - body_cursor > remaining_body_capacity_from[page_idx] + remaining_insert_capacity:
            return None

        speaker_slot_capacity = int(speaker_slot_capacities[page_idx])
        speaker_visible_capacity = int(speaker_visible_capacities[page_idx])
        speaker_original_lines = speaker_original_lines_by_page[page_idx]
        body_capacity = int(body_capacities[page_idx])
        body_template_lines = body_template_lines_by_page[page_idx]

        if speaker_slot_capacity > speaker_visible_capacity and (
            speaker_visible_capacity > 0
            and (
                not speaker_original_lines
                or not all(line == speaker_original_lines[0] for line in speaker_original_lines)
            )
        ):
            return None
        if speaker_cursor + speaker_visible_capacity > total_required_speakers:
            return None

        next_speaker_cursor = int(speaker_cursor + speaker_visible_capacity)
        available_body_lines = int(len(body_lines) - body_cursor)
        later_body_page_count = int(remaining_body_page_count_from[page_idx + 1])
        # When later original pages still have body capacity, keep at least one
        # payload for each of them when possible so page-local control-flow
        # anchors are preserved instead of being greedily collapsed forward.
        min_reserved_for_later_pages = min(max(available_body_lines - 1, 0), later_body_page_count)
        max_body_count = min(body_capacity, max(0, available_body_lines - min_reserved_for_later_pages))
        remaining_insert_capacity_after_step = int(max(0, extra_page_count - inserted_pages_used) * int(insert_page_capacity))

        for body_count in range(max_body_count, -1, -1):
            if (
                len(body_lines) - (body_cursor + body_count)
                > remaining_body_capacity_from[page_idx + 1] + remaining_insert_capacity_after_step
            ):
                continue
            if speaker_slot_capacity <= 0 and body_count <= 0:
                suffix = _search_layout(page_idx + 1, next_speaker_cursor, body_cursor, inserted_pages_used)
                if suffix is not None:
                    return ((0, page_idx, -1),) + suffix
                continue
            if body_count > 0:
                chosen = _choose_body_templates_with_reuse(
                    body_templates=[(b"", line, -1) for line in body_template_lines],
                    body_payloads=list(body_lines[body_cursor:body_cursor + body_count]),
                )
                if chosen is None:
                    continue
            suffix = _search_layout(page_idx + 1, next_speaker_cursor, body_cursor + body_count, inserted_pages_used)
            if suffix is not None:
                return ((0, page_idx, int(body_count)),) + suffix

        if (
            insert_template_index is not None
            and inserted_pages_used < int(extra_page_count)
            and body_cursor < len(body_lines)
        ):
            insert_body_templates = body_template_lines_by_page[int(insert_template_index)]
            max_insert_count = min(int(insert_page_capacity), len(body_lines) - body_cursor)
            for body_count in range(max_insert_count, 0, -1):
                chosen = _choose_body_templates_with_reuse(
                    body_templates=[(b"", line, -1) for line in insert_body_templates],
                    body_payloads=list(body_lines[body_cursor:body_cursor + body_count]),
                )
                if chosen is None:
                    continue
                suffix = _search_layout(
                    page_idx,
                    speaker_cursor,
                    body_cursor + body_count,
                    inserted_pages_used + 1,
                )
                if suffix is not None:
                    return ((1, int(insert_template_index), int(body_count)),) + suffix
        return None

    plan = _search_layout(0, 0, 0, 0)
    if plan is None:
        return None, None, "中文行无法按页面模板分配，无法隐式重排"

    translated_payloads: List[str] = []
    target_page_specs: List[Dict[str, object]] = []
    speaker_cursor = 0
    body_cursor = 0

    for step_kind, template_index, body_count in plan:
        if int(body_count) < 0:
            continue
        original_page = original_pages[template_index]
        speaker_slot_capacity = speaker_slot_capacities[template_index]
        speaker_visible_capacity = speaker_visible_capacities[template_index]
        speaker_original_lines = speaker_original_lines_by_page[template_index]
        speaker_passthrough_lines = speaker_passthrough_lines_by_page[template_index]
        slot_kinds = slot_kinds_by_page[template_index]

        speaker_values: List[str] = []
        if speaker_visible_capacity > 0:
            speaker_values = translated_visible_lines[speaker_cursor:speaker_cursor + speaker_visible_capacity]
            if speaker_slot_capacity > speaker_visible_capacity:
                speaker_values = [speaker_values[0]] * int(speaker_slot_capacity)
        elif speaker_passthrough_lines:
            speaker_values = list(speaker_passthrough_lines)

        page_payloads: List[str] = []
        speaker_slots_emitted = 0
        body_slots_emitted = 0
        for slot_kind in slot_kinds:
            if slot_kind == "speaker":
                if speaker_slots_emitted >= len(speaker_values):
                    return None, None, f"第 {template_index + 1} 页 speaker 槽数量不一致，无法隐式重排"
                page_payloads.append(speaker_values[speaker_slots_emitted])
                speaker_slots_emitted += 1
                continue
            if slot_kind == "control":
                continue
            if body_slots_emitted < int(body_count):
                page_payloads.append(body_lines[body_cursor + body_slots_emitted])
                body_slots_emitted += 1

        if not page_payloads:
            continue

        translated_payloads.extend(page_payloads)
        page_spec = {
            key: value
            for key, value in original_page.items()
            if key not in ("part_start", "part_count")
        }
        page_spec["part_count"] = len(page_payloads)
        page_spec["template_page_index"] = template_index
        target_page_specs.append(page_spec)
        if int(step_kind) == 0:
            speaker_cursor += int(speaker_visible_capacity)
        body_cursor += int(body_count)

    if speaker_cursor != total_required_speakers or body_cursor != len(body_lines):
        return None, None, "中文行分配后仍有残留，无法隐式重排"

    target_pages, error = normalize_target_pages(
        target_page_specs,
        expected_part_count=len(translated_payloads),
    )
    if target_pages is None:
        return None, None, error
    return translated_payloads, target_pages, ""


def resolve_rewrite_payload_lines(payload: Dict[str, object]) -> Tuple[Optional[List[str]], str]:
    part_lines = payload.get("part_lines")
    if not isinstance(part_lines, list):
        return None, "layout rewrite 需要显式 part_lines"
    return resolve_storage_lines("", part_lines)


def encode_exact_line(
    translated_line: str,
    original_line: str,
    char_map: Dict[str, bytes],
    *,
    preserve_visible_placeholder_positions: bool = True,
) -> bytes:
    translated_lines = [tokenize_line(line) for line in translated_line.split("\n")]
    translated_lines = merge_original_control_tokens(
        original_line,
        translated_lines,
        preserve_visible_placeholder_positions=preserve_visible_placeholder_positions,
    )
    encoded_parts = [encode_tokens(tokens, char_map) for tokens in translated_lines]
    encoded = b"\x0A".join(encoded_parts)
    if len(encoded) + 1 > 0xFF:
        raise WritebackFailure("单条文本记录超过255字节")
    return encoded


def _assign_local_idx_for_encoded(
    *,
    scene,
    local_idx: int,
    encoded: bytes,
    replacements: Dict[int, bytes],
    clone_records: List[bytes],
    clone_aliases: Dict[Tuple[int, bytes], int],
) -> int:
    assigned_local_idx = clone_aliases.get((int(local_idx), encoded))
    if assigned_local_idx is None:
        existing_encoded = replacements.get(int(local_idx))
        if existing_encoded is None or existing_encoded == encoded:
            replacements[int(local_idx)] = encoded
            assigned_local_idx = int(local_idx)
        else:
            assigned_local_idx = len(scene.local_texts) + len(clone_records)
            clone_records.append(encoded)
    clone_aliases[(int(local_idx), encoded)] = int(assigned_local_idx)
    return int(assigned_local_idx)


def _seed_residue_replacements_from_part_lines(
    *,
    parts: List[dict],
    original_lines: List[str],
    translated_lines: List[str],
    offset_to_local_idx: Dict[int, int],
    replacements: Dict[int, bytes],
    char_map: Dict[str, bytes],
    preserve_visible_placeholder_positions: bool = False,
) -> None:
    if len(parts) != len(original_lines) or len(parts) != len(translated_lines):
        return
    for part, original_line, translated_line in zip(parts, original_lines, translated_lines):
        part_offset = part.get("offset")
        if not isinstance(part_offset, int):
            continue
        local_idx = offset_to_local_idx.get(int(part_offset))
        if local_idx is None or int(local_idx) in replacements:
            continue
        try:
            encoded = encode_exact_line(
                translated_line,
                original_line,
                char_map,
                preserve_visible_placeholder_positions=preserve_visible_placeholder_positions,
            )
        except Exception:
            continue
        replacements[int(local_idx)] = encoded


def patch_mgp_event_local_idx_v2(
    scene_nested: bytearray,
    scene_abs_start: int,
    part: dict,
    new_local_idx: int,
) -> bool:
    if new_local_idx < 0 or new_local_idx > 0xFFFF:
        return False

    bytecode_offset = part.get("bytecode_offset")
    bytecode_length = part.get("bytecode_length")
    if (
        not isinstance(bytecode_offset, int)
        or isinstance(bytecode_offset, bool)
        or not isinstance(bytecode_length, int)
        or isinstance(bytecode_length, bool)
        or bytecode_length <= 0
    ):
        return False

    rel_offset = int(bytecode_offset) - scene_abs_start
    if rel_offset < 0 or rel_offset + int(bytecode_length) > len(scene_nested):
        return False

    raw_hex = str(part.get("bytecode_raw") or "")
    if raw_hex:
        try:
            expected_raw = bytes.fromhex(raw_hex)
        except ValueError:
            return False
        if bytes(scene_nested[rel_offset:rel_offset + len(expected_raw)]) != expected_raw:
            return False

    decoded_length = opcode_length(bytes(scene_nested), rel_offset, len(scene_nested))
    if decoded_length != int(bytecode_length):
        return False

    try:
        patched = _patch_opcode_local_idx(
            bytes(scene_nested[rel_offset:rel_offset + int(bytecode_length)]), new_local_idx,
        )
    except WritebackFailure:
        return False
    scene_nested[rel_offset:rel_offset + int(bytecode_length)] = patched
    return True


def _collect_part_bytecode_starts(
    entries: Sequence[Dict[str, object]],
    *,
    scene_abs_start: int,
) -> Set[int]:
    starts: Set[int] = set()
    for payload in entries:
        parts = payload.get("parts")
        if not isinstance(parts, list):
            continue
        for part in parts:
            if not isinstance(part, dict):
                continue
            bytecode_offset = part.get("bytecode_offset")
            if isinstance(bytecode_offset, int) and not isinstance(bytecode_offset, bool):
                starts.add(int(bytecode_offset) - int(scene_abs_start))
    return starts


def _build_local_text_block(records: Sequence[bytes], *, file_name: str, offset: Optional[int] = None) -> bytes:
    block = bytearray()
    block.extend(struct.pack("<H", len(records)))
    total_bytes = 0
    for raw in records:
        if len(raw) + 1 > 0xFF:
            fail(file_name, "local_text 超过255字节", offset=offset)
        block.append(len(raw) + 1)
        total_bytes += len(raw) + 1
    block.extend(struct.pack("<H", total_bytes & 0xFFFF))
    for raw in records:
        block.extend(raw)
        block.append(0x00)
    return bytes(block)


def _build_vm_noop_padding(length: int) -> bytes:
    if int(length) <= 0:
        return b""
    # 0x0F is an XBE-confirmed trivial pass-through control opcode with no payload.
    return b"\x0F" * int(length)


def _plan_layout_rewrite_slices(
    *,
    scene,
    scene_abs_start: int,
    payload: Dict[str, object],
    parts: List[dict],
    original_lines: List[str],
    original_pages: List[Dict[str, object]],
    translated_payloads: List[str],
    target_pages: List[Dict[str, object]],
    char_map: Dict[str, bytes],
    offset_to_local_idx: Dict[int, int],
    replacements: Dict[int, bytes],
    clone_records: List[bytes],
    clone_aliases: Dict[Tuple[int, bytes], int],
    file_name: str,
) -> List[Dict[str, object]]:
    def _collect_source_page_anchor_offsets(source_page: Dict[str, object]) -> List[int]:
        anchors: List[int] = []
        segment_start = source_page.get("segment_start")
        if isinstance(segment_start, int) and not isinstance(segment_start, bool):
            anchors.append(int(segment_start))
        marker_start = source_page.get("marker_start")
        if isinstance(marker_start, int) and not isinstance(marker_start, bool):
            anchors.append(int(marker_start))
        for event in list(source_page.get("page_events") or []):
            rel_offset = getattr(event, "rel_offset", None)
            if isinstance(rel_offset, int) and not isinstance(rel_offset, bool):
                anchors.append(int(rel_offset))
        for event_offset, _event_raw in list(source_page.get("pre_marker_gap_events") or []):
            if isinstance(event_offset, int) and not isinstance(event_offset, bool):
                anchors.append(int(event_offset))
        for event_offset, _event_raw in list(source_page.get("trailing_gap_events") or []):
            if isinstance(event_offset, int) and not isinstance(event_offset, bool):
                anchors.append(int(event_offset))
        deduped: List[int] = []
        seen: set[int] = set()
        for value in anchors:
            if value in seen:
                continue
            seen.add(value)
            deduped.append(int(value))
        return deduped

    if not original_pages:
        fail(file_name, "条目缺少原始 pages", offset=payload.get("offset"))
    if not target_pages:
        fail(file_name, "target_pages 不能为空", offset=payload.get("offset"))

    source_page_segments: List[Dict[str, object]] = []
    previous_segment_end: Optional[int] = None
    for page_idx, original_page in enumerate(original_pages):
        original_start = int(original_page["part_start"])
        original_count = int(original_page["part_count"])
        page_parts = parts[original_start:original_start + original_count]
        page_lines = original_lines[original_start:original_start + original_count]
        if len(page_parts) != original_count or len(page_lines) != original_count:
            fail(file_name, "原始页面 part 结构不完整", offset=payload.get("offset"))

        page_target_offsets = [_part_bytecode_rel_start(part, scene_abs_start) for part in page_parts]
        page_start = min(page_target_offsets)
        page_text_end = max(_part_bytecode_rel_end(part, scene_abs_start) for part in page_parts)

        marker_offset = original_page.get("marker_offset")
        marker_length = original_page.get("marker_length")
        marker_raw_hex = str(original_page.get("marker_raw") or "")
        marker_start = page_text_end
        marker_end = page_text_end
        marker_raw = b""
        pre_marker_gap_raw = b""
        pre_marker_gap_events: List[Tuple[int, bytes]] = []
        if (
            isinstance(marker_offset, int)
            and not isinstance(marker_offset, bool)
            and isinstance(marker_length, int)
            and not isinstance(marker_length, bool)
            and int(marker_length) > 0
            and marker_raw_hex
        ):
            try:
                marker_raw = bytes.fromhex(marker_raw_hex)
            except ValueError as exc:
                fail(file_name, "marker_raw 非法", offset=payload.get("offset"))
                raise exc
            marker_raw = reading_wait_marker(original_page)
            marker_start = int(marker_offset) - int(scene_abs_start)
            marker_end = marker_start + int(marker_length)
            if len(marker_raw) != int(marker_length):
                fail(file_name, "marker_raw 与 marker_length 不一致", offset=payload.get("offset"))
            if marker_start < page_text_end:
                fail(file_name, "marker 落在文本 opcode 中间，当前不支持重排", offset=payload.get("offset"))
            if marker_start > page_text_end:
                gap_cursor = int(page_text_end)
                gap_bytes = bytearray()
                while gap_cursor < int(marker_start):
                    event = decode_opcode_event(scene, gap_cursor)
                    if event is None or int(event.rel_offset) != int(gap_cursor):
                        remaining = int(marker_start) - gap_cursor
                        raw_chunk = bytes(scene.code[gap_cursor:gap_cursor + remaining])
                        gap_bytes.extend(raw_chunk)
                        pre_marker_gap_events.append((int(gap_cursor), raw_chunk))
                        gap_cursor += remaining
                        break
                    gap_bytes.extend(bytes(event.raw))
                    pre_marker_gap_events.append((int(event.rel_offset), bytes(event.raw)))
                    gap_cursor += int(event.length)
                if gap_cursor != int(marker_start):
                    remaining = int(marker_start) - gap_cursor
                    raw_chunk = bytes(scene.code[gap_cursor:gap_cursor + remaining])
                    gap_bytes.extend(raw_chunk)
                    pre_marker_gap_events.append((int(gap_cursor), raw_chunk))
                    gap_cursor = int(marker_start)
                pre_marker_gap_raw = bytes(gap_bytes)

        page_offset_to_part_index = {
            _part_bytecode_rel_start(part, scene_abs_start): idx
            for idx, part in enumerate(page_parts)
        }
        page_events: List[object] = []
        cursor = int(page_start)
        while cursor < int(page_text_end):
            event = decode_opcode_event(scene, cursor)
            if event is None or int(event.rel_offset) != int(cursor):
                fail(file_name, "无法重建原始页面 opcode 流", offset=payload.get("offset"))
            page_events.append(event)
            cursor += int(event.length)
        seen_offsets = [
            int(event.rel_offset)
            for event in page_events
            if int(event.rel_offset) in page_offset_to_part_index
        ]
        if seen_offsets != page_target_offsets:
            fail(file_name, "页面文本 opcode 顺序异常，无法安全重排", offset=payload.get("offset"))

        speaker_event_indices: List[int] = []
        body_event_indices: List[int] = []
        for event_idx, event in enumerate(page_events):
            part_idx = page_offset_to_part_index.get(int(event.rel_offset))
            if part_idx is None:
                continue
            original_line = page_lines[int(part_idx)]
            if _part_is_explicit_speaker(page_parts[int(part_idx)]):
                if is_nonvisible_storage_line(original_line):
                    continue
                speaker_event_indices.append(event_idx)
            else:
                body_event_indices.append(event_idx)
        if body_event_indices:
            if speaker_event_indices and speaker_event_indices[0] > body_event_indices[0]:
                fail(file_name, "显式 speaker 位于正文之后，当前不支持重排", offset=payload.get("offset"))
            body_block_start = int(body_event_indices[0])
            body_block_end = int(body_event_indices[-1]) + 1
        else:
            body_block_start = len(page_events)
            body_block_end = len(page_events)

        trailing_gap_raw = b""
        trailing_gap_events: List[Tuple[int, bytes]] = []
        if previous_segment_end is not None and page_start != previous_segment_end:
            gap_cursor = int(previous_segment_end)
            gap_bytes = bytearray()
            while gap_cursor < int(page_start):
                event = decode_opcode_event(scene, gap_cursor)
                if event is None or int(event.rel_offset) != int(gap_cursor):
                    # Unknown opcode — copy raw bytes verbatim
                    remaining = int(page_start) - gap_cursor
                    raw_chunk = bytes(scene.code[gap_cursor:gap_cursor + remaining])
                    gap_bytes.extend(raw_chunk)
                    trailing_gap_events.append((int(gap_cursor), raw_chunk))
                    gap_cursor += remaining
                    break
                gap_bytes.extend(bytes(event.raw))
                trailing_gap_events.append((int(event.rel_offset), bytes(event.raw)))
                gap_cursor += int(event.length)
            if gap_cursor != int(page_start):
                # Copy remaining raw bytes
                remaining = int(page_start) - gap_cursor
                raw_chunk = bytes(scene.code[gap_cursor:gap_cursor + remaining])
                gap_bytes.extend(raw_chunk)
                trailing_gap_events.append((int(gap_cursor), raw_chunk))
                gap_cursor = int(page_start)
            if source_page_segments:
                source_page_segments[-1]["trailing_gap_raw"] = bytes(gap_bytes)
                source_page_segments[-1]["trailing_gap_events"] = list(trailing_gap_events)
        previous_segment_end = marker_end

        source_page_segments.append(
            {
                "page_parts": page_parts,
                "page_lines": page_lines,
                "page_events": page_events,
                "page_offset_to_part_index": page_offset_to_part_index,
                "body_block_start": body_block_start,
                "body_block_end": body_block_end,
                "body_event_indices": list(body_event_indices),
                "pre_marker_gap_raw": pre_marker_gap_raw,
                "pre_marker_gap_events": list(pre_marker_gap_events),
                "marker_start": int(marker_start),
                "marker_raw": marker_raw,
                "trailing_gap_raw": trailing_gap_raw,
                "trailing_gap_events": [],
                "segment_start": int(page_start),
                "segment_end": int(marker_end),
            }
        )

    entry_start = int(source_page_segments[0]["segment_start"])
    entry_end = int(source_page_segments[-1]["segment_end"])
    target_cursor = 0
    replacement = bytearray()
    offset_map: Dict[int, int] = {}
    preserved_spans: List[Dict[str, int]] = []

    def _record_preserved_span(src_start: int, length: int, dst_start: int) -> None:
        if int(length) <= 0:
            return
        preserved_spans.append({
            "src_start": int(src_start),
            "src_end": int(src_start) + int(length),
            "dst_start": int(dst_start),
        })
    last_template_page_index = -1
    for page_idx, target_page in enumerate(target_pages):
        template_page_index = target_page.get("template_page_index", min(page_idx, len(source_page_segments) - 1))
        if (
            not isinstance(template_page_index, int)
            or isinstance(template_page_index, bool)
            or int(template_page_index) < 0
            or int(template_page_index) >= len(source_page_segments)
        ):
            fail(file_name, f"target_pages[{page_idx}].template_page_index 非法", offset=payload.get("offset"))
        source_page = source_page_segments[int(template_page_index)]
        current_page_dst_start = len(replacement)
        if int(template_page_index) > int(last_template_page_index) + 1:
            for skipped_page_index in range(int(last_template_page_index) + 1, int(template_page_index)):
                skipped_page = source_page_segments[int(skipped_page_index)]
                for anchor_offset in _collect_source_page_anchor_offsets(skipped_page):
                    offset_map.setdefault(int(anchor_offset), int(current_page_dst_start))
        target_count = int(target_page["part_count"])
        page_parts = list(source_page["page_parts"])
        page_lines = list(source_page["page_lines"])
        page_events = list(source_page["page_events"])
        page_offset_to_part_index = dict(source_page["page_offset_to_part_index"])
        body_block_start = int(source_page["body_block_start"])
        body_block_end = int(source_page["body_block_end"])
        pre_marker_gap_events = [(int(offset), bytes(raw)) for offset, raw in source_page.get("pre_marker_gap_events", [])]
        body_event_indices = [int(value) for value in source_page.get("body_event_indices", [])]
        pre_marker_gap_raw = bytes(source_page.get("pre_marker_gap_raw") or b"")
        marker_start = int(source_page.get("marker_start", source_page["segment_end"]))
        trailing_gap_events = [(int(offset), bytes(raw)) for offset, raw in source_page.get("trailing_gap_events", [])]
        translated_page_payloads = translated_payloads[target_cursor:target_cursor + target_count]
        target_cursor += target_count

        if len(translated_page_payloads) != target_count:
            fail(file_name, "target_pages 与 part_lines 数量不一致", offset=payload.get("offset"))

        page_slot_kinds: List[str] = []
        speaker_templates: List[Tuple[bytes, str, int]] = []
        body_templates: List[Tuple[bytes, str, int]] = []
        for part, original_line in zip(page_parts, page_lines):
            opcode_raw = _part_opcode_raw(part)
            part_offset = part.get("offset")
            if not isinstance(part_offset, int):
                fail(file_name, "part 缺少 offset", offset=payload.get("offset"))
            local_idx = offset_to_local_idx.get(int(part_offset))
            if local_idx is None:
                fail(file_name, "part offset 未命中 local_text", offset=int(part_offset))
            if _part_is_explicit_speaker(part):
                if is_nonvisible_storage_line(original_line):
                    page_slot_kinds.append("control")
                    continue
                page_slot_kinds.append("speaker")
                speaker_templates.append((opcode_raw, original_line, int(local_idx)))
            else:
                page_slot_kinds.append("body")
                body_templates.append((opcode_raw, original_line, int(local_idx)))

        required_speaker_count = len(speaker_templates)
        required_fixed_count = required_speaker_count
        if target_count < required_fixed_count:
            fail(file_name, f"第 {page_idx + 1} 页 target_pages.part_count 过小，无法保留 speaker", offset=payload.get("offset"))

        body_target_count = target_count - required_fixed_count
        target_slot_kinds: List[str] = []
        kept_original_body_slots = min(body_target_count, len(body_templates))
        kept_body_cursor = 0
        for slot_kind in page_slot_kinds:
            if slot_kind == "control":
                continue
            if slot_kind != "body":
                target_slot_kinds.append(slot_kind)
                continue
            if kept_body_cursor < kept_original_body_slots:
                target_slot_kinds.append("body")
                kept_body_cursor += 1
        while kept_body_cursor < body_target_count:
            target_slot_kinds.append("body")
            kept_body_cursor += 1

        if len(target_slot_kinds) != len(translated_page_payloads):
            fail(file_name, f"第 {page_idx + 1} 页 payload/slot 数量不一致", offset=payload.get("offset"))

        speaker_payloads: List[str] = []
        body_payloads: List[str] = []
        for slot_kind, translated_payload in zip(target_slot_kinds, translated_page_payloads):
            if slot_kind == "speaker":
                speaker_payloads.append(translated_payload)
            elif slot_kind == "body":
                body_payloads.append(translated_payload)

        if len(speaker_payloads) != required_speaker_count:
            fail(file_name, f"第 {page_idx + 1} 页 speaker payload 数量不一致", offset=payload.get("offset"))

        if body_target_count > 0:
            if not body_templates:
                fail(file_name, f"第 {page_idx + 1} 页不存在可复用正文模板", offset=payload.get("offset"))
            template_keys = {_opcode_local_idx_mask(raw) for raw, _, _ in body_templates}
            if body_target_count > len(body_templates) and len(template_keys) != 1:
                fail(
                    file_name,
                    f"第 {page_idx + 1} 页正文 opcode 模板不一致，不能无歧义扩展 payload 数量",
                    offset=payload.get("offset"),
                )
            body_target_templates = _choose_body_templates_with_reuse(
                body_templates=body_templates,
                body_payloads=body_payloads,
            )
            if body_target_templates is None or len(body_target_templates) != body_target_count:
                fail(
                    file_name,
                    f"第 {page_idx + 1} 页无法为中文正文选择合法模板",
                    offset=payload.get("offset"),
                )
        else:
            body_target_templates = []

        emitted_speaker_slots = 0
        last_emitted_body_start: Optional[int] = None
        for event in page_events[:body_block_start]:
            part_idx = page_offset_to_part_index.get(int(event.rel_offset))
            if part_idx is None:
                dst_start = len(replacement)
                offset_map[int(event.rel_offset)] = dst_start
                replacement.extend(bytes(event.raw))
                _record_preserved_span(int(event.rel_offset), int(event.length), dst_start)
                continue
            original_line = page_lines[int(part_idx)]
            if is_nonvisible_storage_line(original_line):
                dst_start = len(replacement)
                offset_map[int(event.rel_offset)] = dst_start
                replacement.extend(bytes(event.raw))
                _record_preserved_span(int(event.rel_offset), int(event.length), dst_start)
                continue
            if not _part_is_explicit_speaker(page_parts[int(part_idx)]):
                fail(file_name, "正文 opcode 出现在页前缀区域，当前不支持重排", offset=payload.get("offset"))
            if emitted_speaker_slots >= len(speaker_payloads):
                fail(file_name, "缺少可复用 speaker 模板", offset=payload.get("offset"))
            opcode_raw, original_line, template_local_idx = speaker_templates[emitted_speaker_slots]
            try:
                encoded = encode_exact_line(
                    speaker_payloads[emitted_speaker_slots],
                    original_line,
                    char_map,
                )
            except WritebackFailure as exc:
                fail(file_name, str(exc), offset=payload.get("offset"))
            new_local_idx = _assign_local_idx_for_encoded(
                scene=scene,
                local_idx=int(template_local_idx),
                encoded=encoded,
                replacements=replacements,
                clone_records=clone_records,
                clone_aliases=clone_aliases,
            )
            offset_map[int(event.rel_offset)] = len(replacement)
            replacement.extend(_patch_opcode_local_idx(opcode_raw, new_local_idx))
            emitted_speaker_slots += 1

        original_body_offsets = [int(page_events[event_idx].rel_offset) for event_idx in body_event_indices]
        body_gap_buckets: List[List[object]] = [[] for _ in range(len(body_event_indices) + 1)]
        seen_body_slots = 0
        for event_idx, event in enumerate(page_events[body_block_start:body_block_end], start=body_block_start):
            part_idx = page_offset_to_part_index.get(int(event.rel_offset))
            if part_idx is not None:
                original_line = page_lines[int(part_idx)]
                if not _part_is_explicit_speaker(page_parts[int(part_idx)]):
                    seen_body_slots += 1
                    continue
            bucket_idx = min(seen_body_slots, len(body_gap_buckets) - 1)
            body_gap_buckets[bucket_idx].append(event)

        def _emit_preserved_event(event) -> None:
            dst_start = len(replacement)
            offset_map[int(event.rel_offset)] = dst_start
            replacement.extend(bytes(event.raw))
            _record_preserved_span(int(event.rel_offset), int(event.length), dst_start)

        for event in body_gap_buckets[0]:
            _emit_preserved_event(event)

        emitted_body_starts: List[int] = []
        body_anchor_start = len(replacement)
        for body_idx, translated_payload in enumerate(body_payloads):
            opcode_raw, original_line, template_local_idx = body_target_templates[body_idx]
            try:
                encoded = encode_exact_line(
                    translated_payload,
                    original_line,
                    char_map,
                )
            except WritebackFailure as exc:
                fail(file_name, str(exc), offset=payload.get("offset"))
            new_local_idx = _assign_local_idx_for_encoded(
                scene=scene,
                local_idx=int(template_local_idx),
                encoded=encoded,
                replacements=replacements,
                clone_records=clone_records,
                clone_aliases=clone_aliases,
            )
            emitted_body_start = len(replacement)
            emitted_body_starts.append(emitted_body_start)
            replacement.extend(_patch_opcode_local_idx(opcode_raw, new_local_idx))
            if body_idx + 1 < len(body_gap_buckets):
                for event in body_gap_buckets[body_idx + 1]:
                    _emit_preserved_event(event)

        for bucket_idx in range(len(body_payloads) + 1, len(body_gap_buckets)):
            for event in body_gap_buckets[bucket_idx]:
                _emit_preserved_event(event)

        if emitted_body_starts:
            for old_body_idx, old_offset in enumerate(original_body_offsets):
                mapped_body_idx = min(old_body_idx, len(emitted_body_starts) - 1)
                offset_map[int(old_offset)] = int(emitted_body_starts[mapped_body_idx])
                last_emitted_body_start = int(emitted_body_starts[mapped_body_idx])
        else:
            for old_offset in original_body_offsets:
                offset_map[int(old_offset)] = int(body_anchor_start)

        for event in page_events[body_block_end:]:
            part_idx = page_offset_to_part_index.get(int(event.rel_offset))
            if part_idx is not None:
                original_line = page_lines[int(part_idx)]
                if is_nonvisible_storage_line(original_line):
                    dst_start = len(replacement)
                    offset_map[int(event.rel_offset)] = dst_start
                    replacement.extend(bytes(event.raw))
                    _record_preserved_span(int(event.rel_offset), int(event.length), dst_start)
                    continue
                if _part_is_explicit_speaker(page_parts[int(part_idx)]):
                    if emitted_speaker_slots >= len(speaker_payloads):
                        fail(file_name, "speaker payload 数量不足，无法重排", offset=payload.get("offset"))
                    opcode_raw, original_line, template_local_idx = speaker_templates[emitted_speaker_slots]
                    try:
                        encoded = encode_exact_line(
                            speaker_payloads[emitted_speaker_slots],
                            original_line,
                            char_map,
                        )
                    except WritebackFailure as exc:
                        fail(file_name, str(exc), offset=payload.get("offset"))
                    new_local_idx = _assign_local_idx_for_encoded(
                        scene=scene,
                        local_idx=int(template_local_idx),
                        encoded=encoded,
                        replacements=replacements,
                        clone_records=clone_records,
                        clone_aliases=clone_aliases,
                    )
                    offset_map[int(event.rel_offset)] = len(replacement)
                    replacement.extend(_patch_opcode_local_idx(opcode_raw, new_local_idx))
                    emitted_speaker_slots += 1
                    continue
                fail(file_name, "正文 opcode 出现在页后缀区域，当前不支持重排", offset=payload.get("offset"))
            dst_start = len(replacement)
            offset_map[int(event.rel_offset)] = dst_start
            replacement.extend(bytes(event.raw))
            _record_preserved_span(int(event.rel_offset), int(event.length), dst_start)
        for event_offset, event_raw in pre_marker_gap_events:
            dst_start = len(replacement)
            offset_map[int(event_offset)] = dst_start
            replacement.extend(event_raw)
            _record_preserved_span(int(event_offset), len(event_raw), dst_start)
        if pre_marker_gap_raw and not pre_marker_gap_events:
            replacement.extend(pre_marker_gap_raw)
        marker_dst_start = len(replacement)
        offset_map[int(marker_start)] = marker_dst_start
        replacement.extend(bytes(source_page["marker_raw"]))
        _record_preserved_span(int(marker_start), len(bytes(source_page["marker_raw"])), marker_dst_start)
        if page_idx + 1 < len(target_pages):
            for event_offset, event_raw in trailing_gap_events:
                dst_start = len(replacement)
                offset_map[int(event_offset)] = dst_start
                replacement.extend(event_raw)
                _record_preserved_span(int(event_offset), len(event_raw), dst_start)
            if source_page.get("trailing_gap_raw") and not trailing_gap_events:
                replacement.extend(bytes(source_page.get("trailing_gap_raw") or b""))
        last_template_page_index = int(template_page_index)

    if target_cursor != len(translated_payloads):
        fail(file_name, "part_lines 存在未分配到页面的 payload", offset=payload.get("offset"))
    original_length = int(entry_end) - int(entry_start)
    if len(replacement) < original_length:
        replacement.extend(_build_vm_noop_padding(original_length - len(replacement)))
    return [
        {
            "start": int(entry_start),
            "end": int(entry_end),
            "replacement": bytes(replacement),
            "offset_map": dict(offset_map),
            "preserved_spans": list(preserved_spans),
            "allow_inside_anchor_fallback": str(file_name).upper() == "RL.MGP",
        }
    ]


def _relocate_code_offset(
    old_offset: int,
    rewrite_slices: Sequence[Dict[str, object]],
    *,
    file_name: str,
    context: str,
    payload_offset: Optional[int],
    allow_deleted_inside_slice: bool = False,
) -> Optional[int]:
    delta = 0
    for slice_info in rewrite_slices:
        start = int(slice_info["start"])
        end = int(slice_info["end"])
        replacement = bytes(slice_info["replacement"])
        offset_map = {
            int(src): int(dst)
            for src, dst in dict(slice_info.get("offset_map") or {}).items()
        }
        preserved_spans = [
            {
                "src_start": int(item.get("src_start", 0)),
                "src_end": int(item.get("src_end", 0)),
                "dst_start": int(item.get("dst_start", 0)),
            }
            for item in list(slice_info.get("preserved_spans") or [])
            if isinstance(item, dict)
        ]
        if old_offset < start:
            return int(old_offset + delta)
        mapped_offset = offset_map.get(int(old_offset))
        if mapped_offset is not None:
            return int(start + delta + mapped_offset)
        if old_offset == start:
            return int(old_offset + delta)
        if start < old_offset < end:
            for preserved in preserved_spans:
                src_start = int(preserved["src_start"])
                src_end = int(preserved["src_end"])
                if src_start <= int(old_offset) < src_end:
                    return int(start + delta + int(preserved["dst_start"]) + (int(old_offset) - src_start))
            if bool(slice_info.get("allow_inside_anchor_fallback")):
                fallback_candidates: List[Tuple[int, int]] = []
                for preserved in preserved_spans:
                    fallback_candidates.append((int(preserved["src_start"]), int(preserved["dst_start"])))
                for src, dst in offset_map.items():
                    fallback_candidates.append((int(src), int(dst)))
                fallback_candidates.sort(key=lambda item: (int(item[0]), int(item[1])))
                for src, dst in fallback_candidates:
                    if int(src) >= int(old_offset):
                        return int(start + delta + int(dst))
                for src, dst in reversed(fallback_candidates):
                    if int(src) < int(old_offset):
                        return int(start + delta + int(dst))
            if allow_deleted_inside_slice:
                return None
            slice_payload_offset = slice_info.get("payload_offset")
            fail(
                file_name,
                (
                    f"{context} 落在被重写的文本 opcode 中间，无法无歧义重定位: "
                    f"target=0x{int(old_offset):X}, slice=[0x{start:X},0x{end:X}), "
                    f"slice_payload=0x{int(slice_payload_offset):X}" if isinstance(slice_payload_offset, int)
                    else f"{context} 落在被重写的文本 opcode 中间，无法无歧义重定位: "
                         f"target=0x{int(old_offset):X}, slice=[0x{start:X},0x{end:X})"
                ),
                offset=payload_offset,
            )
        delta += len(replacement) - (end - start)
    return int(old_offset + delta)


def _scene_label_offset_map_for_rewrite(scene) -> Dict[str, List[int]]:
    mapping: Dict[str, List[int]] = defaultdict(list)
    for offset, label in zip(scene.label_offsets, scene.label_names):
        mapping[str(label)].append(int(offset))
    return dict(mapping)


def _scene_root_offsets_for_rewrite(scene) -> List[int]:
    label_map = _scene_label_offset_map_for_rewrite(scene)
    starts: List[int] = []
    for key in ("INIT", "MAIN"):
        starts.extend(int(offset) for offset in label_map.get(key, []))
    # Named entries may be invoked by scene controllers or kept as unused scripts.
    # Their real instruction spans must still protect operands during rewriting.
    starts.extend(int(offset) for offset in scene.label_offsets)
    if not starts:
        starts.append(int(scene.entry_offset))
    deduped: List[int] = []
    seen: Set[int] = set()
    for offset in starts:
        if 0 <= int(offset) < int(scene.code_end) and int(offset) not in seen:
            seen.add(int(offset))
            deduped.append(int(offset))
    return deduped


def _part_event_conflicts(part, scene_abs_start, event_spans):
    start = _part_bytecode_rel_start(part, scene_abs_start)
    end = _part_bytecode_rel_end(part, scene_abs_start)
    return [(left, right) for left, right in event_spans
            if left != start and start < right and end > left]


def _collect_reachable_event_starts_for_rewrite(scene) -> List[int]:
    starts = _scene_root_offsets_for_rewrite(scene)
    queue = deque((int(start), tuple(), None, tuple()) for start in starts)
    visited_states: Set[Tuple[int, Tuple[int, ...], Optional[int], Tuple[int, ...]]] = set()
    visited_events: Set[int] = set()
    code = scene.code

    while queue:
        pc, stack, ret, timed_targets = queue.popleft()
        state = (int(pc), stack, ret, timed_targets)
        if state in visited_states or not (0 <= int(pc) < int(scene.code_end)):
            continue
        visited_states.add(state)
        visited_events.add(int(pc))

        length = opcode_length(code, int(pc), scene.code_end)
        if length is None or int(pc) + int(length) > int(scene.code_end):
            continue

        op = int(code[int(pc)])
        fallthrough = int(pc) + int(length)

        if op in NONRETURNING_STOP_OPCODES:
            continue
        if op in RESUMING_STOP_OPCODES:
            queue.append((fallthrough, stack, ret, tuple()))
            continue
        if op == 0x10:
            if stack:
                queue.append((stack[-1], stack[:-1], ret, tuple()))
            elif starts:
                queue.append((int(starts[-1]), tuple(), ret, tuple()))
            continue
        if op == 0x0A:
            idx = struct.unpack_from("<H", code, int(pc) + 3)[0]
            queue.append((fallthrough, stack, ret, timed_targets))
            if 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), stack, ret, tuple()))
            continue
        if op in (0x0B, 0x0C):
            idx = struct.unpack_from("<H", code, int(pc) + 25)[0]
            queue.append((fallthrough, stack, ret, timed_targets))
            if 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), stack, ret, tuple()))
            continue
        if op == 0x11:
            idx = struct.unpack_from("<H", code, int(pc) + 1)[0]
            if 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), stack, ret, tuple()))
            continue
        if op == 0x12:
            idx = struct.unpack_from("<H", code, int(pc) + 1)[0]
            next_stack = stack + (fallthrough,)
            if len(stack) >= ROOT_STACK_WARNING_DEPTH:
                queue.append((fallthrough, next_stack, ret, tuple()))
                continue
            if 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), next_stack, ret, tuple()))
            continue
        if op == 0x15:
            dest = struct.unpack_from("<I", code, int(pc) + 1)[0]
            queue.append((fallthrough, stack, ret, timed_targets))
            if 0 <= int(dest) < int(scene.code_end) and code[int(dest)] == 0x16:
                dest += 1
            queue.append((int(dest), stack, ret, tuple()))
            continue
        if op == 0x16:
            if int(length) == 6:
                dest = struct.unpack_from("<I", code, int(pc) + 2)[0]
                queue.append((int(dest), stack, ret, tuple()))
            else:
                queue.append((fallthrough, stack, ret, timed_targets))
            continue
        if op == 0x1B:
            queue.append((fallthrough, stack, ret, tuple()))
            continue
        if op in (0x1A, 0x58):
            event = decode_opcode_event(scene, int(pc))
            idx = event.extra_value
            queue.append((fallthrough, stack, ret, timed_targets))
            if idx is not None and 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), stack, ret, tuple()))
            continue
        if op == 0x1D:
            idx = struct.unpack_from("<H", code, int(pc) + 3)[0]
            next_targets = timed_targets
            if 0 <= idx < len(scene.label_offsets):
                target = int(scene.label_offsets[idx])
                if target not in next_targets:
                    next_targets = timed_targets + (target,)
            queue.append((fallthrough, stack, ret, next_targets))
            continue
        if op == 0x1C:
            for target in timed_targets:
                queue.append((int(target), stack, ret, tuple()))
            queue.append((fallthrough, stack, ret, tuple()))
            continue
        if op == 0x1F:
            queue.append((fallthrough, stack, ret, timed_targets))
            if ret is not None:
                queue.append((int(ret), stack, ret, tuple()))
            continue
        if op == 0x37:
            queue.append((fallthrough, stack, fallthrough, timed_targets))
            continue
        if op == 0x38:
            if ret is not None:
                queue.append((int(ret), stack, ret, tuple()))
            continue
        if op == 0x3F:
            idx = struct.unpack_from("<H", code, int(pc) + 1)[0]
            queue.append((fallthrough, stack, ret, timed_targets))
            if 0 <= idx < len(scene.label_offsets):
                queue.append((int(scene.label_offsets[idx]), stack, ret, tuple()))
            continue
        queue.append((fallthrough, stack, ret, timed_targets))

    return sorted(visited_events)


def _rewrite_scene_code(
    *,
    scene,
    source_code: bytes,
    rewrite_slices: Sequence[Dict[str, object]],
    file_name: str,
) -> Tuple[bytes, List[int]]:
    if not rewrite_slices:
        return bytes(source_code), [int(offset) for offset in scene.label_offsets]

    slices = sorted(
        (
            {
                "start": int(item["start"]),
                "end": int(item["end"]),
                "replacement": bytes(item["replacement"]),
                "offset_map": {int(src): int(dst) for src, dst in dict(item.get("offset_map") or {}).items()},
                "preserved_spans": [
                    {
                        "src_start": int(span.get("src_start", 0)),
                        "src_end": int(span.get("src_end", 0)),
                        "dst_start": int(span.get("dst_start", 0)),
                    }
                    for span in list(item.get("preserved_spans") or [])
                    if isinstance(span, dict)
                ],
                "payload_offset": item.get("payload_offset"),
                "allow_inside_anchor_fallback": bool(item.get("allow_inside_anchor_fallback")),
            }
            for item in rewrite_slices
        ),
        key=lambda item: int(item["start"]),
    )

    cursor = 0
    valid_slices = []
    for slice_info in slices:
        start = int(slice_info["start"])
        end = int(slice_info["end"])
        if start < cursor or not (0 <= start < end <= int(scene.code_end)):
            payload_offset = slice_info.get("payload_offset")
            fail(
                file_name,
                f"layout rewrite slice 非法: [0x{start:X},0x{end:X}) overlaps/out-of-bounds",
                offset=payload_offset if isinstance(payload_offset, int) else None,
            )
        valid_slices.append(slice_info)
        cursor = end
    slices = valid_slices

    new_code = bytearray()
    cursor = 0
    for slice_info in slices:
        start = int(slice_info["start"])
        end = int(slice_info["end"])
        new_code.extend(source_code[cursor:start])
        new_code.extend(bytes(slice_info["replacement"]))
        cursor = end
    new_code.extend(source_code[cursor:int(scene.code_end)])

    relocated_labels = [
        _relocate_code_offset(
            int(offset),
            slices,
            file_name=file_name,
            context="label offset",
            payload_offset=None,
        )
        for offset in scene.label_offsets
    ]

    reachable_event_starts = sorted(_collect_reachable_event_starts_for_rewrite(scene))
    reachable_event_start_set = {int(start) for start in reachable_event_starts}
    overlap_trampolines: Dict[int, int] = {}
    overlap_trampoline_slots_by_old_dest: Dict[int, int] = {}
    shared_jump_pairs_16_to_15: Dict[int, int] = {}
    shared_jump_pairs_15_to_16: Dict[int, int] = {}

    for pc in reachable_event_starts:
        if int(source_code[int(pc)]) != 0x16:
            continue
        length = opcode_length(source_code, int(pc), scene.code_end)
        if int(length or 0) != 6:
            continue
        paired_pc = int(pc) + 1
        if paired_pc not in reachable_event_start_set:
            continue
        if not (0 <= paired_pc + 5 <= int(scene.code_end)):
            continue
        if int(source_code[paired_pc]) != 0x15:
            continue
        shared_jump_pairs_16_to_15[int(pc)] = paired_pc
        shared_jump_pairs_15_to_16[paired_pc] = int(pc)

    def request_overlap_trampoline(
        *,
        opcode_pc: int,
        opcode_len: int,
        original_dest: int,
        relocated_dest: int,
    ) -> Optional[int]:
        if int(original_dest) == int(relocated_dest):
            return None
        overlapping_starts = [
            int(start)
            for start in reachable_event_starts
            if int(opcode_pc) < int(start) < int(opcode_pc) + int(opcode_len)
        ]
        if not overlapping_starts:
            return None
        unsafe_overlaps: List[int] = []
        for overlap_start in overlapping_starts:
            overlap_op = int(source_code[int(overlap_start)])
            overlap_len = opcode_length(source_code, int(overlap_start), scene.code_end)
            if overlap_op == 0x18 and int(overlap_len or 0) == 5:
                continue
            unsafe_overlaps.append(int(overlap_start))
        if not unsafe_overlaps:
            return None
        existing_slot = overlap_trampoline_slots_by_old_dest.get(int(original_dest))
        if existing_slot is not None:
            existing_dest = overlap_trampolines.get(int(existing_slot))
            if existing_dest is None or int(existing_dest) != int(relocated_dest):
                fail(
                    file_name,
                    "overlap raw jump 的旧目标对应多个不同的新目标",
                    offset=opcode_pc,
                )
            return int(existing_slot)
        trampoline_slot = len(new_code)
        new_code.extend(b"\x00" * 6)
        overlap_trampoline_slots_by_old_dest[int(original_dest)] = int(trampoline_slot)
        overlap_trampolines[int(trampoline_slot)] = int(relocated_dest)
        return int(trampoline_slot)

    for pc in reachable_event_starts:
        if not (0 <= int(pc) < int(scene.code_end)):
            continue
        opcode = int(source_code[int(pc)])
        if opcode not in (0x15, 0x16):
            continue

        length = opcode_length(source_code, int(pc), scene.code_end)
        if length is None or int(pc) + int(length) > int(scene.code_end):
            fail(file_name, "scene code 重建后 raw jump opcode 长度校验失败", offset=pc)

        if opcode == 0x15:
            new_pc = _relocate_code_offset(
                int(pc),
                slices,
                file_name=file_name,
                context="opcode start",
                payload_offset=None,
                allow_deleted_inside_slice=True,
            )
            if new_pc is None:
                continue
            dest = int(struct.unpack_from("<I", source_code, int(pc) + 1)[0])
            if not (0 <= int(dest) < int(scene.code_end)):
                continue
            new_dest = _relocate_code_offset(
                dest,
                slices,
                file_name=file_name,
                context="0x15 jump",
                payload_offset=pc,
            )
            trampoline_slot = request_overlap_trampoline(
                opcode_pc=int(pc),
                opcode_len=int(length),
                original_dest=int(dest),
                relocated_dest=int(new_dest),
            )
            if trampoline_slot is not None:
                struct.pack_into("<I", new_code, new_pc + 1, int(trampoline_slot))
                continue
            struct.pack_into("<I", new_code, new_pc + 1, int(new_dest))
        elif opcode == 0x16 and int(length) == 6:
            paired_pc = shared_jump_pairs_16_to_15.get(int(pc))
            if paired_pc is not None:
                paired_new_pc = _relocate_code_offset(
                    int(paired_pc),
                    slices,
                    file_name=file_name,
                    context="paired 0x15 opcode start",
                    payload_offset=None,
                    allow_deleted_inside_slice=True,
                )
                new_pc = _relocate_code_offset(
                    int(pc),
                    slices,
                    file_name=file_name,
                    context="opcode start",
                    payload_offset=None,
                    allow_deleted_inside_slice=True,
                )
                if (
                    paired_new_pc is not None
                    and new_pc is not None
                    and int(new_pc) + 1 == int(paired_new_pc)
                ):
                    # Reachable 0x16 len=6 + inner reachable 0x15 share the same dword target.
                    # Patch the shared dword once from the 0x15 path to avoid double-writing it.
                    continue
            new_pc = _relocate_code_offset(
                int(pc),
                slices,
                file_name=file_name,
                context="opcode start",
                payload_offset=None,
                allow_deleted_inside_slice=True,
            )
            if new_pc is None:
                continue
            dest = int(struct.unpack_from("<I", source_code, int(pc) + 2)[0])
            if not (0 <= int(dest) < int(scene.code_end)):
                continue
            new_dest = _relocate_code_offset(
                dest,
                slices,
                file_name=file_name,
                context="0x16 jump",
                payload_offset=pc,
            )
            trampoline_slot = request_overlap_trampoline(
                opcode_pc=int(pc),
                opcode_len=int(length),
                original_dest=int(dest),
                relocated_dest=int(new_dest),
            )
            if trampoline_slot is not None:
                struct.pack_into("<I", new_code, new_pc + 2, int(trampoline_slot))
                continue
            struct.pack_into("<I", new_code, new_pc + 2, int(new_dest))

    for trampoline_slot, relocated_dest in overlap_trampolines.items():
        new_code[int(trampoline_slot)] = 0x16
        new_code[int(trampoline_slot) + 1] = 0x15
        struct.pack_into("<I", new_code, int(trampoline_slot) + 2, int(relocated_dest))

    return bytes(new_code), relocated_labels


def reading_wait_marker(page):
    """Keep the approved ten-second interval for native dialogue page waits."""
    raw = bytes.fromhex(str(page.get("marker_raw") or ""))
    if (page.get("stop_opcode") == 4 and page.get("stop_kind") == "timed_wait"
            and len(raw) == 5 and raw[0] == 0x04):
        return b"\x04\x00\x00\x80\xbf"
    return raw


def is_timed_wait(raw):
    """A finite nonnegative native delay is an execution boundary, not reader pagination."""
    if len(raw) != 5 or raw[0] != 0x04:
        return False
    duration = struct.unpack_from("<f", raw, 1)[0]
    return math.isfinite(duration) and duration >= 0


def paragraph_source_segments(*, scene, payload, file_name):
    """Group native pages only across gaps without execution/speaker boundaries."""
    parts = ensure_parts(payload)
    pages = normalize_original_pages(payload.get("pages"), len(parts))
    if not pages or not payload.get("pages"):
        fail(file_name, "段落缺少原始页面结构", offset=payload.get("offset"))
    scene_start = int(scene.scene_start) + int(scene.nested_offset)
    units = []
    for page in pages:
        first = int(page["part_start"])
        end_part = first + int(page["part_count"])
        previous_end = None
        body_seen = False
        for part_index in range(first, end_part):
            part = parts[part_index]
            position = _part_bytecode_rel_start(part, scene_start)
            cut = body_seen and _part_is_explicit_speaker(part)
            cursor = previous_end
            while body_seen and cursor is not None and cursor < position and not cut:
                event = decode_opcode_event(scene, cursor)
                if event is None or cursor + event.length > position:
                    fail(file_name, "正文引用之间不是原VM指令边界", offset=scene_start + cursor)
                cut = event.opcode not in {0x04, 0x23} or is_timed_wait(event.raw)
                cursor += event.length
            if cut:
                units.append({"part_start": first, "part_count": part_index - first,
                              "marker_offset": scene_start + previous_end, "marker_length": 0,
                              "marker_raw": "", "stop_opcode": None, "native_continuation": True})
                first = part_index
                body_seen = False
            body_seen |= not _part_is_explicit_speaker(part)
            previous_end = _part_bytecode_rel_end(part, scene_start)
        terminal = {**page, "part_start": first, "part_count": end_part - first}
        tail_end = _part_bytecode_rel_end(parts[end_part - 1], scene_start)
        marker_offset = terminal.get("marker_offset")
        if isinstance(marker_offset, int) and tail_end < marker_offset - scene_start:
            following = decode_opcode_event(scene, tail_end)
            if following is not None and following.opcode == 0x02:
                # Extracted fragments can share a later wait. The next body
                # instruction belongs to another fragment and must execute once.
                terminal.update(marker_offset=scene_start + tail_end, marker_length=0,
                                marker_raw="", stop_opcode=None, native_continuation=True)
        units.append(terminal)
    pages = units
    groups = []
    first_page = 0
    boundary_before = []
    for index, page in enumerate(pages):
        if index == 0:
            continue
        previous = pages[index - 1]
        marker = bytes.fromhex(str(previous.get("marker_raw") or ""))
        if (not marker and not previous.get("native_continuation")) or not isinstance(previous.get("marker_offset"), int):
            fail(file_name, "段落页面缺少可核对终止指令", offset=payload.get("offset"))
        cursor = int(previous["marker_offset"]) - scene_start + len(marker)
        gap_start = cursor
        page_parts = parts[page["part_start"]:page["part_start"] + page["part_count"]]
        end = min(_part_bytecode_rel_start(part, scene_start) for part in page_parts)
        events = []
        execution_gap = False
        while cursor < end:
            event = decode_opcode_event(scene, cursor)
            if event is None or event.rel_offset != cursor or cursor + event.length > end:
                fail(file_name, "页面间指令无法按原VM边界解析", offset=scene_start + cursor)
            events.append({"offset": scene_start + cursor, "opcode": event.opcode, "raw": event.raw.hex()})
            cursor += event.length
            if event.opcode not in {0x04, 0x23} or is_timed_wait(event.raw):
                # Branch operands can overlap other native entry points. The
                # scene relocator already walks those paths; linear decoding
                # beyond this boundary would interpret operands as opcodes.
                execution_gap = True
                events = []
                for position in _collect_reachable_event_starts_for_rewrite(scene):
                    if gap_start <= position < end:
                        native = decode_opcode_event(scene, position)
                        if native is not None:
                            events.append({"offset": scene_start + position, "opcode": native.opcode,
                                           "raw": native.raw.hex()})
                events.sort(key=lambda event: event["offset"])
                break
        if not execution_gap and cursor != end:
            fail(file_name, "页面间指令范围重叠", offset=scene_start + cursor)
        if cursor > end or gap_start > end:
            fail(file_name, "页面间指令范围重叠", offset=scene_start + cursor)
        if is_timed_wait(marker):
            events.insert(0, {"offset": previous["marker_offset"], "opcode": marker[0], "raw": marker.hex()})
        if not marker or marker[0] not in {0x04, 0x23} or is_timed_wait(marker) or execution_gap or any(
            _part_is_explicit_speaker(part) for part in page_parts
        ):
            groups.append((first_page, index, boundary_before))
            first_page = index
            boundary_before = events
    groups.append((first_page, len(pages), boundary_before))
    result = []
    for index, (first, end, before) in enumerate(groups):
        start_part = int(pages[first]["part_start"])
        end_part = int(pages[end - 1]["part_start"]) + int(pages[end - 1]["part_count"])
        result.append({"segment": index, "part_start": start_part, "part_count": end_part - start_part,
                       "pages": [dict(page) for page in pages[first:end]], "boundary_before": before})
    return result


def _plan_segmented_paragraph_rewrite(*, scene, payload, parts, original_lines, char_map,
                                      replacements, clone_records, clone_aliases, file_name):
    source = paragraph_source_segments(scene=scene, payload=payload, file_name=file_name)
    requested = payload["paragraph_segments"]
    if not isinstance(requested, list) or len(requested) != len(source):
        fail(file_name, "必须按原执行顺序提供所有段落分段", offset=payload.get("offset"))
    slices = []
    for descriptor, target in zip(source, requested):
        if (not isinstance(target, dict) or set(target) != {"segment", "pages"}
                or type(target["segment"]) is not int or target["segment"] != descriptor["segment"]):
            fail(file_name, "段落分段身份或顺序不一致", offset=payload.get("offset"))
        first = descriptor["part_start"]
        end = first + descriptor["part_count"]
        child = dict(payload)
        child["parts"] = parts[first:end]
        child["pages"] = [{**page, "part_start": page["part_start"] - first} for page in descriptor["pages"]]
        child["paragraph_pages"] = target["pages"]
        child["offset"] = int(scene.scene_start) + int(scene.nested_offset) + _part_bytecode_rel_start(
            child["parts"][0], int(scene.scene_start) + int(scene.nested_offset)
        )
        if isinstance(payload.get("part_lines"), list):
            child["part_lines"] = payload["part_lines"][first:end]
        if payload.get("native_jp_lines") is not None:
            child["native_jp_lines"] = payload["native_jp_lines"][first:end]
        slices.append(_plan_paragraph_rewrite(
            scene=scene, payload=child, parts=parts[first:end], original_lines=original_lines[first:end],
            char_map=char_map, replacements=replacements, clone_records=clone_records,
            clone_aliases=clone_aliases, file_name=file_name,
        ))
    return slices


def _plan_paragraph_rewrite(
    *, scene, payload, parts, original_lines, char_map, replacements,
    clone_records, clone_aliases, file_name,
):
    """Lay out one uninterrupted paragraph through the existing scene relocator.

    Only body references and existing waits may be removed from its interior.
    Setup and speaker events execute once; labels into deleted instructions are
    rejected by _rewrite_scene_code, rather than guessed onto a new page.
    """
    pages = payload["paragraph_pages"]
    if not isinstance(pages, list) or not pages or any(
        not isinstance(page, list) or not page
        or any(not isinstance(line, str) for line in page)
        for page in pages
    ):
        fail(file_name, "paragraph_pages 必须是非空页面／行数组", offset=payload.get("offset"))
    scene_abs_start = int(scene.scene_start) + int(scene.nested_offset)
    refs = {_part_bytecode_rel_start(part, scene_abs_start): (part, line)
            for part, line in zip(parts, original_lines)}
    body_refs = [(part, line) for part, line in zip(parts, original_lines)
                 if not _part_is_explicit_speaker(part)]
    if not body_refs:
        fail(file_name, "段落缺少正文引用", offset=payload.get("offset"))
    body_opcodes = {_part_opcode_raw(part)[0] for part, _ in body_refs}
    if body_opcodes != {0x02}:
        fail(file_name, "该文本通道需要保留独立槽位，不能作为连续段落", offset=payload.get("offset"))
    body_opcode = next(iter(body_opcodes))
    start = min(refs)
    first_body = min(_part_bytecode_rel_start(part, scene_abs_start) for part, _ in body_refs)
    last_body_end = max(_part_bytecode_rel_end(part, scene_abs_start) for part, _ in body_refs)
    source_segments = paragraph_source_segments(scene=scene, payload=payload, file_name=file_name)
    if len(source_segments) != 1:
        fail(file_name, "段落须按原生执行边界分段", offset=payload.get("offset"))
    source_pages = source_segments[0]["pages"]
    last_page = source_pages[-1]
    marker_start = int(last_page.get("marker_offset", scene_abs_start + last_body_end)) - scene_abs_start
    marker_raw = bytes.fromhex(str(last_page.get("marker_raw") or ""))
    if marker_start < last_body_end:
        fail(file_name, "段落终止指令与正文重叠", offset=payload.get("offset"))
    end = marker_start + len(marker_raw)
    cursor = first_body
    while cursor < last_body_end:
        event = decode_opcode_event(scene, cursor)
        if event is None or event.rel_offset != cursor or event.length <= 0:
            fail(file_name, "段落内部存在未识别指令", offset=scene_abs_start + cursor)
        if cursor in refs:
            if event.opcode != body_opcode:
                fail(file_name, "段落内部切换说话人，须按说话人拆段", offset=scene_abs_start + cursor)
        elif event.opcode not in {0x04, 0x23} or is_timed_wait(event.raw):
            fail(file_name, "段落内部包含演出／逻辑指令，须保留该边界", offset=scene_abs_start + cursor)
        cursor += event.length
    if cursor != last_body_end:
        fail(file_name, "段落页面边界不是指令起点", offset=payload.get("offset"))
    trailing_code = bytes(scene.code[last_body_end:marker_start])
    continuation = bool(last_page.get("native_continuation"))
    if not continuation and (not marker_raw or marker_raw[0] not in {0x00, 0x03, 0x04, 0x46}):
        fail(file_name, "段落终止类型不能自动分页", offset=payload.get("offset"))
    if not continuation:
        terminal = decode_opcode_event(scene, marker_start)
        if terminal is None or terminal.raw != marker_raw:
            fail(file_name, "段落终止指令与原VM不一致", offset=scene_abs_start + marker_start)
        marker_raw = reading_wait_marker(last_page)
    control_lines = payload.get("native_jp_lines")
    if control_lines is not None:
        if not isinstance(control_lines, list) or len(control_lines) != len(parts):
            fail(file_name, "原生日文控制符来源与part身份不一致", offset=payload.get("offset"))
        original_signature = tuple(value for part, line in zip(parts, control_lines)
                                   if not _part_is_explicit_speaker(part)
                                   for value in _control_signature(line))
    else:
        original_signature = tuple(value for _, line in body_refs for value in _control_signature(line))
    requested_signature = tuple(value for page in pages for line in page for value in _control_signature(line))
    requested_control = requested_signature.count(0xFF)
    if original_signature != requested_signature:
        fail(file_name, "段落变量／控制符数量变化", offset=payload.get("offset"))
    offset_to_local_idx = {record.absolute_offset: record.local_idx for record in scene.local_texts}
    templates = [(_part_opcode_raw(part), line, offset_to_local_idx[int(part["offset"])])
                 for part, line in body_refs]
    prefix = bytearray(scene.code[start:first_body])
    translated_slots = payload.get("part_lines")
    for index, (part, original_line) in enumerate(zip(parts, original_lines)):
        if not _part_is_explicit_speaker(part) or is_nonvisible_storage_line(original_line):
            continue
        rel = _part_bytecode_rel_start(part, scene_abs_start)
        if rel >= first_body:
            fail(file_name, "段落内部切换说话人", offset=payload.get("offset"))
        translated_line = translated_slots[index] if isinstance(translated_slots, list) else original_line
        encoded = encode_exact_line(translated_line, original_line, char_map)
        assigned = _assign_local_idx_for_encoded(
            scene=scene, local_idx=offset_to_local_idx[int(part["offset"])], encoded=encoded,
            replacements=replacements, clone_records=clone_records, clone_aliases=clone_aliases,
        )
        raw = _patch_opcode_local_idx(_part_opcode_raw(part), assigned)
        prefix[rel - start:rel - start + len(raw)] = raw
    replacement = bytearray(prefix)
    offset_map = {start: 0, first_body: len(prefix)}
    preserved_spans = []
    if prefix:
        preserved_spans.append({"src_start": start, "src_end": first_body, "dst_start": 0})
    page_stop = b"\x04\x00\x00\x80\xbf"
    if marker_raw and marker_raw[0] == 0x04 and not is_timed_wait(marker_raw):
        page_stop = marker_raw
    encoded_control = 0
    for page_index, page in enumerate(pages):
        # Main-body opcode 02 has only a local index operand. Control tokens
        # have already been verified across the whole paragraph, so new line
        # boundaries need not inherit the old record's token placement.
        opcode_raw, _original_line, local_idx = templates[0]
        for line in page:
            encoded = encode_tokens(tokenize_line(line), char_map)
            if len(encoded) + 1 > 0xFF:
                fail(file_name, "单条文本记录超过255字节", offset=payload.get("offset"))
            encoded_control += encoded.count(0xFF)
            assigned = _assign_local_idx_for_encoded(
                scene=scene, local_idx=local_idx, encoded=encoded,
                replacements=replacements, clone_records=clone_records,
                clone_aliases=clone_aliases,
            )
            replacement.extend(_patch_opcode_local_idx(opcode_raw, assigned))
        if page_index == len(pages) - 1:
            if trailing_code:
                preserved_spans.append({"src_start": last_body_end, "src_end": marker_start,
                                        "dst_start": len(replacement)})
                replacement.extend(trailing_code)
            offset_map[marker_start] = len(replacement)
            if marker_raw and marker_raw[0] == 0x03:
                replacement.extend(page_stop)
            replacement.extend(marker_raw)
        else:
            replacement.extend(page_stop)
    if encoded_control != requested_control:
        fail(file_name, "编码改变了段落控制符数量", offset=payload.get("offset"))
    if len(replacement) < end - start:
        replacement.extend(_build_vm_noop_padding(end - start - len(replacement)))
    return {"start": start, "end": end, "replacement": bytes(replacement),
            "offset_map": offset_map, "preserved_spans": preserved_spans,
            "payload_offset": payload.get("offset")}


def _rewrite_single_scene_nested_v2(
    *,
    scene,
    entries: Sequence[Dict[str, object]],
    char_map: Dict[str, bytes],
    file_name: str,
) -> bytes:
    offset_to_local_idx = {record.absolute_offset: record.local_idx for record in scene.local_texts}
    preserve_existing_records = any(payload.get("preserve_existing_records") for payload in entries)
    replacements: Dict[int, bytes] = (
        {record.local_idx: bytes(record.raw) for record in scene.local_texts}
        if preserve_existing_records else {}
    )
    clone_records: List[bytes] = []
    clone_aliases: Dict[Tuple[int, bytes], int] = {}
    patch_tasks: List[Tuple[dict, int]] = []
    rewrite_slices: List[Dict[str, object]] = []
    setup_replacements: Dict[int, bytes] = {}
    scene_abs_start = int(scene.scene_start) + int(scene.nested_offset)

    for payload in entries:
        # Skip entries with no JP text (empty side of "different" entries)
        jp_text = str(payload.get("text") or "").strip()
        cn_text = str(payload.get("chinese") or "").strip()
        if not jp_text and not cn_text:
            continue
        parts = ensure_parts(payload)
        if not parts:
            fail(file_name, "条目缺少 parts", offset=payload.get("offset"))
        original_lines = resolve_original_part_lines(payload, parts)
        if len(original_lines) != len(parts):
            fail(file_name, "无法恢复原始 part 文本", offset=payload.get("offset"))

        if payload.get("paragraph_segments") is not None:
            if payload.get("paragraph_pages") is not None:
                fail(file_name, "整段排版和执行边界分段排版不能同时提交", offset=payload.get("offset"))
            rewrite_slices.extend(_plan_segmented_paragraph_rewrite(
                scene=scene, payload=payload, parts=parts, original_lines=original_lines,
                char_map=char_map, replacements=replacements, clone_records=clone_records,
                clone_aliases=clone_aliases, file_name=file_name,
            ))
            continue
        if payload.get("paragraph_pages") is not None:
            slice_info = _plan_paragraph_rewrite(
                scene=scene, payload=payload, parts=parts,
                original_lines=original_lines, char_map=char_map,
                replacements=replacements, clone_records=clone_records,
                clone_aliases=clone_aliases, file_name=file_name,
            )
            rewrite_slices.append(slice_info)
            continue

        raw_part_lines = payload.get("part_lines")
        if isinstance(raw_part_lines, list) and len(raw_part_lines) != len(parts):
            original_pages = normalize_original_pages(
                payload.get("pages"),
                len(parts),
                collapse_compatible_pages=True,
            )
            if original_pages is None:
                fail(file_name, "原始 pages 结构非法，无法执行 layout rewrite", offset=payload.get("offset"))
            translated_payloads, error = resolve_rewrite_payload_lines(payload)
            if translated_payloads is None:
                fail(file_name, error, offset=payload.get("offset"))
            target_pages, error = normalize_target_pages(
                payload.get("target_pages"),
                expected_part_count=len(translated_payloads),
            )
            if target_pages is None:
                fail(file_name, error, offset=payload.get("offset"))
            try:
                entry_slices = _plan_layout_rewrite_slices(
                    scene=scene,
                    scene_abs_start=scene_abs_start,
                    payload=payload,
                    parts=parts,
                    original_lines=original_lines,
                    original_pages=original_pages,
                    translated_payloads=translated_payloads,
                    target_pages=target_pages,
                    char_map=char_map,
                    offset_to_local_idx=offset_to_local_idx,
                    replacements=replacements,
                    clone_records=clone_records,
                    clone_aliases=clone_aliases,
                    file_name=file_name,
                )
            except WritebackFailure as _layout_exc:
                fail(file_name, f"layout rewrite 失败: {_layout_exc}", offset=payload.get("offset"))
            for slice_info in entry_slices:
                slice_info["payload_offset"] = payload.get("offset")
                rewrite_slices.append(slice_info)
            residue_lines = expand_payloads_to_all_original_parts_for_residue(
                parts,
                original_lines,
                translated_payloads,
            )
            if residue_lines is not None:
                _seed_residue_replacements_from_part_lines(
                    parts=parts,
                    original_lines=original_lines,
                    translated_lines=residue_lines,
                    offset_to_local_idx=offset_to_local_idx,
                    replacements=replacements,
                    char_map=char_map,
                )
            continue

        translated_lines, error = resolve_translated_part_lines(payload, parts, original_lines)
        if translated_lines is None:
            fail(
                file_name,
                error or "source 行数与原始 slot 数不一致；需要显式 part_lines，禁止隐式拆槽",
                offset=payload.get("offset"),
            )

        for part, original_line, translated_line in zip(parts, original_lines, translated_lines):
            part_offset = part.get("offset")
            if not isinstance(part_offset, int):
                fail(file_name, "part 缺少 offset", offset=payload.get("offset"))
            local_idx = offset_to_local_idx.get(int(part_offset))
            if local_idx is None:
                fail(file_name, "part offset 未命中 local_text", offset=int(part_offset))
            try:
                encoded = encode_exact_line(
                    translated_line,
                    original_line,
                    char_map,
                )
            except WritebackFailure as exc:
                fail(file_name, str(exc), offset=int(part_offset))
            except KeyError as exc:
                missing_char = exc.args[0] if exc.args else "?"
                fail(
                    file_name,
                    f"缺少字符映射: {missing_char!r} | translated_line={translated_line!r}",
                    offset=int(part_offset),
                )

            is_setup_ref = str(part.get("bytecode_kind") or "") == "setup_ref"
            if is_setup_ref:
                existing_encoded = setup_replacements.get(int(local_idx))
                if existing_encoded is not None and existing_encoded != encoded:
                    fail(
                        file_name,
                        "setup_ref local_text 存在冲突翻译，无法安全 clone",
                        offset=int(part_offset),
                    )
                setup_replacements[int(local_idx)] = encoded
                replacements[int(local_idx)] = encoded
                assigned_local_idx = int(local_idx)
            else:
                assigned_local_idx = _assign_local_idx_for_encoded(
                    scene=scene,
                    local_idx=int(local_idx),
                    encoded=encoded,
                    replacements=replacements,
                    clone_records=clone_records,
                    clone_aliases=clone_aliases,
                )

            if int(assigned_local_idx) != int(local_idx):
                patch_tasks.append((dict(part), int(assigned_local_idx)))

    source_code = bytearray(scene.code)
    # The paragraph and layout paths normalize their own terminal markers.
    # Unchanged slot counts use this same policy before the original relocator.
    for payload in entries:
        for page in payload.get("pages", []):
            raw = bytes.fromhex(str(page.get("marker_raw") or ""))
            normalized = reading_wait_marker(page)
            if normalized == raw:
                continue
            position = int(page["marker_offset"]) - scene_abs_start
            if any(int(info["start"]) <= position < int(info["end"]) for info in rewrite_slices):
                continue
            if bytes(source_code[position:position + len(raw)]) != raw:
                fail(file_name, "阅读页等待与原VM不一致", offset=page["marker_offset"])
            source_code[position:position + len(raw)] = normalized
    reachable_event_spans: List[Tuple[int, int]] = []
    for event_start in _collect_reachable_event_starts_for_rewrite(scene):
        event = decode_opcode_event(scene, int(event_start))
        if event is None or event.length <= 0:
            continue
        reachable_event_spans.append((int(event_start), int(event_start) + int(event.length)))
    for part, new_local_idx in patch_tasks:
        bytecode_offset = part.get("bytecode_offset")
        bytecode_length = part.get("bytecode_length")
        if (
            isinstance(bytecode_offset, int)
            and not isinstance(bytecode_offset, bool)
            and isinstance(bytecode_length, int)
            and not isinstance(bytecode_length, bool)
            and int(bytecode_length) > 0
        ):
            rel_offset = int(bytecode_offset) - int(scene_abs_start)
            rel_end = rel_offset + int(bytecode_length)
            overlaps_reachable = bool(_part_event_conflicts(part, scene_abs_start, reachable_event_spans))
            if overlaps_reachable:
                if preserve_existing_records:
                    fail(file_name, "实际文本引用落在另一条可达指令内部", offset=bytecode_offset)
                continue
        if not patch_mgp_event_local_idx_v2(source_code, scene_abs_start, part, new_local_idx):
            rel = _part_bytecode_rel_start(part, scene_abs_start)
            if not any(int(info["start"]) <= rel < int(info["end"]) for info in rewrite_slices):
                fail(file_name, "clone 引用重定向失败", offset=part.get("bytecode_offset"))

    rewritten_code, relocated_labels = _rewrite_scene_code(
        scene=scene,
        source_code=bytes(source_code),
        rewrite_slices=rewrite_slices,
        file_name=file_name,
    )

    records: List[bytes] = []
    for record in scene.local_texts:
        translated = replacements.get(record.local_idx)
        if translated is not None:
            records.append(translated)
        else:
            records.append(record.raw)
    records.extend(clone_records)

    meta_block = bytearray(scene.nested[scene.meta_start:scene.meta_end])
    for label_idx, label_offset in enumerate(relocated_labels):
        struct.pack_into("<I", meta_block, 4 + label_idx * 4, int(label_offset))

    new_text_block = _build_local_text_block(records, file_name=file_name)
    child_block = scene.nested[scene.child_table_start:scene.target_label_table_start]
    target_block = scene.nested[scene.target_label_table_start:]

    new_nested = bytearray()
    new_nested.extend(rewritten_code)
    new_nested.extend(meta_block)
    new_nested.extend(new_text_block)
    new_nested.extend(child_block)
    new_nested.extend(target_block)

    new_code_end = len(rewritten_code)
    new_text_start = new_code_end + len(meta_block)
    new_child_start = new_text_start + len(new_text_block)
    new_target_start = new_child_start + len(child_block)
    struct.pack_into("<4I", new_nested, 2, new_code_end, new_text_start, new_child_start, new_target_start)
    return bytes(new_nested)


def apply_exact_mgs(
    backup_path: Path,
    entries: List[Dict[str, object]],
    char_map: Dict[str, bytes],
) -> Tuple[bytes, int]:
    backup_data = backup_path.read_bytes()
    if any(payload.get("paragraph_pages") is not None or payload.get("paragraph_segments") is not None
           or payload.get("preserve_existing_records") for payload in entries):
        scenes = load_mgs_from_bytes(backup_data, scene_name=backup_path.stem)
        if len(scenes) != 1:
            fail(backup_path.name, "MGS scene 解析失败")
        rebuilt = _rewrite_single_scene_nested_v2(
            scene=scenes[0], entries=entries, char_map=char_map,
            file_name=backup_path.name,
        )
        if len(load_mgs_from_bytes(rebuilt, scene_name=backup_path.stem)) != 1:
            fail(backup_path.name, "MGS 段落重建后解析失败")
        return rebuilt, len(entries)
    try:
        from script_binary_helpers import (
            decode_entry_bytes as legacy_decode_entry_bytes,
            get_mgs_entry_context as legacy_get_mgs_entry_context,
            mgs_raw_chunk_has_vm_visible_text as legacy_mgs_raw_chunk_has_vm_visible_text,
            normalize_mgs_candidate_lines as legacy_normalize_mgs_candidate_lines,
            reflow_mgs_tokens as legacy_reflow_mgs_tokens,
            resolve_mgs_entry_indices as legacy_resolve_mgs_entry_indices,
            resolve_original_decode_map as legacy_resolve_original_decode_map,
            strip_control_markup as legacy_strip_control_markup,
            strip_matching_boundary_text as legacy_strip_matching_boundary_text,
        )
    except Exception as exc:
        fail(backup_path.name, f"MGS legacy writer 导入失败: {exc}")

    try:
        decode_map = legacy_resolve_original_decode_map(char_map)
        parsed_scenes = load_mgs_from_bytes(backup_data, scene_name=backup_path.stem)
        if len(parsed_scenes) != 1:
            if not entries:
                return backup_data, 0
            fail(backup_path.name, "MGS布局解析失败")
        scene = parsed_scenes[0]
        layout_entries: List[Dict[str, int]] = []
        decoded_entries: List[str] = []
        offset_to_index: Dict[int, int] = {}
        text_ff_count = 0
        for local_idx, record in enumerate(scene.local_texts):
            start = int(record.absolute_offset)
            end = start + len(record.raw)
            layout_entries.append({"offset": start, "end": end, "local_idx": int(local_idx)})
            decoded_entries.append(legacy_decode_entry_bytes(record.raw, decode_map))
            offset_to_index[start] = int(local_idx)
            text_ff_count += record.raw.count(0xFF)
        layout = {
            "entries": layout_entries,
            "entry_offsets": [entry["offset"] for entry in layout_entries],
            "decoded_entries": decoded_entries,
            "offset_to_index": offset_to_index,
            "text_start": int(scene.text_start),
            "s2": int(scene.child_table_start),
            "s3": int(scene.target_label_table_start),
            "text_ff_count": int(text_ff_count),
        }

        filtered_entries = list(entries)
        filtered_entries.sort(
            key=lambda payload: (
                int(payload.get("offset") or 0),
                0 if "\n" in str(payload.get("text") or "") else 1,
                -len(payload.get("parts") or []),
                -len(str(payload.get("text") or "")),
                str(payload.get("text") or ""),
            )
        )

        replacements: Dict[int, bytes] = {}
        clone_records: List[bytes] = []
        clone_aliases: Dict[Tuple[int, bytes], int] = {}
        patch_tasks: List[Tuple[dict, int]] = []
        applied = 0

        for payload in filtered_entries:
            raw_offset = payload.get("offset")
            raw_length = payload.get("length")
            if not isinstance(raw_offset, int) or not isinstance(raw_length, int):
                fail(backup_path.name, "MGS 条目缺少 offset/length")
            offset = int(raw_offset)
            orig_length = int(raw_length)
            orig_text = str(payload.get("text") or "")
            current_chinese = str(payload.get("chinese") or "")
            parts, preferred_part_lines = _filter_mgs_mapping_parts(payload)

            indices, map_error = legacy_resolve_mgs_entry_indices(
                offset,
                orig_length,
                orig_text,
                parts,
                layout,
                preferred_scene_entry_index=payload.get("scene_entry_index"),
            )
            if indices is None:
                fail(
                    backup_path.name,
                    map_error or "MGS条目映射失败",
                    offset=offset,
                )

            entry_orig_lines, entry_prefix, entry_suffix = legacy_get_mgs_entry_context(
                backup_data,
                offset,
                orig_length,
                indices,
                layout,
                decode_map,
            )
            translated_visible_lines = resolve_mgs_translated_visible_lines(payload)
            chinese = legacy_strip_matching_boundary_text(
                "\n".join(translated_visible_lines) if translated_visible_lines else current_chinese,
                entry_prefix,
                entry_suffix,
            )
            explicit_part_lines = isinstance(preferred_part_lines, list) and len(preferred_part_lines) == len(indices)
            if explicit_part_lines:
                raw_lines = [str(line or "") for line in preferred_part_lines]
            else:
                raw_lines = split_mgs_text_to_lines(chinese)
                if len(raw_lines) != len(indices):
                    fail(
                        backup_path.name,
                        "MGS 源行数与原始 slot 数不一致: "
                        f"{len(raw_lines)} -> {len(indices)}; "
                        f"raw_lines={raw_lines!r}; original_slots={entry_orig_lines!r}",
                        offset=offset,
                    )

            entry_tokens: List[List[Tuple[str, object]]]
            raw_direct_compatible = len(raw_lines) == len(indices)
            if raw_direct_compatible and not explicit_part_lines:
                for raw_line, original_line in zip(raw_lines, entry_orig_lines):
                    raw_signature = _control_signature(raw_line)
                    original_signature = _control_signature(original_line)
                    if raw_signature != original_signature:
                        fail(
                            backup_path.name,
                            "MGS 非显式 part_lines 的控制码布局不一致: "
                            f"{raw_signature!r} -> {original_signature!r}; "
                            f"raw_line={raw_line!r}; original_slot={original_line!r}",
                            offset=offset,
                        )
            entry_tokens = [tokenize_line(line) for line in raw_lines]

            entry_tokens = merge_original_control_tokens(
                "\n".join(entry_orig_lines),
                entry_tokens,
                preserve_visible_placeholder_positions=isinstance(preferred_part_lines, list),
            )

            try:
                encoded_entries = [encode_tokens(tokens, char_map) for tokens in entry_tokens]
            except KeyError as exc:
                fail(backup_path.name, f"缺失字符: {exc.args[0]}", offset=offset)

            orig_ff = 0
            for idx in indices:
                entry = layout["entries"][idx]
                orig_ff += backup_data[entry["offset"]:entry["end"]].count(0xFF)
            new_ff = sum(chunk.count(0xFF) for chunk in encoded_entries)
            if orig_ff != new_ff:
                fail(
                    backup_path.name,
                    f"控制码数量变化: {orig_ff} -> {new_ff}; "
                    f"raw_lines={raw_lines!r}; encoded={[chunk.hex() for chunk in encoded_entries]!r}",
                    offset=offset,
                )

            part_refs = parts if len(parts) == len(indices) else [{} for _ in indices]
            for part, idx, encoded in zip(part_refs, indices, encoded_entries):
                assigned_idx = clone_aliases.get((int(idx), encoded))
                if assigned_idx is None:
                    existing_encoded = replacements.get(int(idx))
                    if existing_encoded is None or existing_encoded == encoded:
                        replacements[int(idx)] = encoded
                        assigned_idx = int(idx)
                    else:
                        assigned_idx = len(scene.local_texts) + len(clone_records)
                        clone_records.append(encoded)
                    clone_aliases[(int(idx), encoded)] = int(assigned_idx)
                if int(assigned_idx) != int(idx):
                    patch_tasks.append((dict(part), int(assigned_idx)))
            applied += 1

        source_code = bytearray(scene.code)
        scene_abs_start = int(scene.scene_start) + int(scene.nested_offset)
        reachable_event_spans: List[Tuple[int, int]] = []
        known_part_starts = _collect_part_bytecode_starts(filtered_entries, scene_abs_start=scene_abs_start)
        for event_start in _collect_reachable_event_starts_for_rewrite(scene):
            event = decode_opcode_event(scene, int(event_start))
            if event is None or event.length <= 0:
                continue
            reachable_event_spans.append((int(event_start), int(event_start) + int(event.length)))
        for part, new_local_idx in patch_tasks:
            bytecode_offset = part.get("bytecode_offset")
            bytecode_length = part.get("bytecode_length")
            if (
                isinstance(bytecode_offset, int)
                and not isinstance(bytecode_offset, bool)
                and isinstance(bytecode_length, int)
                and not isinstance(bytecode_length, bool)
                and int(bytecode_length) > 0
            ):
                rel_offset = int(bytecode_offset) - int(scene_abs_start)
                rel_end = rel_offset + int(bytecode_length)
                overlaps_reachable = bool(_part_event_conflicts(part, scene_abs_start, reachable_event_spans))
                overlaps_part_start = any(
                    start != rel_offset and rel_offset < start < rel_end
                    for start in known_part_starts
                )
                if overlaps_reachable or overlaps_part_start:
                    continue
            if not patch_mgp_event_local_idx_v2(source_code, scene_abs_start, part, new_local_idx):
                fail(
                    backup_path.name,
                    "MGS clone repoint failed bytecode validation",
                    offset=part.get("bytecode_offset") if isinstance(part.get("bytecode_offset"), int) else None,
                )

        records: List[bytes] = []
        for idx, record in enumerate(scene.local_texts):
            records.append(replacements.get(idx, bytes(record.raw)))
        records.extend(clone_records)
        new_text_block = _build_local_text_block(records, file_name=backup_path.name)
        child_block = scene.nested[scene.child_table_start:scene.target_label_table_start]
        target_block = scene.nested[scene.target_label_table_start:]
        prefix = bytearray(backup_data[:scene.text_start])
        prefix[:scene.code_end] = source_code
        rebuilt = bytearray()
        rebuilt.extend(prefix)
        rebuilt.extend(new_text_block)
        rebuilt.extend(child_block)
        rebuilt.extend(target_block)

        new_s2 = int(scene.text_start) + len(new_text_block)
        new_s3 = new_s2 + len(child_block)
        struct.pack_into("<4I", rebuilt, 2, int(scene.code_end), int(scene.text_start), int(new_s2), int(new_s3))

        if (new_s2 - int(scene.text_start)) != len(new_text_block):
            fail(backup_path.name, "MGS重建后文本区长度校验失败")
        if sum(chunk.count(0xFF) for chunk in records) != layout["text_ff_count"]:
            fail(backup_path.name, "MGS重建后FF控制码总数变化")
        reparsed = load_mgs_from_bytes(bytes(rebuilt), scene_name=backup_path.stem)
        if len(reparsed) != 1:
            fail(backup_path.name, "MGS重建后scene解析失败")
        reparsed_scene = reparsed[0]
        if len(reparsed_scene.local_texts) != len(records):
            fail(
                backup_path.name,
                f"MGS重建后local_text数量不符合预期: {len(records)} -> {len(reparsed_scene.local_texts)}",
            )
        for idx, (old_record, new_record) in enumerate(zip(scene.local_texts, reparsed_scene.local_texts)):
            old_visible = legacy_mgs_raw_chunk_has_vm_visible_text(bytes(old_record.raw))
            new_visible = legacy_mgs_raw_chunk_has_vm_visible_text(bytes(new_record.raw))
            if old_visible == new_visible:
                continue
            old_text = legacy_decode_entry_bytes(bytes(old_record.raw), decode_map)
            new_text = legacy_decode_entry_bytes(bytes(new_record.raw), decode_map)
            fail(
                backup_path.name,
                "MGS顺序条目结构变化: "
                f"local_text#{idx} 0x{int(old_record.absolute_offset):X} {old_text!r} -> {new_text!r}",
            )
    except WritebackFailure as _exc:
        _LAYOUT_FAILURES.append({
            "file": backup_path.name, "error": str(_exc).split("\n")[-1],
        })
        fail(backup_path.name, f"MGS rewrite 失败: {str(_exc).split(chr(10))[-1]}")
    if len(rebuilt) < 0x16 or bytes(rebuilt[:2]) != b"\x7e\x00":
        fail(backup_path.name, "MGS 重建后文件头非法")
    return bytes(rebuilt), applied


def apply_exact_mgp(
    backup_path: Path,
    entries: List[Dict[str, object]],
    char_map: Dict[str, bytes],
) -> Tuple[bytes, int]:
    backup_data = backup_path.read_bytes()
    raw_scene_offsets = collect_mgp_scene_offsets(backup_data)
    raw_scene_slots = collect_mgp_scene_offset_slots(backup_data)
    scenes = load_mgp_from_bytes(backup_data)
    if (
        not raw_scene_offsets
        or not raw_scene_slots
        or len(scenes) != len(raw_scene_offsets)
        or len(raw_scene_slots) != len(raw_scene_offsets)
    ):
        fail(backup_path.name, "MGP scene 解析不完整")

    entries_by_scene: Dict[int, List[Dict[str, object]]] = {}
    for payload in entries:
        scene_idx = payload.get("scene_idx")
        if not isinstance(scene_idx, int):
            fail(backup_path.name, "MGP 条目缺少 scene_idx", offset=payload.get("offset"))
        entries_by_scene.setdefault(int(scene_idx), []).append(payload)

    rebuilt_scenes: List[bytes] = []
    for scene in scenes:
        scene_entries = entries_by_scene.get(int(scene.scene_idx), [])
        if scene_entries:
            try:
                rebuilt_nested = _rewrite_single_scene_nested_v2(
                    scene=scene,
                    entries=scene_entries,
                    char_map=char_map,
                    file_name=backup_path.name,
                )
            except WritebackFailure as _scene_exc:
                _LAYOUT_FAILURES.append({
                    "file": backup_path.name, "scene_idx": int(scene.scene_idx),
                    "error": str(_scene_exc).split("\n")[-1],
                })
                fail(
                    backup_path.name,
                    f"scene {scene.scene_idx} rewrite 失败: {str(_scene_exc).split(chr(10))[-1]}",
                )
        else:
            rebuilt_nested = bytes(scene.nested)
        rebuilt_scenes.append(scene.scene_prefix + rebuilt_nested)

    scene_count = struct.unpack_from("<H", backup_data, 2)[0]
    header_end = 4 + scene_count * 4
    rebuilt = bytearray(backup_data[:header_end])
    cursor = header_end
    for scene_idx, scene_bytes in enumerate(rebuilt_scenes):
        table_slot = raw_scene_slots[scene_idx]
        struct.pack_into("<I", rebuilt, 4 + int(table_slot) * 4, cursor)
        rebuilt.extend(scene_bytes)
        cursor += len(scene_bytes)

    rebuilt_scenes_check = load_mgp_from_bytes(bytes(rebuilt))
    if len(rebuilt_scenes_check) != len(scenes):
        fail(backup_path.name, "MGP 重建后 scene 数量变化")
    return bytes(rebuilt), len(entries)


def apply_json_to_script_v2(
    backup_path: Path,
    json_data: dict,
    char_map: Dict[str, bytes],
    *,
    is_scriptf: bool,
) -> Tuple[bytes, int]:
    entries = iter_side_entries(json_data, is_scriptf)
    if backup_path.suffix.lower() == ".mgs":
        return apply_exact_mgs(backup_path, entries, char_map)
    if backup_path.suffix.lower() == ".mgp":
        return apply_exact_mgp(
            backup_path,
            entries,
            char_map,
        )
    fail(backup_path.name, "不支持的脚本类型")


def apply_all_scripts_v2(
    *,
    translated_dir: Path,
    output_dir: Path,
    backup_media_dir: Path,
    dry_run: bool,
    tbl_path: Optional[Path],
    include_rl: bool,
) -> bool:
    resolved_tbl = resolve_tbl_file(tbl_path)
    if not resolved_tbl.exists():
        print(f"错误: TBL 文件不存在: {resolved_tbl}")
        return False
    if not translated_dir.exists():
        print(f"错误: 翻译目录不存在: {translated_dir}")
        return False
    if not backup_media_dir.exists():
        print(f"错误: 备份 Media 目录不存在: {backup_media_dir}")
        return False

    char_map = load_tbl(resolved_tbl)
    print(f"加载字符表: {resolved_tbl}")
    print(f"  字符数: {len(char_map)}")
    print("  策略: v2 独立 exact 写回，不调用旧 apply_json_to_script")

    json_files = collect_json_files(translated_dir, include_rl)
    if not json_files:
        print("错误: 没有可处理的 JSON")
        return False

    output_dir.mkdir(parents=True, exist_ok=True)
    script_out = output_dir / "Script"
    scriptf_out = output_dir / "Scriptf"
    script_out.mkdir(parents=True, exist_ok=True)
    scriptf_out.mkdir(parents=True, exist_ok=True)

    backup_script_dir = backup_media_dir / "Script"
    backup_scriptf_dir = backup_media_dir / "Scriptf"

    total_applied = 0
    for json_path in json_files:
        with open(json_path, "r", encoding="utf-8") as handle:
            json_data = json.load(handle)
        if not isinstance(json_data, dict):
            continue

        base_name = json_path.stem
        print(f"\n处理: {base_name}")

        backup_script = backup_script_dir / base_name
        if backup_script.exists():
            rebuilt, applied = apply_json_to_script_v2(backup_script, json_data, char_map, is_scriptf=False)
            total_applied += applied
            if not dry_run:
                (script_out / base_name).write_bytes(rebuilt)
            print(f"  Script: {applied}")

        backup_scriptf = backup_scriptf_dir / base_name
        if backup_scriptf.exists():
            rebuilt, applied = apply_json_to_script_v2(backup_scriptf, json_data, char_map, is_scriptf=True)
            total_applied += applied
            if not dry_run:
                (scriptf_out / base_name).write_bytes(rebuilt)
            print(f"  Scriptf: {applied}")

    print(f"\n总写回条目: {total_applied}")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="SMT NINE v2 独立 exact 写回")
    parser.add_argument("--translated-dir", type=Path, default=DEFAULT_TRANSLATED_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--backup-media-dir", type=Path, default=DEFAULT_BACKUP_MEDIA_DIR)
    parser.add_argument("--tbl", type=Path, default=None)
    parser.add_argument("--include-rl", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    apply_all_scripts_v2(
        translated_dir=args.translated_dir,
        output_dir=args.output_dir,
        backup_media_dir=args.backup_media_dir,
        dry_run=args.dry_run,
        tbl_path=args.tbl,
        include_rl=args.include_rl,
    )

    import json as _json
    fail_path = Path(__file__).parent / "review" / "_layout_failures.json"
    fail_path.parent.mkdir(exist_ok=True)
    fail_path.write_text(
        _json.dumps(_LAYOUT_FAILURES, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if _LAYOUT_FAILURES:
        print(f"\n[WARN] {len(_LAYOUT_FAILURES)} 条 layout rewrite 失败，已保存到 {fail_path}")


if __name__ == "__main__":
    main()



