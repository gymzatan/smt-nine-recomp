"""Cold-process continuation of the actual lifted nested x86 call/loop path."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

# Original input instructions: root has a counted loop at the CALL itself;
# child initializes ECX then runs four iterations. Both entries have side effects.
PROGRAM = {
    0x11000: bytes.fromhex('5589e583ec0456c745fcdf9b5713ff0500000200be02000000e8e20f00004e75f88b45fc0305040002005ec9c3'),
    0x12000: bytes.fromhex('5589e5ff0508000200b904000000ff05040002004975f75dc3'),
}

def dispatch_program(kind):
    program = dict(PROGRAM)
    if kind == 'fpu':
        program[0x12000] = bytes.fromhex(
            '5589e5ff0508000200b904000000b820000200'
            'd900d94004dec183f902740190d9580883c00cff05040002004975e45dc3')
        return program, None
    if kind == 'direct':
        return program, None
    # The child clobbers EAX before the snapshot, so reading it again at a
    # restored CALL/JMP would dispatch to the wrong address.
    program[0x12000] = PROGRAM[0x12000][:3] + bytes.fromhex('b8efbeadde') + PROGRAM[0x12000][3:]
    if kind in ('register', 'guarded'):
        call = bytes.fromhex('b800200100ffd0')
    elif kind == 'stack':
        call = bytes.fromhex('6800200100ff54240083c404')
    else:
        call = bytes.fromhex('e8e21f0000')
        program[0x13000] = {
            'tail': bytes.fromhex('e9fbefffff'),
            'indirect_tail': bytes.fromhex('b800200100ffe0'),
            'conditional_tail': bytes.fromhex('85f60f85f8efffffc3'),
        }[kind]
    program[0x11000] = (PROGRAM[0x11000][:25] + call +
                        bytes([0x4e, 0x75, 256 - len(call) - 3]) + PROGRAM[0x11000][33:])
    guards = {0x1101e: [0x12000]} if kind == 'guarded' else None
    return program, guards


def translate(monkeypatch, continuations, program=None, guards=None, setjmp_fn=None, longjmp_fn=None):
    monkeypatch.setattr(config, '_SECTIONS', [config.Section('.text',0x11000,0x6000,0,0x6000,True)])
    data=bytearray(0x6000)
    functions={}
    for va, raw in (PROGRAM if program is None else program).items():
        data[va-0x11000:va-0x11000+len(raw)]=raw
        functions[va]={'start':hex(va),'end':va+len(raw),'size':len(raw),'name':f'sub_{va:08X}'}
    translator=FunctionTranslator(bytes(data),functions,seh_prolog=0,seh_epilog=0,
                                  icall_sites=guards,continuations=continuations,
                                  setjmp_fn=setjmp_fn,longjmp_fn=longjmp_fn)
    return '\n'.join(translator.translate_function(a,f) for a,f in functions.items())

@pytest.fixture(scope='module', params=['direct', 'fpu', 'register', 'stack', 'guarded',
                                      'tail', 'indirect_tail', 'conditional_tail'])
def binaries(tmp_path_factory, request):
    work=tmp_path_factory.mktemp('continuation')
    root=Path(__file__).resolve().parents[2]
    compiler=shutil.which('cl'); assert compiler, 'requires existing MSVC environment'
    monkeypatch=pytest.MonkeyPatch()
    prefix=r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "kernel/recomp_continuation.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
void sub_00011000(void); void sub_00012000(void); void sub_00013000(void);
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;
uint32_t g_xbox_code_lo=0x11000,g_xbox_code_hi=0x14000;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_trace_idx;
volatile uint64_t g_icall_count;
recomp_func_t recomp_lookup(uint32_t va) { return va==0x12000?sub_00012000:NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va;return NULL; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top, g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word=0x027f, g_fp_cc;
static const char *snapshot;
static int saving;
static unsigned char *ram;
static void write_arch(FILE *f) {
    uint32_t regs[]={g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp};
    assert(fwrite(regs,sizeof regs,1,f)==1);
    assert(fwrite(g_fp_stack,sizeof g_fp_stack,1,f)==1);
    assert(fwrite(&g_fp_top,sizeof g_fp_top,1,f)==1);
    assert(fwrite(ram,0x100000,1,f)==1);
}
static void read_arch(FILE *f) {
    uint32_t r[9];assert(fread(r,sizeof r,1,f)==1);
    g_eax=r[0];g_ecx=r[1];g_edx=r[2];g_esp=r[3];g_ebx=r[4];g_esi=r[5];
    g_edi=r[6];g_ebp=r[7];g_seh_ebp=r[8];
    assert(fread(g_fp_stack,sizeof g_fp_stack,1,f)==1);
    assert(fread(&g_fp_top,sizeof g_fp_top,1,f)==1);
    assert(fread(ram,0x100000,1,f)==1);
}
void guest_cpu_yield(void) {
#ifdef TEST_FPU
    unsigned step=MEM32(0x20004)%4;
    if(step) assert(g_eax==0x20020+12*step && g_ecx==4-step && g_fp_top==5 && g_fp_stack[5]==7.25);
#endif
    if (saving && MEM32(0x20004)==2) {
        FILE *f=fopen(snapshot,"wb");assert(f);
        write_arch(f);assert(recomp_cont_save(f));assert(fclose(f)==0);
        exit(23);
    }
}
'''
    suffix=r'''
int main(int argc,char **argv) {
    assert(argc==3);snapshot=argv[2];ram=calloc(1,0x100000);assert(ram);
    g_xbox_mem_offset=(ptrdiff_t)ram;
#ifdef TEST_FPU
    g_fp_top=5;g_fp_stack[5]=7.25;
    const uint32_t input[]={0x3f800000,0x3f000000,0,0x80000000,0x80000000,0,
                            0x7f800000,0x3f800000,0,0x7fc12345,0,0};
    memcpy(ram+0x20020,input,sizeof input);
#endif
    if (!strcmp(argv[1],"opaque") || !strcmp(argv[1],"unsafe_leaf") || !strcmp(argv[1],"loop_parent")) {
        RecompContFrame outer,opaque,inner;
        recomp_cont_enter(&outer,0x11000,NULL,0);
        recomp_cont_site(&outer,0x100011000ULL);
        FILE *f=tmpfile();assert(f);
        if (!strcmp(argv[1],"opaque")) {
            recomp_cont_enter(&opaque,0xfe000004,NULL,0);
            recomp_cont_enter(&inner,0x12000,NULL,0);
            recomp_cont_site(&inner,0x200012000ULL);
            assert(!recomp_cont_save(f));
            recomp_cont_leave(&inner);recomp_cont_leave(&opaque);
        } else if (!strcmp(argv[1],"loop_parent")) {
            recomp_cont_site(&outer,0x200011000ULL);
            recomp_cont_enter(&inner,0x12000,NULL,0);
            recomp_cont_site(&inner,0x200012000ULL);
            assert(!recomp_cont_save(f));recomp_cont_leave(&inner);
        } else assert(!recomp_cont_save(f));
        recomp_cont_leave(&outer);assert(!recomp_cont_active());
        fclose(f);free(ram);return 0;
    }
    if (!strcmp(argv[1],"locals") || !strcmp(argv[1],"wrong_function")) {
        uint32_t integer=0x89abcdef;
        uint64_t bits=0x7ff8000000000042ULL,actual=0;
        double floating;memcpy(&floating,&bits,8);
        RecompContLocal locals[]={{"ebp",&integer,4},{"_fca",&floating,8}};
        RecompContFrame frame;
        assert(recomp_cont_enter(&frame,0x11000,locals,2)==0);
        recomp_cont_site(&frame,0x200011000ULL);
        FILE *f=tmpfile();assert(f);assert(recomp_cont_save(f));
        recomp_cont_leave(&frame);integer=0;floating=0.0;rewind(f);
        assert(recomp_cont_load(f));assert(fclose(f)==0);
        uint32_t function=!strcmp(argv[1],"wrong_function")?0x12000:0x11000;
        assert(recomp_cont_enter(&frame,function,locals,2)==0x200011000ULL);
        memcpy(&actual,&floating,8);assert(integer==0x89abcdef && actual==bits);
        recomp_cont_leave(&frame);free(ram);return 0;
    }
    if (!strcmp(argv[1],"truncated")) {
        FILE *f=tmpfile();assert(f);assert(fwrite("XBCONJ",6,1,f)==1);
        uint32_t fields[]={1,0x11000};assert(fwrite(fields,sizeof fields,1,f)==1);
        rewind(f);assert(!recomp_cont_load(f));fclose(f);free(ram);return 0;
    }
    if (!strcmp(argv[1],"bad_chain")) {
        FILE *f=tmpfile();assert(f);assert(fwrite("XBCONJ",6,1,f)==1);
        uint32_t count=2,none=0;assert(fwrite(&count,4,1,f)==1);
        for(uint32_t fn=0x11000;fn<=0x12000;fn+=0x1000) {
            uint64_t site=(2ULL<<32)|fn;
            assert(fwrite(&fn,4,1,f)==1 && fwrite(&site,8,1,f)==1);
            assert(fwrite(&none,4,1,f)==1 && fwrite(&none,4,1,f)==1);
        }
        rewind(f);assert(!recomp_cont_load(f));fclose(f);free(ram);return 0;
    }
    if (!strcmp(argv[1],"load")) {
        FILE *f=fopen(snapshot,"rb");assert(f);read_arch(f);
        assert(recomp_cont_load(f));assert(fclose(f)==0);
    } else {
        g_esp=0x80000;g_ebp=0x87654321;g_esi=0x12345678;
        PUSH32(g_esp,0x76543210);
    }
    saving=!strcmp(argv[1],"save");sub_00011000();
    assert(MEM32(0x20000)==1 && MEM32(0x20008)==2 && MEM32(0x20004)==8);
    assert(g_eax==0x13579be7 && g_esp==0x80000 && g_esi==0x12345678);
#ifdef TEST_FPU
    assert(MEM32(0x20028)==0x3fc00000 && MEM32(0x20034)==0x80000000);
    assert(MEM32(0x20040)==0x7f800000 && MEM32(0x2004c)==0x7fc12345 && g_fp_top==5 && g_fp_stack[5]==7.25);
#endif
    printf("%08x %08x %08x %u %u %u\n",g_eax,g_esp,g_esi,
           MEM32(0x20000),MEM32(0x20008),MEM32(0x20004));
    free(ram);return 0;
}
'''
    try:
        program, guards = dispatch_program(request.param)
        if request.param == 'fpu':
            prefix = '#define TEST_FPU\n' + prefix
        cache=FunctionTranslator._cache_fpu_block_gprs
        for mode,enabled,cached in [('original',False,False),('cached',False,True),('continuation',True,True)]:
            monkeypatch.setattr(FunctionTranslator,'_cache_fpu_block_gprs',cache if cached else lambda self,statements,*args:statements)
            generated=translate(monkeypatch,enabled,program,guards)
            if request.param == 'fpu' and cached:
                assert '_block_00012013_g_fp_top' in generated
                assert '"_block_' not in generated
            source=work/(mode+'.c');source.write_text(prefix+generated+suffix,encoding='utf-8')
            result=subprocess.run([compiler,'/nologo','/O2','/utf-8','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),str(source),
                str(root/'src/kernel/recomp_continuation.c'),'/Fe:'+str(work/(mode+'.exe'))],
                cwd=work,capture_output=True,text=True,errors='replace')
            assert result.returncode==0,result.stdout+result.stderr
        yield work
    finally:
        monkeypatch.undo()

def test_cold_process_nested_call_and_loop_resume(binaries):
    snapshot=binaries/'guest.cont'
    expected=subprocess.run([str(binaries/'original.exe'),'run',str(snapshot)],capture_output=True,text=True,check=True)
    cached=subprocess.run([str(binaries/'cached.exe'),'run',str(snapshot)],capture_output=True,text=True,check=True)
    assert cached.stdout==expected.stdout
    normal=subprocess.run([str(binaries/'continuation.exe'),'run',str(snapshot)],capture_output=True,text=True,check=True)
    assert normal.stdout==expected.stdout
    saved=subprocess.run([str(binaries/'continuation.exe'),'save',str(snapshot)],capture_output=True,text=True)
    assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(binaries/'continuation.exe'),'load',str(snapshot)],capture_output=True,text=True,check=True)
    assert restored.stdout==expected.stdout

def test_call_and_loop_at_same_instruction_have_distinct_sites(monkeypatch):
    code=translate(monkeypatch,True)
    assert 'case 0x100011019ULL: goto cont_call_00011019;' in code
    assert 'case 0x200011019ULL: goto loc_00011019;' in code

def test_existing_emission_stays_unchanged_by_default(monkeypatch):
    code=translate(monkeypatch,False)
    assert 'recomp_cont_' not in code
    assert 'PUSH32(esp, 0x0001101Eu); RECOMP_ABI_CALL' in code

@pytest.mark.parametrize('mode,expected',[('locals',0),('truncated',0),('wrong_function',5),
                                        ('opaque',0),('unsafe_leaf',0),('loop_parent',0),('bad_chain',0)])
def test_local_fidelity_and_explicit_restore_rejection(binaries,mode,expected):
    result=subprocess.run([str(binaries/'continuation.exe'),mode,str(binaries/'state')],capture_output=True,text=True)
    assert result.returncode==expected,result.stdout+result.stderr

def test_indirect_and_nonlocal_calls_keep_existing_dispatch(monkeypatch):
    monkeypatch.setattr(config,'_SECTIONS',[config.Section('.text',0x11000,3,0,3,True)])
    f={'start':'0x11000','end':0x11003,'name':'sub_00011000'}
    t=FunctionTranslator(bytes.fromhex('ffd0c3'),{0x11000:f},seh_prolog=0,seh_epilog=0,continuations=True)
    code=t.translate_function(0x11000,f)
    assert '_cont_target_00011000' in code
    raw=bytes.fromhex('e8fb0f0000c3')
    target={'start':'0x12000','end':0x12001,'name':'sub_00012000'}
    f={'start':'0x11000','end':0x11006,'name':'sub_00011000'}
    t=FunctionTranslator(raw,{0x11000:f,0x12000:target},seh_prolog=0,seh_epilog=0,
                         setjmp_fn=0x12000,continuations=True)
    assert 'recomp_cont_jump_bind' in t.translate_function(0x11000,f)

def test_unresolved_callee_retains_original_strict_entry(monkeypatch):
    monkeypatch.setattr(config,'_SECTIONS',[config.Section('.text',0x11000,6,0,6,True)])
    f={'start':'0x11000','end':0x11006,'name':'sub_00011000'}
    t=FunctionTranslator(bytes.fromhex('e8fb0f0000c3'),{0x11000:f},seh_prolog=0,seh_epilog=0,continuations=True)
    code=t.translate_function(0x11000,f)
    assert 'RECOMP_ABI_CALL(0x00012000u, sub_00012000)' in code
    assert 'cont_call_00011000:' in code
    assert t.lifter.referenced_calls[0x12000]=='sub_00012000'
