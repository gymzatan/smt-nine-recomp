"""A constructor thunk found through data must expose its tail-call target."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo
from tools.disasm.xrefs import build_xrefs


def test_table_discovered_constructor_thunk_keeps_its_real_body():
    base = 0x10000
    ctor = base + 0x10
    thunk = base + 0x30
    data_va = base + 0x1000
    code = bytearray(b"\x90" * 0x40)
    code[0] = 0xC3
    code[0x10:0x19] = bytes.fromhex("8bc1c70003000000c3")
    code[0x30:0x3A] = (b"\xB9" + struct.pack("<I", data_va)
                       + b"\xE9" + struct.pack("<i", ctor - (thunk + 10)))
    text = SectionInfo(".text", base, len(code), 0, len(code), False, True, "X")
    table = SectionInfo(".rdata", data_va, 4, len(code), 4, False, False, "")
    image = BinaryImage("constructor-fixture", bytes(code) + struct.pack("<I", thunk),
                        base, 0x1004, base, 0, sections=[text, table])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, build_xrefs(engine, image), LabelManager())
    detector.detect_all([text])
    assert thunk in detector.functions
    assert ctor in detector.functions
    assert detector.functions[base].end <= ctor
    assert ctor + 9 <= detector.functions[ctor].end <= thunk
    assert detector.functions[thunk].start == thunk
