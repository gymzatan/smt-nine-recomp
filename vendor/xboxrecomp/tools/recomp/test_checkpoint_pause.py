"""Freeze original guest workers and real APU work at their existing boundaries."""
from pathlib import Path
import shutil
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module')
def native_pause(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_pause')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    program={0x11000:bytes.fromhex('5589e58b5508ff02a11000020085c074f5c9c20800')}
    memory=r'''
#include "kernel/xbox_memory_layout.c"
#include <assert.h>
void *fixture_memory_init(void) {
    void *ram=VirtualAlloc(0,0x02000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(ram);
    g_memory_offset=(ptrdiff_t)ram;g_xbox_mem_offset=g_memory_offset;g_xbox_map_size=0x02000000;
    g_heap_next=0x100000;g_tls_template_va=0x6000;g_tls_total=16;g_tls_thread_size=64;return ram;
}
'''
    prefix=r'''
#include "kernel/kernel_bridge.c"
#include "apu/apu.h"
#include "apu/apu_state.h"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
void sub_00011000(void);void *fixture_memory_init(void);
recomp_func_t recomp_lookup(uint32_t va) { return va==0x11000?sub_00011000:0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return 0; }
static unsigned char *ram;
static volatile LONG stop_device;
static void *physical_resolve(uint32_t address,uint32_t bytes) {
    return address<0x02000000 && bytes<=0x02000000-address?ram+address:NULL;
}
static DWORD WINAPI device_worker(void *arg) {
    (void)arg;
    while(!stop_device) {
        xbox_CheckpointDeviceEnter(0);
        xbox_CheckpointDeviceEnter(0);
        MEM32(0x20008)++;Sleep(2);
        xbox_CheckpointDeviceLeave(0);
        xbox_CheckpointDeviceLeave(0);Sleep(1);
    }
    return 0;
}
'''
    suffix=r'''
#undef esp
static void opaque_guest(void) {
    RecompContFrame opaque;recomp_cont_enter(&opaque,0xfe000004,NULL,0);
    sub_00011000();recomp_cont_leave(&opaque);
}
static void inspect_threads(FILE *file) {
    rewind(file);char magic[8];uint32_t count,previous_pcr=0;
    assert(fread(magic,8,1,file)==1 && !memcmp(magic,"XBTHREAD",8));
    assert(fread(&count,4,1,file)==1 && count==2);
    for(unsigned i=0;i<count;i++) {
        uint32_t bytes,identity[4];XboxGuestRegisters registers;
        assert(fread(&bytes,4,1,file)==1);long start=ftell(file);
        assert(fread(identity,sizeof identity,1,file)==1);
        assert(fread(&registers,sizeof registers,1,file)==1);
        assert(identity[0]==registers.fs_base && identity[0]!=previous_pcr);
        assert(identity[2]==1 && identity[3]==0x11000 && identity[1]);
        assert(registers.esp<identity[1] && registers.esp>identity[1]-0x80000);
        previous_pcr=identity[0];assert(fseek(file,start+bytes,SEEK_SET)==0);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);ram=fixture_memory_init();_putenv_s("RECOMP_GUEST_LOCK","1");
    assert(guest_cpu_enabled());
    /* The creator has the same active generated context needed by the
     * original spawn path. Its frame is not one of the guest workers saved. */
    RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);
    recomp_cont_site(&creator,0x100009000ULL);
    int opaque=!strcmp(argv[1],"opaque");
    HANDLE first=bridge_spawn_thread(opaque?opaque_guest:sub_00011000,0x20000,0,xbox_AllocThreadStack());
    HANDLE second=bridge_spawn_thread(sub_00011000,0x20004,0,xbox_AllocThreadStack());
    recomp_cont_leave(&creator);assert(first && second);
    HANDLE device=CreateThread(NULL,0,device_worker,NULL,0,NULL);assert(device);
    MCPXAPUState *apu=mcpx_apu_init_standalone(ram,physical_resolve);assert(apu);
    mcpx_apu_play_test_tone(apu);
    ULONGLONG limit=GetTickCount64()+5000;
    while(!MEM32(0x20000) || !MEM32(0x20004) || !MEM32(0x20008) || !apu->ep_frame_div) {
        assert(GetTickCount64()<limit);Sleep(1);
    }
    if(opaque) {
        assert(!xbox_CheckpointBegin(80));
        uint32_t first_before=MEM32(0x20000),second_before=MEM32(0x20004);
        Sleep(40);assert(MEM32(0x20000)>first_before && MEM32(0x20004)>second_before);
        assert(!g_checkpoint_requested && !g_checkpoint_busy);
    } else {
        for(unsigned repeat=0;repeat<3;repeat++) {
            assert(xbox_CheckpointBegin(3000));
            uint32_t values[]={MEM32(0x20000),MEM32(0x20004),MEM32(0x20008),apu->ep_frame_div};
            Sleep(60);
            assert(MEM32(0x20000)==values[0] && MEM32(0x20004)==values[1]);
            assert(MEM32(0x20008)==values[2] && apu->ep_frame_div==values[3]);
            FILE *threads=tmpfile(),*devices=tmpfile();assert(threads && devices);
            assert(xbox_CheckpointWriteThreads(threads) && xbox_CheckpointWriteDevices(devices));
            inspect_threads(threads);fclose(threads);fclose(devices);
            xbox_CheckpointEnd();Sleep(40);
            assert(MEM32(0x20000)>values[0] && MEM32(0x20004)>values[1]);
            assert(MEM32(0x20008)>values[2] && apu->ep_frame_div>values[3]);
        }
    }
    MEM32(0x20010)=1;
    assert(WaitForSingleObject(first,5000)==WAIT_OBJECT_0 && WaitForSingleObject(second,5000)==WAIT_OBJECT_0);
    CloseHandle(first);CloseHandle(second);
    InterlockedExchange(&stop_device,1);assert(WaitForSingleObject(device,5000)==WAIT_OBJECT_0);CloseHandle(device);
    mcpx_apu_play_test_tone(apu);mcpx_apu_shutdown(apu);
    assert(!xbox_CheckpointBegin(20));return 0;
}
'''
    (work/'memory.c').write_text(memory,encoding='utf-8')
    monkeypatch=pytest.MonkeyPatch()
    try:body=translate(monkeypatch,True,program)
    finally:monkeypatch.undo()
    (work/'pause.c').write_text(prefix+body+suffix,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(root/'src'),'/I'+str(root/'templates/runtime'),str(work/'memory.c'),
        str(work/'pause.c'),str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/recomp_continuation.c'),
        '/Fe:'+str(work/'pause.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'pause.exe'


@pytest.mark.parametrize('mode',['freeze','opaque'])
def test_real_threads_and_apu_freeze_or_cancel_cleanly(native_pause,mode):
    result=subprocess.run([str(native_pause),mode],capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
