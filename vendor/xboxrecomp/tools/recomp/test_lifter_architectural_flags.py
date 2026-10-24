"""Execute architectural status bits, including widths and preserved flags."""
from pathlib import Path
from .disasm import Instruction, Operand
from .lifter import Lifter
from .test_lifter_pushal import _build_and_run

HEADER=Path(__file__).resolve().parents[4]/'config/recomp_types.h'
PRELUDE=f'''#include "{HEADER.as_posix()}"
RECOMP_TLS uint32_t g_eflags=0x202u;
RECOMP_TLS int g_df;
static int interrupt_state=1;
int xbox_CpuInterruptFlag(void) {{return interrupt_state & 1;}}
void xbox_CpuLeaveInterrupt(int state) {{interrupt_state=state;}}
'''


def instruction(mnemonic,registers):
    insn=Instruction(0x10000,2,mnemonic,'','')
    insn.operands=[Operand(type='reg',reg=r) for r in registers]
    lifter=Lifter();lifter.architectural_flags=True; lifter.needs_cf=True
    return '\n'.join(lifter.lift_instruction(insn))


def szp(value,width):
    return ((value==0)<<6) | (((value>>(width-1))&1)<<7) | (((value&255).bit_count()%2==0)<<2)


def test_materialized_integer_flags():
    functions=[];cases=[];old=0x246fd7
    for width,dst,src in ((8,'al','cl'),(16,'ax','cx'),(32,'eax','ecx')):
        mask=(1<<width)-1;sign=1<<(width-1)
        values=sorted({0,1,2,15,16,sign-1,sign,sign+1,mask-1,mask})
        for m in ('cmp','test','add','sub','adc','sbb','and','or','xor','inc','dec','neg'):
            unary=m in ('inc','dec','neg')
            name=f'run_{m}_{width}';body=instruction(m,[dst] if unary else [dst,src])
            functions.append(f'''static uint32_t {name}(uint32_t a,uint32_t b,uint32_t incoming) {{
 uint32_t eax=a,ecx=b,_fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=0;
 g_eflags=incoming;{body}
 return g_eflags;
}}''')
            for a in values:
                for b in ([0] if unary else values):
                    for carry in (0,1):
                        incoming=(old&~1)|carry
                        if m in ('cmp','sub','sbb','dec','neg'):
                            aa=0 if m=='neg' else a
                            bb=a if m=='neg' else 1 if m=='dec' else b
                            c=carry if m=='sbb' else 0
                            wide=aa-bb-c;r=wide&mask;cf=wide<0
                            of=bool((aa^bb)&(aa^r)&sign);af=(aa^bb^r)&16
                        elif m in ('add','adc','inc'):
                            aa=a;bb=1 if m=='inc' else b;c=carry if m=='adc' else 0
                            wide=aa+bb+c;r=wide&mask;cf=wide>mask
                            of=bool((~(aa^bb))&(aa^r)&sign);af=(aa^bb^r)&16
                        else:
                            r={'test':a&b,'and':a&b,'or':a|b,'xor':a^b}[m]
                            cf=of=False;af=incoming&16
                        if m in ('inc','dec'):cf=carry
                        expected=(incoming&~0x8d5) | szp(r,width) | cf | (of<<11) | af
                        cases.append(f'if({name}(0x{a:x}u,0x{b:x}u,0x{incoming:x}u)!=0x{expected:x}u)return {len(cases)+1};')
    result=_build_and_run(PRELUDE+'\n'.join(functions)+'\nint main(void) {'+'\n'.join(cases)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_shift_count_zero_mask_and_overflow():
    functions=[];cases=[];old=0x246fd7
    for width,dst in ((8,'al'),(16,'ax'),(32,'eax')):
        mask=(1<<width)-1;sign=1<<(width-1)
        for m in ('shl','shr','sar'):
            name=f'run_{m}_{width}';body=instruction(m,[dst,'cl'])
            functions.append(f'''static uint32_t {name}(uint32_t a,uint32_t count,uint32_t *result) {{
 uint32_t eax=a,ecx=count,_fa=0,_fb=0;int32_t _fas=0,_fbs=0;int _cf=1;
 g_eflags=0x{old:x}u;{body}
 *result=eax&0x{mask:x}u;return g_eflags;
}}''')
            for a in (0,1,sign-1,sign,mask):
                for count in (0,1,2,7,8,15,16,17,31,32,33,255):
                    c=count&31;expected=old;r=a
                    if c:
                        signed=a if a<sign else a-(1<<width)
                        r=((a<<c) if m=='shl' else a>>c if m=='shr' else signed>>c)&mask
                        cf=((signed>>(c-1))&1) if m=='sar' else ((a>>(width-c))&1) if m=='shl' and c<=width else ((a>>(c-1))&1) if m=='shr' and c<=width else old&1
                        expected=(old&~0xc5)|szp(r,width)|cf
                        if c==1:
                            of=(((r>>(width-1))^cf)&1) if m=='shl' else a>>(width-1) if m=='shr' else 0
                            expected=(expected&~0x800)|(of<<11)
                    cases.append(f'if({name}(0x{a:x}u,{count},&out)!=0x{expected:x}u || out!=0x{r:x}u)return {len(cases)+1};')
    result=_build_and_run(PRELUDE+'\n'.join(functions)+'\nint main(void) {uint32_t out;'+'\n'.join(cases)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_sahf_lahf_and_saved_control_flags():
    sahf=instruction('sahf',[]);lahf=instruction('lahf',[])
    result=_build_and_run(PRELUDE+f'''int main(void) {{
 uint32_t eax;
 for(unsigned ah=0;ah<256;ah++) {{
   g_eflags=0x800u;eax=0x12340056u|(ah<<8);{sahf}
   if(g_eflags!=(0x802u|(ah&0xd5u)))return 1;
   eax=0x99880011u;{lahf}
   if(eax!=(0x99880011u|(((ah&0xd5u)|2u)<<8)))return 2;
 }}
 g_eflags=0x202;g_df=0;interrupt_state=1;
 recomp_write_eflags(0x00200c93u,32);
 if(!g_df || interrupt_state || !(recomp_read_eflags()&0x200000u))return 3;
 recomp_write_eflags(0x202u,16);
 if(g_df || interrupt_state!=1 || !(recomp_read_eflags()&0x200000u))return 4;
 if(recomp_read_eflags()&0x30000u)return 5;
 return 0;
}}''')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_translated_calls_joins_and_flag_stack(monkeypatch):
    from . import config
    from .translator import FunctionTranslator
    program={
        0x10000:bytes.fromhex('9c5a81f200002000529d9c58c3'),
        0x11000:bytes.fromhex('7406b803000000c3b805000000c3'),
        0x12000:bytes.fromhex('e8fb0f00000f92c0c3'),
        0x13000:bytes.fromhex('83e801c3'),
        0x14000:bytes.fromhex('e8fb0f00000f92c0c3'),
        0x15000:bytes.fromhex('b878563412c3'),
        0x16000:bytes.fromhex('85c0740583f901eb0383f9020f92c0c3'),
        0x17000:bytes.fromhex('31c0b001b120d2e00f94c0c3'),
        0x18000:bytes.fromhex('83c00183e801c3'),
        # NINE F5146 SUB ESP,3C then TEST EAX,EAX: only prior AF survives.
        0x19000:bytes.fromhex('83ec3c85c074009c5a8d64243cc3'),
    }
    data=bytearray(0xa000);functions={}
    for va,raw in program.items():
        data[va-0x10000:va-0x10000+len(raw)]=raw
        functions[va]={'start':hex(va),'end':va+len(raw),'size':len(raw),'name':f'sub_{va:08X}'}
    monkeypatch.setattr(config,'_SECTIONS',[config.Section('.text',0x10000,len(data),0,len(data),True)])
    translator=FunctionTranslator(bytes(data),functions,seh_prolog=0,seh_epilog=0)
    bodies={va:translator.translate_function(va,row) for va,row in functions.items()}
    assert bodies[0x18000].count('g_eflags =')==1
    assert 'g_eflags = (g_eflags & ~0x10u)' in bodies[0x19000]
    prefix='''#define RECOMP_GENERATED_CODE 1
'''+PRELUDE+'''
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x20000];
void guest_cpu_yield(void) {}
'''
    prototypes='\n'.join(f'void sub_{va:08X}(void);' for va in functions)
    main='''int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)memory;
 g_esp=0x10000;g_eflags=0x202;sub_00010000();
 if(g_eax!=0x200202 || g_esp!=0x10004)return 1;
 for(unsigned z=0;z<2;z++) {
   g_esp=0x10000;g_eflags=2|(z<<6);sub_00011000();
   if(g_eax!=(z?5u:3u))return 2;
 }
 g_esp=0x10000;g_eflags=0x202;g_eax=0;sub_00012000();
 if(g_eax!=0xffffff01u || g_esp!=0x10004)return 3;
 g_esp=0x10000;g_eflags=0x203;sub_00014000();
 if(g_eax!=0x12345601 || g_esp!=0x10004)return 4;
 for(unsigned a=0;a<2;a++)for(unsigned b=0;b<4;b++) {
   g_eax=a;g_ecx=b;g_esp=0x10000;sub_00016000();
   if(g_eax!=(b<(a?1u:2u)))return 5;
 }
 g_esp=0x10000;sub_00017000();if(g_eax!=1)return 6;
 g_esp=0x1000c;g_eax=0;g_eflags=0x202;sub_00019000();
 if(g_edx!=0x246 || g_esp!=0x10010)return 7;
 g_esp=0x10004;g_eax=0;g_eflags=0x202;sub_00019000();
 if(g_edx!=0x256 || g_esp!=0x10008)return 8;
 return 0;
}'''
    result=_build_and_run(prefix+prototypes+'\n'+'\n'.join(bodies.values())+main)
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
