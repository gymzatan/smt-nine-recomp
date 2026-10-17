# -*- coding: utf-8 -*-
"""Helpers for explicit XBE percent-related markup.

`default.xbe` mixes true format tokens (`%s`, `%02d`, `%5`, `%c`) with
visible percent signs (`%` / `％`). We persist the JP-side percent sequence
into JSON so writeback does not need to guess whether a translated `%` should
remain halfwidth or become a visible fullwidth `％`.
"""

from __future__ import annotations

import re

from fullwidth_ascii import is_visible_asciiish, to_halfwidth_char


FORMAT_TOKEN_RE = re.compile(r"%(?:[-+#0 ]*\d*(?:\.\d+)?)?(?:[A-Za-z]|\d+)")
PERCENT_CHARS = {"%", "％"}


def _consume_percent_fragment(text: str, start: int) -> tuple[int, str]:
    if text[start] not in PERCENT_CHARS:
        raise ValueError("fragment must start on '%' or '％'")

    end = start + 1
    while end < len(text):
        char = text[end]
        if char in "\n\r\t\x00" or not is_visible_asciiish(char):
            break
        end += 1

    candidate = "".join(to_halfwidth_char(char) for char in text[start:end])
    match = FORMAT_TOKEN_RE.match(candidate)
    if match:
        token = candidate[:match.end()]
        return len(token), token

    return 1, text[start]


def extract_percent_marks(text: str) -> list[str]:
    marks: list[str] = []
    idx = 0
    while idx < len(text):
        if text[idx] not in PERCENT_CHARS:
            idx += 1
            continue
        frag_len, fragment = _consume_percent_fragment(text, idx)
        marks.append(fragment)
        idx += frag_len
    return marks


def apply_percent_marks(text: str, percent_marks: list[str] | None) -> str:
    if not percent_marks:
        return text

    out: list[str] = []
    idx = 0
    mark_idx = 0
    while idx < len(text):
        if text[idx] not in PERCENT_CHARS:
            out.append(text[idx])
            idx += 1
            continue

        if mark_idx >= len(percent_marks):
            out.append(text[idx])
            idx += 1
            continue

        frag_len, _ = _consume_percent_fragment(text, idx)
        out.append(percent_marks[mark_idx])
        mark_idx += 1
        idx += frag_len

    return "".join(out)
