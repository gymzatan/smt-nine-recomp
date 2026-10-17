"""Build an isolated XBE popup layout patch; never touch the active game image."""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor/xboxrecomp"))
from tools.xbe_parser.xbe_parser import XBEParser
sys.path.insert(0, str(ROOT / "writers"))
sys.path.insert(0, str(ROOT / "writers"))
from apply_xbe import refresh_section_digests

WIDTH_PADDING = 42


def placement_assembly(name, clamp):
    if name == "world_popup_place":
        return f"push ecx; push dword ptr [esp+8]; call 0x106fe0; pop ecx; call {clamp}; ret 4"
    if name == "scene_popup_place":
        return f"push ecx; push dword ptr [esp+12]; push dword ptr [esp+12]; call 0x105930; pop ecx; call {clamp}; ret 8"
    raise ValueError("Unknown original popup placement wrapper")


def apply_width_padding(image, layout_receipt):
    """Update the existing measurement helper without replacing other patches."""
    patches = []
    old_padding = layout_receipt["width_padding"]
    for name, operation, operand, before, after in (
        ("popup_measure_lines", "add", "ecx", old_padding, WIDTH_PADDING),
        ("choice_marker_cells", "sub", "eax", old_padding + 18, WIDTH_PADDING + 18),
    ):
        item = layout_receipt["functions"][name]
        raw = layout_receipt["section_raw"] + item["address"] - layout_receipt["section_va"]
        body = bytes(image[raw:raw + item["bytes"]])
        instructions = list(Cs(CS_ARCH_X86, CS_MODE_32).disasm(body, item["address"]))
        expected_operands = {f"{operand}, 0x{value:x}" for value in (before, after)}
        matches = [i for i in instructions if i.mnemonic == operation and i.op_str in expected_operands]
        if len(matches) != 1:
            raise ValueError("Completed popup padding instruction differs: " + name)
        instruction = matches[0]
        replacement = bytes(Ks(KS_ARCH_X86, KS_MODE_32).asm(f"{operation} {operand}, {after}", instruction.address)[0])
        if len(replacement) != instruction.size:
            raise ValueError("Popup padding changes its original instruction boundary")
        at = raw + instruction.address - item["address"]
        image[at:at + instruction.size] = replacement
        patches.append({"function": name, "address": instruction.address,
                        "before": instruction.bytes.hex(), "after": replacement.hex()})
    for name in ("world_popup_place", "scene_popup_place"):
        item = layout_receipt["functions"][name]
        raw = layout_receipt["section_raw"] + item["address"] - layout_receipt["section_va"]
        previous = bytes(image[raw:raw + item["bytes"]])
        replacement = bytes(Ks(KS_ARCH_X86, KS_MODE_32).asm(
            placement_assembly(name, layout_receipt["functions"]["popup_clamp"]["address"]), item["address"])[0])
        if len(replacement) != item["bytes"]:
            raise ValueError("Popup placement changes its existing helper boundary")
        image[raw:raw + item["bytes"]] = replacement
        patches.append({"function": name, "address": item["address"],
                        "before": previous.hex(), "after": replacement.hex()})
    refresh_section_digests(image)
    return {"instructions": patches, "width_padding": WIDTH_PADDING}


def build(source, output, width=384):
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve() or output.exists():
        raise ValueError("output must be a new, isolated file")
    if width < 256 or width > 448 or width % 18 != 6:
        raise ValueError("width must be 258..438 on an 18-pixel grid; default 384")
    image = bytearray(source.read_bytes())
    parsed = XBEParser(str(source)).parse()
    header = parsed.header
    if header.num_sections != 13 or header.headers_size != 4096:
        raise ValueError("unexpected original section/header contract")
    section = parsed.sections[-1]
    if section.name != "$$XSIMAGE" or section.virtual_size != 4096 or section.raw_size != 4096:
        raise ValueError("unexpected final image resource section")
    va = section.virtual_addr + section.raw_size
    raw_offset = section.raw_addr + section.raw_size
    if len(image) < raw_offset + 4096 or any(image[raw_offset:raw_offset+4096]):
        raise ValueError("final section extension page is occupied")
    code = bytearray()
    functions = {}
    assembler = Ks(KS_ARCH_X86, KS_MODE_32)

    def emit(name, assembly):
        address = va + len(code)
        encoded, _ = assembler.asm(assembly, address)
        functions[name] = {"address": address, "bytes": len(encoded)}
        code.extend(bytes(encoded))
        return address

    measure_lines = emit("popup_measure_lines", f"""
        push ebx
        push edi
        mov ecx, edx
        xor eax, eax
        xor ebx, ebx
    measure_loop:
        cmp dword ptr [ecx], -1
        je measure_end
        mov edi, [ecx]
        cmp edi, ebx
        jle measure_next
        mov ebx, edi
    measure_next:
        inc eax
        add ecx, 4
        cmp eax, 32
        jl measure_loop
    measure_end:
        push eax
        cmp dword ptr [edx], 0
        jne measure_named
        fild dword ptr [esp]
        fmul dword ptr [0x3339e4]
        jmp measure_return
    measure_named:
        inc dword ptr [esp]
        fild dword ptr [esp]
        dec dword ptr [esp]
        fmul dword ptr [0x3339e4]
        fadd dword ptr [0x346b7c]
    measure_return:
        pop eax
        imul ecx, ebx, 18
        add ecx, {WIDTH_PADDING}
        cmp ecx, 132
        jge width_maximum
        mov ecx, 132
    width_maximum:
        cmp ecx, {width}
        jle width_convert
        mov ecx, {width}
    width_convert:
        push ecx
        fild dword ptr [esp]
        fstp dword ptr [esp]
        pop ecx
        pop edi
        pop ebx
        ret
    """)
    measure = emit("popup_measure", f"""
        mov edx, [ecx+0x194]
        jmp {measure_lines}
    """)
    # Both the original background draw and the placement policy call this
    # single measurement function; no second height formula is maintained.
    clamp = emit("popup_clamp", f"""
        pushad
        mov esi, ecx
        call {measure}
        sub esp, 20
        fstp dword ptr [esp]
        mov [esp+16], ecx
        mov eax, [esi+0x50]
        mov [esp+4], eax
        mov eax, [esi+0x54]
        mov [esp+8], eax
        mov [esp+12], ecx
        push 0x441c0000
        fld dword ptr [esp]
        fsub dword ptr [esp+16]
        fstp dword ptr [esp]
        pop ecx
        cmp dword ptr [esi+0x50], ecx
        jbe clamp_x_min
        mov [esi+0x50], ecx
    clamp_x_min:
        cmp dword ptr [esi+0x50], 0x41800000
        jae clamp_y
        mov dword ptr [esi+0x50], 0x41800000
    clamp_y:
        mov dword ptr [esp+12], 0x43e80000
        fld dword ptr [esp+12]
        fsub dword ptr [esp]
        cmp dword ptr [edx], 0
        jne clamp_y_limit
        fsub dword ptr [0x331990]
    clamp_y_limit:
        fstp dword ptr [esp+12]
        mov eax, [esi+0x54]
        cmp eax, [esp+12]
        jbe clamp_y_min
        mov eax, [esp+12]
        mov [esi+0x54], eax
    clamp_y_min:
        cmp dword ptr [esi+0x54], 0x41800000
        jae clamp_derived
        mov dword ptr [esi+0x54], 0x41800000
    clamp_derived:
        fld dword ptr [esi+0x50]
        fsub dword ptr [esp+4]
        fld dword ptr [esi+0x5c]
        fadd st(0), st(1)
        fstp dword ptr [esi+0x5c]
        fstp st(0)
        fld dword ptr [esi+0x5c]
        fadd dword ptr [esp+16]
        push 0x42100000
        fsub dword ptr [esp]
        add esp, 4
        fstp dword ptr [esi+0x68]
        fld dword ptr [esi+0x54]
        fsub dword ptr [esp+8]
        fld dword ptr [esi+0x60]
        fadd st(0), st(1)
        fstp dword ptr [esi+0x60]
        fld dword ptr [esi+0x6c]
        fadd st(0), st(1)
        fstp dword ptr [esi+0x6c]
        fstp st(0)
        mov eax, [esi+0x19c]
        mov edx, [esi+0x50]
        mov [eax+8], edx
        mov edx, [esi+0x54]
        mov [eax+12], edx
        add esp, 20
        popad
        ret
    """)
    rows = emit("choice_rows", """
        push ecx
        push edx
        mov eax, 1
    rows_loop:
        mov dl, [ecx]
        test dl, dl
        jz rows_done
        cmp dl, 10
        jne rows_next
        inc eax
    rows_next:
        inc ecx
        jmp rows_loop
    rows_done:
        pop edx
        pop ecx
        ret
    """)
    row_offset = emit("choice_row_offset", f"""
        push ebx
        push esi
        push edi
        lea esi, [ecx+0x3fc]
        mov edi, edx
        xor ebx, ebx
        test edi, edi
        jz offset_done
    offset_loop:
        mov ecx, esi
        call {rows}
        add ebx, eax
        add esi, 64
        dec edi
        jnz offset_loop
    offset_done:
        mov eax, ebx
        pop edi
        pop esi
        pop ebx
        ret
    """)
    choice_layout = emit("choice_layout", """
        pushad
        mov esi, eax
        mov edi, [esi+0x1940]
        imul edi, edi, 0x738
        add edi, esi
        mov ebx, [esi+0x1b8]
        imul ebx, ebx, 0x1cc
        mov edx, [ebx+esi+0x3c8]
        shl edx, 6
        add edx, ebx
        lea edx, [edx+esi+0x3fc]
        xor ecx, ecx
    layout_loop:
        mov al, [edx]
        test al, al
        jz layout_final
        cmp al, 13
        je layout_break
        cmp al, 10
        je layout_break
        inc ecx
        inc edx
        jmp layout_loop
    layout_break:
        mov eax, ecx
        shr eax, 1
        call layout_store
        xor ecx, ecx
        cmp byte ptr [edx], 13
        jne layout_advance
        cmp byte ptr [edx+1], 10
        jne layout_advance
        inc edx
    layout_advance:
        inc edx
        jmp layout_loop
    layout_final:
        mov eax, ecx
        shr eax, 1
        call layout_store
        mov dword ptr [edi+0x206c], 1
        popad
        ret
    layout_store:
        push ebx
        mov ebx, [edi+0x1c68]
        mov [edi+ebx*4+0x1c6c], eax
        inc ebx
        mov [edi+0x1c68], ebx
        mov dword ptr [edi+ebx*4+0x1c6c], -1
        pop ebx
        ret
    """)
    world_wrapper = emit("world_popup_place", placement_assembly("world_popup_place", clamp))
    other_wrapper = emit("scene_popup_place", placement_assembly("scene_popup_place", clamp))
    highlight = emit("choice_highlight_offset", f"""
        push eax
        push edx
        mov ecx, esi
        mov edx, [esi+0x1bc]
        call {row_offset}
        imul ecx, eax, 26
        pop edx
        pop eax
        ret
    """)
    next_choice = emit("choice_next_y", f"""
        push eax
        push ecx
        push edx
        mov ecx, edi
        call {rows}
        imul eax, eax, 26
        add ebx, eax
        add edi, 64
        pop edx
        pop ecx
        pop eax
        ret
    """)
    marker_cells = emit("choice_marker_cells", f"""
        push eax
        push ebx
        push edx
        mov edx, [esi+0x1940]
        imul edx, edx, 0x738
        lea edx, [esi+edx+0x1c6c]
        call {measure_lines}
        fstp st(0)
        push ecx
        fld dword ptr [esp]
        fistp dword ptr [esp]
        pop eax
        sub eax, {WIDTH_PADDING + 18}
        cdq
        mov ebx, 18
        idiv ebx
        mov ecx, eax
        pop edx
        pop ebx
        pop eax
        ret
    """)
    marker_count = emit("choice_marker_count", f"""
        push ecx
        call {marker_cells}
        mov [ebp-8], ecx
        pop ecx
        ret
    """)
    marker_right = emit("choice_marker_right", f"""
        push ecx
        call {marker_cells}
        inc ecx
        imul ecx, ecx, 18
        add edi, ecx
        pop ecx
        ret
    """)
    layout_info = functions["choice_layout"]
    layout_start = layout_info["address"]
    layout_end = layout_start + layout_info["bytes"]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    local_calls = {int(i.op_str, 16) for i in md.disasm(
        bytes(code[layout_start-va:layout_end-va]), layout_start)
        if i.mnemonic == "call" and i.op_str.startswith("0x")
        and layout_start < int(i.op_str, 16) < layout_end}
    if len(local_calls) != 1:
        raise ValueError("choice length-store callable entry is ambiguous")
    store_start = next(iter(local_calls))
    functions["choice_layout"]["bytes"] = store_start-layout_start
    functions["choice_store_length"] = {"address": store_start, "bytes": layout_end-store_start}
    edits = []

    def offset(address):
        matches = [s for s in parsed.sections if s.virtual_addr <= address < s.virtual_addr+s.raw_size]
        if len(matches) != 1:
            raise ValueError("patch has no unique original section")
        section = matches[0]
        return section.raw_addr + address - section.virtual_addr

    def patch(address, expected, replacement):
        expected, replacement = bytes(expected), bytes(replacement)
        if len(expected) != len(replacement):
            raise ValueError("patch changes original instruction region size")
        off = offset(address)
        if image[off:off+len(expected)] != expected:
            raise ValueError(f"original producer differs at {address:X}")
        image[off:off+len(expected)] = replacement
        edits.append({"address": address, "before": expected.hex(), "after": replacement.hex()})

    def call_patch(address, expected, target):
        expected = bytes.fromhex(expected) if isinstance(expected, str) else expected
        jump = bytes(assembler.asm(f"call {target}", address)[0])
        patch(address, expected, jump + b"\x90"*(len(expected)-len(jump)))

    original = (ROOT / "data/japanese/default.xbe").read_bytes()
    # Current layout instructions must still be the verified retail producers.
    def original_bytes(address, length):
        return original[offset(address):offset(address)+length]

    measure_call = bytes(assembler.asm(f"mov ecx, esi; call {measure}; mov [esp+0x24], ecx", 0x105D40)[0])
    patch(0x105D40, original_bytes(0x105D40, 30), measure_call+b"\x90"*(30-len(measure_call)))
    for address, length in [(0x105D88, 9), (0x105DA8, 6), (0x105DB2, 6),
                            (0x105DD5, 4), (0x105DF0, 6)]:
        patch(address, original_bytes(address, length), b"\x90"*length)
    # The width is saved in an allocated local, above the callee-saved registers.
    # Seven drawing arguments have been pushed when this operand is consumed.
    push_width = bytes(assembler.asm("push dword ptr [esp+0x40]; nop", 0x105E0C)[0])
    patch(0x105E0C, original_bytes(0x105E0C, 5), push_width)
    patch(0x105FF7, b"\x06", b"\x20")
    call_patch(0x102C4F, original_bytes(0x102C4F, 5), world_wrapper)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    other_calls = [i for i in md.disasm(original_bytes(0x1A2DA1, 0x1A2F7B-0x1A2DA1), 0x1A2DA1)
                   if i.mnemonic == "call" and i.op_str == "0x105930"]
    if len(other_calls) != 1:
        raise ValueError("scene popup setter call is ambiguous")
    call_patch(other_calls[0].address, other_calls[0].bytes, other_wrapper)
    call_patch(0x14FA32, original_bytes(0x14FA32, 0x14FADF-0x14FA32), choice_layout)
    call_patch(0x152691, original_bytes(0x152691, 9), highlight)
    # Keep the original CMP in its caller: the existing C generator keeps
    # condition state local to each function, so flags cannot cross this call.
    call_patch(0x152766, original_bytes(0x152766, 6), next_choice)
    # Match the selected-option marker to the wider popup, without changing
    # selected index, input, timer or any target label.
    call_patch(0x1526BF, original_bytes(0x1526BF, 7), marker_count)
    call_patch(0x1526F0, original_bytes(0x1526F0, 6), marker_right)
    if len(code) > 4096:
        raise ValueError("popup helper section exceeds one page")
    # Preserve the original image resource and append in its verified zero page.
    section_header = header.section_headers_addr - header.base_address + (header.num_sections-1)*56
    struct.pack_into("<I", image, section_header, section.flags | 6)
    struct.pack_into("<I", image, section_header+8, section.virtual_size+4096)
    struct.pack_into("<I", image, section_header+16, section.raw_size+len(code))
    struct.pack_into("<I", image, 0x10C, va+4096-header.base_address)
    image[raw_offset:raw_offset+len(code)] = code
    refresh_section_digests(image)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    checked = XBEParser(str(output)).parse()
    assert len(checked.sections) == 13 and checked.sections[-1].name == "$$XSIMAGE"
    assert image[section.raw_addr:raw_offset] == source.read_bytes()[section.raw_addr:raw_offset]
    receipt = {"input": str(source), "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
               "output": str(output), "output_sha256": hashlib.sha256(image).hexdigest(),
               "width_min": 132, "width_max": width, "width_padding": WIDTH_PADDING, "width_mode": "longest expanded layout row",
               "height_line_limit": 32, "font_advance": 18, "font_line_advance": 26,
               "source_runtime_analysis": "script_tools/local_lab/dialogue_runtime_control_flow.md",
               "functions": functions, "patches": edits, "section_va": va, "section_raw": raw_offset,
               "helper_section": "$$XSIMAGE", "original_resource_preserved": True,
               "tested_in_game": False, "active_image_modified": False,
               "pc_integration": "Apply the same patch to the prepared native XBE and compile these helper entries with the existing generator. Do not replace active native_game.xbe during the main chat's session."}
    output.with_suffix(".receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "default.xbe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=384)
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.output, args.width), indent=2))
