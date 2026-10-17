"""Restore the original PsCreateSystemThreadEx inline bootstrap call chain."""
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module')
def native_inline(tmp_path_factory):
    root=Path(__file__).resolve().parents[2]
    work=tmp_path_factory.mktemp('continuation_inline_main')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    args=[0x20010,0,0,0,0,0x11223344,0x55667788,0,0,0x12000]
    pushes=b''.join(b'\x68'+struct.pack('<I',v) for v in reversed(args))
    program={
        0x11000:bytes.fromhex('5589e5ff0500000200')+pushes+
                 bytes.fromhex('ff1520000200a304000200ff0514000200c9c3'),
        0x12000:bytes.fromhex('5589e5ff0508000200b904000000ff050c0002004975f7'
                              '8b450833450ca318000200c9c3'),
    }
    memory=r'''
#include "kernel/xbox_memory_layout.c"
#include <assert.h>
void *fixture_memory_init(void) {
    void *ram=VirtualAlloc(0,0x100000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(ram);
    g_memory_offset=(ptrdiff_t)ram;g_xbox_mem_offset=g_memory_offset;return ram;
}
'''
    prefix=r'''
#include "kernel/kernel_bridge.c"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
void sub_00011000(void);void sub_00012000(void);
void *fixture_memory_init(void);
recomp_func_t recomp_lookup(uint32_t va) { return va==0x12000?sub_00012000:0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return 0; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
static unsigned char *ram;
static const char *snapshot;
static int saving,cold;
static void checkpoint(void) {
    if(saving && MEM32(0x2000c)==2) {
        assert(t_dispatch_depth==0 && t_guest_held && t_kernel_wait);
        assert((t_kernel_wait->frame.site>>32)==6 && g_thread_call_count==1);
        XboxGuestRegisters registers;xbox_CaptureGuestRegisters(&registers);
        XboxThreadCreationState creation;xbox_CaptureThreadCreationState(&creation);
        FILE *f=fopen(snapshot,"wb");assert(f);
        assert(fwrite(&registers,sizeof registers,1,f)==1);
        assert(fwrite(&creation,sizeof creation,1,f)==1);
        assert(fwrite(ram,0x100000,1,f)==1 && recomp_cont_save(f));assert(fclose(f)==0);
        saving=0;if(cold)exit(23);
    }
}
static void load(void) {
    XboxGuestRegisters registers;XboxThreadCreationState creation;
    FILE *f=fopen(snapshot,"rb");assert(f);
    assert(fread(&registers,sizeof registers,1,f)==1);
    assert(fread(&creation,sizeof creation,1,f)==1 && fread(ram,0x100000,1,f)==1);
    assert(xbox_RestoreGuestRegisters(&registers));
    assert(xbox_RestoreThreadCreationState(&creation));
    assert(recomp_cont_load(f) && fclose(f)==0);
}
'''
    suffix=r'''
static void check(void) {
    assert(g_esp==0x80000 && MEM32(0x20004)==0 && MEM32(0x20018)==0x444444cc);
    assert(MEM32(0x20000)==1 && MEM32(0x20008)==1 && MEM32(0x2000c)==4 && MEM32(0x20014)==1);
    assert(MEM32(0x20010)==0xbeef0001 && g_thread_call_count==1 && g_thread_mode==0);
    assert(!recomp_cont_active() && !t_kernel_wait && t_dispatch_depth==0 && t_guest_held);
}
int main(int argc,char **argv) {
    assert(argc==3);snapshot=argv[2];ram=fixture_memory_init();
    _putenv_s("RECOMP_GUEST_LOCK","1");guest_cpu_join();
    g_slot_ordinals[0]=255;g_slot_bridges[0]=bridge_PsCreateSystemThreadEx;g_slot_arg_bytes[0]=40;
    BRIDGE_MEM32(0x20020)=KERNEL_VA_BASE;xbox_kernel_set_checkpoint_hook(checkpoint);
    if(!strcmp(argv[1],"load")) load();else {g_esp=0x80000;PUSH32(g_esp,0x76543210);}
    saving=!strcmp(argv[1],"save") || !strcmp(argv[1],"roundtrip");cold=!strcmp(argv[1],"save");
    sub_00011000();check();
    if(!strcmp(argv[1],"roundtrip")) {load();sub_00011000();check();}
    printf("%08x %08x %u %u %u %u %d\n",g_esp,MEM32(0x20004),MEM32(0x20000),
           MEM32(0x20008),MEM32(0x2000c),MEM32(0x20014),g_thread_call_count);
    guest_cpu_part();return 0;
}
'''
    (work/'memory.c').write_text(memory,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    monkeypatch=pytest.MonkeyPatch()
    try:
        for mode,enabled in [('original',False),('continuation',True)]:
            source=work/(mode+'.c');source.write_text(prefix+translate(monkeypatch,enabled,program)+suffix,encoding='utf-8')
            result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
                '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),str(work/'memory.c'),
                str(source),str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/recomp_continuation.c'),
                '/Fe:'+str(work/(mode+'.exe')),'/link','/OPT:REF',*[str(p) for p in libraries],
                'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
                'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
                'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
                cwd=work,capture_output=True,text=True,errors='replace')
            assert result.returncode==0,result.stdout+result.stderr
        yield work
    finally:monkeypatch.undo()


@pytest.mark.parametrize('resume',['load','roundtrip'])
def test_original_inline_bootstrap_not_replayed(native_inline,resume):
    snapshot=native_inline/(resume+'.state')
    expected=subprocess.run([str(native_inline/'original.exe'),'run',str(snapshot)],
                            capture_output=True,text=True,check=True,timeout=12)
    if resume=='load':
        saved=subprocess.run([str(native_inline/'continuation.exe'),'save',str(snapshot)],
                             capture_output=True,text=True,timeout=12)
        assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(native_inline/'continuation.exe'),resume,str(snapshot)],
                            capture_output=True,text=True,timeout=12)
    assert restored.returncode==0,restored.stdout+restored.stderr
    assert restored.stdout==expected.stdout
