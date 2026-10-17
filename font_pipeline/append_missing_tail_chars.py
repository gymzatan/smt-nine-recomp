#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

from pathlib import Path
import sys
import numpy as np
from PIL import ImageFont

sys.path.insert(0, str(Path(__file__).parent))

from build_smt_nine_f24 import (
    read_sysfont_tbl_f24,
    read_xpr_f24,
    write_xpr_f24,
    render_char_masks,
    encode_cell_to_bc1_blocks,
    rgb888_to_565,
)
from build_smt_nine_f18 import (
    read_sysfont_tbl,
    patch_f18_preserve,
)


TAIL_CHARS = [
    (0xED88, "喏"),
    (0xED89, "娴"),
    (0xED8A, "実"),
    (0xED8B, "様"),
    (0xED8C, "紙"),
    (0xED8D, "討"),
    (0xED8E, "踽"),
    (0xED8F, "遠"),
    (0xED90, "間"),
    (0xED91, "仕"),
    (0xED92, "婊"),
    (0xED93, "枣"),
    (0xED94, "楽"),
    (0xED95, "飲"),
]


def patch_f24_tail(tbl_path: Path, xpr_in: Path, xpr_out: Path, ttf_path: Path, *, characters=TAIL_CHARS) -> None:
    f24 = read_sysfont_tbl_f24(tbl_path)
    code_to_slot = {cp: idx for idx, cp in enumerate(f24.codes)}
    xpr = read_xpr_f24(xpr_in)

    cell_px = 24
    oversample = 8
    font_size = int(cell_px * oversample * 0.92)
    font = ImageFont.truetype(str(ttf_path), size=font_size)
    fill565 = rgb888_to_565(180, 182, 205)
    outline565 = rgb888_to_565(0, 0, 0)
    per_page = 21 * 21

    for cp, ch in characters:
        slot_index = code_to_slot[cp]
        page = slot_index // per_page
        local = slot_index % per_page
        row = local // 21
        col = local % 21

        fill, edge, outline = render_char_masks(
            ch,
            font,
            oversample=oversample,
            cell_px=cell_px,
            fill_threshold=80,
            edge_threshold=20,
            y_shift=0,
        )

        cell_cls = np.zeros((24, 24), dtype=np.uint8)
        cell_cls[fill] = 1
        cell_cls[edge] = 3
        cell_cls[outline] = 2
        blocks = encode_cell_to_bc1_blocks(cell_cls, fill565=fill565, outline565=outline565)

        block_x = col * 6
        block_y = row * 6
        page_data = xpr.pages[page]
        for by in range(6):
            for bx in range(6):
                bi = (block_y + by) * 128 + (block_x + bx)
                off = bi * 8
                page_data[off:off + 8] = blocks[by * 6 + bx]

        print(f"[f24-tail] {cp:04X}={ch} -> page={page} row={row} col={col} slot={slot_index}")

    write_xpr_f24(xpr_out, xpr)


def patch_f18_tail(tbl_path: Path, xpr_in: Path, xpr_out: Path, ttf_path: Path, *, characters=TAIL_CHARS) -> None:
    codes18, _ = read_sysfont_tbl(tbl_path)
    sjis_to_char = {cp: ch for cp, ch in characters}
    patch_f18_preserve(
        sys_f18_xpr=xpr_in,
        out_path=xpr_out,
        sjis_to_char=sjis_to_char,
        codes18=codes18,
        font_path=ttf_path,
        font_size=18,
        render_mode="cov",
        cov_oversample=4,
        cov_thr_fill=80.0,
        cov_thr_edge=20.0,
        cov_hyst_iters=4,
        fill_rgb=(197, 198, 214),
        shadow_rgb=(74, 77, 82),
    )


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    media_sysfont = root / "Media" / "SysFont"
    work = root / "script_tools" / "work"
    work.mkdir(parents=True, exist_ok=True)

    tbl_path = media_sysfont / "sysfont.tbl"
    f24_in = media_sysfont / "sys_f24.xpr"
    f18_in = media_sysfont / "sys_f18.xpr"
    f24_out = work / "sys_f24_tailpatched.xpr"
    f18_out = work / "sys_f18_tailpatched.xpr"

    patch_f24_tail(tbl_path, f24_in, f24_out, Path(__file__).resolve().parent / "SimSun.ttf")
    patch_f18_tail(tbl_path, f18_in, f18_out, Path(__file__).resolve().parent / "simhei.ttf")

    print(f"[ok] wrote {f24_out}")
    print(f"[ok] wrote {f18_out}")


if __name__ == "__main__":
    main()
