"""Persist complete production JIT/DMA state and restore original device owners."""
from pathlib import Path
import shutil
import struct
import subprocess

import pytest


@pytest.fixture(scope='module')
def native_dsp_devices(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_dsp_devices')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include "kernel/kernel_bridge.c"
#include "apu/dsp/dsp_jit.c"
#include <assert.h>
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
static HANDLE request[3],done[3];static int cold;static volatile LONG stop;
static uint32_t observed[3][3];
static DWORD WINAPI device(void *argument) {
    unsigned id=(unsigned)(uintptr_t)argument;
    xbox_IrqlBindThreadPcr((cold?0x50000:0x30000)+id*0x1000);
    g_eax=(cold?0xdead0000:0x12340000)+id;g_esp=0x70000+id*0x1000;
    for(;;) {
        assert(WaitForSingleObject(request[id],INFINITE)==WAIT_OBJECT_0);if(stop) break;
        xbox_CheckpointDeviceEnter(id);
        observed[id][0]=g_eax;observed[id][1]=g_fs_base;observed[id][2]=g_esp;
        xbox_CheckpointDeviceLeave(id);assert(SetEvent(done[id]));
    }
    return 0;
}
static void scratch(void *opaque,uint8_t *data,uint32_t address,size_t bytes,bool direction) {
    (void)opaque;(void)address;if(!direction) memset(data,0,bytes);
}
static void fifo(void *opaque,uint8_t *data,unsigned index,size_t bytes,bool direction) {
    (void)opaque;(void)index;if(!direction) memset(data,0,bytes);
}
static void seed(DSPState *dsp) {
    Dsp56300State state;memset(&state,0,sizeof state);
    state.pc=0xabcdef;state.pc_advance=2;state.pc_on_rep=true;state.cycle_count=0x12345678;
    state.cycle_budget=-123;state.loop_rep=true;state.halt_requested=true;state.power_state=1;
    state.interrupts.pending_bits[0]=0x1000000000000001ULL;state.interrupts.pending_bits[1]=0x8000001000000000ULL;
    state.interrupts.vector_addr=0xfedcba;state.interrupts.saved_pc=0xabcdef;state.interrupts.state=1;
    state.interrupts.pipeline_stage=2;state.interrupts.ipl_to_raise=3;
    for(unsigned i=0;i<128;i++) state.interrupts.ipl[i]=(int8_t)(i%4);
    for(unsigned i=0;i<64;i++) state.registers[i]=0x123400+i;
    for(unsigned i=0;i<16;i++) {state.stack[0][i]=0xabcdef-i;state.stack[1][i]=0x987654-i;}
    dsp56300_set_state(jit_be(dsp)->jit,&state);
    dsp_write_memory(dsp,'X',0xc00,0x112233);dsp_write_memory(dsp,'Y',0x7ff,0x445566);
    dsp_write_memory(dsp,'P',0xfff,0x778899);
    dsp->interrupts=0x83;dsp->save_cycles=-88;dsp->dma.configuration=0x1234;dsp->dma.control=DMA_CONTROL_RUNNING;
    dsp->dma.start_block=0x40;dsp->dma.next_block=0x80;dsp->dma.dma_read_count=2;dsp->dma.eol=true;
}
static void verify(DSPState *dsp) {
    Dsp56300State state;dsp56300_get_state(jit_be(dsp)->jit,&state);
    assert(state.pc==0xabcdef && state.pc_advance==2 && state.pc_on_rep && state.cycle_count==0x12345678);
    assert(state.cycle_budget==-123 && state.loop_rep && state.halt_requested && state.power_state==1);
    assert(state.interrupts.vector_addr==0xfedcba && state.interrupts.saved_pc==0xabcdef);
    assert(state.interrupts.pending_bits[0]==0x1000000000000001ULL && state.interrupts.pending_bits[1]==0x8000001000000000ULL);
    assert(state.interrupts.state==1 && state.interrupts.pipeline_stage==2 && state.interrupts.ipl_to_raise==3);
    for(unsigned i=0;i<128;i++) assert(state.interrupts.ipl[i]==(int8_t)(i%4));
    for(unsigned i=0;i<64;i++) assert(state.registers[i]==0x123400+i);
    for(unsigned i=0;i<16;i++) assert(state.stack[0][i]==0xabcdef-i && state.stack[1][i]==0x987654-i);
    assert(dsp_read_memory(dsp,'X',0x1400)==0x112233 && dsp_read_memory(dsp,'X',0xc00)==0x112233);
    assert(dsp_read_memory(dsp,'Y',0x7ff)==0x445566 && dsp_read_memory(dsp,'P',0xfff)==0x778899);
    assert(dsp->interrupts==0x83 && dsp->save_cycles==-88 && dsp->dma.dma_read_count==2 && dsp->dma.eol);
    assert(dsp->dma.mem_opaque==jit_be(dsp)->jit && dsp->dma.scratch_rw==scratch && dsp->dma.fifo_rw==fifo);
    assert(dsp_read_memory(dsp,'X',0xffffd6)&DMA_CONTROL_STOPPED);assert(dsp->dma.dma_read_count==0);
}
static void verify_execution_cache(DSPState *dsp) {
    dsp_reset(dsp);dsp_write_memory(dsp,'P',0,0x0c0042);
    FILE *saved=tmpfile();assert(saved && dsp_save_checkpoint(dsp,saved));
    dsp_step(dsp);Dsp56300State state;dsp56300_get_state(jit_be(dsp)->jit,&state);assert(state.pc==0x42);
    dsp_reset(dsp);dsp_write_memory(dsp,'P',0,0x0c0055);dsp_step(dsp);
    dsp56300_get_state(jit_be(dsp)->jit,&state);assert(state.pc==0x55);
    rewind(saved);assert(dsp_load_checkpoint(dsp,saved));fclose(saved);dsp_step(dsp);
    dsp56300_get_state(jit_be(dsp)->jit,&state);assert(state.pc==0x42);
}
int main(int argc,char **argv) {
    assert(argc==6);cold=strcmp(argv[1],"save")!=0;
    FILE *xbe_file=fopen(argv[2],"rb");assert(xbe_file);fseek(xbe_file,0,SEEK_END);long bytes=ftell(xbe_file);
    rewind(xbe_file);void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,xbe_file)==1);fclose(xbe_file);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);_putenv_s("RECOMP_GUEST_LOCK","1");assert(guest_cpu_enabled());
    HANDLE threads[2];for(unsigned i=1;i<=2;i++) {
        request[i]=CreateEvent(NULL,FALSE,FALSE,NULL);done[i]=CreateEvent(NULL,FALSE,FALSE,NULL);
        threads[i-1]=CreateThread(NULL,0,device,(void *)(uintptr_t)i,0,NULL);assert(request[i] && done[i] && threads[i-1]);
        SetEvent(request[i]);assert(WaitForSingleObject(done[i],3000)==WAIT_OBJECT_0);
    }
    assert(xbox_CheckpointBootstrapBegin());DSPState *dsp=dsp_init(NULL,scratch,fifo,atoi(argv[5])!=0);assert(dsp);
    if(!cold) {
        seed(dsp);FILE *file=fopen(argv[3],"wb");assert(file && dsp_save_checkpoint(dsp,file));fclose(file);
        file=fopen(argv[4],"wb");assert(file && xbox_CheckpointWriteDevices(file));fclose(file);verify(dsp);
    } else if(!strcmp(argv[1],"reject")) {
        dsp_write_memory(dsp,'X',0xc00,0xfedcba);FILE *file=fopen(argv[3],"rb");assert(file && !dsp_load_checkpoint(dsp,file));fclose(file);
        assert(dsp_read_memory(dsp,'X',0xc00)==0xfedcba);
        file=fopen(argv[4],"rb");assert(file && !xbox_CheckpointLoadDevices(file));fclose(file);
        assert(!g_checkpoint_device_restore_pending[1] && !g_checkpoint_device_restore_pending[2]);
        return 0;
    } else {
        FILE *file=fopen(argv[3],"rb");assert(file && dsp_load_checkpoint(dsp,file));fclose(file);verify(dsp);
        file=fopen(argv[4],"rb");assert(file && xbox_CheckpointLoadDevices(file));fclose(file);
        assert(g_checkpoint_device_restore_pending[1] && g_checkpoint_device_restore_pending[2]);
        for(unsigned i=1;i<=2;i++) {SetEvent(request[i]);assert(WaitForSingleObject(done[i],10)==WAIT_TIMEOUT);}
    }
    verify_execution_cache(dsp);dsp_destroy(dsp);xbox_CheckpointEnd();
    if(cold) for(unsigned i=1;i<=2;i++) {
        assert(WaitForSingleObject(done[i],3000)==WAIT_OBJECT_0);
        assert(observed[i][0]==0x12340000+i && observed[i][1]==0x30000+i*0x1000 && observed[i][2]==0x70000+i*0x1000);
    }
    InterlockedExchange(&stop,1);for(unsigned i=1;i<=2;i++)SetEvent(request[i]);
    assert(WaitForMultipleObjects(2,threads,TRUE,3000)==WAIT_OBJECT_0);
    for(unsigned i=1;i<=2;i++) {CloseHandle(threads[i-1]);CloseHandle(request[i]);CloseHandle(done[i]);}
    xbox_MemoryLayoutShutdown();return 0;
}
'''
    (work/'dsp_devices.c').write_text(source,encoding='utf-8')
    libraries=list((root.parents[1]/'build/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root/'src'),
        '/I'+str(root.parents[1]/'reference/dsp56300_release/dsp56300-0.1.3/crates/emu-ffi/include'),
        str(work/'dsp_devices.c'),'/Fe:'+str(work/'dsp_devices.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'dsp_devices.exe',root/'tools/conformance/test.xbe'


@pytest.mark.parametrize('is_gp',[1,0])
def test_native_full_jit_dma_and_device_owner_contexts_survive_new_process(native_dsp_devices,tmp_path,is_gp):
    executable,xbe=native_dsp_devices;dsp=tmp_path/'dsp.state';devices=tmp_path/'devices.state'
    def run(mode,ds=dsp,dev=devices):
        result=subprocess.run([str(executable),mode,str(xbe),str(ds),str(dev),str(is_gp)],
                              capture_output=True,text=True,errors='replace',timeout=15)
        assert result.returncode==0,result.stdout+result.stderr
    run('save');run('load')
    bad_dsp=tmp_path/'bad_dsp.state';bad_dev=tmp_path/'bad_devices.state'
    bad_dsp.write_bytes(dsp.read_bytes()[:-1]);bad_dev.write_bytes(devices.read_bytes()[:-1]);run('reject',bad_dsp,bad_dev)
    bad_address=bytearray(devices.read_bytes());struct.pack_into('<I',bad_address,56,0xffffffff)
    bad_dev.write_bytes(bad_address);run('reject',bad_dsp,bad_dev)
