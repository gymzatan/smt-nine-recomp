#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SMT NINE (OG Xbox) sys_f18.xpr Chinese font patcher (SimHei) — v10
(coverage + hysteresis + *4-state BC1* to avoid "too bold" and preserve strokes)

What was wrong with earlier f18 attempts?
- With only 3 states {transparent, shadow, fill}, any antialiased edge pixels that we "keep"
  (via low weak-threshold + hysteresis) must become FULL fill pixels. That inevitably makes
  glyphs look ~1px thicker ("字肥一圈").
- Trying to fix thickness with per-glyph Otsu/adaptive thresholds causes exactly what you saw:
  radical thickness becomes inconsistent, and alignment drifts.

What this version changes (the important bit):
- We use the same *coverage + double threshold + hysteresis* logic that worked for f24,
  BUT we also keep an intermediate "edge" state.
- BC1/DXT1 in 1-bit-alpha mode has 4 indices:
    0 = color0
    1 = color1
    2 = (color0+color1)/2   <-- we use this as AA/edge
    3 = transparent
  So for f18 we set:
    color0 = shadow, color1 = fill  (and force c0 <= c1 to enable alpha mode)
    idx0 = shadow
    idx1 = fill
    idx2 = edge (AA, midtone)
    idx3 = transparent
- Result: weak pixels are preserved as *edge* instead of being promoted to full fill,
  so strokes keep their SimHei shape without getting bloated.

Other constraints we still respect:
- f18 style: NO outline. Only a small down-right shadow.
- sys_f18 uses 18×18 cells on a 512×512 BC1 texture; 18 doesn't align to 4×4 blocks.
  We therefore patch at the *block state* level: decode a BC1 block, classify each pixel
  to our 4 states, overlay only pixels inside the target cell, then re-encode with our
  strict palette. This avoids "last write wins" corruption between neighboring cells.
- Page 8 (index 8) is kept byte-identical (kana/symbol page).

Dependencies:
  pip install pillow numpy

Typical usage:
  python build_smt_nine_f18_simhei_cov_shadow_v10.py ^
    --xpr-in sys_f18.xpr ^
    --tbl-in sysfont.tbl ^
    --mapping-json simsun_cov_v7.json ^
    --ttf simhei.ttf ^
    --out-xpr sys_f18_simhei_cn_f18v10.xpr ^
    --out-prefix f18_simhei_cov_shadow_v10 ^
    --no-preview
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from pathlib import Path
from typing import Dict, List, Tuple, Optional

import numpy as np
from PIL import Image, ImageDraw, ImageFont


# ----------------------------
# XPR helpers (XPR0 container)
# ----------------------------

BC1_PAGE_BYTES = 512 * 512 // 2  # 512x512 BC1 (4bpp) = 131072

def read_xpr(path: Path) -> Tuple[bytes, List[bytes]]:
    data = path.read_bytes()
    if data[:4] != b"XPR0":
        raise ValueError("Not an XPR0 file")
    header_size = struct.unpack("<I", data[8:12])[0]
    header = data[:header_size]
    blob = data[header_size:]
    if len(blob) % BC1_PAGE_BYTES != 0:
        raise ValueError(f"Texture blob size not multiple of {BC1_PAGE_BYTES}")
    pages = [blob[i:i+BC1_PAGE_BYTES] for i in range(0, len(blob), BC1_PAGE_BYTES)]
    return header, pages

def write_xpr(path: Path, header: bytes, pages: List[bytes]) -> None:
    blob = b"".join(pages)
    total_size = len(header) + len(blob)
    header2 = bytearray(header)
    if len(header2) >= 8:
        struct.pack_into("<I", header2, 4, total_size)
    path.write_bytes(bytes(header2) + blob)


# ----------------------------
# sysfont.tbl (f18 + f24 codes)
# ----------------------------

def read_sysfont_tbl(path: Path) -> Tuple[List[int], List[int]]:
    """
    sysfont.tbl layout (as observed in SMT NINE):
      u32le off2
      [f18 table] big-endian u16 codes from byte 4 .. off2-1
      [f24 header] 8 bytes at off2 .. off2+7
      [f24 table] big-endian u16 codes from byte off2+8 .. EOF
    """
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError("sysfont.tbl too small")

    off2 = struct.unpack("<I", data[:4])[0]
    if off2 < 4 or off2 + 8 > len(data):
        raise ValueError(f"Invalid f24 offset: {off2:#x}")

    t1 = data[4:off2]
    t2 = data[off2 + 8:]
    if (len(t1) % 2) != 0 or (len(t2) % 2) != 0:
        raise ValueError("sysfont.tbl code sections must be even-sized")

    codes18 = [struct.unpack(">H", t1[i:i+2])[0] for i in range(0, len(t1), 2)]
    codes24 = [struct.unpack(">H", t2[i:i+2])[0] for i in range(0, len(t2), 2)]
    return codes18, codes24


def is_sjis_kanji(code: int) -> bool:
    b1 = (code >> 8) & 0xFF
    return (0x88 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xFC)


def f18_code_consumes_slot(code: int) -> bool:
    # Runtime f18 loader skips ZN/HN markers when advancing atlas coordinates.
    return code not in (0x5A4E, 0x484E)


# ----------------------------
# BC1/DXT1 helpers
# ----------------------------

def rgb565_to_rgb888(c: int) -> Tuple[int, int, int]:
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    r8 = (r * 255 + 15) // 31
    g8 = (g * 255 + 31) // 63
    b8 = (b * 255 + 15) // 31
    return r8, g8, b8

def rgb888_to_565(r: int, g: int, b: int) -> int:
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5

def decode_bc1_block(block8: bytes) -> np.ndarray:
    """Decode a BC1 block -> RGBA uint8 (4,4,4)."""
    c0, c1, bits = struct.unpack("<HHI", block8)
    r0, g0, b0 = rgb565_to_rgb888(c0)
    r1, g1, b1 = rgb565_to_rgb888(c1)

    # alpha-mode if c0 <= c1
    if c0 > c1:
        # opaque 4-color
        c2 = ((2*r0 + r1)//3, (2*g0 + g1)//3, (2*b0 + b1)//3, 255)
        c3 = ((r0 + 2*r1)//3, (g0 + 2*g1)//3, (b0 + 2*b1)//3, 255)
    else:
        # 3-color + transparent
        c2 = ((r0 + r1)//2, (g0 + g1)//2, (b0 + b1)//2, 255)
        c3 = (0, 0, 0, 0)

    pal = [
        (r0, g0, b0, 255),
        (r1, g1, b1, 255),
        c2,
        c3,
    ]

    out = np.zeros((4, 4, 4), dtype=np.uint8)
    for y in range(4):
        for x in range(4):
            idx = (bits >> (2 * (4*y + x))) & 0x3
            out[y, x] = pal[idx]
    return out

def pack_bc1_block(color0_565: int, color1_565: int, idx: np.ndarray) -> bytes:
    """Pack endpoints + 2bpp index table into 8 bytes."""
    assert idx.shape == (4, 4)
    v = 0
    bit = 0
    for y in range(4):
        for x in range(4):
            v |= (int(idx[y, x]) & 0x3) << bit
            bit += 2
    return struct.pack("<HHI", color0_565 & 0xFFFF, color1_565 & 0xFFFF, v & 0xFFFFFFFF)


# ----------------------------
# f18 state classification + encoding
# ----------------------------
# State semantics (uint8):
#   0 = transparent
#   1 = shadow  (BC1 idx0 or idx1 if endpoints swapped)
#   2 = fill    (BC1 idx1 or idx0 if endpoints swapped)
#   3 = edge    (BC1 idx2, average color)
#
# We re-encode blocks with strict endpoints {shadow, fill} and allow idx2=edge.

def luminance(rgb: Tuple[int, int, int]) -> float:
    r, g, b = rgb
    return 0.2126*r + 0.7152*g + 0.0722*b

def classify_rgba_to_states_f18(block_rgba: np.ndarray,
                                lum_shadow: float,
                                lum_edge: float,
                                lum_fill: float) -> np.ndarray:
    """
    Map decoded RGBA pixels to {transparent, shadow, edge, fill} by luminance bands.
    This is only used to preserve non-target pixels when we patch overlapping BC1 blocks.
    """
    assert block_rgba.shape == (4, 4, 4)
    out = np.zeros((4, 4), dtype=np.uint8)  # default transparent
    # thresholds midway between the three levels
    t0 = (lum_shadow + lum_edge) / 2.0
    t1 = (lum_edge + lum_fill) / 2.0
    for y in range(4):
        for x in range(4):
            a = int(block_rgba[y, x, 3])
            if a < 128:
                out[y, x] = 0
                continue
            r, g, b = int(block_rgba[y, x, 0]), int(block_rgba[y, x, 1]), int(block_rgba[y, x, 2])
            lum = 0.2126*r + 0.7152*g + 0.0722*b
            if lum < t0:
                out[y, x] = 1  # shadow
            elif lum < t1:
                out[y, x] = 3  # edge
            else:
                out[y, x] = 2  # fill
    return out

def encode_block_states_bc1(states4: np.ndarray, shadow565: int, fill565: int) -> bytes:
    """
    Encode a 4x4 state map (f18) into a BC1 block, forcing 1-bit-alpha mode.
    We allow idx2=edge.
    """
    assert states4.shape == (4, 4)
    c0 = int(shadow565) & 0xFFFF
    c1 = int(fill565) & 0xFFFF

    # Force alpha mode: c0 <= c1 (as 16-bit unsigned compare)
    shadow_idx = 0
    fill_idx = 1
    if c0 > c1:
        c0, c1 = c1, c0
        # endpoints swapped, so indices swap for shadow/fill
        shadow_idx, fill_idx = 1, 0

    idx = np.full((4, 4), 3, dtype=np.uint8)  # transparent by default
    idx[states4 == 1] = shadow_idx
    idx[states4 == 2] = fill_idx
    idx[states4 == 3] = 2  # edge always uses idx2 (average color)

    # states4==0 stays idx3
    return pack_bc1_block(c0, c1, idx)


# ----------------------------
# Morphology + hysteresis
# ----------------------------

def dilate8(mask: np.ndarray) -> np.ndarray:
    h, w = mask.shape
    out = np.zeros_like(mask, dtype=bool)
    # pad by 1
    m = np.pad(mask.astype(bool), ((1,1),(1,1)), mode="constant", constant_values=False)
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            out |= m[1+dy:1+dy+h, 1+dx:1+dx+w]
    return out

def hysteresis8(strong: np.ndarray, weak: np.ndarray, iters: int = 4) -> np.ndarray:
    """Keep weak pixels connected (8-neigh) to strong pixels."""
    out = strong.copy()
    for _ in range(iters):
        out = np.logical_and(weak, dilate8(out))
    return out


# ----------------------------
# Glyph rasterization (fill + edge)
# ----------------------------

def render_mono_into_box(char: str, font: ImageFont.FreeTypeFont, box_w: int, box_h: int) -> np.ndarray:
    """1-bit (hinted) raster into fixed box; returns bool mask (box_h, box_w)."""
    im = Image.new("L", (box_w, box_h), 0)
    dr = ImageDraw.Draw(im)
    dr.fontmode = "1"
    bbox = dr.textbbox((0, 0), char, font=font)
    if bbox is None:
        return np.zeros((box_h, box_w), dtype=bool)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (box_w - w) // 2 - bbox[0]
    y = (box_h - h) // 2 - bbox[1]
    dr.text((x, y), char, font=font, fill=255)
    arr = np.array(im, dtype=np.uint8)
    return arr > 0

def render_cov_into_box(
    char: str,
    font_path: Path,
    base_size: int,
    box_w: int,
    box_h: int,
    oversample: int,
    thr_fill: float,
    thr_edge: float,
    hyst_iters: int = 4,
) -> Tuple[np.ndarray, np.ndarray]:
    """
    AA raster (optionally oversampled), coverage downsample, hysteresis.
    Returns (fill_mask, edge_mask), both bool arrays (box_h, box_w).
    """
    os = max(1, int(oversample))
    font_os = ImageFont.truetype(str(font_path), size=base_size * os)

    W = box_w * os
    H = box_h * os
    im = Image.new("L", (W, H), 0)
    dr = ImageDraw.Draw(im)
    dr.fontmode = "L"  # antialiased
    bbox = dr.textbbox((0, 0), char, font=font_os)
    if bbox is None:
        z = np.zeros((box_h, box_w), dtype=bool)
        return z, z
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (W - w) // 2 - bbox[0]
    y = (H - h) // 2 - bbox[1]
    dr.text((x, y), char, font=font_os, fill=255)

    arr = np.array(im, dtype=np.float32)
    if os > 1:
        # exact box average (coverage)
        arr = arr.reshape(box_h, os, box_w, os).mean(axis=(1, 3))

    thr_fill = float(thr_fill)
    thr_edge = float(thr_edge)
    if thr_edge > thr_fill:
        thr_edge, thr_fill = thr_fill, thr_edge

    strong = arr >= thr_fill
    weak = arr >= thr_edge

    if np.any(strong):
        body = hysteresis8(strong, weak, iters=hyst_iters)
    else:
        # extreme case: no pixel reaches strong threshold
        body = weak.copy()
        strong = body.copy()

    fill = np.logical_and(body, strong)
    edge = np.logical_and(body, np.logical_not(fill))
    return fill.astype(bool), edge.astype(bool)


# ----------------------------
# Font size selection (global, no clipping)
# ----------------------------

def glyph_pixel_rel_bbox(char: str, font: ImageFont.FreeTypeFont, pad: int) -> Optional[Tuple[int,int,int,int]]:
    """
    Render at (pad,pad) then get pixel bbox; return bbox relative to pad.
    Used to select a font size that won't clip.
    """
    canvas_w = font.size * 2 + pad * 2
    canvas_h = font.size * 2 + pad * 2
    im = Image.new("L", (canvas_w, canvas_h), 0)
    dr = ImageDraw.Draw(im)
    dr.fontmode = "L"
    dr.text((pad, pad), char, font=font, fill=255)
    bb = im.getbbox()
    if bb is None:
        return None
    l, t, r, b = bb
    return (l - pad, t - pad, r - pad, b - pad)

def find_max_font_size_global_bbox(font_path: Path,
                                  chars: List[str],
                                  box_w: int,
                                  box_h: int,
                                  size_max: int,
                                  size_min: int) -> int:
    """
    Find the largest font size such that *all* chars' pixel bbox fits inside box_w x box_h.
    This keeps alignment consistent (one global size), and avoids per-glyph surprises.
    """
    pad = 4
    for size in range(size_max, size_min - 1, -1):
        font = ImageFont.truetype(str(font_path), size=size)
        ok = True
        for ch in chars:
            bb = glyph_pixel_rel_bbox(ch, font, pad=pad)
            if bb is None:
                continue
            w = bb[2] - bb[0]
            h = bb[3] - bb[1]
            if w > box_w or h > box_h:
                ok = False
                break
        if ok:
            return size
    return size_min


# ----------------------------
# f18 cell states (18x18)
# ----------------------------

def build_f18_cell_states(
    ch: str,
    font: ImageFont.FreeTypeFont,
    *,
    mode: str,
    cov_font_path: Path,
    cov_oversample: int,
    cov_thr_fill: float,
    cov_thr_edge: float,
    cov_hyst_iters: int,
    native_scale: int = 1,
) -> np.ndarray:
    """
    Return uint8 states (18,18):
      0 = transparent
      1 = shadow
      2 = fill
      3 = edge(AA)
    """
    cell_h = cell_w = 18 * native_scale
    fill_w, fill_h = 16 * native_scale, 17 * native_scale  # keep consistent with game layout (room for shadow)

    # rasterize into fill box
    if mode == "mono":
        m = render_mono_into_box(ch, font, fill_w, fill_h)
        fill = m
        edge = np.zeros_like(m, dtype=bool)
    elif mode == "cov":
        fill, edge = render_cov_into_box(
            ch, cov_font_path, font.size * native_scale, fill_w, fill_h,
            oversample=cov_oversample,
            thr_fill=cov_thr_fill,
            thr_edge=cov_thr_edge,
            hyst_iters=cov_hyst_iters,
        )
    else:
        raise ValueError(f"Unknown mode: {mode}")

    body = np.logical_or(fill, edge)  # what the glyph "is" (for shadow offset)

    states = np.zeros((cell_h, cell_w), dtype=np.uint8)  # transparent

    # put fill/edge into top-left
    states[:fill_h, :fill_w][edge] = 3
    states[:fill_h, :fill_w][fill] = 2

    # shadow: shift body by (1,1) in *cell space*, then remove any overlap with body
    body_cell = np.zeros((cell_h, cell_w), dtype=bool)
    body_cell[:fill_h, :fill_w] = body

    sh_cell = np.zeros((cell_h, cell_w), dtype=bool)
    sh_cell[native_scale:native_scale+fill_h, native_scale:native_scale+fill_w] = body
    sh_cell &= (~body_cell)

    states[sh_cell] = 1
    return states


# ----------------------------
# Patch sys_f18.xpr (preserve page8)
# ----------------------------

def patch_f18_preserve(
    sys_f18_xpr: Path,
    out_path: Path,
    sjis_to_char: Dict[int, str],
    codes18: List[int],
    font_path: Path,
    font_size: int,
    render_mode: str,
    cov_oversample: int,
    cov_thr_fill: float,
    cov_thr_edge: float,
    cov_hyst_iters: int,
    fill_rgb: Tuple[int,int,int],
    shadow_rgb: Tuple[int,int,int],
) -> None:
    header, pages_in = read_xpr(sys_f18_xpr)
    pages_out = [bytearray(p) for p in pages_in]

    fill565 = rgb888_to_565(*fill_rgb)
    shadow565 = rgb888_to_565(*shadow_rgb)

    lum_sh = luminance(shadow_rgb)
    lum_fi = luminance(fill_rgb)
    lum_ed = (lum_sh + lum_fi) / 2.0

    per_page = 28 * 28
    font = ImageFont.truetype(str(font_path), size=font_size)

    block_state_cache: Dict[Tuple[int,int,int], np.ndarray] = {}

    touched = 0
    runtime_slot_idx = 0
    for sjis in codes18:
        consumes_slot = f18_code_consumes_slot(sjis)
        if not consumes_slot:
            continue

        page = runtime_slot_idx // per_page
        if page == 8:
            runtime_slot_idx += 1
            continue  # keep page 8 (kana/symbols) untouched
        if page >= len(pages_out):
            break  # beyond available pages

        ch = sjis_to_char.get(sjis)
        if ch is None:
            runtime_slot_idx += 1
            continue

        cell_states = build_f18_cell_states(
            ch, font,
            mode=render_mode,
            cov_font_path=font_path,
            cov_oversample=cov_oversample,
            cov_thr_fill=cov_thr_fill,
            cov_thr_edge=cov_thr_edge,
            cov_hyst_iters=cov_hyst_iters,
        )

        local = runtime_slot_idx % per_page
        row = local // 28
        col = local % 28
        x0 = col * 18
        y0 = row * 18

        # blocks overlapping this 18x18 cell
        bx0 = x0 // 4
        by0 = y0 // 4
        bx1 = (x0 + 18 - 1) // 4
        by1 = (y0 + 18 - 1) // 4

        page_bytes = pages_out[page]

        for by in range(by0, by1 + 1):
            for bx in range(bx0, bx1 + 1):
                key = (page, bx, by)
                off = (by * 128 + bx) * 8

                if key in block_state_cache:
                    states4 = block_state_cache[key]
                else:
                    block_rgba = decode_bc1_block(bytes(page_bytes[off:off+8]))
                    states4 = classify_rgba_to_states_f18(block_rgba, lum_sh, lum_ed, lum_fi).copy()

                # overlay: pixels inside the cell take NEW states
                for py in range(4):
                    gy = by * 4 + py
                    if not (y0 <= gy < y0 + 18):
                        continue
                    cy = gy - y0
                    for px in range(4):
                        gx = bx * 4 + px
                        if not (x0 <= gx < x0 + 18):
                            continue
                        cx = gx - x0
                        states4[py, px] = int(cell_states[cy, cx])

                # write back
                page_bytes[off:off+8] = encode_block_states_bc1(states4, shadow565, fill565)
                block_state_cache[key] = states4

        touched += 1
        runtime_slot_idx += 1

    write_xpr(out_path, bytes(header), [bytes(p) for p in pages_out])
    print(f"[f18] patched slots: {touched} (page8 preserved)")


# ----------------------------
# Preview mosaic
# ----------------------------

def decode_page_bc1(page_bytes: bytes) -> np.ndarray:
    out = np.zeros((512,512,4), dtype=np.uint8)
    for by in range(128):
        for bx in range(128):
            off = (by * 128 + bx) * 8
            block = page_bytes[off:off+8]
            rgba4 = decode_bc1_block(block)
            out[by*4:by*4+4, bx*4:bx*4+4] = rgba4
    return out

def flatten_preview_rgba(img: Image.Image,
                         bg_rgba: Tuple[int, int, int, int] = (40, 40, 40, 255)) -> Image.Image:
    bg = Image.new("RGBA", img.size, bg_rgba)
    return Image.alpha_composite(bg, img).convert("RGB")

def save_xpr_mosaic(xpr_path: Path, out_png: Path) -> None:
    header, pages = read_xpr(xpr_path)
    n = len(pages)
    grid = int(math.ceil(math.sqrt(n)))
    mosaic = np.zeros((512*grid, 512*grid, 4), dtype=np.uint8)
    for i,p in enumerate(pages):
        r = i // grid
        c = i % grid
        mosaic[r*512:(r+1)*512, c*512:(c+1)*512] = decode_page_bc1(p)
    flatten_preview_rgba(Image.fromarray(mosaic, mode="RGBA")).save(out_png)


# ----------------------------
# Main
# ----------------------------

def parse_rgb_triplet(s: str) -> Tuple[int,int,int]:
    parts = [int(x.strip()) for x in s.split(",")]
    if len(parts) != 3:
        raise ValueError("RGB must be like '197,198,214'")
    return (parts[0], parts[1], parts[2])

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--xpr-in", required=True, type=Path, help="Input sys_f18.xpr")
    ap.add_argument("--tbl-in", required=True, type=Path, help="Input sysfont.tbl")
    ap.add_argument("--mapping-json", required=True, type=Path, help="Mapping JSON: {char: 'HHHH'}")
    ap.add_argument("--ttf", required=True, type=Path, help="SimHei.ttf")
    ap.add_argument("--out-xpr", required=True, type=Path, help="Output sys_f18_*.xpr")
    ap.add_argument("--out-prefix", required=True, type=str, help="Prefix for mapping dumps / previews")

    ap.add_argument("--render-mode", choices=["mono", "cov"], default="cov",
                    help="Fill raster mode. mono uses hinted 1-bit. cov uses AA+coverage+hysteresis+edge.")
    ap.add_argument("--font-size", type=int, default=0,
                    help="If 0, auto-pick max size that fits. Otherwise force this size.")
    ap.add_argument("--size-max", type=int, default=24, help="auto font size upper bound")
    ap.add_argument("--size-min", type=int, default=10, help="auto font size lower bound")
    ap.add_argument("--size-shrink", type=int, default=0,
                    help="After auto-pick, subtract this many pixels from font size (uniform thinning).")

    ap.add_argument("--cov-oversample", type=int, default=4, help="cov mode oversample factor (2~6 recommended)")
    ap.add_argument("--cov-fill-threshold", type=float, default=80.0,
                    help="strong threshold (0..255, coverage). Lower = thicker fill core.")
    ap.add_argument("--cov-edge-threshold", type=float, default=20.0,
                    help="weak threshold (0..255, coverage). Lower = more edge pixels preserved.")
    ap.add_argument("--cov-hyst-iters", type=int, default=4, help="hysteresis propagation iterations")

    ap.add_argument("--fill-rgb", type=parse_rgb_triplet, default=parse_rgb_triplet("197,198,214"))
    ap.add_argument("--shadow-rgb", type=parse_rgb_triplet, default=parse_rgb_triplet("74,77,82"))

    ap.add_argument("--no-preview", action="store_true", help="Skip mosaic preview PNG")

    args = ap.parse_args()

    codes18, codes24 = read_sysfont_tbl(args.tbl_in)

    # Find duplicate kanji codes in f24 — these must be skipped in f18 too,
    # because f24 build preserves them (doesn't overwrite duplicates).
    from collections import Counter
    freq24 = Counter(codes24)
    f24_dup_kanji: set = set()
    for cp, n in freq24.items():
        if n > 1:
            if is_sjis_kanji(cp):
                f24_dup_kanji.add(cp)
    if f24_dup_kanji:
        print(f"[f18] skipping {len(f24_dup_kanji)} f24-duplicate kanji: "
              f"{', '.join(f'{cp:04X}' for cp in sorted(f24_dup_kanji))}")

    mapping = json.loads(args.mapping_json.read_text(encoding="utf-8"))
    sjis_to_char: Dict[int, str] = {}
    for ch, hx in mapping.items():
        cp = int(hx, 16)
        if cp in f24_dup_kanji:
            continue  # skip f24 duplicate kanji
        sjis_to_char[cp] = ch

    # Pick one global font size to fit inside the fill box (16x17)
    fill_w, fill_h = 16, 17
    if args.font_size and args.font_size > 0:
        font_size = int(args.font_size)
    else:
        chars = list(mapping.keys())
        font_size = find_max_font_size_global_bbox(args.ttf, chars, fill_w, fill_h,
                                                   size_max=int(args.size_max),
                                                   size_min=int(args.size_min))
    if args.size_shrink:
        font_size = max(int(args.size_min), font_size - int(args.size_shrink))

    print(f"[f18] render={args.render_mode} size={font_size} os={args.cov_oversample} "
          f"fill_thr={args.cov_fill_threshold} edge_thr={args.cov_edge_threshold}")

    patch_f18_preserve(
        sys_f18_xpr=args.xpr_in,
        out_path=args.out_xpr,
        sjis_to_char=sjis_to_char,
        codes18=codes18,
        font_path=args.ttf,
        font_size=font_size,
        render_mode=args.render_mode,
        cov_oversample=int(args.cov_oversample),
        cov_thr_fill=float(args.cov_fill_threshold),
        cov_thr_edge=float(args.cov_edge_threshold),
        cov_hyst_iters=int(args.cov_hyst_iters),
        fill_rgb=args.fill_rgb,
        shadow_rgb=args.shadow_rgb,
    )

    # mapping dumps for traceability
    out_json = Path(f"{args.out_prefix}.json")
    out_csv = Path(f"{args.out_prefix}.csv")
    out_json.write_text(json.dumps(mapping, ensure_ascii=False, indent=2), encoding="utf-8")
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["char", "sjis_hex"])
        for ch, hx in mapping.items():
            w.writerow([ch, hx])

    if not args.no_preview:
        out_png = Path(f"{args.out_prefix}_mosaic.png")
        save_xpr_mosaic(args.out_xpr, out_png)

if __name__ == "__main__":
    main()
