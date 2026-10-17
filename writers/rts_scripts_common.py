from __future__ import annotations

import re
import struct
from pathlib import Path
from typing import Any


PACK_ENTRY_SIZE = 0x50
MESSAGE_RE = re.compile(r"^\s*Message\(([^)]*)\)\s*$")
LABEL_RE = re.compile(r"^#([^\r\n]+)$")
COMMAND_RE = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\(")
MENU_QUOTED_RE = re.compile(r'"([^"]+)"')


def load_text_tbl(tbl_path: Path) -> dict[str, bytes]:
    char_map: dict[str, bytes] = {}
    with open(tbl_path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\r\n")
            if not line or line.startswith("#") or "=" not in line:
                continue
            hex_code, chars = line.split("=", 1)
            hex_code = hex_code.strip()
            if not hex_code:
                continue
            try:
                code_bytes = bytes.fromhex(hex_code)
            except ValueError:
                continue
            for char in chars:
                char_map.setdefault(char, code_bytes)
    return char_map


def load_pack_entries(pack_path: Path) -> tuple[bytes, list[dict[str, Any]]]:
    data = pack_path.read_bytes()
    if len(data) < 8 or data[:4] != b"PACK":
        raise ValueError(f"unexpected RTS pack header: {pack_path}")

    count = struct.unpack_from("<I", data, 4)[0]
    toc_base = 0x10
    data_base = toc_base + count * PACK_ENTRY_SIZE
    entries: list[dict[str, Any]] = []

    for index in range(count):
        entry_off = toc_base + index * PACK_ENTRY_SIZE
        raw = data[entry_off:entry_off + PACK_ENTRY_SIZE]
        kind = raw[:8].split(b"\0", 1)[0].decode("ascii", errors="ignore")
        rel_offset = struct.unpack_from("<I", raw, 8)[0]
        size = struct.unpack_from("<I", raw, 12)[0]
        name = raw[16:].split(b"\0", 1)[0].decode("ascii", errors="ignore")
        abs_offset = data_base + rel_offset
        blob = data[abs_offset:abs_offset + size]
        entries.append(
            {
                "index": index,
                "kind": kind,
                "name": name,
                "rel_offset": rel_offset,
                "abs_offset": abs_offset,
                "size": size,
                "blob": blob,
                "header_offset": entry_off,
            }
        )
    return data[:data_base], entries


def rebuild_pack(prefix: bytes, entries: list[dict[str, Any]]) -> bytes:
    out = bytearray(prefix)
    rel_offset = 0
    payload = bytearray()

    for entry in entries:
        struct.pack_into("<I", out, entry["header_offset"] + 8, rel_offset)
        struct.pack_into("<I", out, entry["header_offset"] + 12, len(entry["blob"]))
        payload.extend(entry["blob"])
        rel_offset += len(entry["blob"])

    out.extend(payload)
    return bytes(out)


def decode_inf_blob(blob: bytes) -> bytes:
    return bytes((byte ^ 0xFF) for byte in blob)


def encode_inf_blob(decoded: bytes) -> bytes:
    return bytes((byte ^ 0xFF) for byte in decoded)


def split_lines_with_offsets(data: bytes) -> list[dict[str, Any]]:
    lines: list[dict[str, Any]] = []
    pos = 0
    while pos < len(data):
        newline = data.find(b"\n", pos)
        if newline == -1:
            end = len(data)
            raw_line = data[pos:end]
            line_ending = b""
        else:
            end = newline + 1
            raw_line = data[pos:end]
            line_ending = b"\r\n" if raw_line.endswith(b"\r\n") else b"\n"

        content = raw_line[: len(raw_line) - len(line_ending)] if line_ending else raw_line
        text = content.decode("cp932", errors="replace")
        lines.append(
            {
                "start": pos,
                "end": end,
                "raw_line": raw_line,
                "content": content,
                "line_ending": line_ending,
                "text": text,
            }
        )
        pos = end
    return lines


def parse_message_params(raw_params: str) -> dict[str, Any]:
    parts = [part.strip() for part in raw_params.split(",")]
    result: dict[str, Any] = {"raw": raw_params, "parts": parts}
    if len(parts) >= 1:
        try:
            result["speaker_id"] = int(parts[0], 0)
        except ValueError:
            result["speaker_id"] = parts[0]
    if len(parts) >= 2:
        try:
            result["window_y"] = int(parts[1], 0)
        except ValueError:
            result["window_y"] = parts[1]
    if len(parts) >= 3:
        try:
            result["window_width"] = int(parts[2], 0)
        except ValueError:
            result["window_width"] = parts[2]
    return result


def body_bytes_to_text(body: bytes) -> str:
    lines = split_lines_with_offsets(body)
    pages: list[list[str]] = [[]]

    for line in lines:
        text = str(line["text"]).rstrip("\r")
        if text == ";;":
            pages.append([])
            continue
        if text.endswith("\\"):
            text = text[:-1]
        pages[-1].append(text)

    while len(pages) > 1 and not pages[-1]:
        pages.pop()

    page_texts = ["\n".join(page) for page in pages]
    return "\n\n".join(page_texts)


def text_to_pages(text: str) -> list[list[str]]:
    lines = text.replace("\r", "").split("\n")
    pages: list[list[str]] = [[]]
    saw_nonempty = False

    for line in lines:
        if line == "":
            if saw_nonempty:
                pages.append([])
                saw_nonempty = False
            else:
                pages[-1].append("")
            continue
        pages[-1].append(line)
        saw_nonempty = True

    while len(pages) > 1 and not pages[-1]:
        pages.pop()
    return pages


def encode_visible_text(text: str, char_map: dict[str, bytes]) -> bytes:
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == "<" and i + 3 < len(text) and text[i + 3] == ">":
            hex_part = text[i + 1 : i + 3]
            if all(char in "0123456789abcdefABCDEF" for char in hex_part):
                out.append(int(hex_part, 16))
                i += 4
                continue

        char = text[i]
        if char in char_map:
            out.extend(char_map[char])
        else:
            try:
                out.extend(char.encode("ascii"))
            except UnicodeEncodeError:
                try:
                    out.extend(char.encode("cp932"))
                except UnicodeEncodeError as exc:
                    raise ValueError(f"cannot encode RTS character: {char!r}") from exc
        i += 1
    return bytes(out)


def text_to_body_bytes(text: str, char_map: dict[str, bytes]) -> bytes:
    pages = text_to_pages(text)
    chunks: list[bytes] = []

    for page_index, page in enumerate(pages):
        for line_index, line in enumerate(page):
            encoded_line = bytearray(encode_visible_text(line, char_map))
            if line_index < len(page) - 1:
                encoded_line.extend(b"\\")
            chunks.append(bytes(encoded_line) + b"\r\n")
        if page_index < len(pages) - 1:
            chunks.append(b";;\r\n")

    return b"".join(chunks)


def parse_inf_messages(decoded: bytes) -> list[dict[str, Any]]:
    lines = split_lines_with_offsets(decoded)
    label = ""
    messages: list[dict[str, Any]] = []
    message_index = 0
    i = 0

    while i < len(lines):
        line_text = str(lines[i]["text"])
        label_match = LABEL_RE.match(line_text)
        if label_match:
            label = label_match.group(1).strip()
            i += 1
            continue

        stripped = line_text.lstrip()
        if stripped.startswith(";Message("):
            i += 1
            continue

        match = MESSAGE_RE.match(line_text)
        if not match:
            i += 1
            continue

        params = parse_message_params(match.group(1))
        body_start = lines[i + 1]["start"] if i + 1 < len(lines) else lines[i]["end"]
        start_line = i + 2
        i += 1

        while i < len(lines):
            candidate_text = str(lines[i]["text"])
            candidate_stripped = candidate_text.strip()
            if candidate_stripped == "&x":
                body_end = lines[i]["start"]
                break
            if MESSAGE_RE.match(candidate_text) or candidate_text.lstrip().startswith(";Message("):
                body_end = lines[i]["start"]
                break
            if candidate_stripped == "}":
                body_end = lines[i]["start"]
                break
            if COMMAND_RE.match(candidate_text) and candidate_stripped != ";;":
                body_end = lines[i]["start"]
                break
            i += 1
        else:
            body_end = len(decoded)

        body = decoded[body_start:body_end]
        messages.append(
            {
                "message_index": message_index,
                "label": label,
                "start_line": start_line,
                "command": f"Message({match.group(1)})",
                "body_start": body_start,
                "body_end": body_end,
                "body_bytes": body,
                "body_text": body.decode("cp932", errors="replace"),
                "text": body_bytes_to_text(body),
                **params,
            }
        )
        message_index += 1

        if i < len(lines) and lines[i]["text"].strip() == "&x":
            i += 1

    return messages


def parse_menu_inf_strings(decoded: bytes) -> list[str]:
    text = decoded.decode("cp932", errors="replace")
    matches = MENU_QUOTED_RE.findall(text)
    unique: list[str] = []
    seen: set[str] = set()
    for s in matches:
        if s in seen:
            continue
        seen.add(s)
        if all(ord(c) < 128 for c in s):
            continue
        unique.append(s)
    unique.sort()
    return unique
