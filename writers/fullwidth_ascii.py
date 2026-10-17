# -*- coding: utf-8 -*-
"""
可见 ASCII 全角化工具。

只处理最终会显示在游戏里的可见字符，不碰：
- Script 控制标记，如 <FF><01>
- XBE 格式占位符，如 %s / %02d / %03.01f
"""

from __future__ import annotations

import re


CONTROL_TAG_RE = re.compile(r"<[0-9A-Fa-f]{2}>")
# XBE strings use both standard C-style tokens (`%s`, `%02d`, `%03.01f`)
# and game-specific numeric placeholders such as `%5`.
# Bare `%` is treated as a visible literal, not as a format token.
FORMAT_TOKEN_RE = re.compile(r"%(?:[-+#0 ]*\d*(?:\.\d+)?)?(?:[A-Za-z]|\d+)")

ASCII_SYMBOL_MAP = {
    " ": "　",
    "!": "！",
    "#": "＃",
    "$": "＄",
    "%": "％",
    "&": "＆",
    "(": "（",
    ")": "）",
    "*": "＊",
    "+": "＋",
    ",": "，",
    "-": "ー",
    ".": "．",
    "/": "／",
    ":": "：",
    ";": "；",
    "<": "＜",
    "=": "＝",
    ">": "＞",
    "?": "？",
    "@": "＠",
    "[": "［",
    "\\": "＼",
    "]": "］",
    "^": "＾",
    "_": "＿",
    "`": "｀",
    "{": "｛",
    "|": "｜",
    "}": "｝",
    "~": "～",
}
FULL_TO_HALF_SYMBOL_MAP = {value: key for key, value in ASCII_SYMBOL_MAP.items()}


def is_visible_asciiish(char: str) -> bool:
    code = ord(char)
    return (
        char in {"“", "”", "‘", "’", "　"}
        or 0x20 <= code <= 0x7E
        or 0xFF01 <= code <= 0xFF5E
    ) and char not in "\n\r\t\x00"


def to_fullwidth_char(char: str) -> str:
    if char in ASCII_SYMBOL_MAP:
        return ASCII_SYMBOL_MAP[char]

    code = ord(char)
    if 0x30 <= code <= 0x39:
        return chr(code - 0x30 + 0xFF10)
    if 0x41 <= code <= 0x5A:
        return chr(code - 0x41 + 0xFF21)
    if 0x61 <= code <= 0x7A:
        return chr(code - 0x61 + 0xFF41)
    return char


def to_halfwidth_char(char: str) -> str:
    if char in {"“", "”"}:
        return '"'
    if char in {"‘", "’"}:
        return "'"
    if char in FULL_TO_HALF_SYMBOL_MAP:
        return FULL_TO_HALF_SYMBOL_MAP[char]

    code = ord(char)
    if 0xFF10 <= code <= 0xFF19:
        return chr(code - 0xFF10 + 0x30)
    if 0xFF21 <= code <= 0xFF3A:
        return chr(code - 0xFF21 + 0x41)
    if 0xFF41 <= code <= 0xFF5A:
        return chr(code - 0xFF41 + 0x61)
    return char


def infer_ascii_style(
    reference: str,
    *,
    preserve_control_tags: bool = True,
    preserve_format_tokens: bool = False,
) -> str:
    has_full = False
    has_half = False

    i = 0
    while i < len(reference):
        if preserve_control_tags and reference[i] == "<":
            match = CONTROL_TAG_RE.match(reference, i)
            if match:
                i = match.end()
                continue

        if preserve_format_tokens and reference[i] == "%":
            match = FORMAT_TOKEN_RE.match(reference, i)
            if match:
                i = match.end()
                continue

        char = reference[i]
        code = ord(char)
        if char in {"“", "”", "‘", "’", "　"} or 0xFF01 <= code <= 0xFF5E:
            has_full = True
        elif 0x20 <= code <= 0x7E and char not in "\n\r\t\x00":
            has_half = True
        i += 1

    if has_full and not has_half:
        return "full"
    if has_half and not has_full:
        return "half"
    if has_full:
        return "full"
    if has_half:
        return "half"
    return "keep"


def normalize_format_tokens_halfwidth(
    text: str,
    *,
    preserve_control_tags: bool = True,
) -> str:
    """Canonicalize visible format tokens like ％０２ｄ back to halfwidth %02d."""
    out: list[str] = []
    i = 0

    while i < len(text):
        if preserve_control_tags and text[i] == "<":
            match = CONTROL_TAG_RE.match(text, i)
            if match:
                out.append(match.group(0))
                i = match.end()
                continue

        if text[i] not in {"%", "％"}:
            out.append(text[i])
            i += 1
            continue

        j = i
        while j < len(text):
            if preserve_control_tags and text[j] == "<":
                break
            if text[j] in "\n\r\t\x00":
                break
            if not is_visible_asciiish(text[j]):
                break
            j += 1

        candidate = "".join(to_halfwidth_char(char) for char in text[i:j])
        match = FORMAT_TOKEN_RE.match(candidate)
        if match:
            token = candidate[:match.end()]
            out.append(token)
            i += len(token)
            continue

        out.append(text[i])
        i += 1

    return "".join(out)


def _normalize_asciiish_token_halfwidth(token: str) -> str:
    return "".join(to_halfwidth_char(char) for char in token)


def _normalize_asciiish_token_fullwidth(token: str) -> str:
    out: list[str] = []
    open_double_quote = True
    open_single_quote = True

    for char in token:
        if char in {'"', "＂"}:
            out.append("“" if open_double_quote else "”")
            open_double_quote = not open_double_quote
        elif char in {"'", "＇"}:
            out.append("‘" if open_single_quote else "’")
            open_single_quote = not open_single_quote
        else:
            out.append(to_fullwidth_char(char))

    return "".join(out)


def _extract_reference_ascii_tokens(
    reference: str,
    *,
    preserve_control_tags: bool = True,
    preserve_format_tokens: bool = False,
) -> dict[str, str]:
    tokens: dict[str, str] = {}
    i = 0

    while i < len(reference):
        if preserve_control_tags and reference[i] == "<":
            match = CONTROL_TAG_RE.match(reference, i)
            if match:
                i = match.end()
                continue

        if preserve_format_tokens and reference[i] == "%":
            match = FORMAT_TOKEN_RE.match(reference, i)
            if match:
                i = match.end()
                continue

        if not is_visible_asciiish(reference[i]):
            i += 1
            continue

        start = i
        while i < len(reference):
            if preserve_control_tags and reference[i] == "<":
                break
            if preserve_format_tokens and reference[i] == "%":
                match = FORMAT_TOKEN_RE.match(reference, i)
                if match:
                    break
            if not is_visible_asciiish(reference[i]):
                break
            i += 1

        token = reference[start:i]
        normalized = _normalize_asciiish_token_halfwidth(token)
        if normalized and normalized not in tokens:
            tokens[normalized] = token

    return tokens


def normalize_visible_ascii(
    text: str,
    *,
    preserve_control_tags: bool = True,
    preserve_format_tokens: bool = False,
) -> str:
    out: list[str] = []
    i = 0
    open_double_quote = True
    open_single_quote = True
    while i < len(text):
        if preserve_control_tags and text[i] == "<":
            match = CONTROL_TAG_RE.match(text, i)
            if match:
                out.append(match.group(0))
                i = match.end()
                continue

        if preserve_format_tokens and text[i] == "%":
            match = FORMAT_TOKEN_RE.match(text, i)
            if match:
                out.append(match.group(0))
                i = match.end()
                continue

        char = text[i]
        if char in "\n\r\t\x00":
            out.append(char)
        elif char in {'"', '＂'}:
            out.append("“" if open_double_quote else "”")
            open_double_quote = not open_double_quote
        elif char in {"'", "＇"}:
            out.append("‘" if open_single_quote else "’")
            open_single_quote = not open_single_quote
        else:
            out.append(to_fullwidth_char(char))
        i += 1

    return "".join(out)


def normalize_visible_ascii_like_reference(
    text: str,
    reference: str,
    *,
    preserve_control_tags: bool = True,
    preserve_format_tokens: bool = False,
) -> str:
    token_map = _extract_reference_ascii_tokens(
        reference,
        preserve_control_tags=preserve_control_tags,
        preserve_format_tokens=preserve_format_tokens,
    )
    style = infer_ascii_style(
        reference,
        preserve_control_tags=preserve_control_tags,
        preserve_format_tokens=preserve_format_tokens,
    )

    out: list[str] = []
    i = 0
    while i < len(text):
        if preserve_control_tags and text[i] == "<":
            match = CONTROL_TAG_RE.match(text, i)
            if match:
                out.append(match.group(0))
                i = match.end()
                continue

        if preserve_format_tokens and text[i] == "%":
            match = FORMAT_TOKEN_RE.match(text, i)
            if match:
                out.append(match.group(0))
                i = match.end()
                continue

        if text[i] in "\n\r\t\x00":
            out.append(text[i])
            i += 1
            continue

        if is_visible_asciiish(text[i]):
            start = i
            while i < len(text):
                if preserve_control_tags and text[i] == "<":
                    break
                if preserve_format_tokens and text[i] == "%":
                    match = FORMAT_TOKEN_RE.match(text, i)
                    if match:
                        break
                if not is_visible_asciiish(text[i]):
                    break
                i += 1

            token = text[start:i]
            normalized = _normalize_asciiish_token_halfwidth(token)
            if normalized in token_map:
                out.append(token_map[normalized])
            elif style == "full":
                out.append(_normalize_asciiish_token_fullwidth(token))
            elif style == "half":
                out.append(_normalize_asciiish_token_halfwidth(token))
            else:
                out.append(token)
            continue

        out.append(text[i])
        i += 1

    return "".join(out)
