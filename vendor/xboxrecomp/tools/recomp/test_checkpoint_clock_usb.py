"""Original scheduler, frozen clock and USB control transfer recovery."""
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture(scope='module')
def native_clock_usb(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_clock_usb')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include "kernel/kernel_bridge.c"
#include "usb/usb_gamepad.c"
#include "usb/ohci.c"
#include "apu/apu_shim.h"
#include <assert.h>
static volatile LONG dpc_count,isr_count;
static int periodic_mode;
static HANDLE periodic_done;
static uint32_t observed[4][4];
static void original_dpc(void) {
    LONG slot=InterlockedIncrement(&dpc_count)-1;assert(slot<4);
    for(unsigned i=0;i<4;i++) observed[slot][i]=BRIDGE_MEM32(g_esp+4+i*4);
    g_esp+=20;
    if(periodic_mode) {
        assert(BRIDGE_MEM32(0x92004)==1 && BRIDGE_MEM8(0x92003)==1);
        uint64_t due=(uint64_t)BRIDGE_MEM32(0x92010)|((uint64_t)BRIDGE_MEM32(0x92014)<<32);
        assert(due==(uint64_t)g_timers[0].due_ms*10000);
        if(slot==2) {
            assert(xbox_kernel_cancel_timer(0x92000)==1);
            assert(SetEvent(periodic_done));
        }
    }
}
static void original_isr(void) {InterlockedIncrement(&isr_count);g_eax=1;g_esp+=12;}
recomp_func_t recomp_lookup(uint32_t va) {
    return va==0x11000?original_dpc:va==0x11004?original_isr:NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) {(void)va;return NULL;}
static void clock_values(uint64_t values[8]) {
    LARGE_INTEGER counter=xbox_KeQueryPerformanceCounter(),system;
    xbox_KeQuerySystemTime(&system);values[0]=counter.QuadPart;values[1]=system.QuadPart;
    values[2]=xbox_KeQueryInterruptTime();values[3]=xbox_ReadTimeStampCounter();
    values[4]=xbox_RuntimeTickCount64();values[5]=qemu_clock_get_ns(0);values[6]=qemu_clock_get_us(0);
    xbox_UpdateKernelTick(g_xbox_mem_offset);
    values[7]=BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE+KDATA_TICK_COUNT);
}
static void memory_objects(void) {
    g_esp=0x90000;
    BRIDGE_MEM32(g_esp)=0x92000;BRIDGE_MEM32(g_esp+4)=0;bridge_KeInitializeTimerEx();
    memset(XBOX_TO_NATIVE(0x91000),0,32);BRIDGE_MEM32(0x9100c)=0x11000;BRIDGE_MEM32(0x91010)=0x55;
    memset(XBOX_TO_NATIVE(0x93000),0,44);BRIDGE_MEM32(0x93000)=0x11004;BRIDGE_MEM32(0x93008)=7;BRIDGE_MEM32(0x9300c)=20;
}
static void transfer(void) {
    wr32(0x50000,TD_DP_IN<<19);wr32(0x50004,0x51008);wr32(0x5000c,0x5100f);
    assert(ohci_do_td(&s_hc[0],3,0x50000)==TD_CC_NOERROR);
    assert(!memcmp(guest_ptr(0x51008,8),s_device_desc+8,8) && g_ctrl_sent_[0]==16);
}
static void seed(void) {
    assert(xbox_PortRead(0x80C0,1,0x11000)==0x20);
    g_connected_isr[7]=0x93000;
    BRIDGE_MEM32(g_esp)=0x91000;BRIDGE_MEM32(g_esp+4)=0x11;BRIDGE_MEM32(g_esp+8)=0x22;
    bridge_KeInsertQueueDpc();assert(g_eax==1);
    kernel_set_timer(0x92000,-2000000,0,0x91000);
    assert(g_eax==0 && BRIDGE_MEM8(0x92003)==1 && BRIDGE_MEM32(0x92004)==0);
    assert(BRIDGE_MEM32(0x92020)==0x91000 && BRIDGE_MEM32(0x92024)==0);
    BRIDGE_MEM32(g_esp)=0x92000;bridge_KeCancelTimer();assert(g_eax==1 && !BRIDGE_MEM8(0x92003));
    bridge_KeCancelTimer();assert(g_eax==0);
    LARGE_INTEGER system_time;xbox_KeQuerySystemTime(&system_time);
    kernel_set_timer(0x92000,system_time.QuadPart+2000000,0,0x91000);
    assert(g_eax==0 && BRIDGE_MEM8(0x92001)==1);
    assert(g_timers[0].due_ms==(long long)xbox_RuntimeTickCount64()+200);
    kernel_set_timer(0x92000,-2000000,0,0x91000);
    assert(g_eax==1 && BRIDGE_MEM8(0x92001)==0);
    s_hc[0].reg[HcControl/4]=0x80;s_hc[0].reg[HcInterruptEnable/4]=0;
    s_hc[0].reg[HcDoneHead/4]=0x12340;s_hc[0].ack_seq=42;
    s_worker.plugged=1;s_worker.last_ack=41;s_worker.repeats=3;s_worker.settle=19;s_plugged_pads=1;
    UsbSetup address={0,5,3,0,0},configuration={0,9,1,0,0};uint8_t out[32];
    assert(usb_gamepad_control(0,&address,out,32)==0 && usb_gamepad_control(0,&configuration,out,32)==0);
    UsbSetup setup={0x80,6,0x100,0,18};memcpy(guest_ptr(0x51000,8),&setup,8);
    wr32(0x50000,TD_DP_SETUP<<19);wr32(0x50004,0x51000);wr32(0x5000c,0x51007);
    assert(ohci_do_td(&s_hc[0],3,0x50000)==TD_CC_NOERROR);
    wr32(0x50000,TD_DP_IN<<19);wr32(0x50004,0x51000);wr32(0x5000c,0x51007);
    assert(ohci_do_td(&s_hc[0],3,0x50000)==TD_CC_NOERROR);
    assert(g_ctrl_sent_[0]==8 && g_ctrl_len_[0]==18);
    s_script_len=1;s_script[0]=(PadStep){0,500,1,0,0,0,0};
    assert(usb_gamepad_report(0,out,32)==20 && (out[2]&1));
    assert(s_pad_live.path && s_pad_live.consumed==4 && s_pad_live.npending==4 && !memcmp(s_pad_live.pending,"p2-a",4));
}
static void verify(void) {
    assert(xbox_PortRead(0x80C0,1,0x11000)==0 && xbox_PortRead(0x80C0,1,0x11000)==0x20);
    assert(xbox_GetConnectedInterrupt(7)==0x93000 && g_dpc_head!=g_dpc_tail);
    assert(g_timers[0].timer_va==0x92000 && g_timers[0].due_ms==(long long)xbox_RuntimeTickCount64()+200);
    assert(s_hc[0].reg[HcDoneHead/4]==0x12340 && s_hc[0].ack_seq==42);
    assert(s_worker.last_ack==41 && s_worker.repeats==3 && s_worker.settle==19);
    assert(usb_gamepad_address(0)==3 && usb_gamepad_configured(0));
    assert(g_ctrl_len_[0]==18 && g_ctrl_sent_[0]==8 && g_setup_pending_[0]);
    assert(s_pad_live.path && s_pad_live.consumed==4 && s_pad_live.npending==4 && !memcmp(s_pad_live.pending,"p2-a",4));
    transfer();uint8_t out[32];assert(usb_gamepad_report(0,out,32)==20 && (out[2]&1));
    assert(kernel_raise_interrupt(7)==1 && isr_count>0);
}
static void consume_scheduler(void) {
    LARGE_INTEGER before,after;xbox_KeQuerySystemTime(&before);
    xbox_CheckpointEnd();ULONGLONG started=GetTickCount64();
    while(dpc_count<2 && GetTickCount64()-started<2000) Sleep(1);
    assert(dpc_count==2 && !BRIDGE_MEM8(0x92003) && BRIDGE_MEM32(0x92004)==1);
    assert(WaitForSingleObject(ke_shadow_lookup(0x92000),0)==WAIT_OBJECT_0);
    assert(observed[0][0]==0x91000 && observed[0][1]==0x55 && observed[0][2]==0x11 && observed[0][3]==0x22);
    xbox_KeQuerySystemTime(&after);
    uint64_t fired=observed[1][2]|((uint64_t)observed[1][3]<<32);
    assert(observed[1][0]==0x91000 && observed[1][1]==0x55);
    assert(fired>=(uint64_t)before.QuadPart && fired<=(uint64_t)after.QuadPart);
}
int main(int argc,char **argv) {
    assert(argc==4);_putenv_s("RECOMP_GUEST_LOCK","1");_putenv_s("RECOMP_USB","1");
    char live[1024];assert(snprintf(live,sizeof live,"%s.live",argv[3])<(int)sizeof live);
    _putenv_s("RECOMP_PAD_LIVE",live);
    if(!strcmp(argv[1],"save")) {FILE *input=fopen(live,"wb");assert(input && fwrite("p2-a",4,1,input)==1);fclose(input);}
    FILE *file=fopen(argv[2],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,file)==1);fclose(file);
    *(uint32_t *)((char *)xbe+0x12c)=0x11020;memset((char *)xbe+0x1020,0,24);
    *(uint32_t *)((char *)xbe+0x1030)=16;
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);memory_objects();
    xbox_CheckpointStartKernelDevices();xbox_OhciInit();(void)xbox_ReadTimeStampCounter();Sleep(20);
    assert(xbox_CheckpointBootstrapBegin());uint64_t expected[8],later[8];clock_values(expected);Sleep(80);clock_values(later);
    assert(!memcmp(expected,later,sizeof expected));
    if(!strcmp(argv[1],"periodic")) {
        periodic_mode=1;periodic_done=CreateEventW(NULL,TRUE,FALSE,NULL);assert(periodic_done);
        kernel_set_timer(0x92000,-10000,10,0x91000);
        assert(BRIDGE_MEM32(0x92024)==10 && g_eax==0);
        xbox_CheckpointEnd();assert(WaitForSingleObject(periodic_done,2000)==WAIT_OBJECT_0);
        assert(!BRIDGE_MEM8(0x92003) && !g_timers[0].timer_va);
        Sleep(40);assert(dpc_count==3);
        CloseHandle(periodic_done);return 0;
    }
    if(!strcmp(argv[1],"save")) {
        seed();clock_values(expected);file=fopen(argv[3],"wb");
        assert(file && xbox_SaveKernelScheduler(file) && xbox_OhciSaveCheckpoint(file) && fwrite(expected,sizeof expected,1,file)==1);fclose(file);
        verify();consume_scheduler();assert(xbox_CheckpointBootstrapBegin());
        assert(xbox_PortRead(0x80C0,1,0x11000)==0);
        dpc_count=0;memset(observed,0,sizeof observed);g_connected_isr[7]=0;s_hc[0].reg[HcDoneHead/4]=0;
        s_script_len=0;s_address[0]=7;
    }
    file=fopen(argv[3],"rb");assert(file && xbox_LoadKernelScheduler(file));
    if(!strcmp(argv[1],"reject")) {
        s_hc[0].reg[HcDoneHead/4]=0xdeadbeef;s_address[0]=7;
        assert(!xbox_OhciLoadCheckpoint(file) && s_hc[0].reg[HcDoneHead/4]==0xdeadbeef && s_address[0]==7);return 0;
    }
    assert(xbox_OhciLoadCheckpoint(file) && fread(expected,sizeof expected,1,file)==1);fclose(file);
    clock_values(later);assert(!memcmp(expected,later,sizeof expected));Sleep(80);clock_values(later);assert(!memcmp(expected,later,sizeof expected));
    verify();consume_scheduler();return 0;
}
'''
    (work/'clock_usb.c').write_text(source,encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/D_USE_MATH_DEFINES',
        '/I'+str(root/'src'),'/I'+str(root/'src/kernel'),str(work/'clock_usb.c'),
        '/Fe:'+str(work/'clock_usb.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'clock_usb.exe',root/'tools/conformance/test.xbe'


def test_original_clock_scheduler_and_usb_resume_same_and_new_process(native_clock_usb,tmp_path):
    executable,xbe=native_clock_usb;snapshot=tmp_path/'clock_usb.state'
    def run(mode,path=snapshot):
        result=subprocess.run([str(executable),mode,str(xbe),str(path)],
                              capture_output=True,text=True,errors='replace',timeout=30)
        assert result.returncode==0,result.stdout+result.stderr
    run('save');run('load');bad=tmp_path/'bad.state';bad.write_bytes(snapshot.read_bytes()[:-65]);run('reject',bad)


def test_periodic_timer_signals_before_callback_and_cancels_from_callback(native_clock_usb,tmp_path):
    executable,xbe=native_clock_usb
    result=subprocess.run([str(executable),'periodic',str(xbe),str(tmp_path/'periodic')],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
