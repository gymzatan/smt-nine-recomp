"""Narrow divides, aliased operands, overflow traps and resumed instructions."""
from .test_lifter_architectural_flags import PRELUDE,instruction
from .test_lifter_pushal import _build_and_run


def test_division_width_and_aliased_divisor():
    helpers=PRELUDE+'''
RECOMP_TLS uint32_t g_ebp;
uint32_t xbox_CpuException(uint32_t c,uint32_t a,uint32_t b){exit(201);return 0;}
void xbox_ExceptionContinue(uint32_t a,uint32_t b){exit(202);}
'''
    functions=[];checks=[]
    for width,reg,alias in ((8,'cl','al'),(16,'cx','ax'),(32,'ecx','eax')):
        mask=(1<<width)-1;sign=1<<(width-1)
        for mnemonic in ('div','idiv'):
            for divisor_reg in (reg,alias):
                name=f'run_{mnemonic}_{width}_{divisor_reg}'
                functions.append(f'''static void {name}(uint32_t a,uint32_t d,uint32_t c,uint32_t *out) {{
 uint32_t eax=a,edx=d,ecx=c;{instruction(mnemonic,[divisor_reg])}
 out[0]=eax;out[1]=edx;
}}''')
                for dividend in (0,1,17,255,256,32767,32768,65535,65536,-1,-17,-32768,-32769):
                    if mnemonic=='div' and dividend<0:continue
                    bits=dividend&((1<<(width*2))-1)
                    if mnemonic=='idiv' and not -(1<<(width*2-1))<=dividend<(1<<(width*2-1)):continue
                    for denominator in (1,2,3,17,sign-1,-1,-3,-sign):
                        if mnemonic=='div' and denominator<0:continue
                        if divisor_reg==alias and (denominator&mask)!=(bits&mask):continue
                        quotient=(abs(dividend)//abs(denominator))*(-1 if (dividend<0)!=(denominator<0) else 1)
                        remainder=dividend-quotient*denominator
                        if not (-sign<=quotient<sign if mnemonic=='idiv' else 0<=quotient<=mask):continue
                        a=(0x76540000|(bits&65535)) if width==8 else (0x76540000&~mask)|(bits&mask)
                        d=0x98760000 if width==8 else (0x98760000&~mask)|((bits>>width)&mask)
                        ea=((a&0xffff0000)|((quotient&255)|((remainder&255)<<8))) if width==8 else (a&~mask)|(quotient&mask)
                        ed=d if width==8 else (d&~mask)|(remainder&mask)
                        checks.append(f'{name}(0x{a:x}u,0x{d:x}u,0x{denominator&mask:x}u,out);if(out[0]!=0x{ea:x}u||out[1]!=0x{ed:x}u)return {len(checks)+1};')
    result=_build_and_run(helpers+'\n'.join(functions)+'\nint main(void){uint32_t out[2];'+'\n'.join(checks)+'return 0;}')
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)


def test_translated_divide_traps_retry_and_skip(monkeypatch):
    from . import config
    from .translator import FunctionTranslator
    program={0x10000:bytes.fromhex('f7f1c3'),0x11000:bytes.fromhex('f7f9c3'),0x12000:bytes.fromhex('f730c3'),
             0x13000:bytes.fromhex('83f802f7f183c001c3')}
    data=bytearray(0x4000);functions={}
    for va,raw in program.items():
        data[va-0x10000:va-0x10000+len(raw)]=raw
        functions[va]={'start':hex(va),'end':va+len(raw),'size':len(raw),'name':f'sub_{va:08X}'}
    monkeypatch.setattr(config,'_SECTIONS',[config.Section('.text',0x10000,len(data),0,len(data),True)])
    translator=FunctionTranslator(bytes(data),functions,seh_prolog=0,seh_epilog=0)
    bodies='\n'.join(translator.translate_function(va,row) for va,row in functions.items())
    prefix='#define RECOMP_GENERATED_CODE 1\n'+PRELUDE+'''
RECOMP_TLS uint32_t g_ebp,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x20000];
static unsigned traps,skip;static uint32_t expected_a,expected_d;
void guest_cpu_yield(void){}
uint32_t xbox_CpuException(uint32_t code,uint32_t address,uint32_t frame){
 if(code!=0xc0000094u||g_eax!=expected_a||g_edx!=expected_d)exit(200);
 if(address==0x13003 && !(g_eflags&1u))exit(203);
 ++traps;g_ecx=1;g_edx=0;return address+(skip?2:0);
}
void xbox_ExceptionContinue(uint32_t a,uint32_t b){exit(201);}
'''
    main='''int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 g_esp=0x10000;g_eax=expected_a=17;g_edx=expected_d=0;g_ecx=0;sub_00010000();
 if(traps!=1||g_eax!=17||g_edx||g_esp!=0x10004)return 1;
 skip=1;g_esp=0x10000;g_eax=expected_a=0x80000000u;g_edx=expected_d=0x80000000u;g_ecx=0xffffffffu;sub_00011000();
 if(traps!=2||g_eax!=expected_a||g_esp!=0x10004)return 2;
 g_esp=0x10000;g_eax=expected_a=1;g_edx=expected_d=1;g_ecx=1;sub_00010000();
 if(traps!=3||g_eax!=1)return 3;
 g_esp=0x10000;g_eax=expected_a=0;g_edx=expected_d=0x80000000u;g_ecx=0xffffffffu;sub_00011000();
 if(traps!=4||g_eax!=0)return 4;
 g_esp=0x10000;g_eax=0x15000;g_edx=0;MEM32(g_eax)=13;sub_00012000();
 if(g_eax!=0x15000u/13u||g_edx!=0x15000u%13u)return 5;
 g_esp=0x10000;g_eax=expected_a=1;g_edx=expected_d=0;g_ecx=0;g_eflags=0x202;sub_00013000();
 if(traps!=5||g_eax!=2)return 6;
 return 0;
}'''
    result=_build_and_run(prefix+bodies+main)
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
