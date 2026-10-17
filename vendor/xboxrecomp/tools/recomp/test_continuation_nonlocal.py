"""Resume armed guest jumps through the production native setjmp implementation."""
from pathlib import Path
import shutil
import subprocess

import pytest

from tools.recomp.test_continuations import translate

PROGRAM = {
    # Two live buffers in one parent; child jumps to the older buffer.
    0x11000: bytes.fromhex('5589e56800000200e8f32f000083c40485c00f8526000000'
                          '6800010200e8de2f000083c40485c00f8511000000'
                          'ff0540000200e8c80f0000ff0544000200c9c3'),
    0x12000: bytes.fromhex('5589e5b904000000ff05480002004975f76a076800000200e8e32f0000ccc9c3'),
    # Fixed x86 buffer stores consumed by the existing recomp_guest_longjmp.
    0x14000: bytes.fromhex('8b542404892a895a04897a0889720c89621031c0c3'),
    0x15000: bytes.fromhex('ccc3'),
}


@pytest.fixture(scope='module')
def native_jumps(tmp_path_factory):
    root=Path(__file__).resolve().parents[2]
    work=tmp_path_factory.mktemp('continuation_nonlocal')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    memory=r'''
#include "kernel/xbox_memory_layout.c"
#include <assert.h>
void *fixture_memory_init(void) {
    void *ram=VirtualAlloc(0,0x100000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(ram);
    g_memory_offset=(ptrdiff_t)ram;g_xbox_mem_offset=g_memory_offset;
    return ram;
}
'''
    prefix=r'''
#define RECOMP_GENERATED_CODE
#define guest_cpu_yield fixture_guest_cpu_yield
#include "recomp_types.h"
#include "kernel/xbox_memory_layout.h"
#include "kernel/recomp_continuation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void sub_00011000(void);void sub_00012000(void);void sub_00014000(void);void sub_00015000(void);
void *fixture_memory_init(void);
static unsigned char *ram;
static const char *snapshot;
static int saving,cold;
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return 0; }
static void load(void) {
    XboxGuestRegisters state;FILE *f=fopen(snapshot,"rb");assert(f);
    assert(fread(&state,sizeof state,1,f)==1);
    assert(fread(ram,0x100000,1,f)==1);
    assert(xbox_RestoreGuestRegisters(&state));assert(recomp_cont_load(f));
    assert(fclose(f)==0);
}
void guest_cpu_yield(void) {
    if(saving && MEM32(0x20048)==2) {
        XboxGuestRegisters state;xbox_CaptureGuestRegisters(&state);
        FILE *f=fopen(snapshot,"wb");assert(f);
        assert(fwrite(&state,sizeof state,1,f)==1);
        assert(fwrite(ram,0x100000,1,f)==1);assert(recomp_cont_save(f));
        assert(fclose(f)==0);saving=0;if(cold) exit(23);
    }
}
'''
    suffix=r'''
static void check(void) {
    assert(g_eax==7 && g_esp==0x80000);
    assert(MEM32(0x20040)==1 && MEM32(0x20044)==0 && MEM32(0x20048)==4);
    assert(!recomp_cont_active());
}
int main(int argc,char **argv) {
    assert(argc==3);snapshot=argv[2];ram=fixture_memory_init();
    if(!strcmp(argv[1],"load")) load();
    else { g_esp=0x80000;PUSH32(g_esp,0x76543210); }
    saving=!strcmp(argv[1],"save") || !strcmp(argv[1],"roundtrip");
    cold=!strcmp(argv[1],"save");
    sub_00011000();check();
    if(!strcmp(argv[1],"roundtrip")) { load();sub_00011000();check(); }
    printf("%u %08x %u %u %u\n",g_eax,g_esp,MEM32(0x20040),MEM32(0x20044),MEM32(0x20048));
    return 0;
}
'''
    (work/'memory.c').write_text(memory,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'))
    assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    monkeypatch=pytest.MonkeyPatch()
    try:
        for mode,enabled in [('original',False),('continuation',True)]:
            body=translate(monkeypatch,enabled,PROGRAM,setjmp_fn=0x14000,longjmp_fn=0x15000)
            source=work/(mode+'.c');source.write_text(prefix+body+suffix,encoding='utf-8')
            result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
                '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),
                str(work/'memory.c'),str(source),str(root/'src/kernel/kernel_hal.c'),
                str(root/'src/kernel/recomp_continuation.c'),'/Fe:'+str(work/(mode+'.exe')),
                '/link','/OPT:REF',*[str(p) for p in libraries],
                'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
                'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
                'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
                cwd=work,capture_output=True,text=True,errors='replace')
            assert result.returncode==0,result.stdout+result.stderr
        yield work
    finally:
        monkeypatch.undo()


@pytest.mark.parametrize('resume',['load','roundtrip'])
def test_existing_nonlocal_unwind_after_persisted_continuation(native_jumps,resume):
    snapshot=native_jumps/(resume+'.state')
    expected=subprocess.run([str(native_jumps/'original.exe'),'run',str(snapshot)],
                            capture_output=True,text=True,check=True,timeout=12)
    if resume=='load':
        saved=subprocess.run([str(native_jumps/'continuation.exe'),'save',str(snapshot)],
                             capture_output=True,text=True,timeout=12)
        assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(native_jumps/'continuation.exe'),resume,str(snapshot)],
                            capture_output=True,text=True,timeout=12)
    assert restored.returncode==0,restored.stdout+restored.stderr
    assert restored.stdout==expected.stdout
