"""Cross-function conditional branches must expose an existing shared tail."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import Function, FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo
from tools.disasm.xrefs import build_xrefs


def fixture(target_offset=0x18):
    base=0x10000
    code=bytearray(b"\x90"*0x20)
    code[:10]=bytes.fromhex("83f8010f85")+struct.pack("<i",target_offset-9)+b"\xc3"
    code[0x10:0x16]=bytes.fromhex("b801000000c3")
    code[0x18:0x1b]=bytes.fromhex("31c0c3")
    section=SectionInfo(".text",base,len(code),0,len(code),False,True,"X")
    image=BinaryImage("conditional-tail-fixture",bytes(code),base,len(code),base,0,sections=[section])
    engine=DisasmEngine(image)
    engine.linear_sweep(section)
    detector=FunctionDetector(engine,image,build_xrefs(engine,image),LabelManager())
    detector.functions={base:Function(base,base+10,"caller"),base+0x10:Function(base+0x10,base+0x1b,"owner")}
    bodies=[(base,base+10),(base+0x10,base+0x1b)]
    return detector,bodies,[b[0] for b in bodies]


def test_cross_function_conditional_tail_is_callable_without_splitting_owner():
    detector,bodies,starts=fixture()
    assert detector._pass_cond_branch_orphans(bodies,starts)
    assert detector._alias_entries=={0x10018:0x1001b}
    detector._build_alias_entries()
    assert detector.functions[0x10018].end==0x1001b
    assert detector.functions[0x10010].end==0x1001b
    assert detector.functions[0x10000].end==0x1000a


def test_local_conditional_branch_does_not_create_an_entry():
    detector,bodies,starts=fixture(9)
    assert not detector._pass_cond_branch_orphans(bodies,starts)
    assert not detector._alias_entries


def test_mid_instruction_conditional_target_is_not_an_entry():
    detector,bodies,starts=fixture(0x19)
    assert not detector._pass_cond_branch_orphans(bodies,starts)
    assert not detector._alias_entries
