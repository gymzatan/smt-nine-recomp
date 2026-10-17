#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMT: NINE (OG Xbox) sys_f24.xpr 中文字库构建脚本（“干净边框/无杂质/保留符号区”版）

目标：
- 仅替换 sysfont.tbl 的 f24 表中“汉字(kanji)槽位”对应的 24×24 字形；
- 保留所有“非汉字/假名”的符号、拉丁字母、数字、图形等槽位（字节级不改）；
- 输出一个仍然是 9×(512×512 BC1/DXT1) 的 sys_f24.xpr，可直接覆盖原文件；
- 生成“中文字符 -> Shift-JIS 码位(两字节)”映射表（CSV/JSON/TBL）。
- 追加输出完整槽位码表（含符号/假名，未改动保持原样）：*_full.csv

核心原则（解决你遇到的“边框缺口/串色/杂点”）：
- 不让压缩器“自由发挥”。我们直接按 BC1(DXT1) block 写入：
  * 透明 block 强制使用 BC1 透明模式，并且所有透明像素用 index=3（完全透明）；
  * 新字形仅使用：描边色 + 填充色 + 透明（三种状态）；
  * 编码时只使用端点色 index(0/1) 与透明 index(3)，不使用插值色 index(2)，从根源杜绝“串色”。

依赖：
  pip install pillow numpy pypinyin

用法示例：
  python build_smt_nine_f24.py ^
    --xpr-in sys_f24.xpr ^
    --tbl-in sysfont.tbl ^
    --ttf SimSun.ttf ^
    --mapping-json final_game_f24.json ^
    --demon-names-md "SMT_NINE_demon_names.md" ^
    --skill-names-md "SMT_NINE_skill_names.md" ^
    --gb2312-n 3204 ^
    --out-xpr sys_f24_final_game.xpr ^
    --out-prefix final_game_f24

说明：
- --gb2312-n：从 GB2312 一级字表按顺序取前 N 个（作为“常用字”来源）。
  你的 sysfont.tbl(f24) 里“可用唯一汉字槽位”大约只有 3204 个，所以 N 建议 <= 3204。
- 你要额外补字：用 --extra-chars-file 指向一个 UTF-8 文本文件（随便一行一堆字都行）。

作者注：这个脚本只做 f24（24×24）——因为它恰好 4×4 block 对齐，才能做到真正“干净”。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont


# -----------------------------
# Shift-JIS 分类（用于“保留符号区”）
# -----------------------------

def is_sjis_kana(code: int) -> bool:
    # ひらがな: 829F-82F1, カタカナ: 8340-8396
    return (0x829F <= code <= 0x82F1) or (0x8340 <= code <= 0x8396)

def is_sjis_kanji(code: int) -> bool:
    # 粗分类：原始 kanji 区 + 扩容后自定义双字节槽位
    b1 = (code >> 8) & 0xFF
    return (0x88 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xFC)

def decode_sjis_char(code: int) -> str:
    if code == 0:
        return ""
    if code <= 0xFF:
        b = bytes([code & 0xFF])
    else:
        b = bytes([(code >> 8) & 0xFF, code & 0xFF])
    for enc in ("shift_jis", "cp932"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            continue
    return ""

def is_cjk_unified_ideograph(ch: str) -> bool:
    if not ch:
        return False
    cp = ord(ch)
    # CJK Unified Ideographs + 扩展 A/B/C...（简单覆盖，够用）
    return (
        0x4E00 <= cp <= 0x9FFF
        or 0x3400 <= cp <= 0x4DBF
        or 0x20000 <= cp <= 0x2A6DF
        or 0x2A700 <= cp <= 0x2B73F
        or 0x2B740 <= cp <= 0x2B81F
        or 0x2B820 <= cp <= 0x2CEAF
        or 0x2CEB0 <= cp <= 0x2EBEF
    )


# -----------------------------
# sysfont.tbl 解析（只取 f24 表）
# -----------------------------

@dataclass(frozen=True)
class F24Table:
    header8: bytes
    codes: List[int]

def read_sysfont_tbl_f18_codes(tbl_path: Path) -> List[int]:
    data = tbl_path.read_bytes()
    if len(data) < 16:
        raise ValueError("sysfont.tbl 太小，无法解析。")
    off2 = struct.unpack("<I", data[:4])[0]
    if off2 <= 4 or off2 >= len(data):
        raise ValueError(f"sysfont.tbl 表1/表2分界异常: {off2:#x}")
    head = data[4:off2]
    if len(head) % 2 != 0:
        raise ValueError("sysfont.tbl 表1数据长度不是2的倍数。")
    return [struct.unpack(">H", head[i:i+2])[0] for i in range(0, len(head), 2)]

def read_sysfont_tbl_f24(tbl_path: Path) -> F24Table:
    data = tbl_path.read_bytes()
    if len(data) < 16:
        raise ValueError("sysfont.tbl 太小，无法解析。")
    off2 = struct.unpack("<I", data[:4])[0]
    if off2 <= 0 or off2 + 8 >= len(data):
        raise ValueError(f"sysfont.tbl 表2偏移异常: {off2:#x}")
    header8 = data[off2:off2+8]
    tail = data[off2+8:]
    if len(tail) % 2 != 0:
        raise ValueError("sysfont.tbl 表2数据长度不是2的倍数。")
    codes = [struct.unpack(">H", tail[i:i+2])[0] for i in range(0, len(tail), 2)]
    return F24Table(header8=header8, codes=codes)


# -----------------------------
# XPR（sys_f24.xpr）读取/写回
# -----------------------------

@dataclass
class XprF24:
    header: bytes         # header_size bytes
    pages: List[bytearray]  # each 131072 bytes (DXT1 512x512)

def read_xpr_f24(xpr_path: Path) -> XprF24:
    blob = xpr_path.read_bytes()
    if len(blob) < 16 or blob[:4] != b"XPR0":
        raise ValueError("不是 XPR0 文件。")
    total_size = struct.unpack("<I", blob[4:8])[0]
    header_size = struct.unpack("<I", blob[8:12])[0]
    if total_size != len(blob):
        # 有些工具会把 total_size 写错；这里不强制，但给提示
        print(f"[warn] XPR 头 total_size={total_size} 与文件实际大小={len(blob)} 不一致，继续按实际大小处理。")
        total_size = len(blob)
    if header_size <= 0 or header_size > total_size:
        raise ValueError(f"header_size 异常: {header_size}")
    header = blob[:header_size]
    tex_blob = blob[header_size:total_size]
    if len(tex_blob) % 131072 != 0:
        raise ValueError("纹理数据长度不是 131072 的整数倍，无法按 512×512 DXT1 分页。")
    num_pages = len(tex_blob) // 131072
    pages = [bytearray(tex_blob[i*131072:(i+1)*131072]) for i in range(num_pages)]
    return XprF24(header=header, pages=pages)

def write_xpr_f24(out_path: Path, xpr: XprF24) -> None:
    out_path.write_bytes(xpr.header + b"".join(bytes(p) for p in xpr.pages))


# -----------------------------
# BC1/DXT1 编码（只写“端点色/透明索引”，不插值）
# -----------------------------

def rgb888_to_565(r: int, g: int, b: int) -> int:
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5

def pack_bc1_block(color0_565: int, color1_565: int, idx: np.ndarray) -> bytes:
    """
    idx: shape (4,4), dtype uint8, values 0..3
    BC1 索引打包：2 bits per pixel，row-major，(0,0) 是最低 2 bits。
    """
    assert idx.shape == (4, 4)
    v = 0
    bit = 0
    for y in range(4):
        for x in range(4):
            v |= (int(idx[y, x]) & 0x3) << bit
            bit += 2
    return struct.pack("<HHI", color0_565 & 0xFFFF, color1_565 & 0xFFFF, v & 0xFFFFFFFF)

def encode_cell_to_bc1_blocks(cell_cls: np.ndarray,
                              fill565: int,
                              outline565: int) -> List[bytes]:
    """
    cell_cls: (24,24) uint8,
      0=transparent, 1=fill, 2=outline, 3=edge(AA)
    return: 6*6 blocks, row-major (by,bx)

    重要：为了避免 BC1/DXT1 压缩器“自由发挥”导致串色，我们固定端点色：
      color0 = outline, color1 = fill （并强制走 BC1 透明模式：color0 <= color1）
    然后用 index：
      0=outline, 1=fill, 2=interpolate(AA), 3=transparent
    """
    assert cell_cls.shape == (24, 24)
    blocks: List[bytes] = []
    c0 = outline565 & 0xFFFF
    c1 = fill565 & 0xFFFF
    if c0 > c1:
        c0, c1 = c1, c0  # 保险：确保 BC1 透明模式
    for by in range(6):
        for bx in range(6):
            sub = cell_cls[by*4:(by+1)*4, bx*4:(bx+1)*4]
            idx = np.full((4,4), 3, dtype=np.uint8)  # 默认全透明
            idx[sub == 2] = 0  # outline -> index0
            idx[sub == 1] = 1  # fill    -> index1
            idx[sub == 3] = 2  # edge AA -> index2
            # sub==0 仍保持 index3（透明）
            blocks.append(pack_bc1_block(c0, c1, idx))
    return blocks



# -----------------------------
# 字形渲染（SimHei -> 24×24 mask -> 描边）
# -----------------------------

def dilate8(mask: np.ndarray) -> np.ndarray:
    """8 邻域膨胀，mask 为 bool"""
    h, w = mask.shape
    out = np.zeros_like(mask, dtype=bool)
    # pad 1
    p = np.pad(mask, ((1,1),(1,1)), mode="constant", constant_values=False)
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            out |= p[1+dy:1+dy+h, 1+dx:1+dx+w]
    return out

def hysteresis8(strong: np.ndarray, weak: np.ndarray) -> np.ndarray:
    """
    8 邻域滞后阈值（hysteresis）：保留所有与 strong 连通的 weak 像素。
    这比单一 threshold 稳定：细笔画/斜线不会因为某一两个像素偏暗就断掉。
    """
    assert strong.shape == weak.shape
    h, w = strong.shape
    out = np.zeros_like(strong, dtype=bool)
    if not np.any(strong):
        return out
    stack = [tuple(x) for x in np.argwhere(strong)]
    out[strong] = True
    dirs = [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]
    while stack:
        y, x = stack.pop()
        for dy, dx in dirs:
            ny = y + dy
            nx = x + dx
            if 0 <= ny < h and 0 <= nx < w and (not out[ny, nx]) and weak[ny, nx]:
                out[ny, nx] = True
                stack.append((ny, nx))
    return out


def render_char_masks(char: str,
                      font: ImageFont.FreeTypeFont,
                      oversample: int = 8,
                      cell_px: int = 24,
                      fill_threshold: int = 80,
                      edge_threshold: int = 20,
                      y_shift: int = 0,
                      outline_width: int = 1) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """
    return (fill_mask, edge_mask, outline_mask) as bool arrays shape (24,24)

    关键点：
    - 不再对 24×24 的灰度图做单一阈值（二值化），因为那会天然导致“有些笔画被啃掉”。
    - 我们在高分辨率(oversample)上渲染，然后做“面积平均”(coverage)。
      coverage 再做双阈值滞后(hysteresis)：
        * fill_threshold: 认为是“实心”的强像素
        * edge_threshold: 认为是“可能属于笔画”的弱像素
      然后把所有与强像素连通的弱像素也保留，从而保住细笔画。
    """
    edge_threshold = int(edge_threshold)
    fill_threshold = int(fill_threshold)
    if edge_threshold > fill_threshold:
        edge_threshold, fill_threshold = fill_threshold, edge_threshold

    canvas = cell_px * oversample
    img = Image.new("L", (canvas, canvas), 0)
    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0, 0), char, font=font)
    if bbox is None:
        z = np.zeros((cell_px, cell_px), dtype=bool)
        return z, z, z
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (canvas - w) // 2 - bbox[0]
    y = (canvas - h) // 2 - bbox[1] + y_shift
    draw.text((x, y), char, fill=255, font=font)

    # coverage: 24×24，每个像素等于 oversample×oversample 子像素平均亮度（0..255）
    arr = np.array(img, dtype=np.uint8)
    cov = arr.reshape(cell_px, oversample, cell_px, oversample).mean(axis=(1,3))

    strong = cov >= fill_threshold
    weak = cov >= edge_threshold
    if np.any(strong):
        body = hysteresis8(strong, weak)
    else:
        # 极端情况：整个字都很细，强阈值一个点也没有。那就退化为弱阈值。
        body = weak.copy()
        strong = body.copy()

    fill = body & strong
    edge = body & (~fill)
    expanded = body
    for _ in range(outline_width):
        expanded = dilate8(expanded)
    outline = expanded & (~body)

    # 防止描边跑出边界（硬裁）
    outline[0,:] = False
    outline[-1,:] = False
    outline[:,0] = False
    outline[:,-1] = False

    return fill, edge, outline

# -----------------------------
# 字符集构建
# -----------------------------

def gb2312_level1_chars() -> List[str]:
    """
    GB2312 一级汉字：0xB0A1-0xD7F9（共3755）
    这里按编码顺序生成，通常可作为“常用字”基底。
    """
    chars: List[str] = []
    for high in range(0xB0, 0xD8):  # B0..D7
        for low in range(0xA1, 0xFF):  # A1..FE
            b = bytes([high, low])
            try:
                ch = b.decode("gb2312")
            except UnicodeDecodeError:
                continue
            # 只取汉字
            if is_cjk_unified_ideograph(ch):
                chars.append(ch)
    # 防御：确保数量接近 3755
    return chars


def read_chars_from_file(path: Path) -> List[str]:
    txt = path.read_text(encoding="utf-8", errors="ignore")
    out: List[str] = []
    for ch in txt:
        if is_cjk_unified_ideograph(ch):
            out.append(ch)
    return out

def read_chars_from_markdown_cn_column(path: Path) -> List[str]:
    """
    Read only the Chinese translation column from the project's markdown term
    tables. This avoids pulling JP-side kanji like `兎` into the live font set.
    """
    out: List[str] = []
    for raw_line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw_line.strip()
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) < 3:
            continue
        cn_cell = cells[2]
        if cn_cell in {"中译名", "--------"}:
            continue
        cn_cell = cn_cell.replace("**", "")
        for ch in cn_cell:
            if is_cjk_unified_ideograph(ch):
                out.append(ch)
    return out

def read_chars_from_string(s: str) -> List[str]:
    out: List[str] = []
    for ch in s:
        if is_cjk_unified_ideograph(ch):
            out.append(ch)
    return out

def sort_chars_by_pinyin(chars: List[str]) -> List[str]:
    """
    纯粹为了让输出映射表更“人类友好”。
    如果没有安装 pypinyin，就退化为按 Unicode 排序（不影响游戏显示）。
    """
    try:
        from pypinyin import Style, lazy_pinyin  # type: ignore
        def key(ch: str):
            py = lazy_pinyin(ch, style=Style.NORMAL, errors="ignore")
            if py:
                return (py[0], ch)
            return ("", ch)
        return sorted(chars, key=key)
    except Exception:
        return sorted(chars, key=lambda c: ord(c))

def unique_preserve_order(seq: Iterable[str]) -> List[str]:
    seen = set()
    out = []
    for x in seq:
        if x in seen:
            continue
        seen.add(x)
        out.append(x)
    return out


# -----------------------------
# 主流程
# -----------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description="Build SMT:NINE sys_f24.xpr Chinese font (clean outline, keep symbols).")
    ap.add_argument("--xpr-in", type=Path, default=Path("sys_f24.xpr"), help="原始 sys_f24.xpr")
    ap.add_argument("--tbl-in", type=Path, default=Path("sysfont.tbl"), help="原始 sysfont.tbl")
    ap.add_argument("--ttf", type=Path, default=Path("SimSun.ttf"), help="字体文件（f24 默认应使用 SimSun.ttf）")
    ap.add_argument("--mapping-json", type=Path, default=None,
                    help="锁定已有字符->SJIS 映射，只重建字形，不重新分配槽位")

    ap.add_argument("--demon-names-md", type=Path, default=Path("SMT_NINE_demon_names.md"),
                    help="恶魔中译名 md（默认 SMT_NINE_demon_names.md）")
    ap.add_argument("--skill-names-md", type=Path, default=Path("SMT_NINE_skill_names.md"),
                    help="技能中译名 md（默认 SMT_NINE_skill_names.md）")
    ap.add_argument("--extra-chars-file", type=Path, default=None, help="额外补字文本（UTF-8，可选）")
    ap.add_argument("--common-chars-file", type=Path, default=Path("Common_3000.txt"),
                    help="常用字列表（UTF-8，默认 Common_3000.txt；仅取其中汉字）")
    ap.add_argument("--exclude-chars-file", type=Path, default=None,
                    help="要剔除的汉字列表（UTF-8，可选；默认只从 common 里剔除）")
    ap.add_argument("--exclude-chars", type=str, default="",
                    help="要剔除的汉字字符串（默认只从 common 里剔除）")
    ap.add_argument("--exclude-all", action="store_true",
                    help="剔除时也影响 demon/force/extra（默认只剔除 common）")
    ap.add_argument("--force-chars", type=str, default="", help="强制包含的字符（默认空）")
    ap.add_argument("--gb2312-n", type=int, default=3000,
                    help="从 GB2312 一级字表取前 N 个作为常用字（仅当 common-chars-file 不存在时生效）")
    ap.add_argument("--max-chars", type=int, default=0, help="强制限制总字符数（0=不限制，默认按槽位上限）")

    ap.add_argument("--fill-rgb", type=str, default="180,182,205", help="填充色 RGB，例如 180,182,205")
    ap.add_argument("--outline-rgb", type=str, default="0,0,0", help="描边色 RGB，例如 0,0,0")
    ap.add_argument("--threshold", type=int, default=80, help="fill 强阈值（0-255，基于 coverage）。越低字越粗；建议 64~96")
    ap.add_argument("--edge-threshold", type=int, default=20, help="edge 弱阈值（0-255，基于 coverage，用于 hysteresis）。越低越不容易断笔；建议 16~32")
    ap.add_argument("--oversample", type=int, default=8, help="超采样倍率（建议 8；越大越慢但更稳）")
    ap.add_argument("--font-scale", type=float, default=0.92, help="字号 = cell_px*oversample*font_scale")
    ap.add_argument("--y-shift", type=int, default=0, help="渲染时整体 Y 偏移（以 oversample 像素为单位）")

    ap.add_argument("--out-xpr", type=Path, default=Path("sys_f24_simhei_cn_clean.xpr"), help="输出 xpr")
    ap.add_argument("--out-prefix", type=str, default="simhei_cn_clean", help="映射表输出前缀")
    ap.add_argument("--no-preview", action="store_true", help="跳过生成预览 PNG")
    ap.add_argument("--include-kana", action="store_true",
                    help="允许占用日文假名槽位来放中文（会破坏日文显示）")
    ap.add_argument("--verify-keep", action="store_true",
                    help="校验符号/假名槽位是否字节级未改（若 --include-kana 则允许改假名）")
    ap.add_argument("--require-f18-compatible", action="store_true", default=True,
                    help="只使用在 f18 中也有可写槽位的 codepoint（默认开启）")
    ap.add_argument("--allow-f24-only", action="store_true",
                    help="允许使用仅存在于 f24 或在 f18 中只落在保留页的 codepoint")

    args = ap.parse_args()

    # 读取原表与原字体
    if args.allow_f24_only:
        args.require_f18_compatible = False

    f24 = read_sysfont_tbl_f24(args.tbl_in)
    f18_codes = read_sysfont_tbl_f18_codes(args.tbl_in)
    xpr = read_xpr_f24(args.xpr_in)

    # 统计 codepoint 重复
    codes = f24.codes
    freq = Counter(codes)

    # 找出“可用汉字槽位”：必须是唯一 codepoint + 代码属于 kanji
    f18_writable_codes = set()
    if args.require_f18_compatible:
        per_page_f18 = 28 * 28
        for idx, cp in enumerate(f18_codes):
            if (idx // per_page_f18) == 8:
                continue
            f18_writable_codes.add(cp)

    kanji_slots: List[Tuple[int, int]] = []  # (codepoint, index)
    for idx, cp in enumerate(codes):
        if freq[cp] != 1:
            continue
        if is_sjis_kanji(cp):
            if args.require_f18_compatible and cp not in f18_writable_codes:
                continue
            kanji_slots.append((cp, idx))

    # 可选：把“假名槽位”也纳入
    kana_slots: List[Tuple[int, int]] = []
    if args.include_kana:
        for idx, cp in enumerate(codes):
            if freq[cp] != 1:
                continue
            if is_sjis_kana(cp):
                if args.require_f18_compatible and cp not in f18_writable_codes:
                    continue
                kana_slots.append((cp, idx))

    # 为了保持顺序稳定，按 codepoint 升序（也更方便你调试）
    kanji_slots.sort(key=lambda t: t[0])
    kana_slots.sort(key=lambda t: t[0])
    slots = kanji_slots + kana_slots  # 先占用 kanji，再扩展到 kana，保持旧映射尽量稳定
    capacity = len(slots)
    print(f"[info] f24 可用唯一 kanji 槽位: {len(kanji_slots)}")
    if args.include_kana:
        print(f"[info] f24 可用唯一 kana 槽位: {len(kana_slots)}（将被占用）")
    if args.require_f18_compatible:
        print(f"[info] 已启用 f18 兼容过滤：仅保留在 f18 非保留页也可写的 codepoint")
    print(f"[info] f24 总可用槽位: {capacity}")

    # 处理剔除列表（默认只影响 common）
    exclude: List[str] = []
    if args.exclude_chars:
        exclude.extend(read_chars_from_string(args.exclude_chars))
    if args.exclude_chars_file and args.exclude_chars_file.exists():
        exclude.extend(read_chars_from_file(args.exclude_chars_file))
    exclude_set = set(exclude)

    # 构建字符集（common）
    common: List[str] = []
    if args.common_chars_file and args.common_chars_file.exists():
        common = read_chars_from_file(args.common_chars_file)
        if exclude_set:
            common = [ch for ch in common if ch not in exclude_set]
        print(f"[info] common chars from file: {args.common_chars_file} ({len(common)})")
    else:
        gb_all = gb2312_level1_chars()
        target_n = max(0, min(args.gb2312_n, len(gb_all)))
        if exclude_set:
            for ch in gb_all:
                if ch in exclude_set:
                    continue
                common.append(ch)
                if len(common) >= target_n:
                    break
        else:
            common = gb_all[:target_n]
        print(f"[info] common chars from gb2312 level1: {len(common)}")

    required: List[str] = []
    if args.force_chars:
        required.extend([ch for ch in args.force_chars if is_cjk_unified_ideograph(ch)])
    if args.demon_names_md and args.demon_names_md.exists():
        demon_md_chars = read_chars_from_markdown_cn_column(args.demon_names_md)
        required.extend(demon_md_chars)
        print(f"[info] demon names md: {args.demon_names_md} ({len(demon_md_chars)})")
    if args.skill_names_md and args.skill_names_md.exists():
        skill_md_chars = read_chars_from_markdown_cn_column(args.skill_names_md)
        required.extend(skill_md_chars)
        print(f"[info] skill names md: {args.skill_names_md} ({len(skill_md_chars)})")
    if args.extra_chars_file and args.extra_chars_file.exists():
        required.extend(read_chars_from_file(args.extra_chars_file))

    fixed_mapping: Optional[Dict[str, int]] = None
    if args.mapping_json and args.mapping_json.exists():
        raw_mapping = json.loads(args.mapping_json.read_text(encoding="utf-8"))
        fixed_mapping = {}
        for ch, hx in raw_mapping.items():
            if not is_cjk_unified_ideograph(ch):
                continue
            fixed_mapping[ch] = int(str(hx), 16)
        charset = list(fixed_mapping.keys())
        print(f"[info] fixed mapping json: {args.mapping_json} ({len(charset)})")
    else:
        # 先按输入优先级选集合，再按拼音排序输出。
        # 否则会因为“先全量按拼音排、再截断”，把大量高频常用字从字尾硬切掉。
        charset = unique_preserve_order(required + common)
        if args.exclude_all and exclude_set:
            charset = [ch for ch in charset if ch not in exclude_set]

        priority_charset = list(charset)
        if args.max_chars and args.max_chars > 0:
            priority_charset = priority_charset[:args.max_chars]

        if len(priority_charset) > capacity:
            print(f"[warn] 字符数 {len(priority_charset)} 超过槽位容量 {capacity}，将按优先顺序截断到容量上限。")
            priority_charset = priority_charset[:capacity]

        # 增量构建：现有字参与拼音排序，新追加的字保持在末尾不参与排序
        _NEW_APPEND_CHARS = set('\u3e62\u3ea2\u5d0e\u72d3\u80eb\u82dc\u84ff\u89d0\u8eac\u97a0')
        _existing = [c for c in priority_charset if c not in _NEW_APPEND_CHARS]
        _new = [c for c in priority_charset if c in _NEW_APPEND_CHARS]
        charset = sort_chars_by_pinyin(_existing) + _new
        if _new:
            print(f"[info] 增量构建: {len(_existing)} 个现有字参与排序, {len(_new)} 个新字追加在末尾")

    print(f"[info] 计划写入中文字符数: {len(charset)}")

    # 颜色
    fr = tuple(int(x) for x in args.fill_rgb.split(","))
    orr = tuple(int(x) for x in args.outline_rgb.split(","))
    if len(fr) != 3 or len(orr) != 3:
        raise ValueError("--fill-rgb / --outline-rgb 格式应为 r,g,b")
    fill565 = rgb888_to_565(*fr)
    outline565 = rgb888_to_565(*orr)

    # 字体
    cell_px = 24
    canvas = cell_px * args.oversample
    font_size = int(canvas * args.font_scale)
    font = ImageFont.truetype(str(args.ttf), size=font_size)

    # 建立 codepoint 分配（中文 -> SJIS）
    char_to_cp: Dict[str, int] = {}
    cp_to_slot_index: Dict[int, int] = {cp: idx for cp, idx in slots}  # unique, safe
    slot_list = [cp for cp, _ in slots]

    # 逐字写入
    used_slots: List[Tuple[str, int, int]] = []  # (char, cp, slot_index)
    skipped_missing: List[str] = []

    for i, ch in enumerate(charset):
        if fixed_mapping is None:
            cp = slot_list[i]
        else:
            cp = fixed_mapping[ch]
            if cp not in cp_to_slot_index:
                raise ValueError(f"mapping-json 中的码位不存在于可写 f24 槽位: {ch} -> {cp:04X}")
        slot_index = cp_to_slot_index[cp]
        # 检查字体是否真的有这个字
        try:
            bbox = font.getbbox(ch)
        except Exception:
            bbox = None
        if bbox is None:
            skipped_missing.append(ch)
            continue

        fill, edge, outline = render_char_masks(
            ch, font,
            oversample=args.oversample,
            cell_px=cell_px,
            fill_threshold=args.threshold,
            edge_threshold=args.edge_threshold,
            y_shift=args.y_shift,
        )
        if (not fill.any()) and (not edge.any()) and (not outline.any()):
            skipped_missing.append(ch)
            continue

        cell_cls = np.zeros((24,24), dtype=np.uint8)
        cell_cls[fill] = 1
        cell_cls[edge] = 3
        cell_cls[outline] = 2

        blocks = encode_cell_to_bc1_blocks(cell_cls, fill565=fill565, outline565=outline565)

        # 写入对应纹理页
        page = slot_index // (21*21)
        local = slot_index % (21*21)
        row = local // 21
        col = local % 21
        block_x = col * 6
        block_y = row * 6
        page_data = xpr.pages[page]

        for by in range(6):
            for bx in range(6):
                bi = (block_y + by) * 128 + (block_x + bx)
                off = bi * 8
                page_data[off:off+8] = blocks[by*6 + bx]

        char_to_cp[ch] = cp
        used_slots.append((ch, cp, slot_index))

    if skipped_missing:
        print(f"[warn] 有 {len(skipped_missing)} 个字符在字体里渲染为空/缺失（已跳过）。示例：{''.join(skipped_missing[:20])}")

    # 输出 XPR
    write_xpr_f24(args.out_xpr, xpr)
    print(f"[ok] 输出 XPR: {args.out_xpr}")

    # 输出映射表
    prefix = Path(args.out_prefix)
    csv_path = prefix.with_suffix(".csv")
    json_path = prefix.with_suffix(".json")
    tbl_path = prefix.with_suffix(".tbl")
    full_tbl_path = prefix.with_name(prefix.name + "_full.tbl")

    with csv_path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["char", "sjis_hex", "sjis_dec", "slot_index", "page", "row", "col", "pixel_x", "pixel_y"])
        for ch, cp, slot_index in used_slots:
            page = slot_index // (21*21)
            local = slot_index % (21*21)
            row = local // 21
            col = local % 21
            w.writerow([ch, f"{cp:04X}", cp, slot_index, page, row, col, col*24, row*24])

    with json_path.open("w", encoding="utf-8") as f:
        json.dump({ch: f"{cp:04X}" for ch, cp in char_to_cp.items()}, f, ensure_ascii=False, indent=2)

    with tbl_path.open("w", encoding="utf-8") as f:
        for ch, cp, _slot_index in used_slots:
            f.write(f"{cp:04X}={ch}\n")

    slot_to_char = {slot_index: ch for ch, _cp, slot_index in used_slots}
    full_csv_path = prefix.with_name(prefix.name + "_full.csv")
    with full_tbl_path.open("w", encoding="utf-8") as f:
        f.write("# Full f24 slot export\n")
        f.write("# Includes preserved symbols/kana/letters and repeated SJIS codes.\n")
        f.write("# Empty/undecodable slots are emitted as comments with slot index.\n")
        for slot_index, cp in enumerate(codes):
            if slot_index in slot_to_char:
                ch = slot_to_char[slot_index]
                source = "new_cn"
            else:
                ch = decode_sjis_char(cp)
                source = "orig_sjis" if ch else "orig_unknown"

            if ch:
                f.write(f"{cp:04X}={ch}\n")
            else:
                f.write(f"# {cp:04X} slot={slot_index} source={source}\n")

    with full_csv_path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "slot_index", "sjis_hex", "sjis_dec", "char", "source",
            "kind", "unique", "page", "row", "col", "pixel_x", "pixel_y",
        ])
        for slot_index, cp in enumerate(codes):
            page = slot_index // (21*21)
            local = slot_index % (21*21)
            row = local // 21
            col = local % 21
            if slot_index in slot_to_char:
                ch = slot_to_char[slot_index]
                source = "new_cn"
            else:
                ch = decode_sjis_char(cp)
                source = "orig_sjis" if ch else "orig_unknown"
            if is_sjis_kanji(cp):
                kind = "kanji"
            elif is_sjis_kana(cp):
                kind = "kana"
            else:
                kind = "other"
            unique = 1 if freq[cp] == 1 else 0
            w.writerow([slot_index, f"{cp:04X}", cp, ch, source, kind, unique, page, row, col, col*24, row*24])

    print(f"[ok] 输出映射表: {csv_path}, {json_path}, {tbl_path}, {full_tbl_path}, {full_csv_path}")

    # 可选：校验符号/假名槽位未改（字节级）
    if args.verify_keep:
        orig = read_xpr_f24(args.xpr_in)
        # 逐槽位比对（只比对非kanji or 非唯一的那些槽位，确保我们没动）
        changed = 0
        checked = 0
        for slot_index, cp in enumerate(codes):
            # 我们只允许改“唯一 kanji（或可选 kana）且在 used_slots 里”的槽位
            allowed = (freq[cp] == 1) and (
                is_sjis_kanji(cp) or (args.include_kana and is_sjis_kana(cp))
            ) and (cp in char_to_cp.values())
            if allowed:
                continue
            # compare 24x24 blocks
            page = slot_index // (21*21)
            local = slot_index % (21*21)
            row = local // 21
            col = local % 21
            block_x = col * 6
            block_y = row * 6
            for by in range(6):
                for bx in range(6):
                    bi = (block_y + by) * 128 + (block_x + bx)
                    off = bi * 8
                    checked += 1
                    if orig.pages[page][off:off+8] != xpr.pages[page][off:off+8]:
                        changed += 1
                        break  # 这个槽位变了就算一次
                else:
                    continue
                break
        if changed == 0:
            print(f"[ok] verify_keep: 符号/假名/重复槽位均未被改动（检查blocks={checked}）。")
        else:
            print(f"[warn] verify_keep: 发现 {changed} 个“本应保留”的槽位发生变化（检查blocks={checked}）。")

    # 可选：输出预览（会慢）
    if not args.no_preview:
        # 这里给一个最简单的 BC1 解码器用于拼图预览（只做调试，不追求极致性能）
        preview_path = prefix.with_name(prefix.name + "_mosaic.png")
        mosaic = make_mosaic_preview(xpr.pages)
        mosaic.save(preview_path)
        print(f"[ok] 输出预览: {preview_path}")


# -----------------------------
# 预览（BC1 解码 + 3×3 拼图）
# -----------------------------

def rgb565_to_rgb888(c: int) -> Tuple[int, int, int]:
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    # 5/6-bit expand
    r8 = (r * 255 + 15) // 31
    g8 = (g * 255 + 31) // 63
    b8 = (b * 255 + 15) // 31
    return r8, g8, b8

def decode_bc1_block(block: bytes) -> np.ndarray:
    """
    return RGBA uint8 array (4,4,4)
    """
    c0, c1, bits = struct.unpack("<HHI", block)
    r0,g0,b0 = rgb565_to_rgb888(c0)
    r1,g1,b1 = rgb565_to_rgb888(c1)
    # palette
    if c0 > c1:
        # 4-color
        p = [
            (r0,g0,b0,255),
            (r1,g1,b1,255),
            ((2*r0+r1)//3, (2*g0+g1)//3, (2*b0+b1)//3, 255),
            ((r0+2*r1)//3, (g0+2*g1)//3, (b0+2*b1)//3, 255),
        ]
    else:
        # 3-color + transparent
        p = [
            (r0,g0,b0,255),
            (r1,g1,b1,255),
            ((r0+r1)//2, (g0+g1)//2, (b0+b1)//2, 255),
            (0,0,0,0),
        ]
    out = np.zeros((4,4,4), dtype=np.uint8)
    for y in range(4):
        for x in range(4):
            idx = (bits >> (2*(4*y+x))) & 0x3
            out[y,x] = p[idx]
    return out

def decode_page_to_image(page: bytes) -> Image.Image:
    # 512x512 -> 128x128 blocks
    out = np.zeros((512,512,4), dtype=np.uint8)
    for by in range(128):
        for bx in range(128):
            off = (by*128 + bx) * 8
            block = page[off:off+8]
            pix = decode_bc1_block(block)
            out[by*4:(by+1)*4, bx*4:(bx+1)*4] = pix
    return Image.fromarray(out, mode="RGBA")

def flatten_preview_rgba(img: Image.Image,
                         bg_rgba: Tuple[int, int, int, int] = (40, 40, 40, 255)) -> Image.Image:
    bg = Image.new("RGBA", img.size, bg_rgba)
    return Image.alpha_composite(bg, img).convert("RGB")

def make_mosaic_preview(pages: Sequence[bytes]) -> Image.Image:
    n = len(pages)
    cols = int(math.ceil(math.sqrt(n)))
    rows = int(math.ceil(n / cols))
    imgs = [decode_page_to_image(p) for p in pages]
    mosaic = Image.new("RGBA", (512*cols, 512*rows), (0,0,0,0))
    for i, im in enumerate(imgs):
        x = (i % cols) * 512
        y = (i // cols) * 512
        mosaic.alpha_composite(im, (x,y))
    return flatten_preview_rgba(mosaic)


if __name__ == "__main__":
    main()
