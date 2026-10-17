# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import io
import json
import re
import sys
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Optional, Sequence


SCRIPT_DIR = Path(__file__).parent
CACHE_DIR = SCRIPT_DIR / "cache"
DEFAULT_CANONICAL_CACHE_PATH = CACHE_DIR / "translation_cache.json"
DEFAULT_EVE_CACHE_PATH = CACHE_DIR / "EVE_cache.json"
DEFAULT_RL_CACHE_PATH = CACHE_DIR / "RL_cache.json"
DEFAULT_TRANSLATED_DIR = SCRIPT_DIR / "translated"

_ZEN_SP = "\u3000"
_RL_LINE_WIDTH = 20
_CACHE_RECORD_STR_FIELDS = (
    "route",
    "speaker_cn",
    "body_cn",
)
_CACHE_RECORD_PART_LINE_FIELDS = (
    "part_lines",
    "script_part_lines",
    "scriptf_part_lines",
)
_OPTION_TEXT_FAMILIES = {"list_item_with_value", "list_weighted_text"}
_OPTION_NONVISIBLE_PLACEHOLDER_CHARS = {"⊇", "⊂"}


class CacheSyncFailure(RuntimeError):
    pass


def _normalize_cache_side(raw_side: object) -> Optional[str]:
    if not isinstance(raw_side, str):
        return None
    side = raw_side.strip().lower()
    if side not in {"common", "script", "scriptf"}:
        return None
    return side


def _normalize_ref(raw_ref: object) -> Optional[dict]:
    if not isinstance(raw_ref, dict):
        return None
    file_name = raw_ref.get("file")
    offset = raw_ref.get("offset")
    if not isinstance(file_name, str) or not file_name.strip():
        return None
    if not isinstance(offset, int) or isinstance(offset, bool):
        return None
    return {
        "file": file_name.strip(),
        "offset": int(offset),
    }


def _normalize_refs(raw_refs: object) -> List[dict]:
    if not isinstance(raw_refs, list):
        return []
    refs: List[dict] = []
    seen: set[tuple[str, int]] = set()
    for raw_ref in raw_refs:
        ref = _normalize_ref(raw_ref)
        if ref is None:
            continue
        key = (str(ref["file"]), int(ref["offset"]))
        if key in seen:
            continue
        seen.add(key)
        refs.append(ref)
    refs.sort(key=lambda item: (str(item["file"]).lower(), int(item["offset"])))
    return refs


def _normalize_ref_files(raw_refs: object) -> List[str]:
    if not isinstance(raw_refs, list):
        return []
    files: List[str] = []
    seen: set[str] = set()
    for raw_ref in raw_refs:
        if not isinstance(raw_ref, str):
            continue
        file_name = raw_ref.strip()
        if not file_name or file_name in seen:
            continue
        seen.add(file_name)
        files.append(file_name)
    files.sort(key=str.lower)
    return files


def _normalize_part_lines(raw_part_lines: object) -> Optional[List[str]]:
    if not isinstance(raw_part_lines, list):
        return None
    normalized: List[str] = []
    for raw_line in raw_part_lines:
        if raw_line is None:
            normalized.append("")
            continue
        line = str(raw_line)
        if "\n" in line or "\r" in line:
            return None
        normalized.append(line)
    return normalized


def _tokenize_option_line(line: str) -> List[tuple[str, object]]:
    tokens: List[tuple[str, object]] = []
    i = 0
    while i < len(line):
        if line[i] == "<" and i + 3 < len(line) and line[i + 3] == ">":
            hex_part = line[i + 1 : i + 3]
            if all(ch in "0123456789abcdefABCDEF" for ch in hex_part):
                tokens.append(("ctrl", int(hex_part, 16)))
                i += 4
                continue
        tokens.append(("char", line[i]))
        i += 1
    return tokens


def _option_visible_payload(line: str) -> str:
    payload: List[str] = []
    for kind, value in _tokenize_option_line(str(line or "")):
        if kind != "char":
            continue
        if str(value) in _OPTION_NONVISIBLE_PLACEHOLDER_CHARS:
            continue
        payload.append(str(value))
    return "".join(payload)


def _is_visible_placeholder_expansion_line(line: str) -> bool:
    tokens = _tokenize_option_line(str(line or ""))
    for idx, (kind, value) in enumerate(tokens):
        if kind != "ctrl" or int(value) != 0xFF:
            continue
        if idx + 1 >= len(tokens):
            continue
        next_kind, next_value = tokens[idx + 1]
        if next_kind == "ctrl" and 0x01 <= int(next_value) <= 0x0B:
            return True
    return False


def _is_pure_placeholder_expansion_line(line: str) -> bool:
    if not _is_visible_placeholder_expansion_line(line):
        return False
    return _option_visible_payload(line) == ""


def _is_nonvisible_option_line(line: str) -> bool:
    if _is_visible_placeholder_expansion_line(line):
        return False
    return _option_visible_payload(line) == ""


def _copy_cache_record_extras(raw_entry: dict) -> dict:
    extras: dict = {}
    for key in _CACHE_RECORD_STR_FIELDS:
        value = raw_entry.get(key)
        if isinstance(value, str):
            extras[key] = value
    for key in _CACHE_RECORD_PART_LINE_FIELDS:
        value = _normalize_part_lines(raw_entry.get(key))
        if value is not None:
            extras[key] = value
    return extras


def _normalize_cache_record_speaker_fields(record: dict) -> dict:
    normalized = dict(record)

    def _line_key(value: object) -> str:
        return str(value or "").replace("\u3000", " ").strip()

    cn = str(normalized.get("cn") or "")
    speaker_cn = str(normalized.get("speaker_cn") or "")
    body_cn = str(normalized.get("body_cn") or "")
    speaker_source = str(normalized.get("speaker_source") or "")
    speaker = str(normalized.get("speaker") or "")
    body_text = str(normalized.get("body_text") or "")

    if speaker_source != "inherited" and speaker and speaker_cn and body_text and body_cn:
        body_lines = body_cn.split("\n")
        jp_body_lines = body_text.split("\n")
        if (
            body_lines
            and jp_body_lines
            and _line_key(body_lines[0]) == _line_key(speaker_cn)
            and _line_key(jp_body_lines[0]) != _line_key(speaker)
        ):
            body_cn = "\n".join(body_lines[1:])

    if speaker_source == "inherited":
        if body_cn:
            cn = body_cn
    elif speaker_cn:
        cn = speaker_cn + ("\n" + body_cn if body_cn else "")
    elif body_cn:
        cn = body_cn

    normalized["cn"] = cn
    normalized["speaker_cn"] = speaker_cn
    normalized["body_cn"] = body_cn
    return normalized


def load_canonical_cache_records(cache_path: Path) -> Dict[str, List[dict]]:
    if not cache_path.exists():
        raise CacheSyncFailure(f"cache 文件不存在: {cache_path}")

    data = json.loads(cache_path.read_text(encoding="utf-8"))
    if isinstance(data, dict) and isinstance(data.get("entries"), list):
        raw_entries = data["entries"]
    elif isinstance(data, dict):
        raw_entries = [{"jp": jp, "cn": cn, "refs": []} for jp, cn in data.items()]
    else:
        raw_entries = []

    records: Dict[str, List[dict]] = {}
    for raw_entry in raw_entries:
        if not isinstance(raw_entry, dict):
            continue
        jp = raw_entry.get("jp")
        cn = raw_entry.get("cn")
        if not isinstance(jp, str) or not isinstance(cn, str):
            continue

        record = {
            "jp": jp,
            "cn": cn,
            "refs": _normalize_refs(raw_entry.get("refs")),
            "ref_files": _normalize_ref_files(raw_entry.get("refs")),
            **_copy_cache_record_extras(raw_entry),
        }
        file_name = raw_entry.get("file")
        if isinstance(file_name, str) and file_name.strip():
            record["file"] = file_name.strip()
        category = raw_entry.get("category")
        if isinstance(category, str) and category.strip():
            record["category"] = category.strip()
        side_name = _normalize_cache_side(raw_entry.get("side"))
        if side_name is not None:
            record["side"] = side_name
        offset = raw_entry.get("offset")
        if isinstance(offset, int) and not isinstance(offset, bool):
            record["offset"] = int(offset)
        for int_key in ("scene_idx", "scene_entry_index", "original_offset"):
            value = raw_entry.get(int_key)
            if isinstance(value, int) and not isinstance(value, bool):
                record[int_key] = int(value)
        route = _normalize_cache_side(raw_entry.get("route"))
        if route is not None:
            record["route"] = route
        elif isinstance(raw_entry.get("route"), str) and str(raw_entry.get("route") or "").strip():
            record["route"] = str(raw_entry.get("route") or "").strip().lower()
        record = _normalize_cache_record_speaker_fields(record)
        records.setdefault(jp, []).append(record)

    return records


def load_exact_cache_records(cache_path: Path) -> Dict[tuple[str, str, int], dict]:
    if not cache_path.exists():
        raise CacheSyncFailure(f"cache 文件不存在: {cache_path}")

    data = json.loads(cache_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        return {}

    records: Dict[tuple[str, str, int], dict] = {}
    for raw_entry in data["entries"]:
        if not isinstance(raw_entry, dict):
            continue
        file_name = raw_entry.get("file")
        side_name = _normalize_cache_side(raw_entry.get("side"))
        offset = raw_entry.get("offset")
        jp = raw_entry.get("jp")
        cn = raw_entry.get("cn")
        if not isinstance(file_name, str) or not file_name.strip():
            continue
        if side_name is None:
            continue
        if not isinstance(offset, int) or isinstance(offset, bool):
            continue
        if not isinstance(jp, str) or not isinstance(cn, str):
            continue
        record = {
            "file": file_name.strip(),
            "side": side_name,
            "offset": int(offset),
            "jp": jp,
            "cn": cn,
            **_copy_cache_record_extras(raw_entry),
        }
        route = _normalize_cache_side(raw_entry.get("route"))
        if route is not None:
            record["route"] = route
        elif isinstance(raw_entry.get("route"), str) and str(raw_entry.get("route") or "").strip():
            record["route"] = str(raw_entry.get("route") or "").strip().lower()
        record = _normalize_cache_record_speaker_fields(record)
        records[(record["file"], record["side"], record["offset"])] = record

    return records


def load_exact_text_cache_records(cache_path: Path) -> Dict[str, List[dict]]:
    if not cache_path.exists():
        raise CacheSyncFailure(f"cache 文件不存在: {cache_path}")

    data = json.loads(cache_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        return {}

    records: Dict[str, List[dict]] = {}
    for raw_entry in data["entries"]:
        if not isinstance(raw_entry, dict):
            continue
        file_name = raw_entry.get("file")
        side_name = _normalize_cache_side(raw_entry.get("side"))
        jp = raw_entry.get("jp")
        cn = raw_entry.get("cn")
        if not isinstance(file_name, str) or not file_name.strip():
            continue
        if side_name is None:
            continue
        if not isinstance(jp, str) or not isinstance(cn, str):
            continue
        record = {
            "file": file_name.strip(),
            "side": side_name,
            "jp": jp,
            "cn": cn,
            "route": side_name,
            **_copy_cache_record_extras(raw_entry),
        }
        route = _normalize_cache_side(raw_entry.get("route"))
        if route is not None:
            record["route"] = route
        record = _normalize_cache_record_speaker_fields(record)
        records.setdefault(jp, []).append(record)
    return records


def load_window_cache_records(cache_path: Path) -> Dict[tuple[str, str], dict]:
    if not cache_path.exists():
        raise CacheSyncFailure(f"cache 文件不存在: {cache_path}")

    data = json.loads(cache_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise CacheSyncFailure(f"window cache 结构非法: {cache_path}")
    if isinstance(data.get("entries"), list):
        return {}
    if not isinstance(data.get("entries"), dict):
        raise CacheSyncFailure(f"window cache 结构非法: {cache_path}")

    records: Dict[tuple[str, str], dict] = {}
    for jp, raw_entry in data["entries"].items():
        if not isinstance(raw_entry, dict):
            continue
        if not isinstance(jp, str):
            continue
        cn = raw_entry.get("cn")
        if not isinstance(cn, str):
            continue
        route = _normalize_cache_side(raw_entry.get("route"))
        if route is None:
            route = "common"
        record = {"jp": jp, "cn": cn, "route": route}
        for key, value in _copy_cache_record_extras(raw_entry).items():
            record[key] = value
        record = _normalize_cache_record_speaker_fields(record)
        records[(route, jp)] = record

    return records


def load_cache_bundle(
    *,
    canonical_cache_path: Path,
    eve_cache_path: Path,
    rl_cache_path: Path,
    claude_canonical_output_path: Sequence[Path] | Path,
    claude_eve_output_path: Sequence[Path] | Path,
    claude_rl_output_path: Sequence[Path] | Path,
) -> dict[str, object]:
    canonical = load_canonical_cache_records(canonical_cache_path)
    return {
        "canonical": canonical,
        "canonical_exact": load_exact_cache_records(canonical_cache_path),
        "eve": load_window_cache_records(eve_cache_path),
        "eve_exact": load_exact_cache_records(eve_cache_path),
        "eve_text": load_exact_text_cache_records(eve_cache_path),
        "eve_window_authority": {},
        "rl": load_window_cache_records(rl_cache_path),
        "rl_exact": load_exact_cache_records(rl_cache_path),
        "rl_text": load_exact_text_cache_records(rl_cache_path),
        "rl_window_authority": {},
        "reviewed": load_reviewed_output_bundle(
            canonical_output_path=claude_canonical_output_path,
            eve_output_path=claude_eve_output_path,
            rl_output_path=claude_rl_output_path,
        ),
    }


def _is_rl_style(jp_lines: List[str]) -> bool:
    return len(jp_lines) >= 2 and all(line.startswith(_ZEN_SP) for line in jp_lines)


def _apply_rl_padding(jp_text: str, cn_text: str) -> str:
    if _ZEN_SP not in jp_text:
        return cn_text
    jp_lines = jp_text.split("\n")
    if not _is_rl_style(jp_lines):
        return cn_text

    cn_lines = cn_text.split("\n")
    has_trailing = any(line.endswith(_ZEN_SP * 2) for line in jp_lines)
    padded: List[str] = []
    for idx, cn_line in enumerate(cn_lines):
        jp_line = jp_lines[idx] if idx < len(jp_lines) else None
        leading = 2
        if jp_line is not None:
            leading = 0
            for ch in jp_line:
                if ch == _ZEN_SP:
                    leading += 1
                else:
                    break
        stripped = cn_line.strip(_ZEN_SP)
        content = _ZEN_SP * leading + stripped
        if has_trailing and len(content) < _RL_LINE_WIDTH:
            content += _ZEN_SP * (_RL_LINE_WIDTH - len(content))
        padded.append(content)
    return "\n".join(padded)


def _jp_uses_rl_speaker_indent(japanese: str) -> bool:
    if "\n" not in japanese:
        return False
    return any(line.startswith(_ZEN_SP) for line in japanese.split("\n")[1:] if line)


def _add_rl_speaker_indent(text: str, japanese: str) -> str:
    if "「" not in text or "\n" not in text:
        return text
    if not _jp_uses_rl_speaker_indent(japanese):
        return text
    first_bracket = text.index("「")
    speaker_width = first_bracket + 1
    if speaker_width < 2 or speaker_width > 8:
        return text
    indent = _ZEN_SP * speaker_width
    lines = text.split("\n")
    result = [lines[0]]
    for line in lines[1:]:
        if line and not line.startswith(_ZEN_SP):
            result.append(indent + line.lstrip(_ZEN_SP))
        else:
            result.append(line)
    return "\n".join(result)


def _split_rl_window_chunks(raw_text: str) -> List[dict]:
    def _speaker_quote_in_line(value: str) -> str:
        for candidate in ("「",):
            if candidate in value:
                prefix, _ = value.split(candidate, 1)
                if prefix.strip():
                    return candidate
        return ""

    def _is_rl_special_header(value: str) -> bool:
        stripped = value.lstrip(_ZEN_SP)
        return stripped.startswith("『") or stripped.startswith("【")

    chunks: List[dict] = []
    lines = str(raw_text or "").replace("\r\n", "\n").replace("\r", "\n").split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if line == "ＣＡＬＬ":
            chunks.append({
                "raw": line,
                "body": line,
                "quote": "",
            })
            i += 1
            continue

        quote = ""
        quote = _speaker_quote_in_line(line)
        if quote:
            _, body = line.split(quote, 1)
            chunk_lines = [line]
            body_lines = [body]
            i += 1
            while i < len(lines):
                next_line = lines[i]
                if next_line == "ＣＡＬＬ":
                    break
                if _speaker_quote_in_line(next_line):
                    break
                if (
                    next_line.startswith(_ZEN_SP)
                    or next_line.startswith(("…", "‥"))
                ):
                    chunk_lines.append(next_line)
                    body_lines.append(next_line)
                    i += 1
                    continue
                if _is_rl_special_header(next_line):
                    chunk_lines.append(next_line)
                    body_lines.append(next_line)
                    i += 1
                    continue
                break
            chunks.append({
                "raw": "\n".join(chunk_lines),
                "body": "\n".join(body_lines),
                "quote": quote,
            })
            continue

        chunk_lines = [line]
        body_lines = [line]
        i += 1
        if _is_rl_special_header(line):
            while i < len(lines):
                next_line = lines[i]
                if next_line == "ＣＡＬＬ" or _speaker_quote_in_line(next_line):
                    break
                chunk_lines.append(next_line)
                body_lines.append(next_line)
                i += 1
        else:
            while i < len(lines) and lines[i].startswith(_ZEN_SP):
                chunk_lines.append(lines[i])
                body_lines.append(lines[i])
                i += 1
        chunks.append({
            "raw": "\n".join(chunk_lines),
            "body": "\n".join(body_lines),
            "quote": "",
        })
    return chunks


def _merge_rl_speaker_chunk(speaker_cn: str, quote: str, body_cn: str) -> str:
    body_lines = str(body_cn or "").split("\n")
    prefix = speaker_cn + quote
    if not body_lines:
        return prefix
    merged = [prefix + body_lines[0]]
    indent = _ZEN_SP * len(prefix)
    for line in body_lines[1:]:
        if line:
            merged.append(indent + line.lstrip(_ZEN_SP))
        else:
            merged.append(line)
    return "\n".join(merged)


def _resolve_rl_window_body_record(
    records: Dict[tuple[str, str], dict],
    *,
    side_name: str,
    body: str,
) -> Optional[dict]:
    direct = records.get((side_name, body)) or records.get(("common", body))
    if isinstance(direct, dict):
        return direct

    candidates = [
        record
        for (route, source_text), record in records.items()
        if source_text == body and isinstance(route, str) and isinstance(record, dict)
    ]
    if not candidates:
        return None

    unique_cn = {str(record.get("cn") or "") for record in candidates}
    unique_speaker = {str(record.get("speaker_cn") or "") for record in candidates}
    if len(unique_cn) == 1 and len(unique_speaker) == 1:
        return candidates[0]
    return None


def _resolve_rl_window_record(
    records: Dict[tuple[str, str], dict],
    *,
    side_name: str,
    raw_text: str,
) -> Optional[dict]:
    def project_single_chunk(chunk: dict, record: dict) -> Optional[str]:
        chunk_cn = str(record.get("cn") or "")
        quote = str(chunk.get("quote") or "")
        if quote:
            speaker_cn = str(record.get("speaker_cn") or "")
            if not speaker_cn:
                return None
            chunk_cn = _merge_rl_speaker_chunk(speaker_cn, quote, chunk_cn)
        return chunk_cn

    def project_chunk_span(chunks: List[dict], start_index: int) -> Optional[tuple[int, List[str]]]:
        for span in range(2, len(chunks) - start_index + 1):
            span_chunks = chunks[start_index : start_index + span]
            quoted_indexes = [
                index
                for index, chunk in enumerate(span_chunks)
                if str(chunk.get("quote") or "")
            ]
            if quoted_indexes not in ([], [0]):
                continue
            combined_body = "\n".join(str(chunk.get("body") or "") for chunk in span_chunks)
            record = _resolve_rl_window_body_record(records, side_name=side_name, body=combined_body)
            if not isinstance(record, dict):
                continue
            cn_lines = str(record.get("cn") or "").split("\n")
            expected_counts = [len(str(chunk.get("body") or "").split("\n")) for chunk in span_chunks]
            if sum(expected_counts) != len(cn_lines):
                continue
            projected_span: List[str] = []
            cursor = 0
            for chunk, line_count in zip(span_chunks, expected_counts):
                piece = "\n".join(cn_lines[cursor : cursor + line_count])
                cursor += line_count
                piece_record = dict(record)
                piece_record["cn"] = piece
                projected_piece = project_single_chunk(chunk, piece_record)
                if projected_piece is None:
                    projected_span = []
                    break
                projected_span.append(projected_piece)
            if projected_span:
                return span, projected_span
        return None

    projected_chunks: List[str] = []
    chunks = _split_rl_window_chunks(raw_text)
    index = 0
    while index < len(chunks):
        chunk = chunks[index]
        body = str(chunk.get("body") or "")
        if body == "ＣＡＬＬ":
            projected_chunks.append(body)
            index += 1
            continue

        record = _resolve_rl_window_body_record(records, side_name=side_name, body=body)
        if isinstance(record, dict):
            chunk_cn = project_single_chunk(chunk, record)
            if chunk_cn is None:
                return None
            projected_chunks.append(chunk_cn)
            index += 1
            continue

        span_projection = project_chunk_span(chunks, index)
        if span_projection is None:
            return None
        span, projected_span = span_projection
        projected_chunks.extend(projected_span)
        index += span

    return {
        "cn": "\n".join(projected_chunks),
        "rl_projected": True,
    }


def _normalize_part_line_map(part_line_map: object, parts: object) -> Optional[List[int]]:
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


def _expected_storage_line_count(parts: object, part_line_map: object) -> Optional[int]:
    normalized_map = _normalize_part_line_map(part_line_map, parts)
    if normalized_map is not None:
        return max(normalized_map) + 1
    if isinstance(parts, list) and parts:
        return len(parts)
    return None


def _collect_item_ref_offsets(item: dict, *, offset_key: str, parts_key: Optional[str]) -> set[int]:
    offsets: set[int] = set()
    primary = item.get(offset_key)
    if isinstance(primary, int) and not isinstance(primary, bool):
        offsets.add(int(primary))

    original_key = "original_offset"
    if offset_key.startswith("scriptf_"):
        original_key = "scriptf_original_offset"
    elif offset_key.startswith("script_"):
        original_key = "script_original_offset"

    original = item.get(original_key)
    if isinstance(original, int) and not isinstance(original, bool):
        offsets.add(int(original))

    if isinstance(parts_key, str):
        raw_parts = item.get(parts_key)
        if isinstance(raw_parts, list):
            for raw_part in raw_parts:
                if not isinstance(raw_part, dict):
                    continue
                part_offset = raw_part.get("offset")
                if isinstance(part_offset, int) and not isinstance(part_offset, bool):
                    offsets.add(int(part_offset))
    return offsets


def _normalize_reviewed_target_cache(raw_target_cache: object) -> Optional[str]:
    if not isinstance(raw_target_cache, str):
        return None
    target_cache = raw_target_cache.strip().lower()
    if target_cache not in {"canonical", "eve", "rl"}:
        return None
    return target_cache


def _normalize_reviewed_output_paths(paths: Sequence[Path] | Path) -> List[Path]:
    if isinstance(paths, Path):
        return [paths]
    normalized: List[Path] = []
    for raw_path in paths:
        if isinstance(raw_path, Path):
            normalized.append(raw_path)
    return normalized


def _load_reviewed_output_records(paths: Sequence[Path] | Path, *, expected_target_cache: str) -> Dict[tuple[str, str, int], dict]:
    records: Dict[tuple[str, str, int], dict] = {}
    record_sources: Dict[tuple[str, str, int], Path] = {}

    for path in _normalize_reviewed_output_paths(paths):
        if not path.exists():
            raise CacheSyncFailure(f"Claude 输出文件不存在: {path}")

        with path.open("r", encoding="utf-8") as handle:
            for line_no, raw_line in enumerate(handle, start=1):
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise CacheSyncFailure(f"Claude 输出 JSONL 非法: {path}:{line_no}: {exc}") from exc
                if not isinstance(entry, dict):
                    continue

                target_cache = _normalize_reviewed_target_cache(entry.get("target_cache"))
                if target_cache != expected_target_cache:
                    raise CacheSyncFailure(
                        f"Claude 输出 target_cache 不匹配: {path}:{line_no}: 期望 {expected_target_cache}, 实际 {entry.get('target_cache')}"
                    )

                file_name = entry.get("file")
                side_name = _normalize_cache_side(entry.get("side"))
                offset = entry.get("offset")
                cn = entry.get("cn")
                if not isinstance(file_name, str) or not file_name.strip():
                    raise CacheSyncFailure(f"Claude 输出缺少 file: {path}:{line_no}")
                if side_name is None:
                    raise CacheSyncFailure(f"Claude 输出 side 非法: {path}:{line_no}")
                if not isinstance(offset, int) or isinstance(offset, bool):
                    raise CacheSyncFailure(f"Claude 输出 offset 非法: {path}:{line_no}")
                if not isinstance(cn, str):
                    raise CacheSyncFailure(f"Claude 输出缺少 cn: {path}:{line_no}")

                key = (file_name.strip(), side_name, int(offset))
                previous_source = record_sources.get(key)
                if previous_source == path and key in records and records[key].get("cn") != cn:
                    raise CacheSyncFailure(f"Claude 输出存在冲突重复键: {path}:{line_no}: {key}")

                normalized = {
                    "target_cache": target_cache,
                    "file": key[0],
                    "side": side_name,
                    "offset": key[2],
                    "cn": cn,
                }
                speaker_cn = entry.get("speaker_cn")
                body_cn = entry.get("body_cn")
                if isinstance(speaker_cn, str):
                    normalized["speaker_cn"] = speaker_cn
                if isinstance(body_cn, str):
                    normalized["body_cn"] = body_cn

                records[key] = normalized
                record_sources[key] = path
    return records


def load_reviewed_output_bundle(
    *,
    canonical_output_path: Sequence[Path] | Path,
    eve_output_path: Sequence[Path] | Path,
    rl_output_path: Sequence[Path] | Path,
) -> Dict[str, Dict[tuple[str, str, int], dict]]:
    return {
        "canonical": _load_reviewed_output_records(canonical_output_path, expected_target_cache="canonical"),
        "eve": _load_reviewed_output_records(eve_output_path, expected_target_cache="eve"),
        "rl": _load_reviewed_output_records(rl_output_path, expected_target_cache="rl"),
    }


def _record_matches_ref(record: dict, *, file_name: str, offsets: set[int]) -> bool:
    if not offsets:
        return False
    record_file = record.get("file")
    record_offset = record.get("offset")
    if isinstance(record_file, str) and isinstance(record_offset, int):
        return record_file == file_name and int(record_offset) in offsets
    refs = record.get("refs")
    if isinstance(refs, list) and refs:
        for ref in refs:
            if not isinstance(ref, dict):
                continue
            if ref.get("file") == file_name and ref.get("offset") in offsets:
                return True
    ref_files = record.get("ref_files")
    if isinstance(ref_files, list) and file_name in ref_files:
        return True
    return False


def _record_matches_side(record: dict, side_name: str) -> bool:
    route = record.get("route")
    if isinstance(route, str) and route:
        return route == side_name
    record_side = str(record.get("side") or "")
    if record_side in ("common", ""):
        return True
    return record_side == side_name


def _prefer_exact_side_records(records: List[dict], side_name: str) -> List[dict]:
    exact_side = []
    for record in records:
        route = record.get("route")
        if isinstance(route, str) and route:
            if route == side_name:
                exact_side.append(record)
            continue
        record_side = str(record.get("side") or "")
        if record_side == side_name:
            exact_side.append(record)
    return exact_side or records


def _prefer_same_file_records(records: List[dict], file_name: str) -> List[dict]:
    matched = [
        record
        for record in records
        if isinstance(record.get("file"), str) and str(record.get("file")) == file_name
    ]
    return matched or records


def _prefer_same_scene_records(
    records: List[dict],
    *,
    scene_idx: Optional[int],
    scene_entry_index: Optional[int],
) -> List[dict]:
    if not isinstance(scene_idx, int):
        return records
    matched = [
        record
        for record in records
        if isinstance(record.get("scene_idx"), int)
        and int(record.get("scene_idx")) == scene_idx
        and (
            not isinstance(scene_entry_index, int)
            or not isinstance(record.get("scene_entry_index"), int)
            or int(record.get("scene_entry_index")) == scene_entry_index
        )
    ]
    return matched or records


def _prefer_ref_records(records: List[dict], *, file_name: str, offsets: set[int]) -> List[dict]:
    matched = [
        record
        for record in records
        if _record_matches_ref(record, file_name=file_name, offsets=offsets)
    ]
    return matched or records


def _pick_unique_record(records: List[dict]) -> Optional[dict]:
    if not records:
        return None
    canonical_key = {
        (
            str(record.get("cn") or ""),
            str(record.get("speaker_cn") or ""),
            tuple(record.get("part_lines") or []),
            tuple(record.get("script_part_lines") or []),
            tuple(record.get("scriptf_part_lines") or []),
        )
        for record in records
    }
    if len(canonical_key) != 1:
        return None
    return records[0]


def _resolve_canonical_sequence_record(
    records_by_jp: Dict[str, List[dict]],
    *,
    file_name: str,
    side_name: str,
    text: str,
    scene_idx: Optional[int] = None,
    scene_entry_index: Optional[int] = None,
) -> Optional[dict]:
    candidates = records_by_jp.get(text, [])
    if not candidates:
        return None
    matched = [
        record
        for record in candidates
        if _record_matches_side(record, side_name)
    ]
    matched = _prefer_exact_side_records(matched, side_name)
    matched = _prefer_same_file_records(matched, file_name)
    matched = _prefer_same_scene_records(
        matched,
        scene_idx=scene_idx,
        scene_entry_index=scene_entry_index,
    )
    return _pick_unique_record(matched)


def _resolve_canonical_record(
    records_by_jp: Dict[str, List[dict]],
    *,
    file_name: str,
    side_name: str,
    text: str,
    offsets: set[int],
) -> Optional[dict]:
    candidates = records_by_jp.get(text, [])
    if not candidates:
        return None
    matched = [
        record
        for record in candidates
        if _record_matches_side(record, side_name)
    ]
    matched = _prefer_exact_side_records(matched, side_name)
    matched = _prefer_same_file_records(matched, file_name)
    matched = _prefer_ref_records(matched, file_name=file_name, offsets=offsets)
    return _pick_unique_record(matched)


def _resolve_window_record(
    records: Dict[tuple[str, str], dict],
    *,
    side_name: str,
    text: Optional[str] = None,
    body_text: Optional[str] = None,
    speaker: Optional[str] = None,
    rl_project: bool = False,
) -> Optional[dict]:
    if isinstance(text, str):
        if rl_project:
            projected = _resolve_rl_window_record(records, side_name=side_name, raw_text=text)
            if isinstance(projected, dict):
                return projected
        record = records.get((side_name, text)) or records.get(("common", text))
        if isinstance(record, dict):
            return record
    if isinstance(body_text, str) and body_text:
        record = records.get((side_name, body_text)) or records.get(("common", body_text))
        if not isinstance(record, dict):
            return None
        cn = str(record.get("cn") or "")
        speaker_cn = str(record.get("speaker_cn") or "")
        if speaker and speaker_cn:
            merged = dict(record)
            merged["cn"] = speaker_cn + "\n" + cn
            return merged
        return record
    return None


def _resolve_exact_text_record(
    records_by_jp: Dict[str, List[dict]],
    *,
    file_name: str,
    side_name: str,
    text: str,
) -> Optional[dict]:
    candidates = records_by_jp.get(text, [])
    if not candidates:
        return None
    matched = [
        record
        for record in candidates
        if str(record.get("file") or "") == file_name
        and (
            str(record.get("route") or "") in {"", side_name, "common"}
            or str(record.get("side") or "") in {"", side_name, "common"}
        )
    ]
    return _pick_unique_record(matched)


def _collect_json_files(translated_dir: Path) -> List[Path]:
    json_files = sorted(translated_dir.glob("*.json"))
    return [
        path for path in json_files
        if not path.name.startswith(("xbe_", "script_overflow", "script_preserved_original"))
    ]


def _print_cache_summary(cache_bundle: dict[str, object], *, group: str, path: Path) -> None:
    print(f"加载 {group.upper()} cache: {path}")
    if group == "canonical":
        canonical_records = cache_bundle.get("canonical")
        exact_records = cache_bundle.get("canonical_exact")
        print(f"  JP索引记录数: {len(canonical_records) if isinstance(canonical_records, dict) else 0}")
        print(f"  exact记录数: {len(exact_records) if isinstance(exact_records, dict) else 0}")
        return

    exact_records = cache_bundle.get(f"{group}_exact")
    text_records = cache_bundle.get(f"{group}_text")
    window_records = cache_bundle.get(group)
    print(f"  exact记录数: {len(exact_records) if isinstance(exact_records, dict) else 0}")
    print(f"  exact文本键数: {len(text_records) if isinstance(text_records, dict) else 0}")
    print(f"  当前window索引数: {len(window_records) if isinstance(window_records, dict) else 0}")


def _sync_part_lines(
    item: dict,
    record: dict,
    *,
    file_name: str,
    display_offset: int,
    ref_offsets: set[int],
    parts_key: str,
    part_line_map_key: str,
    part_lines_key: str,
    record_part_line_keys: Sequence[str],
) -> bool:
    if not _record_matches_ref(record, file_name=file_name, offsets=ref_offsets):
        return False

    replacement: Optional[List[str]] = None
    for key in record_part_line_keys:
        value = record.get(key)
        if isinstance(value, list):
            replacement = list(value)
            break
    if replacement is None:
        return False

    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return False
    if len(replacement) != expected_count:
        return False
    if item.get(part_lines_key) == replacement:
        return False
    item[part_lines_key] = replacement
    return True


def _synthesize_option_part_lines(
    item: dict,
    *,
    cn_text: str,
    text_family_key: Optional[str],
    storage_lines_key: Optional[str],
    parts_key: str,
    part_line_map_key: str,
) -> Optional[List[str]]:
    if not text_family_key or not storage_lines_key:
        return None
    text_family = str(item.get(text_family_key) or "")
    if text_family not in _OPTION_TEXT_FAMILIES:
        return None
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return None
    raw_storage_lines = item.get(storage_lines_key)
    if not isinstance(raw_storage_lines, list) or len(raw_storage_lines) != expected_count:
        return None

    storage_lines = [str(line or "") for line in raw_storage_lines]
    translated_lines = str(cn_text or "").split("\n") if cn_text else []
    if len(translated_lines) == expected_count:
        return translated_lines

    visible_slot_count = sum(1 for line in storage_lines if not _is_nonvisible_option_line(line))
    if visible_slot_count != len(translated_lines):
        return None

    replacement: List[str] = []
    translated_idx = 0
    for storage_line in storage_lines:
        if _is_nonvisible_option_line(storage_line):
            replacement.append("")
            continue
        replacement.append(translated_lines[translated_idx])
        translated_idx += 1
    return replacement


def _synthesize_placeholder_part_lines(
    item: dict,
    *,
    cn_text: str,
    storage_lines_key: Optional[str],
    parts_key: str,
    part_line_map_key: str,
) -> Optional[List[str]]:
    if not storage_lines_key:
        return None
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return None
    raw_storage_lines = item.get(storage_lines_key)
    if not isinstance(raw_storage_lines, list) or len(raw_storage_lines) != expected_count:
        return None

    storage_lines = [str(line or "") for line in raw_storage_lines]
    if not any(_is_pure_placeholder_expansion_line(line) for line in storage_lines):
        return None

    translated_lines = str(cn_text or "").split("\n") if cn_text else []

    replacement: List[str] = []
    translated_idx = 0
    for storage_line in storage_lines:
        if _is_pure_placeholder_expansion_line(storage_line):
            if (
                translated_idx < len(translated_lines)
                and translated_lines[translated_idx] == storage_line
            ):
                translated_idx += 1
            replacement.append(storage_line)
            continue
        if _is_nonvisible_option_line(storage_line):
            replacement.append("")
            continue
        if translated_idx >= len(translated_lines):
            return None
        replacement.append(translated_lines[translated_idx])
        translated_idx += 1
    if translated_idx != len(translated_lines):
        return None
    return replacement


def _synthesize_direct_part_lines(
    item: dict,
    *,
    cn_text: str,
    storage_lines_key: Optional[str],
    parts_key: str,
    part_line_map_key: str,
) -> Optional[List[str]]:
    if not storage_lines_key:
        return None
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return None
    raw_storage_lines = item.get(storage_lines_key)
    if not isinstance(raw_storage_lines, list) or len(raw_storage_lines) != expected_count:
        return None

    storage_lines = [str(line or "") for line in raw_storage_lines]
    translated_lines = str(cn_text or "").split("\n") if cn_text else []
    if len(translated_lines) != expected_count:
        return None

    for storage_line in storage_lines:
        if storage_line.strip() == "":
            return None
        if _is_nonvisible_option_line(storage_line):
            return None
        if _is_pure_placeholder_expansion_line(storage_line):
            return None

    return translated_lines


def _synthesize_structural_slot_part_lines(
    item: dict,
    *,
    cn_text: str,
    storage_lines_key: Optional[str],
    parts_key: str,
    part_line_map_key: str,
) -> Optional[List[str]]:
    if not storage_lines_key:
        return None
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return None
    raw_storage_lines = item.get(storage_lines_key)
    if not isinstance(raw_storage_lines, list) or len(raw_storage_lines) != expected_count:
        return None

    storage_lines = [str(line or "") for line in raw_storage_lines]
    translated_lines = str(cn_text or "").split("\n") if cn_text else []
    structural_slot_count = 0
    replacement: List[str] = []
    translated_idx = 0
    for storage_line in storage_lines:
        if _is_pure_placeholder_expansion_line(storage_line):
            structural_slot_count += 1
            translated_line = translated_lines[translated_idx] if translated_idx < len(translated_lines) else None
            if translated_line is not None and translated_line.strip() == "":
                translated_idx += 1
            replacement.append(storage_line)
            continue
        if _is_nonvisible_option_line(storage_line):
            replacement.append("")
            continue
        if storage_line.strip() == "":
            structural_slot_count += 1
            translated_line = translated_lines[translated_idx] if translated_idx < len(translated_lines) else None
            if translated_line is not None and translated_line.strip() == "":
                translated_idx += 1
            replacement.append(storage_line)
            continue
        if translated_idx >= len(translated_lines):
            return None
        translated_line = translated_lines[translated_idx]
        if translated_line.strip() == "":
            return None
        replacement.append(translated_lines[translated_idx])
        translated_idx += 1
    if structural_slot_count == 0:
        return None
    if translated_idx != len(translated_lines):
        return None
    return replacement


def _storage_slot_lines(raw_line: object) -> List[str]:
    text = str(raw_line or "").replace("\r\n", "\n").replace("\r", "\n")
    return text.split("\n")


def _storage_slot_visible_payload(raw_line: object) -> str:
    payload: List[str] = []
    for kind, value in _tokenize_option_line(str(raw_line or "")):
        if kind != "char":
            continue
        payload.append(str(value))
    return "".join(payload).strip()


def _join_part_line_segments(lines: Sequence[str]) -> str:
    return "<0A>".join(str(line or "") for line in lines)


def _derive_body_slot_line_counts(
    storage_lines: Sequence[object],
    body_lines: Sequence[str],
) -> Optional[List[int]]:
    slot_lines = [_storage_slot_lines(line) for line in storage_lines]
    body = [str(line or "") for line in body_lines]

    @lru_cache(None)
    def _solve(slot_idx: int, body_idx: int) -> Optional[tuple[int, ...]]:
        if slot_idx >= len(slot_lines):
            return tuple() if body_idx == len(body) else None

        current_slot = slot_lines[slot_idx]
        consume = len(current_slot)
        if body_idx + consume <= len(body) and body[body_idx : body_idx + consume] == current_slot:
            tail = _solve(slot_idx + 1, body_idx + consume)
            if tail is not None:
                return (consume, *tail)

        if _storage_slot_visible_payload(storage_lines[slot_idx]) == "":
            tail = _solve(slot_idx + 1, body_idx)
            if tail is not None:
                return (0, *tail)

        return None

    solved = _solve(0, 0)
    if solved is None:
        return None
    return list(solved)


def _synthesize_body_mapped_part_lines(
    item: dict,
    *,
    cn_text: str,
    storage_lines_key: Optional[str],
    body_text_key: Optional[str],
    parts_key: str,
    part_line_map_key: str,
) -> Optional[List[str]]:
    if not storage_lines_key or not body_text_key:
        return None
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return None

    raw_storage_lines = item.get(storage_lines_key)
    if not isinstance(raw_storage_lines, list) or len(raw_storage_lines) != expected_count:
        return None

    body_text = str(item.get(body_text_key) or "")
    if not body_text:
        return None
    body_lines = body_text.split("\n")
    counts = _derive_body_slot_line_counts(raw_storage_lines, body_lines)
    if counts is None or len(counts) != expected_count:
        return None

    translated_lines = str(cn_text or "").split("\n") if cn_text else []
    if sum(counts) != len(translated_lines):
        return None

    replacement: List[str] = []
    translated_idx = 0
    for count, storage_line in zip(counts, raw_storage_lines):
        if count <= 0:
            replacement.append(_join_part_line_segments(_storage_slot_lines(storage_line)))
            continue
        replacement.append(
            _join_part_line_segments(translated_lines[translated_idx : translated_idx + count])
        )
        translated_idx += count

    if translated_idx != len(translated_lines):
        return None
    return replacement


def _sync_common_side_cn_fields(item: dict, *, side: str) -> bool:
    changed = False
    for base_key, side_key in (
        ("chinese", f"{side}_chinese"),
        ("speaker_cn", f"{side}_speaker_cn"),
        ("body_cn", f"{side}_body_cn"),
    ):
        base_value = str(item.get(base_key) or "")
        current_value = str(item.get(side_key) or "")
        if base_value:
            if current_value != base_value:
                item[side_key] = base_value
                changed = True
            continue
        if side_key in item and current_value:
            item[side_key] = ""
            changed = True
    return changed


def _sync_common_side_part_lines(item: dict, *, side: str) -> bool:
    chinese = str(item.get("chinese") or "")
    body_cn = str(item.get("body_cn") or "")
    target_cn = body_cn if body_cn else chinese

    part_lines_key = f"{side}_part_lines"
    existing_part_lines = item.get(part_lines_key)

    parts_key = f"{side}_parts"
    part_line_map_key = f"{side}_part_line_map"
    expected_count = _expected_storage_line_count(item.get(parts_key), item.get(part_line_map_key))
    if expected_count is None:
        return False

    base_part_lines = item.get("part_lines")
    if isinstance(base_part_lines, list) and len(base_part_lines) == expected_count:
        replacement = list(base_part_lines)
        if existing_part_lines == replacement:
            return False
        item[part_lines_key] = replacement
        return True

    synthesized = _synthesize_body_mapped_part_lines(
        item,
        cn_text=target_cn,
        storage_lines_key=f"{side}_storage_jp_lines",
        body_text_key=f"{side}_body_text",
        parts_key=parts_key,
        part_line_map_key=part_line_map_key,
    )
    if synthesized is None:
        synthesized = _synthesize_body_mapped_part_lines(
            item,
            cn_text=target_cn,
            storage_lines_key=f"{side}_storage_jp_lines",
            body_text_key="body_text",
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )

    if synthesized is None:
        synthesized = _synthesize_direct_part_lines(
            item,
            storage_lines_key=f"{side}_storage_jp_lines",
            cn_text=chinese,
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )
    if synthesized is None:
        synthesized = _synthesize_option_part_lines(
            item,
            cn_text=chinese,
            text_family_key=f"{side}_vm_text_family",
            storage_lines_key=f"{side}_storage_jp_lines",
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )
    if synthesized is None:
        synthesized = _synthesize_placeholder_part_lines(
            item,
            cn_text=chinese,
            storage_lines_key=f"{side}_storage_jp_lines",
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )
    if synthesized is None:
        synthesized = _synthesize_structural_slot_part_lines(
            item,
            cn_text=chinese,
            storage_lines_key=f"{side}_storage_jp_lines",
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )
    if synthesized is None:
        return False
    if existing_part_lines == synthesized:
        return False
    item[part_lines_key] = synthesized
    return True


def _split_cn_components(
    *,
    item: dict,
    cn_text: str,
    speaker_source_key: Optional[str],
    speaker_key: Optional[str],
    body_text_key: Optional[str],
) -> tuple[str, str]:
    if not cn_text:
        return "", ""
    if isinstance(speaker_source_key, str) and speaker_source_key:
        item_speaker_source = str(item.get(speaker_source_key) or "")
    else:
        item_speaker_source = str(item.get("speaker_source") or "")
    item_speaker = str(item.get(speaker_key) or "") if isinstance(speaker_key, str) else ""
    item_body_text = str(item.get(body_text_key) or "") if isinstance(body_text_key, str) else ""
    if item_speaker_source == "inherited":
        return "", cn_text
    if not item_speaker.strip() or not item_body_text.strip():
        return "", ""
    body_lines = item_body_text.split("\n")
    cn_lines = cn_text.split("\n")
    if len(cn_lines) != len(body_lines) + 1:
        return "", ""
    return cn_lines[0], "\n".join(cn_lines[1:])


def _sync_item(
    item: dict,
    *,
    file_name: str,
    cache_bundle: dict[str, object],
    cache_group: str,
    rl_mode: bool,
    side_name: str,
    text_key: str,
    chinese_key: str,
    offset_key: str,
    speaker_source_key: Optional[str] = None,
    speaker_cn_key: Optional[str] = None,
    body_cn_key: Optional[str] = None,
    speaker_key: Optional[str] = None,
    body_text_key: Optional[str] = None,
    text_family_key: Optional[str] = None,
    storage_lines_key: Optional[str] = None,
    parts_key: Optional[str] = None,
    part_line_map_key: Optional[str] = None,
    part_lines_key: Optional[str] = None,
    record_part_line_keys: Sequence[str] = (),
    clear_unmatched: bool = False,
) -> tuple[bool, bool]:
    raw_text = item.get(text_key)
    if not isinstance(raw_text, str):
        return False, False
    if not raw_text.strip():
        if not (part_lines_key and storage_lines_key and parts_key and part_line_map_key):
            return False, False
        synthesized = _synthesize_structural_slot_part_lines(
            item,
            cn_text=str(item.get(chinese_key) or ""),
            storage_lines_key=storage_lines_key,
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
        )
        if synthesized is None or item.get(part_lines_key) == synthesized:
            return False, False
        item[part_lines_key] = synthesized
        return True, False

    offset = item.get(offset_key)
    record: Optional[dict] = None
    exact_record: Optional[dict] = None
    ref_offsets = _collect_item_ref_offsets(item, offset_key=offset_key, parts_key=parts_key)
    exact_bundle_key = f"{cache_group}_exact"
    exact_records = cache_bundle.get(exact_bundle_key)
    if isinstance(exact_records, dict) and isinstance(offset, int) and not isinstance(offset, bool):
        maybe_exact = exact_records.get((file_name, side_name, int(offset)))
        if isinstance(maybe_exact, dict) and str(maybe_exact.get("jp") or "") == raw_text:
            exact_record = maybe_exact
            record = maybe_exact
    canonical_records = cache_bundle["canonical"]
    if record is None and cache_group == "canonical":
        if isinstance(canonical_records, dict):
            record = _resolve_canonical_record(
                canonical_records,
                file_name=file_name,
                side_name=side_name,
                text=raw_text,
                offsets=ref_offsets,
            )
    elif record is None:
        text_records = cache_bundle.get(f"{cache_group}_text")
        if isinstance(text_records, dict):
            record = _resolve_exact_text_record(
                text_records,
                file_name=file_name,
                side_name=side_name,
                text=raw_text,
            )
        window_records = cache_bundle.get(f"{cache_group}_window_authority")
        if not isinstance(window_records, dict):
            window_records = cache_bundle[cache_group]
        if record is None and isinstance(window_records, dict):
            record = _resolve_window_record(
                window_records,
                side_name=side_name,
                text=raw_text,
                body_text=str(item.get(body_text_key) or "") if isinstance(body_text_key, str) else None,
                speaker=str(item.get(speaker_key) or "") if isinstance(speaker_key, str) else None,
                rl_project=rl_mode,
            )
    if not isinstance(record, dict):
        reviewed_bundle = cache_bundle.get("reviewed")
        if isinstance(reviewed_bundle, dict):
            reviewed_group = reviewed_bundle.get(cache_group)
            if isinstance(reviewed_group, dict) and isinstance(offset, int) and not isinstance(offset, bool):
                reviewed_record = reviewed_group.get((file_name, side_name, int(offset)))
                if isinstance(reviewed_record, dict):
                    record = reviewed_record
    if not isinstance(record, dict):
        changed = False
        if part_lines_key and storage_lines_key and parts_key and part_line_map_key:
            synthesized_part_lines = _synthesize_structural_slot_part_lines(
                item,
                cn_text=str(item.get(chinese_key) or ""),
                storage_lines_key=storage_lines_key,
                parts_key=parts_key,
                part_line_map_key=part_line_map_key,
            )
            if synthesized_part_lines is not None and item.get(part_lines_key) != synthesized_part_lines:
                item[part_lines_key] = synthesized_part_lines
                changed = True
        if clear_unmatched and str(item.get(chinese_key) or ""):
            item[chinese_key] = ""
            changed = True
        if clear_unmatched and isinstance(speaker_cn_key, str) and str(item.get(speaker_cn_key) or ""):
            item[speaker_cn_key] = ""
            changed = True
        if clear_unmatched and isinstance(body_cn_key, str) and str(item.get(body_cn_key) or ""):
            item[body_cn_key] = ""
            changed = True
        if clear_unmatched and part_lines_key and part_lines_key in item and not changed:
            item.pop(part_lines_key, None)
            changed = True
        return changed, False

    cached_cn = str(record.get("cn") or "")
    if rl_mode:
        projected_rl_record: Optional[dict] = None
        window_records = cache_bundle.get(f"{cache_group}_window_authority")
        if not isinstance(window_records, dict):
            window_records = cache_bundle.get(cache_group)
        if isinstance(window_records, dict):
            projected = _resolve_rl_window_record(
                window_records,
                side_name=side_name,
                raw_text=raw_text,
            )
            if isinstance(projected, dict):
                projected_rl_record = projected
        if projected_rl_record is not None:
            record = {**record, **projected_rl_record}
            cached_cn = str(record.get("cn") or "")
        # Cache CN already contains correct RL indentation;
        # skip _apply_rl_padding / _add_rl_speaker_indent to avoid double-processing.

    cached_speaker_cn = str(record.get("speaker_cn") or "")
    cached_body_cn = str(record.get("body_cn") or "")
    if isinstance(speaker_source_key, str) and speaker_source_key:
        item_speaker_source = str(item.get(speaker_source_key) or "")
    else:
        item_speaker_source = str(item.get("speaker_source") or "")
    item_speaker = str(item.get(speaker_key) or "") if isinstance(speaker_key, str) else ""
    item_body_text = str(item.get(body_text_key) or "") if isinstance(body_text_key, str) else ""
    if item_body_text.strip() and cached_speaker_cn and not cached_body_cn and cached_cn == cached_speaker_cn:
        return False, False
    if cached_cn and (not cached_speaker_cn and not cached_body_cn):
        inferred_speaker_cn, inferred_body_cn = _split_cn_components(
            item=item,
            cn_text=cached_cn,
            speaker_source_key=speaker_source_key,
            speaker_key=speaker_key,
            body_text_key=body_text_key,
        )
        if inferred_speaker_cn:
            cached_speaker_cn = inferred_speaker_cn
        if inferred_body_cn:
            cached_body_cn = inferred_body_cn
    if item_speaker_source == "inherited":
        if cached_body_cn:
            cached_cn = cached_body_cn
    elif item_speaker.strip() and cached_speaker_cn and (cached_body_cn or not item_body_text.strip()):
        cached_cn = cached_speaker_cn + ("\n" + cached_body_cn if cached_body_cn else "")
    elif cached_body_cn and not item_speaker.strip():
        cached_cn = cached_body_cn

    changed = False
    if str(item.get(chinese_key) or "") != cached_cn:
        item[chinese_key] = cached_cn
        changed = True
    if isinstance(speaker_cn_key, str):
        if str(item.get(speaker_cn_key) or "") != cached_speaker_cn:
            item[speaker_cn_key] = cached_speaker_cn
            changed = True
    if isinstance(body_cn_key, str):
        if str(item.get(body_cn_key) or "") != cached_body_cn:
            item[body_cn_key] = cached_body_cn
            changed = True

    synthesized_part_lines: Optional[List[str]] = None
    if (
        parts_key
        and part_line_map_key
        and part_lines_key
        and record_part_line_keys
    ):
        if _sync_part_lines(
            item,
            record,
            file_name=file_name,
            display_offset=int(item.get(offset_key) or 0),
            ref_offsets=ref_offsets,
            parts_key=parts_key,
            part_line_map_key=part_line_map_key,
            part_lines_key=part_lines_key,
            record_part_line_keys=record_part_line_keys,
        ):
            changed = True
        else:
            synthesized_part_lines = _synthesize_option_part_lines(
                item,
                cn_text=cached_cn,
                text_family_key=text_family_key,
                storage_lines_key=storage_lines_key,
                parts_key=parts_key,
                part_line_map_key=part_line_map_key,
            )
            if synthesized_part_lines is not None and item.get(part_lines_key) != synthesized_part_lines:
                item[part_lines_key] = synthesized_part_lines
                changed = True
            elif synthesized_part_lines is None:
                synthesized_part_lines = _synthesize_placeholder_part_lines(
                    item,
                    cn_text=cached_cn,
                    storage_lines_key=storage_lines_key,
                    parts_key=parts_key,
                    part_line_map_key=part_line_map_key,
                )
                if synthesized_part_lines is not None and item.get(part_lines_key) != synthesized_part_lines:
                    item[part_lines_key] = synthesized_part_lines
                    changed = True
            if synthesized_part_lines is None:
                synthesized_part_lines = _synthesize_structural_slot_part_lines(
                    item,
                    cn_text=cached_cn,
                    storage_lines_key=storage_lines_key,
                    parts_key=parts_key,
                    part_line_map_key=part_line_map_key,
                )
                if synthesized_part_lines is not None and item.get(part_lines_key) != synthesized_part_lines:
                    item[part_lines_key] = synthesized_part_lines
                    changed = True
        if exact_record is not None and part_lines_key and part_lines_key in item:
            has_exact_part_lines = any(isinstance(record.get(key), list) for key in record_part_line_keys)
            if not has_exact_part_lines and synthesized_part_lines is None:
                item.pop(part_lines_key, None)
                changed = True
    elif exact_record is not None and part_lines_key and part_lines_key in item:
        has_exact_part_lines = any(isinstance(record.get(key), list) for key in record_part_line_keys)
        if not has_exact_part_lines:
            item.pop(part_lines_key, None)
            changed = True

    return changed, True


def _make_side_rows(data: dict, side_name: str) -> List[dict]:
    rows: List[dict] = []
    categories = ("common", "script_only", "scriptf_only", "different")
    for category in categories:
        for item in data.get(category, []):
            if not isinstance(item, dict):
                continue
            if category == "common":
                if side_name == "script":
                    text_key = "text"
                    chinese_key = "chinese"
                    offset_key = "script_offset" if "script_offset" in item else "offset"
                    speaker_source_key = "speaker_source"
                    speaker_cn_key = "speaker_cn"
                    body_cn_key = "body_cn"
                    speaker_key = "speaker"
                    body_text_key = "body_text"
                else:
                    text_key = "text"
                    chinese_key = "chinese"
                    offset_key = "scriptf_offset" if "scriptf_offset" in item else "offset"
                    speaker_source_key = "speaker_source"
                    speaker_cn_key = "speaker_cn"
                    body_cn_key = "body_cn"
                    speaker_key = "speaker"
                    body_text_key = "body_text"
            elif category == "script_only":
                if side_name != "script":
                    continue
                text_key = "text"
                chinese_key = "chinese"
                offset_key = "offset"
                speaker_source_key = "speaker_source"
                speaker_cn_key = "speaker_cn"
                body_cn_key = "body_cn"
                speaker_key = "speaker"
                body_text_key = "body_text"
            elif category == "scriptf_only":
                if side_name != "scriptf":
                    continue
                text_key = "text"
                chinese_key = "chinese"
                offset_key = "offset"
                speaker_source_key = "speaker_source"
                speaker_cn_key = "speaker_cn"
                body_cn_key = "body_cn"
                speaker_key = "speaker"
                body_text_key = "body_text"
            else:
                text_key = f"{side_name}_text"
                chinese_key = f"{side_name}_chinese"
                offset_key = f"{side_name}_offset" if f"{side_name}_offset" in item else "offset"
                speaker_source_key = f"{side_name}_speaker_source"
                speaker_cn_key = f"{side_name}_speaker_cn"
                body_cn_key = f"{side_name}_body_cn"
                speaker_key = f"{side_name}_speaker"
                body_text_key = f"{side_name}_body_text"
            raw_text = item.get(text_key)
            if not isinstance(raw_text, str) or not raw_text.strip():
                continue
            rows.append(
                {
                    "item": item,
                    "text_key": text_key,
                    "chinese_key": chinese_key,
                    "offset_key": offset_key,
                    "speaker_source_key": speaker_source_key,
                    "speaker_cn_key": speaker_cn_key,
                    "body_cn_key": body_cn_key,
                    "speaker_key": speaker_key,
                    "body_text_key": body_text_key,
                    "speaker_source": str(item.get(speaker_source_key) or ""),
                    "speaker": str(item.get(speaker_key) or ""),
                    "body_text": str(item.get(body_text_key) or ""),
                }
            )
    rows.sort(
        key=lambda row: (
            int(row["item"].get("scene_idx")) if isinstance(row["item"].get("scene_idx"), int) else 1 << 30,
            int(row["item"].get("scene_entry_index")) if isinstance(row["item"].get("scene_entry_index"), int) else 1 << 30,
            int(row["item"].get(row["offset_key"])) if isinstance(row["item"].get(row["offset_key"]), int) else 1 << 30,
        )
    )
    return rows


def _fill_sequence_rows(
    rows: Sequence[dict],
    *,
    cn_text: str,
) -> bool:
    if not rows:
        return False
    first = rows[0]
    first_speaker = str(first["item"].get(first["speaker_key"]) or "")
    first_body_text = str(first["item"].get(first["body_text_key"]) or "")
    if not first_speaker.strip() or not first_body_text.strip():
        return False
    body_counts = []
    for row in rows:
        body_text = str(row["item"].get(row["body_text_key"]) or "")
        if not body_text.strip():
            return False
        body_counts.append(len(body_text.split("\n")))
    cn_lines = cn_text.split("\n")
    expected_total = 1 + sum(body_counts)
    if len(cn_lines) != expected_total:
        return False
    speaker_cn = cn_lines[0]
    cursor = 1
    changed = False
    for index, row in enumerate(rows):
        line_count = body_counts[index]
        body_cn = "\n".join(cn_lines[cursor : cursor + line_count])
        cursor += line_count
        item = row["item"]
        chinese_key = str(row["chinese_key"])
        speaker_cn_key = str(row["speaker_cn_key"])
        body_cn_key = str(row["body_cn_key"])
        speaker_source = str(row.get("speaker_source") or "")
        projected_cn = body_cn if speaker_source == "inherited" else speaker_cn + ("\n" + body_cn if body_cn else "")
        if not str(item.get(chinese_key) or "").strip():
            item[chinese_key] = projected_cn
            changed = True
        if speaker_cn_key and not str(item.get(speaker_cn_key) or "").strip():
            item[speaker_cn_key] = speaker_cn
            changed = True
        if body_cn_key and not str(item.get(body_cn_key) or "").strip():
            item[body_cn_key] = body_cn
            changed = True
    return changed


def _project_canonical_sequence_cn(
    data: dict,
    *,
    file_name: str,
    cache_bundle: dict[str, object],
) -> int:
    canonical_records = cache_bundle.get("canonical")
    if not isinstance(canonical_records, dict):
        return 0
    changed_count = 0
    for side_name in ("script", "scriptf"):
        rows = _make_side_rows(data, side_name)
        idx = 0
        while idx < len(rows):
            start = rows[idx]
            item = start["item"]
            speaker = str(item.get(start["speaker_key"]) or "")
            speaker_source = str(start.get("speaker_source") or "")
            if not speaker.strip() or speaker_source == "inherited":
                idx += 1
                continue
            scene_idx = item.get("scene_idx")
            chain: List[dict] = [start]
            next_idx = idx + 1
            while next_idx < len(rows):
                candidate = rows[next_idx]
                candidate_item = candidate["item"]
                if candidate_item.get("scene_idx") != scene_idx:
                    break
                if str(candidate.get("speaker_source") or "") != "inherited":
                    break
                if str(candidate_item.get(candidate["speaker_key"]) or "") != speaker:
                    break
                chain.append(candidate)
                next_idx += 1
            matched = False
            for chain_len in range(len(chain), 1, -1):
                candidate_rows = chain[:chain_len]
                combined_jp = str(candidate_rows[0]["item"].get(candidate_rows[0]["text_key"]) or "")
                if not combined_jp.strip():
                    continue
                valid = True
                for follower in candidate_rows[1:]:
                    follower_body = str(follower["item"].get(follower["body_text_key"]) or "")
                    if not follower_body.strip():
                        valid = False
                        break
                    combined_jp += "\n" + follower_body
                if not valid:
                    continue
                record = _resolve_canonical_sequence_record(
                    canonical_records,
                    file_name=file_name,
                    side_name=side_name,
                    text=combined_jp,
                    scene_idx=candidate_rows[0]["item"].get("scene_idx"),
                    scene_entry_index=candidate_rows[0]["item"].get("scene_entry_index"),
                )
                if not isinstance(record, dict):
                    continue
                cn_text = str(record.get("cn") or "")
                if not cn_text.strip():
                    continue
                if _fill_sequence_rows(candidate_rows, cn_text=cn_text):
                    changed_count += 1
                matched = True
                break
            idx += len(chain) if matched else 1
    return changed_count


def sync_json_file(
    path: Path,
    cache_bundle: dict[str, object],
    *,
    clear_unmatched: bool,
) -> tuple[int, int, int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise CacheSyncFailure(f"{path.name}: JSON 根对象必须是 dict")

    rl_mode = path.name.upper() == "RL.MGP.JSON"
    eve_mode = path.name.upper() == "EVE.MGP.JSON"
    if rl_mode:
        cache_group = "rl"
    elif eve_mode:
        cache_group = "eve"
    else:
        cache_group = "canonical"
    synced = 0
    covered = 0
    missing = 0

    base_category_side = {
        "common": "common",
        "script_only": "script",
        "scriptf_only": "scriptf",
    }
    for cat in ("common", "script_only", "scriptf_only"):
        for item in data.get(cat, []):
            if not isinstance(item, dict):
                continue
            changed, found = _sync_item(
                item,
                file_name=path.name,
                cache_bundle=cache_bundle,
                cache_group=cache_group,
                rl_mode=rl_mode,
                side_name=base_category_side[cat],
                text_key="text",
                chinese_key="chinese",
                offset_key="offset",
                speaker_source_key="speaker_source",
                speaker_cn_key="speaker_cn",
                body_cn_key="body_cn",
                speaker_key="speaker",
                body_text_key="body_text",
                text_family_key="vm_text_family",
                storage_lines_key="storage_jp_lines",
                parts_key="parts",
                part_line_map_key="part_line_map",
                part_lines_key="part_lines",
                record_part_line_keys=("part_lines",),
                clear_unmatched=clear_unmatched,
            )
            if found:
                covered += 1
                if changed:
                    synced += 1
            elif cat != "common" and isinstance(item.get("text"), str) and str(item.get("text")).strip():
                missing += 1
            if cat == "common":
                common_side_changed = False
                for side in ("script", "scriptf"):
                    side_changed, side_found = _sync_item(
                        item,
                        file_name=path.name,
                        cache_bundle=cache_bundle,
                        cache_group=cache_group,
                        rl_mode=rl_mode,
                        side_name=side,
                        text_key=f"{side}_text" if f"{side}_text" in item else "text",
                        chinese_key=f"{side}_chinese",
                        offset_key=f"{side}_offset" if f"{side}_offset" in item else "offset",
                        speaker_source_key=f"{side}_speaker_source" if f"{side}_speaker_source" in item else "speaker_source",
                        speaker_cn_key=f"{side}_speaker_cn",
                        body_cn_key=f"{side}_body_cn",
                        speaker_key=f"{side}_speaker" if f"{side}_speaker" in item else "speaker",
                        body_text_key=f"{side}_body_text" if f"{side}_body_text" in item else "body_text",
                        text_family_key=f"{side}_vm_text_family" if f"{side}_vm_text_family" in item else "vm_text_family",
                        storage_lines_key=f"{side}_storage_jp_lines" if f"{side}_storage_jp_lines" in item else "storage_jp_lines",
                        parts_key=f"{side}_parts" if f"{side}_parts" in item else "parts",
                        part_line_map_key=f"{side}_part_line_map" if f"{side}_part_line_map" in item else "part_line_map",
                        part_lines_key=f"{side}_part_lines",
                        record_part_line_keys=(f"{side}_part_lines", "part_lines"),
                        clear_unmatched=clear_unmatched,
                    )
                    if side_found:
                        covered += 1
                        if side_changed:
                            common_side_changed = True
                        continue
                    if _sync_common_side_cn_fields(item, side=side):
                        common_side_changed = True
                    if _sync_common_side_part_lines(item, side=side):
                        common_side_changed = True
                if common_side_changed:
                    synced += 1

    for item in data.get("different", []):
        if not isinstance(item, dict):
            continue
        for side in ("script", "scriptf"):
            changed, found = _sync_item(
                item,
                file_name=path.name,
                cache_bundle=cache_bundle,
                cache_group=cache_group,
                rl_mode=rl_mode,
                side_name=side,
                text_key=f"{side}_text",
                chinese_key=f"{side}_chinese",
                offset_key=f"{side}_offset" if f"{side}_offset" in item else "offset",
                speaker_source_key=f"{side}_speaker_source",
                speaker_cn_key=f"{side}_speaker_cn",
                body_cn_key=f"{side}_body_cn",
                speaker_key=f"{side}_speaker",
                body_text_key=f"{side}_body_text",
                text_family_key=f"{side}_vm_text_family",
                storage_lines_key=f"{side}_storage_jp_lines",
                parts_key=f"{side}_parts",
                part_line_map_key=f"{side}_part_line_map",
                part_lines_key=f"{side}_part_lines",
                record_part_line_keys=(f"{side}_part_lines", "part_lines"),
                clear_unmatched=clear_unmatched,
            )
            if found:
                covered += 1
                if changed:
                    synced += 1
            elif isinstance(item.get(f"{side}_text"), str) and str(item.get(f"{side}_text")).strip():
                missing += 1

    if cache_group == "canonical":
        projected = _project_canonical_sequence_cn(
            data,
            file_name=path.name,
            cache_bundle=cache_bundle,
        )
        synced += projected

    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return synced, covered, missing


def main() -> None:
    parser = argparse.ArgumentParser(description="将三份权威 cache 精确同步回 translated JSON")
    parser.add_argument("--canonical-cache", type=Path, default=DEFAULT_CANONICAL_CACHE_PATH)
    parser.add_argument("--eve-cache", type=Path, default=DEFAULT_EVE_CACHE_PATH)
    parser.add_argument("--rl-cache", type=Path, default=DEFAULT_RL_CACHE_PATH)
    parser.add_argument("--claude-canonical-output", action="append", type=Path, default=[])
    parser.add_argument("--claude-eve-output", action="append", type=Path, default=[])
    parser.add_argument("--claude-rl-output", action="append", type=Path, default=[])
    parser.add_argument("--translated-dir", type=Path, default=DEFAULT_TRANSLATED_DIR)
    parser.add_argument("--clear-unmatched", action="store_true")
    args = parser.parse_args()

    if not args.translated_dir.exists():
        print(f"错误: 翻译目录不存在: {args.translated_dir}")
        return

    try:
        cache_bundle = load_cache_bundle(
            canonical_cache_path=args.canonical_cache,
            eve_cache_path=args.eve_cache,
            rl_cache_path=args.rl_cache,
            claude_canonical_output_path=args.claude_canonical_output,
            claude_eve_output_path=args.claude_eve_output,
            claude_rl_output_path=args.claude_rl_output,
        )
    except CacheSyncFailure as exc:
        print(f"错误: {exc}")
        return

    json_files = _collect_json_files(args.translated_dir)
    if not json_files:
        print("错误: 没有可处理的 JSON")
        return

    print("=" * 60)
    print("以三份权威 cache 精确覆盖 translated")
    print("  规则: 默认精确读取 translation_cache / EVE_cache / RL_cache")
    print("  规则: 默认不读取任何 reviewed JSONL；只有显式传参时才按 (target_cache, file, side, offset) 精确补齐")
    print("  规则: canonical 按 file + offset + exact JP 命中；EVE/RL 按 exact JP 或 speaker+body exact 命中")
    if args.clear_unmatched:
        print("  规则: 未命中权威 cache 的现有中文与 part_lines 会被清空")
    print("=" * 60)
    print()
    _print_cache_summary(cache_bundle, group="canonical", path=args.canonical_cache)
    _print_cache_summary(cache_bundle, group="eve", path=args.eve_cache)
    _print_cache_summary(cache_bundle, group="rl", path=args.rl_cache)
    reviewed_bundle = cache_bundle["reviewed"]
    print(f"加载 reviewed canonical 输出: {args.claude_canonical_output}")
    print(f"  记录数: {len(reviewed_bundle['canonical'])}")
    print(f"加载 reviewed EVE 输出: {args.claude_eve_output}")
    print(f"  记录数: {len(reviewed_bundle['eve'])}")
    print(f"加载 reviewed RL 输出: {args.claude_rl_output}")
    print(f"  记录数: {len(reviewed_bundle['rl'])}")

    total_synced = 0
    total_covered = 0
    total_missing = 0

    try:
        for json_path in json_files:
            synced, covered, missing = sync_json_file(
                json_path,
                cache_bundle,
                clear_unmatched=args.clear_unmatched,
            )
            print(f"  {json_path.name}: 同步 {synced}, cache覆盖 {covered}, cache缺失 {missing}")
            total_synced += synced
            total_covered += covered
            total_missing += missing
    except CacheSyncFailure as exc:
        print(f"\n错误: {exc}")
        return

    print("\n完成")
    print(f"  总同步: {total_synced}")
    print(f"  总覆盖: {total_covered}")
    print(f"  总缺失: {total_missing}")


if __name__ == "__main__":
    main()
