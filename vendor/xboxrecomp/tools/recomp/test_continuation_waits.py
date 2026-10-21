"""Persist the real kernel-thunk wait and resume through its original bridge."""
from pathlib import Path
import shutil
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module',params=['delay','multiple_any','multiple_all'])
def native_waits(tmp_path_factory,request):
    root=Path(__file__).resolve().parents[2]
    work=tmp_path_factory.mktemp('continuation_wait')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    multiple=request.param!='delay'
    wait_all=request.param=='multiple_all'
    args=(bytes.fromhex('68800002006a006a006a')+bytes([0 if wait_all else 1])+
          bytes.fromhex('68900002006a02')) if multiple else bytes.fromhex('68800002006a006a00')
    program={0x11000:bytes.fromhex('5589e5ff0500000200')+args+
             bytes.fromhex('ff1520000200a304000200ff0508000200c9c3')}
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
void sub_00011000(void);
void *fixture_memory_init(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return 0; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
static unsigned char *ram;
static const char *snapshot;
static int saving,cold;
static DWORD saved_remaining;
static ULONGLONG began;
static void checkpoint(void) {
    if(saving && GetTickCount64()-began>=400) {
        assert(t_kernel_wait && t_dispatch_depth==1);
        saved_remaining=t_kernel_wait->remaining_ms;assert(saved_remaining>200 && saved_remaining<800);
        XboxGuestRegisters state;xbox_CaptureGuestRegisters(&state);
        FILE *f=fopen(snapshot,"wb");assert(f);
        assert(fwrite(&saved_remaining,sizeof saved_remaining,1,f)==1);
        assert(fwrite(&state,sizeof state,1,f)==1);
        assert(fwrite(ram,0x100000,1,f)==1 && recomp_cont_save(f));
        assert(fclose(f)==0);saving=0;if(cold)exit(23);
    }
}
static void load(void) {
    XboxGuestRegisters state;FILE *f=fopen(snapshot,"rb");assert(f);
    assert(fread(&saved_remaining,sizeof saved_remaining,1,f)==1);
    assert(fread(&state,sizeof state,1,f)==1 && fread(ram,0x100000,1,f)==1);
    assert(xbox_RestoreGuestRegisters(&state) && recomp_cont_load(f));assert(fclose(f)==0);
}
'''
    suffix=r'''
static void check(void) {
    assert(g_esp==0x80000 && MEM32(0x20000)==1 && MEM32(0x20008)==1);
    assert(MEM32(0x20004)==EXPECTED_STATUS);
    assert(!recomp_cont_active() && !t_kernel_wait && t_dispatch_depth==0);
}
int main(int argc,char **argv) {
    assert(argc==3);snapshot=argv[2];ram=fixture_memory_init();
    g_slot_ordinals[0]=WAIT_ORDINAL;g_slot_bridges[0]=WAIT_BRIDGE;g_slot_arg_bytes[0]=WAIT_BYTES;
    BRIDGE_MEM32(0x20020)=KERNEL_VA_BASE;
    *(LONGLONG *)XBOX_TO_NATIVE(0x20080)=-10000000;
    /* Reconstruct fixture events at the same existing token slots. This test
     * covers execution/wait continuation, not full object serialization. */
    HANDLE first=CreateEventA(0,1,0,0),second=CreateEventA(0,1,0,0);assert(first && second);
    BRIDGE_MEM32(0x20090)=bridge_handle_token(first);
    BRIDGE_MEM32(0x20094)=bridge_handle_token(second);
    xbox_kernel_set_checkpoint_hook(checkpoint);
    int restored=!strcmp(argv[1],"load");
    if(restored) load();else {g_esp=0x80000;PUSH32(g_esp,0x76543210);}
    saving=!strcmp(argv[1],"save") || !strcmp(argv[1],"roundtrip");cold=!strcmp(argv[1],"save");
    began=GetTickCount64();sub_00011000();check();
    if(restored) {
        ULONGLONG elapsed=GetTickCount64()-began;
        assert(elapsed+20>=saved_remaining && elapsed<=saved_remaining+250);
    }
    if(!strcmp(argv[1],"roundtrip")) {
        load();began=GetTickCount64();sub_00011000();check();
        ULONGLONG elapsed=GetTickCount64()-began;
        assert(elapsed+20>=saved_remaining && elapsed<=saved_remaining+250);
    }
    printf("%08x %u %u %u\n",g_esp,MEM32(0x20000),MEM32(0x20004),MEM32(0x20008));
    CloseHandle(first);CloseHandle(second);return 0;
}
'''
    defines=(f'#define WAIT_ORDINAL {235 if multiple else 99}\n'
             f'#define WAIT_BYTES {24 if multiple else 12}\n'
             f'#define WAIT_BRIDGE {"bridge_NtWaitForMultipleObjectsEx" if multiple else "bridge_KeDelayExecutionThread"}\n'
             f'#define EXPECTED_STATUS {258 if multiple else 0}\n')
    (work/'memory.c').write_text(memory,encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    monkeypatch=pytest.MonkeyPatch()
    try:
        for mode,enabled in [('original',False),('continuation',True)]:
            source=work/(mode+'.c')
            source.write_text(defines+prefix+translate(monkeypatch,enabled,program)+suffix,encoding='utf-8')
            result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
                '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'src'),'/I'+str(root.parents[1]/'config'),str(work/'memory.c'),
                str(source),str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/kernel_sync.c'),str(root/'src/kernel/recomp_continuation.c'),
                '/Fe:'+str(work/(mode+'.exe')),'/link','/OPT:REF',*[str(p) for p in libraries],
                'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
                'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
                'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
                cwd=work,capture_output=True,text=True,errors='replace')
            assert result.returncode==0,result.stdout+result.stderr
        yield work
    finally:
        monkeypatch.undo()


@pytest.mark.parametrize('resume',['load','roundtrip'])
def test_original_wait_resumes_with_saved_remaining_time(native_waits,resume):
    snapshot=native_waits/(resume+'.state')
    expected=subprocess.run([str(native_waits/'original.exe'),'run',str(snapshot)],
                            capture_output=True,text=True,check=True,timeout=12)
    if resume=='load':
        saved=subprocess.run([str(native_waits/'continuation.exe'),'save',str(snapshot)],
                             capture_output=True,text=True,timeout=12)
        assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(native_waits/'continuation.exe'),resume,str(snapshot)],
                            capture_output=True,text=True,timeout=12)
    assert restored.returncode==0,restored.stdout+restored.stderr
    assert restored.stdout==expected.stdout
