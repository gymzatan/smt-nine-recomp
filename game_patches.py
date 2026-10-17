"""Prepare the same native inputs from the user retail disc and source translations."""
from pathlib import Path
import hashlib,json,sys
ROOT=Path(__file__).resolve().parent
PATCH=ROOT/'game/patches/rts_party_entry.bin'
ENTRY=0x1DE445
ORIGINAL_SHA256='d2b6b63ba8ff655f4c6c6883023a246ca67e6234d6db3914c3081d590822050f'
def apply_native_field_axes(image):
    # Original field shader templates: MUL R11.yw,R11.yyyx,c36.xxxx.
    # c36 supplies independent axis scales; preserve X for W, use Y for Y.
    templates=(
        ((0x36d7d4,0x36d9fc),'0000000054804400009800b40000b025'),
        ((0x36deb4,),'0000000054804400009800a40000b025'),
        ((0x36c434,0x36c65c,0x36cb14,0x36cd4c,0x36d024,0x36d59c),'0000000015804400009800240000002c'),
    )
    for offsets, instruction in templates:
      for offset in offsets:
        if image[offset:offset+16]!=bytes.fromhex(instruction):
            raise ValueError('Original field shader axis instruction differs')
        image[offset+10]|=0x20


def _prepare_original_layout(source_path, output):
    sys.path.insert(0, str(ROOT / 'writers'))
    sys.path.insert(0, str(ROOT / 'writers'))
    from apply_xbe import apply_font_sample_range
    sys.path.insert(0, str(ROOT / 'vendor/xboxrecomp'))
    from tools.xbe_parser.xbe_parser import XBEParser
    source = source_path.read_bytes()
    xbe = XBEParser(str(source_path)).parse()
    sections = [s for s in xbe.sections if s.virtual_addr <= ENTRY and ENTRY + 202 <= s.virtual_addr + s.raw_size]
    if len(sections) != 1:
        raise ValueError('Original RTS entry does not have one file-backed section')
    offset = sections[0].raw_addr + ENTRY - sections[0].virtual_addr
    if hashlib.sha256(source[offset:offset+202]).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original RTS entry differs from the verified input')
    patch = PATCH.read_bytes()
    if len(patch) != 202:
        raise ValueError('Manual RTS entry patch must preserve its original boundary')
    result = bytearray(source[:offset] + patch + source[offset+202:])
    apply_font_sample_range(result)
    apply_native_field_axes(result)
    output.parent.mkdir(exist_ok=True)
    if not output.exists() or output.read_bytes() != result:
        output.write_bytes(result)
    return output


def prepare_battle_camera(directory):
    """Keep the original camera consumer and convert its four frame constants."""
    path=Path(directory)/'recomp_0049.c'
    text=path.read_text(encoding='utf-8')
    start=text.index('void sub_001B5C6A(void)');end=text.index('\n}',start)+2
    body=text[start:end]
    declaration='    extern float smt9_battle_camera_constant(float,unsigned);'
    constants=('31B6E4','33E2B0','33E2AC','346B7C')
    if declaration not in body:
        body=body.replace('{\n','{\n'+declaration+'\n',1)
        for role,address in enumerate(constants):
            original=f'MEMF(0x{address})'
            assert body.count(original)==1,(path,address)
            body=body.replace(original,f'smt9_battle_camera_constant({original},{role})')
        path.write_text(text[:start]+body+text[end:],encoding='utf-8')
    for role,address in enumerate(constants):
        assert body.count(f'smt9_battle_camera_constant(MEMF(0x{address}),{role})')==1

