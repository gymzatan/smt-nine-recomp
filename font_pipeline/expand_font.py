#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
expand_font.py — Expand SMT NINE font resources for more Chinese characters.

Expands:
  - sys_f24.xpr: 9 → 12 pages (+3 blank pages, +3 texture descriptors)
  - sys_f18.xpr: 9 → 11 pages (+2 blank pages, +2 texture descriptors)
  - sysfont.tbl: append new SJIS codes from 0xEB-0xFC range to both f18 and f24 tables

Usage:
  python expand_font.py [--f24-pages 3] [--f18-pages 2]

Reads from current directory:
  sys_f24.xpr, sys_f18.xpr, sysfont.tbl

Outputs:
  sys_f24.xpr (overwritten), sys_f18.xpr (overwritten), sysfont.tbl (overwritten)
  new_slots.txt — mapping of new SJIS codes to slot indices
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path
from typing import List, Tuple


BC1_PAGE_BYTES = 131072  # 512x512 DXT1 = 128 KB per page
HEADER_SIZE = 0x800      # 2048 bytes, fixed
DESCRIPTOR_SIZE = 20     # 5 x u32
SENTINEL = 0xFFFFFFFF
COMMON_FIELD = 0x00040001
FORMAT_FIELD = 0x09910C29  # 512x512 DXT1
TRANSPARENT_BC1_BLOCK = b"\x00\x00\x00\x00\xFF\xFF\xFF\xFF"
TRANSPARENT_BC1_PAGE = TRANSPARENT_BC1_BLOCK * (BC1_PAGE_BYTES // 8)
F18_FULLWIDTH_MARKER = 0x5A4E  # Runtime loader flips subsequent f18 entries back to FFxxxx.
EMPTY_SLOT_MARKER = 0x2E2E      # Runtime advances atlas coordinates for this placeholder.


def parse_xpr_descriptors(header: bytes) -> List[Tuple[int, int, int, int, int]]:
    """Parse texture descriptors from XPR header. Returns list of (common, data_offset, lock, format, padding)."""
    descriptors = []
    off = 0x0C
    while off + 4 <= HEADER_SIZE:
        val = struct.unpack_from("<I", header, off)[0]
        if val == SENTINEL:
            break
        if val == COMMON_FIELD and off + DESCRIPTOR_SIZE <= HEADER_SIZE:
            common = val
            data_offset = struct.unpack_from("<I", header, off + 4)[0]
            lock = struct.unpack_from("<I", header, off + 8)[0]
            fmt = struct.unpack_from("<I", header, off + 12)[0]
            pad = struct.unpack_from("<I", header, off + 16)[0]
            descriptors.append((common, data_offset, lock, fmt, pad))
            off += DESCRIPTOR_SIZE
        else:
            break
    return descriptors


def expand_xpr(xpr_path: Path, add_pages: int) -> None:
    """Add blank texture pages to an XPR file, updating header descriptors and total size."""
    if add_pages < 0:
        raise ValueError(f"{xpr_path}: add_pages must be >= 0")

    data = bytearray(xpr_path.read_bytes())

    if data[:4] != b"XPR0":
        raise ValueError(f"{xpr_path}: not an XPR0 file")

    total_size = struct.unpack_from("<I", data, 4)[0]
    header_size = struct.unpack_from("<I", data, 8)[0]
    assert header_size == HEADER_SIZE, f"Unexpected header size: {header_size}"

    header = bytearray(data[:HEADER_SIZE])
    tex_data = data[HEADER_SIZE:]

    existing_pages = len(tex_data) // BC1_PAGE_BYTES
    assert len(tex_data) % BC1_PAGE_BYTES == 0, "Texture data not aligned to page size"

    descriptors = parse_xpr_descriptors(bytes(header))
    assert len(descriptors) == existing_pages, \
        f"Descriptor count ({len(descriptors)}) != page count ({existing_pages})"

    print(f"  Existing: {existing_pages} pages, {len(descriptors)} descriptors")

    # Find sentinel position
    sentinel_off = 0x0C + len(descriptors) * DESCRIPTOR_SIZE
    assert struct.unpack_from("<I", header, sentinel_off)[0] == SENTINEL

    # Check we have room for new descriptors
    new_descriptors_end = sentinel_off + add_pages * DESCRIPTOR_SIZE + 4  # +4 for new sentinel
    if new_descriptors_end > HEADER_SIZE:
        raise ValueError(
            f"Not enough header space: need {new_descriptors_end} bytes, have {HEADER_SIZE}. "
            f"Cannot add {add_pages} pages."
        )

    # Write new descriptors
    for i in range(add_pages):
        page_idx = existing_pages + i
        data_offset = page_idx * BC1_PAGE_BYTES
        desc_off = sentinel_off + i * DESCRIPTOR_SIZE
        struct.pack_into("<I", header, desc_off, COMMON_FIELD)
        struct.pack_into("<I", header, desc_off + 4, data_offset)
        struct.pack_into("<I", header, desc_off + 8, 0)  # lock
        struct.pack_into("<I", header, desc_off + 12, FORMAT_FIELD)
        struct.pack_into("<I", header, desc_off + 16, 0)  # padding

    # Write new sentinel
    new_sentinel_off = sentinel_off + add_pages * DESCRIPTOR_SIZE
    struct.pack_into("<I", header, new_sentinel_off, SENTINEL)

    # Fill rest with 0xAD padding
    for i in range(new_sentinel_off + 4, HEADER_SIZE):
        header[i] = 0xAD

    # Append transparent pages so unused slots preview as empty instead of opaque black.
    new_tex_data = tex_data + (TRANSPARENT_BC1_PAGE * add_pages)

    # Update total size
    new_total = HEADER_SIZE + len(new_tex_data)
    struct.pack_into("<I", header, 4, new_total)

    # Write output
    xpr_path.write_bytes(bytes(header) + bytes(new_tex_data))

    new_pages = existing_pages + add_pages
    print(f"  Expanded: {new_pages} pages, file size {new_total} bytes")


def generate_new_sjis_codes(count: int, existing_codes: set) -> List[int]:
    """Generate new SJIS codes from unused lead byte ranges.

    The game uses TBL lookup (not standard SJIS decoding), so any double-byte
    code with a valid lead byte (0x81-0x9F, 0xE0-0xFC) and valid low byte
    (0x40-0x7E, 0x80-0xFC) works. We use 0xEB-0xFC which are all unused
    in the original TBL. Each lead byte has 188 valid low bytes.
    18 lead bytes x 188 = 3384 possible codes.
    """
    codes = []
    for lead in range(0xEB, 0xFD):  # 0xEB..0xFC
        for low in range(0x40, 0xFD):  # 0x40..0xFC
            if low == 0x7F:
                continue  # 0x7F is not valid in SJIS
            code = (lead << 8) | low
            if code not in existing_codes:
                codes.append(code)
                if len(codes) >= count:
                    return codes
    if len(codes) < count:
        print(f"  [warn] Only generated {len(codes)} new codes out of {count} requested")
    return codes


def count_f18_runtime_slots(codes: List[int]) -> int:
    # Runtime f18 atlas coordinates advance for every entry except the ZN/HN mode markers.
    return sum(1 for code in codes if code not in (0x5A4E, 0x484E))


def expand_tbl(tbl_path: Path, f24_add_pages: int, f18_add_pages: int) -> Tuple[int, int, List[int]]:
    """Expand sysfont.tbl to accommodate new pages.

    Returns (new_f18_count, new_f24_count, new_codes).
    """
    if f24_add_pages < 0 or f18_add_pages < 0:
        raise ValueError("Page deltas must be >= 0")

    data = tbl_path.read_bytes()

    # Parse existing structure
    off2 = struct.unpack("<I", data[:4])[0]
    f18_data = data[4:off2]
    f24_header = data[off2:off2 + 8]
    f24_data = data[off2 + 8:]

    f18_codes = [struct.unpack(">H", f18_data[i:i+2])[0] for i in range(0, len(f18_data), 2)]
    f24_codes = [struct.unpack(">H", f24_data[i:i+2])[0] for i in range(0, len(f24_data), 2)]

    print(f"  Existing f18: {len(f18_codes)} entries")
    print(f"  Existing f24: {len(f24_codes)} entries")

    # Calculate how many new slots each font gets
    # f24: 21x21 = 441 slots per page
    # f18: 28x28 = 784 slots per page (28.44 cells per 512px row at 18px each; game uses 28)
    f24_slots_per_page = 21 * 21  # 441
    f18_slots_per_page = 28 * 28  # 784

    f24_new_slots = f24_add_pages * f24_slots_per_page
    f18_new_slots = f18_add_pages * f18_slots_per_page

    print(f"  New f24 slots: {f24_new_slots} ({f24_add_pages} pages x {f24_slots_per_page})")
    print(f"  New f18 slots: {f18_new_slots} ({f18_add_pages} pages x {f18_slots_per_page})")

    # Need codes for the larger of the two (they share codes)
    # Actually, both tables need to grow independently
    total_new_codes = max(f24_new_slots, f18_new_slots)

    # Collect all existing codes
    existing = set(f18_codes) | set(f24_codes)
    new_codes = generate_new_sjis_codes(total_new_codes, existing)
    if len(new_codes) < total_new_codes:
        raise ValueError(
            f"Not enough free SJIS codes: need {total_new_codes}, got {len(new_codes)}"
        )

    print(f"  Generated {len(new_codes)} new SJIS codes")

    # Append to f24 table
    f24_append = new_codes[:f24_new_slots]
    f24_codes_new = f24_codes + f24_append

    # Append to f18 table.
    # The runtime loader advances atlas coordinates for every entry except ZN/HN markers.
    # Align the appended block to the next page boundary with 0x2E2E placeholders, then
    # insert one ZN marker so the new slots stay in the normal FFxxxx full-width table.
    f18_append = new_codes[:f18_new_slots]
    f18_runtime_slots = count_f18_runtime_slots(f18_codes)
    f18_fillers = (-f18_runtime_slots) % f18_slots_per_page
    f18_codes_new = (
        f18_codes
        + ([EMPTY_SLOT_MARKER] * f18_fillers)
        + [F18_FULLWIDTH_MARKER]
        + f18_append
    )

    # Rebuild TBL
    # f18 section: big-endian u16 array
    f18_bytes = b"".join(struct.pack(">H", c) for c in f18_codes_new)

    # f24 section: 8-byte header + big-endian u16 array
    f24_code_bytes = b"".join(struct.pack(">H", c) for c in f24_codes_new)

    # Update f24 header: bytes 4-5 (LE u16) = data byte count
    f24_header_new = bytearray(f24_header)
    struct.pack_into("<H", f24_header_new, 4, len(f24_code_bytes))

    # Update f24 offset (byte 0-3): points to start of f24 section
    new_off2 = 4 + len(f18_bytes)

    # Write
    out = bytearray()
    out += struct.pack("<I", new_off2)
    out += f18_bytes
    out += bytes(f24_header_new)
    out += f24_code_bytes

    tbl_path.write_bytes(bytes(out))

    print(f"  New f18: {len(f18_codes_new)} entries")
    print(f"  New f24: {len(f24_codes_new)} entries")
    print(f"  New TBL size: {len(out)} bytes")

    return len(f18_codes_new), len(f24_codes_new), new_codes


def write_slot_mapping(path: Path, new_codes: List[int],
                       f24_old_count: int, f18_old_count: int,
                       f24_new_count: int, f18_new_count: int) -> None:
    """Write a text file listing new SJIS codes and their slot indices."""
    lines = []
    lines.append("# New font slots added by expand_font.py")
    lines.append(f"# f24: {f24_old_count} -> {f24_new_count} entries ({f24_new_count - f24_old_count} new)")
    lines.append(f"# f18: {f18_old_count} -> {f18_new_count} entries ({f18_new_count - f18_old_count} new)")
    lines.append("#")
    lines.append("# Format: SJIS_hex  f24_slot_index  f18_slot_index")
    lines.append("")

    f24_added = f24_new_count - f24_old_count
    f18_added = f18_new_count - f18_old_count

    for i, code in enumerate(new_codes):
        f24_idx = f24_old_count + i if i < f24_added else "-"
        f18_idx = f18_old_count + i if i < f18_added else "-"
        lines.append(f"{code:04X}  {f24_idx}  {f18_idx}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description="Expand SMT NINE font XPR and TBL for more Chinese characters")
    ap.add_argument("--f24-pages", type=int, default=3,
                    help="Number of pages to add to sys_f24.xpr (default: 3, for 9->12)")
    ap.add_argument("--f18-pages", type=int, default=2,
                    help="Number of pages to add to sys_f18.xpr (default: 2, for 9->11)")
    ap.add_argument("--f24-xpr", type=Path, default=Path("sys_f24.xpr"))
    ap.add_argument("--f18-xpr", type=Path, default=Path("sys_f18.xpr"))
    ap.add_argument("--tbl", type=Path, default=Path("sysfont.tbl"))
    ap.add_argument("--dry-run", action="store_true", help="Only print what would be done")
    args = ap.parse_args()

    # Verify inputs exist
    for p in [args.f24_xpr, args.f18_xpr, args.tbl]:
        if not p.exists():
            raise FileNotFoundError(f"Required file not found: {p}")

    # Read original counts for mapping output
    tbl_data = args.tbl.read_bytes()
    off2 = struct.unpack("<I", tbl_data[:4])[0]
    f18_old_count = (off2 - 4) // 2
    f24_old_count = (len(tbl_data) - off2 - 8) // 2

    if args.dry_run:
        print("[dry-run] Would expand:")
        print(f"  sys_f24.xpr: +{args.f24_pages} pages ({9 + args.f24_pages} total)")
        print(f"  sys_f18.xpr: +{args.f18_pages} pages ({9 + args.f18_pages} total)")
        f24_new = f24_old_count + args.f24_pages * 441
        f18_new = f18_old_count + args.f18_pages * 784
        print(f"  sysfont.tbl f24: {f24_old_count} -> {f24_new}")
        print(f"  sysfont.tbl f18: {f18_old_count} -> {f18_new}")
        return

    # Step 1: Expand XPR files
    print(f"[1/3] Expanding sys_f24.xpr (+{args.f24_pages} pages)...")
    expand_xpr(args.f24_xpr, args.f24_pages)

    print(f"[2/3] Expanding sys_f18.xpr (+{args.f18_pages} pages)...")
    expand_xpr(args.f18_xpr, args.f18_pages)

    # Step 2: Expand TBL
    print(f"[3/3] Expanding sysfont.tbl...")
    f18_new_count, f24_new_count, new_codes = expand_tbl(
        args.tbl, args.f24_pages, args.f18_pages
    )

    # Step 3: Output mapping
    mapping_path = Path("new_slots.txt")
    write_slot_mapping(mapping_path, new_codes, f24_old_count, f18_old_count,
                       f24_new_count, f18_new_count)
    print(f"\n[ok] Slot mapping written to: {mapping_path}")
    print(f"[ok] All files expanded successfully.")
    print(f"     sys_f24.xpr: {args.f24_xpr.stat().st_size} bytes")
    print(f"     sys_f18.xpr: {args.f18_xpr.stat().st_size} bytes")
    print(f"     sysfont.tbl: {args.tbl.stat().st_size} bytes")


if __name__ == "__main__":
    main()
