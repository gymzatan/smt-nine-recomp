"""Execute decoded IN/OUT lifts, checking width, truncation and subregisters."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

from .disasm import Disassembler
from .lifter import Lifter


@pytest.mark.parametrize("opcode, read, width, immediate", [
    ("ec", True, 1, False), ("66ed", True, 2, False),
    ("ed", True, 4, False), ("e4c0", True, 1, True),
    ("66e5c0", True, 2, True), ("e5c0", True, 4, True),
    ("ee", False, 1, False), ("66ef", False, 2, False),
    ("ef", False, 4, False), ("e6c0", False, 1, True),
    ("66e7c0", False, 2, True), ("e7c0", False, 4, True),
])
def test_decoded_port_instruction_executes(opcode, read, width, immediate):
    compiler = shutil.which("cl")
    if not compiler:
        pytest.skip("requires the MSVC developer environment")
    decoder = Disassembler()
    insn = decoder._decode_instruction(next(decoder._cs.disasm(bytes.fromhex(opcode), 0x2F941C)))
    lifter = Lifter()
    statements = "\n".join(lifter.lift_instruction(insn))
    assert not lifter.unimplemented
    mask = (1 << (width * 8)) - 1
    initial = 0xA1B2C3D4
    input_value = 0x88776655
    expected_eax = ((initial & ~mask) | (input_value & mask)) if read else initial
    expected_port = 0xC0 if immediate else 0x80C0
    header = Path(__file__).resolve().parents[2] / "templates/runtime/recomp_types.h"
    program = f'''
#include "{header.as_posix()}"
#include <assert.h>
RECOMP_TLS uint32_t g_eax, g_edx;
#define eax g_eax
#define edx g_edx
static unsigned calls;
uint32_t xbox_PortRead(uint16_t p, unsigned w, uint32_t va) {{
    assert(p == {expected_port}); assert(w == {width}); assert(va == 0x2F941C);
    ++calls; return 0x{input_value:X}u;
}}
void xbox_PortWrite(uint16_t p, uint32_t v, unsigned w, uint32_t va) {{
    assert(p == {expected_port}); assert(w == {width}); assert(va == 0x2F941C);
    assert(v == 0x{initial & mask:X}u); ++calls;
}}
int main(void) {{
    g_eax = 0x{initial:X}u; g_edx = 0xCAFE80C0u;
    {statements}
    assert(g_eax == 0x{expected_eax:X}u); assert(g_edx == 0xCAFE80C0u);
    assert(calls == 1); return 0;
}}
'''
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "ports.c"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "/nologo", "/O2", str(source),
                                f"/Fe:{root / 'ports.exe'}"],
                               cwd=root, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr
        subprocess.run([str(root / "ports.exe")], check=True)
