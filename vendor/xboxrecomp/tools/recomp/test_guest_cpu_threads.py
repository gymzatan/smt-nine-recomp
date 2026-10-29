"""Production thread PCR binding and original guest CPU/thunk ownership."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


@pytest.fixture(scope="module")
def native_thread_probe():
    root = Path(__file__).resolve().parents[2]
    compiler = shutil.which("cl")
    assert compiler, "requires the existing MSVC developer environment"
    memory = r'''
#include "kernel/xbox_memory_layout.c"
#include <assert.h>
void fixture_memory_init(void) {
    void *ram=VirtualAlloc(0,0x02000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(ram);
    g_memory_offset=(ptrdiff_t)(uintptr_t)ram;
    g_xbox_mem_offset=g_memory_offset;
    g_xbox_map_size=0x02000000;
    g_heap_next=0x00100000;
    g_tls_template_va=0x6000; g_tls_total=16; g_tls_thread_size=64;
    g_fs_base=XBOX_TIB_MAIN; g_esp=0x80000;
}
void fixture_prepare_dpc_stack(void) {
    /* This DPC scenario bypasses main-PCR bootstrap. Use its original allocator. */
    uint32_t tib=xbox_AllocThreadTib();assert(tib);
    *(uint32_t *)((uintptr_t)g_memory_offset+XBOX_TIB_MAIN+0x5c)=
        *(uint32_t *)((uintptr_t)g_memory_offset+tib+0x5c);
}
void fixture_deferred_fp_capture(void) {
    XboxGuestRegisters saved,observed;xbox_CaptureGuestRegisters(&saved);
    g_fp_stack[3]=1.25;g_fp_alias_dirty=8;g_fp_tag_word=0xffff;
    xbox_CaptureGuestRegisters(&observed);
    assert(!g_fp_alias_dirty && observed.fp_tag_word==0xffff);
    assert(observed.mm[3]==0xa000000000000000ULL);
    assert(observed.fp_raw[3][8]==0xff && observed.fp_raw[3][9]==0x3f);
    g_fp_alias_dirty=1;assert(xbox_RestoreGuestRegisters(&saved));assert(!g_fp_alias_dirty);
}
void fixture_pcr_tls_layout(void) {
#define TEST_GUEST(va) ((void *)((uintptr_t)g_memory_offset+(va)))
    for(unsigned i=0;i<16;i++)*(uint8_t*)TEST_GUEST(0x6000+i)=(uint8_t)(0x80+i);
    uint32_t tib=xbox_AllocThreadTib();assert(tib);
    uint32_t tls_end=*(uint32_t*)TEST_GUEST(tib+4),thread=*(uint32_t*)TEST_GUEST(tib+0x28);
    assert(tls_end==tib+0x60+16);
    uint32_t tls_thread=*(uint32_t*)TEST_GUEST(tls_end-16);
    assert(tls_thread==tls_end && thread==tls_thread+64);
    uint32_t top=*(uint32_t*)TEST_GUEST(tib+0x5c);
    assert(top-0x3000>=thread+0x140 && top<=tib+xbox_HeapBlockSize(tib));
    memset(TEST_GUEST(top-0x3000),0x5a,0x3000);
    for(unsigned i=4;i<16;i++)assert(*(uint8_t*)TEST_GUEST(tls_end-16+i)==0x80+i);
    assert(*(uint32_t*)TEST_GUEST(tib+0x58)==0);
    assert(*(uint32_t*)TEST_GUEST(thread+0x134)==thread+0x134);
    xbox_HeapFree(tib);assert(!xbox_HeapBlockSize(tib));
}
#undef TEST_GUEST
'''
    program = r'''
#include <windows.h>
static volatile LONG fixture_tick_reads,fixture_summary_tick_reads;
static DWORD WINAPI fixture_summary_tick_count(void) {
    return (DWORD)InterlockedIncrement(&fixture_summary_tick_reads)*3001;
}
static ULONGLONG WINAPI fixture_tick_count(void) {
    InterlockedIncrement(&fixture_tick_reads);return GetTickCount64();
}
#define GetTickCount fixture_summary_tick_count
#define GetTickCount64 fixture_tick_count
#include "kernel/kernel_bridge.c"
#undef GetTickCount64
#undef GetTickCount
#include "kernel/kernel_rtl.c"
#include "apu/apu_state.h"
#include "apu/apu.h"
#include <assert.h>
void fixture_memory_init(void);
void fixture_pcr_tls_layout(void);
void fixture_prepare_dpc_stack(void);
extern MCPXAPUState *g_state;
static HANDLE entered,started;
static volatile LONG changed;
static volatile LONG apu_retired;
static HANDLE retire_started,retire_thread;
static int mode;
static void backedge_checkpoint(void) {
    assert(t_guest_held && t_dispatch_depth==0);
    assert(recomp_cont_current()->site==0x200010000ULL);
    FILE *file=tmpfile();assert(file && recomp_cont_save(file));fclose(file);
    InterlockedIncrement(&changed);
}
static VOID CALLBACK wait_apc(ULONG_PTR value) { (void)value;InterlockedIncrement(&changed); }
static void wait_objects_probe(void) {
    LARGE_INTEGER zero;zero.QuadPart=0;
    HANDLE automatic=CreateEventA(0,FALSE,TRUE,0),notification=CreateEventA(0,TRUE,TRUE,0);
    assert(automatic && notification);
    assert(xbox_NtWaitForSingleObject(automatic,FALSE,&zero)==STATUS_SUCCESS);
    assert(xbox_NtWaitForSingleObjectEx(automatic,0,FALSE,&zero)==STATUS_TIMEOUT);
    assert(xbox_NtWaitForSingleObject(notification,FALSE,&zero)==STATUS_SUCCESS);
    assert(xbox_NtWaitForSingleObject(notification,FALSE,&zero)==STATUS_SUCCESS);
    assert(ResetEvent(notification));
    assert(xbox_NtWaitForSingleObject(notification,FALSE,&zero)==STATUS_TIMEOUT);
    HANDLE pair[2]={automatic,CreateEventA(0,FALSE,FALSE,0)};assert(pair[1]);
    assert(SetEvent(pair[0]));
    assert(xbox_NtWaitForMultipleObjectsEx(2,pair,0,UserMode,FALSE,&zero)==STATUS_TIMEOUT);
    assert(SetEvent(pair[1]));
    assert(xbox_NtWaitForMultipleObjectsEx(2,pair,0,UserMode,FALSE,&zero)==STATUS_SUCCESS);
    assert(xbox_NtWaitForSingleObject(pair[0],FALSE,&zero)==STATUS_TIMEOUT);
    assert(xbox_NtWaitForSingleObject(pair[1],FALSE,&zero)==STATUS_TIMEOUT);
    assert(SetEvent(pair[1]));
    assert(xbox_NtWaitForMultipleObjectsEx(2,pair,1,UserMode,FALSE,&zero)==1);
    assert(xbox_NtWaitForSingleObject(pair[1],FALSE,&zero)==STATUS_TIMEOUT);
    assert(QueueUserAPC(wait_apc,GetCurrentThread(),0));
    assert(xbox_NtWaitForSingleObject(notification,TRUE,&zero)==STATUS_ALERTED && changed==1);
    CloseHandle(pair[1]);CloseHandle(automatic);CloseHandle(notification);
}
static DWORD WINAPI context_worker(void *param) {
    XboxGuestRegisters state;
    xbox_CaptureGuestRegisters(&state);
    assert(state.eax==0 && state.interrupt_state==1 && state.irql==0);
    state.eax=0x1234; state.fs_base=0;
    assert(xbox_RestoreGuestRegisters(&state));
    xbox_CaptureGuestRegisters(&state); assert(state.eax==0x1234);
    return 0;
}
void fixture_deferred_fp_capture(void);
static void context_probe(const char *path,int restore) {
    fixture_deferred_fp_capture();
    XboxGuestRegisters initial,expected,actual;
    xbox_CaptureGuestRegisters(&initial);
    expected=initial;
    expected.eax=0x89abcdef; expected.ebp=0x23456789; expected.seh_ebp=0x3456789a;
    expected.fs_base=0x6000; expected.fp_top=7; expected.fp_cmp=-1; expected.df=1;
    expected.fp_control_word=0x127f; expected.fp_cc=0x4500;
    expected.fp_status_word=0x82a1; expected.fp_tag_word=0x391b; expected.fp_opcode=0x6db;
    expected.fp_instruction_pointer=0x239c19; expected.fp_data_pointer=0x87654321;
    expected.fp_code_selector=8;expected.fp_data_selector=16;expected.mxcsr=0x5f80;
    for(unsigned i=0;i<8;i++)for(unsigned j=0;j<10;j++)expected.fp_raw[i][j]=(uint8_t)(i*13+j);
    expected.interrupt_state=3; expected.irql=16; expected.eflags=0x246ed7;
    for(unsigned i=0;i<8;i++) {
        expected.fp_stack[i]=0x7ff8000000000042ULL+i;
        expected.mm[i]=0x89abcdef01234567ULL+i;
        for(unsigned j=0;j<16;j++) expected.xmm[i][j]=(uint8_t)(i*31+j);
    }
    if(restore) {
        FILE *file=fopen(path,"rb");assert(file);
        assert(fread(&actual,sizeof actual,1,file)==1 && fclose(file)==0);
        assert(!memcmp(&expected,&actual,sizeof expected));
        assert(xbox_RestoreGuestRegisters(&actual));
    } else assert(xbox_RestoreGuestRegisters(&expected));
    assert(xbox_IrqlRaisedCount()==1 && xbox_IrqlBlocksInterrupts());
    assert(*(uint8_t *)XBOX_TO_NATIVE(expected.fs_base+0x24)==16);
    xbox_CaptureGuestRegisters(&actual); assert(!memcmp(&expected,&actual,sizeof expected));
    if(path && !restore) {
        FILE *file=fopen(path,"wb");assert(file);
        assert(fwrite(&actual,sizeof actual,1,file)==1 && fclose(file)==0);
    }
    /* Reapplying the same state does not double the original gate counts. */
    assert(xbox_RestoreGuestRegisters(&expected)); assert(xbox_IrqlRaisedCount()==1);
    HANDLE worker=CreateThread(0,0,context_worker,0,0,0);assert(worker);
    assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);CloseHandle(worker);
    xbox_CaptureGuestRegisters(&actual);assert(!memcmp(&expected,&actual,sizeof expected));
    XboxGuestRegisters invalid=expected;invalid.fp_top=8;
    assert(!xbox_RestoreGuestRegisters(&invalid));
    xbox_CaptureGuestRegisters(&actual);assert(!memcmp(&expected,&actual,sizeof expected));
    assert(xbox_RestoreGuestRegisters(&initial));
    assert(!xbox_IrqlBlocksInterrupts() && xbox_IrqlRaisedCount()==0);
    xbox_CaptureGuestRegisters(&actual);assert(!memcmp(&initial,&actual,sizeof initial));
}
static DWORD WINAPI retire_worker(void *p) {
    SetEvent(retire_started);
    guest_cpu_join();
    InterlockedExchange(&apu_retired,1);
    guest_cpu_part(); return 0;
}
static const char *fixture_identity_action;
static void fixture_apc_identity(void) {
    assert(t_guest_thread && t_guest_held);
    uint32_t fs=g_fs_base,thread=ke_current_thread();
    assert(fs && thread && BRIDGE_MEM8(thread+0x4b));
    assert(!BRIDGE_MEM8(thread+0x49));
    assert(BRIDGE_MEM32(thread+0x34)==thread+0x34);
    assert(BRIDGE_MEM32(thread+0x3c)==thread+0x3c);
    assert(xbox_CurrentIrql()==PASSIVE_LEVEL && xbox_CpuInterruptFlag());
    uint8_t enabled=BRIDGE_MEM8(thread+0x4b);
    int was_thread=t_guest_thread;
    if(!strcmp(fixture_identity_action,"apc_identity")) {
        uint8_t *memory=malloc(0x02000000);assert(memory);
        unsigned cases=0;
        for(unsigned owned=0;owned<2;owned++) {
            for(unsigned test=0;test<9;test++) {
                for(unsigned user=0;user<2;user++) {
                    g_fs_base=fs;BRIDGE_MEM32(fs+0x28)=thread;
                    BRIDGE_MEM8(thread+0x4b)=test==1?0:test==2?2:test==3?255:enabled;
                    if(test==4)g_fs_base=0;
                    if(test==5)BRIDGE_MEM32(fs+0x28)=0;
                    if(test==6)assert(xbox_KfRaiseIrql(APC_LEVEL)==PASSIVE_LEVEL);
                    if(test==7)xbox_CpuCli();
                    if(!owned)assert(xbox_kernel_wait_release_cpu());
                    if(test==8)t_guest_thread=0;
                    int expected_enabled=test!=1 && test!=4 && test!=5 && test!=8;
                    assert(xbox_ThreadApcsEnabled()==expected_enabled);
                    XboxGuestRegisters before,after;
                    xbox_CaptureGuestRegisters(&before);
                    memcpy(memory,(void *)(uintptr_t)xbox_GetMemoryOffset(),0x02000000);
                    assert(xbox_DeliverThreadApcs((BOOLEAN)user)==0);
                    xbox_CaptureGuestRegisters(&after);
                    assert(!memcmp(&before,&after,sizeof before));
                    assert(!memcmp(memory,(void *)(uintptr_t)xbox_GetMemoryOffset(),0x02000000));
                    assert(t_guest_held==(int)owned);
                    cases++;
                    t_guest_thread=was_thread;
                    if(!owned)guest_cpu_join();
                    g_fs_base=fs;BRIDGE_MEM32(fs+0x28)=thread;
                    BRIDGE_MEM8(thread+0x4b)=enabled;
                    if(test==6)xbox_KfLowerIrql(PASSIVE_LEVEL);
                    if(test==7){xbox_CpuSti();xbox_CpuCompleteStiShadow();}
                }
            }
        }
        free(memory);
        assert(cases==36);
        printf("[APC_IDENTITY] cases=%u guest_bytes=%zu ram_bytes=%u\n",cases,sizeof(XboxGuestRegisters),0x02000000u);
    } else {
        static const char *names[]={"held_empty","held_user_empty","unheld_empty","yield_idle"};
        LARGE_INTEGER frequency;assert(QueryPerformanceFrequency(&frequency));
        const unsigned iterations=2000000;
        for(unsigned mode_index=0;mode_index<4;mode_index++) {
            if(mode_index==2)assert(xbox_kernel_wait_release_cpu());
            LARGE_INTEGER started,ended;
            volatile unsigned result=0;
            assert(QueryPerformanceCounter(&started));
            for(unsigned i=0;i<iterations;i++) {
                if(mode_index==3)guest_cpu_yield();
                else result+=(unsigned)xbox_DeliverThreadApcs(mode_index==1);
            }
            assert(QueryPerformanceCounter(&ended));
            assert(result==0);
            if(mode_index==2)guest_cpu_join();
            printf("[APC_COST] mode=%s iterations=%u ticks=%lld frequency=%lld result=%u\n",names[mode_index],iterations,ended.QuadPart-started.QuadPart,frequency.QuadPart,result);
        }
    }
    assert(g_fs_base==fs && ke_current_thread()==thread);
    assert(t_guest_thread && t_guest_held);
}

static void probe_guest(void) {
    if(mode==6) {
        fixture_apc_identity();
    } else if(mode==1) {
        assert(*(uint8_t *)XBOX_TO_NATIVE(g_fs_base+0x24)==PASSIVE_LEVEL);
        SetEvent(entered);
    } else if(mode==3) {
        assert(xbox_CurrentIrql()==DISPATCH_LEVEL);
        InterlockedIncrement(&changed); SetEvent(entered);
        g_esp+=20;
    } else if(mode==5) {
        assert(t_guest_held && xbox_CurrentIrql()==22);
        assert(!apu_retired && changed==0);
        retire_thread=CreateThread(0,0,retire_worker,0,0,0); assert(retire_thread);
        assert(WaitForSingleObject(retire_started,5000)==WAIT_OBJECT_0);
        Sleep(20); assert(!apu_retired);
        mcpx_apu_mmio_write(g_state,NV_PAPU_ISTS,0x11,4);
        mcpx_apu_mmio_write(g_state,NV_PAPU_FECTL,0x1F8F,4);
        mcpx_apu_mmio_write(g_state,NV_PAPU_FECTL,0x1F0F,4);
        InterlockedIncrement(&changed); SetEvent(entered);
        g_eax=1; g_esp+=12;
    } else {
        assert(t_dispatch_depth==0 && t_guest_held);
    }
}
recomp_func_t recomp_lookup(uint32_t va) { return va==0x20000 ? probe_guest : 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { return 0; }
static DWORD WINAPI cpu_worker(void *p) {
    SetEvent(started);
    guest_cpu_join();
    if(mode==4) Sleep(40);
    InterlockedIncrement(&changed); SetEvent(entered);
    guest_cpu_part(); return 0;
}
static DWORD WINAPI dpc_worker(void *p) {
    g_esp=0x90000;
    SetEvent(started);
    assert(kernel_run_dpc(0x2000,0,0));
    return 0;
}
static DWORD WINAPI apu_worker(void *p) {
    g_esp=0x90000;
    SetEvent(started);
    kernel_apu_tick();
    return 0;
}
static void ownership_probe(void) {
    /* Host observation inside the production thunk, not a guest wait. */
    guest_cpu_yield();
    assert(WaitForSingleObject(entered,200)==WAIT_TIMEOUT);
    assert(changed==0);
}
static void wait_at_guest_backedge(void) {
    ULONGLONG deadline=GetTickCount64()+5000;
    while(!changed) {
        guest_cpu_yield();
        assert(GetTickCount64()<deadline);
    }
    assert(t_guest_held);
}
static volatile LONG stress_ready,stress_done,stress_inside,stress_peak;
static DWORD WINAPI handoff_stress_worker(void *param) {
    InterlockedIncrement(&stress_ready);
    guest_cpu_join();
    for(unsigned i=0;i<500;i++) {
        assert(t_guest_held && InterlockedIncrement(&stress_inside)==1);
        InterlockedDecrement(&stress_inside);
        LONG demand=ReadAcquire(&g_guest_cpu_handoff_waiters),peak=ReadAcquire(&stress_peak);
        while(demand>peak) {
            LONG seen=InterlockedCompareExchange(&stress_peak,demand,peak);
            if(seen==peak)break;peak=seen;
        }
        InterlockedIncrement(&stress_done);
        t_guest_yield_tick=0;guest_cpu_yield();
    }
    guest_cpu_part();return 0;
}
static void raise_ownership_probe(void) {
    bridge_KfRaiseIrql();
    assert(xbox_CurrentIrql()==DISPATCH_LEVEL);
    ownership_probe();
}
static void wait_ownership_probe(void) {
    assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    g_esp+=4;
}
static void call_slot(ULONG ordinal,bridge_func_t bridge,uint32_t ecx) {
    g_slot_ordinals[0]=ordinal; g_slot_bridges[0]=bridge;
    g_kernel_dispatch_slot=0; g_ecx=ecx;
    g_esp-=4; BRIDGE_MEM32(g_esp)=0x12345;
    kernel_thunk_dispatch();
}
static void critical_call(ULONG ordinal) {
    int slot=ordinal==277?1:ordinal==294?2:3;
    g_esp-=8;BRIDGE_MEM32(g_esp)=0x12345;BRIDGE_MEM32(g_esp+4)=0x4000;
    g_kernel_dispatch_slot=slot;kernel_thunk_dispatch();
}
static DWORD WINAPI critical_worker(void *param) {
    g_esp=0x90000;SetEvent(started);guest_cpu_join();
    uint64_t joins=t_guest_cpu_timing->join_count;
    InterlockedExchange(&changed,1);critical_call(277);
    assert(t_guest_cpu_timing->join_count==joins+1);
    assert(t_guest_held);InterlockedExchange(&changed,2);
    critical_call(294);guest_cpu_part();return 0;
}
static LONG native_wait_report;
static HANDLE native_cs_entered;
static DWORD WINAPI native_critical_holder(void *param) {
    assert(!t_guest_held);xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(0x4000));
    SetEvent(native_cs_entered);ULONGLONG deadline=GetTickCount64()+5000;
    while(ReadAcquire(&g_cs_contention_reports)==native_wait_report){assert(GetTickCount64()<deadline);Sleep(0);}
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(0x4000));assert(!t_guest_held);return 0;
}

/* Count only calls emitted by the included production bridge. The existing
 * runtime libraries keep their real host clock and file implementations. */
static void diagnostic_call(ULONG ordinal,bridge_func_t bridge,unsigned count,const uint32_t *args) {
    uint32_t top=g_esp;
    g_slot_ordinals[0]=ordinal;g_slot_bridges[0]=bridge;g_slot_arg_bytes[0]=count*4;
    g_kernel_dispatch_slot=0;g_esp-=4+count*4;BRIDGE_MEM32(g_esp)=0x13579;
    for(unsigned i=0;i<count;i++)BRIDGE_MEM32(g_esp+4+i*4)=args[i];
    kernel_thunk_dispatch();
    assert(g_esp==top && g_xbox_kernel_caller==0x13579 && t_dispatch_depth==0);
}
static void diagnostic_summary_probe(int enabled) {
    _putenv_s("RECOMP_GUEST_LOCK","");_putenv_s("RECOMP_GPU_TIMING","");
    _putenv_s("RECOMP_KERNEL_LOG_BUDGET",enabled?"1":"0");
    uint32_t args[]={0x7000,0};fixture_summary_tick_reads=0;fixture_tick_reads=0;
    for(unsigned i=0;i<205;i++) {
        BRIDGE_MEM32(0x7000)=0xffffffff;
        diagnostic_call(289,bridge_RtlInitAnsiString,2,args);
        assert(g_eax==0 && BRIDGE_MEM32(0x7000)==0 && BRIDGE_MEM32(0x7004)==0);
    }
    fprintf(stderr,"[DIAGNOSTIC_SUMMARY] enabled=%d imports=%lld ordinal=%llu summary_clock=%ld clock64=%ld\n",
            enabled,g_kernel_call_count,g_ordinal_calls[289],fixture_summary_tick_reads,fixture_tick_reads);
    assert(g_kernel_call_count==205);
    assert(enabled?(fixture_summary_tick_reads==205 && g_ordinal_calls[289]==205):
                   (!fixture_summary_tick_reads && !g_ordinal_calls[289]));
    assert(!fixture_tick_reads);
    /* The same original watched write remains live with summary disabled. */
    g_kernel_watch_va=0x7000;BRIDGE_MEM32(0x7000)=0xdeadbeef;
    diagnostic_call(289,bridge_RtlInitAnsiString,2,args);
    assert(g_eax==0 && !BRIDGE_MEM32(0x7000) && g_kernel_call_count==206);
    fprintf(stderr,"[DIAGNOSTIC_WATCH] original bridge/status/stack/total/watch retained\n");
}
static int fixture_file_logging;
static void diagnostic_load_probe(int enabled,const char *xbe_path,const char *directory) {
    _putenv_s("RECOMP_GUEST_LOCK","");_putenv_s("RECOMP_GPU_TIMING","");
    _putenv_s("RECOMP_KERNEL_LOG_BUDGET",fixture_file_logging==1?"16":"0");
    _putenv_s("RECOMP_LOAD_TIMING",enabled?"1":"");
    if(fixture_file_logging) _putenv_s("XBOX_LOG_LEVEL",fixture_file_logging==1?"4":"2");
    int initial_logging=KERNEL_LOG_ON();
    FILE *file=fopen(xbe_path,"rb");assert(file);fseek(file,0,SEEK_END);
    long size=ftell(file);rewind(file);void *xbe=malloc(size);assert(xbe);
    assert(fread(xbe,size,1,file)==1 && fclose(file)==0);
    assert(xbox_MemoryLayoutInit(xbe,size));free(xbe);
    if(fixture_file_logging) xbox_kernel_init();
    xbox_path_init(directory,directory);assert(xbox_kernel_init_devices()==0);
    XBOX_ANSI_STRING drive={6,6,"\\??\\D:"},device={14,14,"\\Device\\CdRom0"};
    assert(xbox_IoCreateSymbolicLink(&drive,&device)==0);
    char filename[MAX_PATH];assert(snprintf(filename,sizeof filename,"%s/input.bin",directory)>0);
    file=fopen(filename,"wb");assert(file);assert(fwrite("NINE",4,1,file)==1 && !fclose(file));
    const char *name="D:\\input.bin";size_t length=strlen(name);
    memcpy(XBOX_TO_NATIVE(0x25000),name,length);
    BRIDGE_MEM16(0x25100)=(uint16_t)length;BRIDGE_MEM16(0x25102)=(uint16_t)length;
    BRIDGE_MEM32(0x25104)=0x25000;BRIDGE_MEM32(0x25200)=0;
    BRIDGE_MEM32(0x25204)=0x25100;BRIDGE_MEM32(0x25208)=0x40;
    uint32_t open_args[]={0x25300,GENERIC_READ,0x25200,0x25400,0,0,3,1,0x20};
    g_esp=0x80000;fixture_tick_reads=0;
    diagnostic_call(190,bridge_NtCreateFile,9,open_args);
    LONG open_clocks=fixture_tick_reads;
    assert(g_eax==0 && BRIDGE_MEM32(0x25400)==0 && BRIDGE_MEM32(0x25300));
    uint32_t token=BRIDGE_MEM32(0x25300);
    uint32_t read_args[]={token,0,0,0,0x25400,0x25500,4,0};
    fixture_tick_reads=0;diagnostic_call(219,bridge_NtReadFile,8,read_args);
    LONG read_clocks=fixture_tick_reads;
    assert(g_eax==0 && BRIDGE_MEM32(0x25400)==0 && BRIDGE_MEM32(0x25404)==4);
    assert(!memcmp(XBOX_TO_NATIVE(0x25500),"NINE",4));
    uint32_t close_args[]={token};diagnostic_call(187,bridge_NtClose,1,close_args);assert(g_eax==0);
    fprintf(stderr,"[DIAGNOSTIC_LOAD] enabled=%d open_clock=%ld read_clock=%ld data=NINE status=0 bytes=4\n",
            enabled,open_clocks,read_clocks);
    assert(open_clocks==(enabled?2:0) && read_clocks==(enabled?2:0));
    assert(DeleteFileA(filename));
    if(fixture_file_logging) {
        fprintf(stderr,"[DIAGNOSTIC_FILE] enabled=%d initial_logging=%d\n",fixture_file_logging==1,initial_logging);
        xbox_kernel_shutdown();
    }
}

int main(int argc,char **argv) {
    if(argc>=2 && !strncmp(argv[1],"diagnostic_",11)) {
        if(!strncmp(argv[1],"diagnostic_summary_",19)) {
            fixture_memory_init();diagnostic_summary_probe(!strcmp(argv[1],"diagnostic_summary_on"));
        } else {
            assert(argc==4);
            if(!strncmp(argv[1],"diagnostic_file_",16)) fixture_file_logging=!strcmp(argv[1],"diagnostic_file_on")?1:2;
            diagnostic_load_probe(!strcmp(argv[1],"diagnostic_load_on"),argv[2],argv[3]);
        }
        return 0;
    }
    fixture_memory_init(); _putenv_s("RECOMP_GUEST_LOCK","1"); _putenv_s("RECOMP_GPU_TIMING","1");
    entered=CreateEventA(0,1,0,0); started=CreateEventA(0,1,0,0);
    guest_cpu_join();
    if(!strcmp(argv[1],"apc_identity") || !strcmp(argv[1],"apc_cost")) {
        mode=6;fixture_identity_action=argv[1];
        HANDLE worker=bridge_spawn_thread(probe_guest,0,0,xbox_AllocThreadStack());assert(worker);
        guest_cpu_part();
        assert(WaitForSingleObject(worker,INFINITE)==WAIT_OBJECT_0);CloseHandle(worker);
    } else if(strcmp(argv[1],"yield_idle")==0) {
        assert(!ReadAcquire(&g_guest_cpu_waiters) && !ReadAcquire(&g_dpc_cpu_waiters));
        uint64_t joins=t_guest_cpu_timing->join_count,yields=t_guest_cpu_timing->yield_count;
        t_guest_yield_tick=0;InterlockedExchange(&fixture_tick_reads,0);
        for(unsigned i=0;i<128;i++)guest_cpu_yield();
        assert(!ReadAcquire(&fixture_tick_reads) && t_guest_yield_tick==0);
        assert(t_guest_held && t_guest_cpu_timing->join_count==joins && t_guest_cpu_timing->yield_count==yields);
        guest_cpu_part();
    } else if(strcmp(argv[1],"pcr_layout")==0) {fixture_pcr_tls_layout();}
    else if(strcmp(argv[1],"critical_leave")==0) {
        ULONG ordinals[]={277,294,291};bridge_func_t bridges[]={bridge_RtlEnterCriticalSection,bridge_RtlLeaveCriticalSection,bridge_RtlInitializeCriticalSection};
        for(unsigned i=0;i<3;i++){g_slot_ordinals[i+1]=ordinals[i];g_slot_bridges[i+1]=bridges[i];g_slot_arg_bytes[i+1]=4;}
        critical_call(291);uint64_t fast_before=t_guest_cpu_timing->join_count;
        critical_call(277);critical_call(277);uint64_t fast_joins=t_guest_cpu_timing->join_count-fast_before;
        HANDLE worker=CreateThread(0,0,critical_worker,0,0,0);assert(worker);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);wait_at_guest_backedge();
        assert(changed==1 && t_guest_held);
        uint64_t before=t_guest_cpu_timing->join_count;
        critical_call(294);assert(t_guest_held && changed==1);critical_call(294);
        assert(t_guest_held);uint64_t leave_joins=t_guest_cpu_timing->join_count-before;
        guest_cpu_part();assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && changed==2);CloseHandle(worker);
        native_wait_report=ReadAcquire(&g_cs_contention_reports);native_cs_entered=CreateEventA(0,TRUE,FALSE,0);assert(native_cs_entered);
        worker=CreateThread(0,0,native_critical_holder,0,0,0);assert(worker);
        assert(WaitForSingleObject(native_cs_entered,5000)==WAIT_OBJECT_0);
        xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(0x4000));assert(!t_guest_held && !t_guest_thread);
        assert(ReadAcquire(&g_cs_contention_reports)>native_wait_report);
        xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(0x4000));
        assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);CloseHandle(worker);CloseHandle(native_cs_entered);guest_cpu_join();
        LARGE_INTEGER frequency,begin,end;assert(QueryPerformanceFrequency(&frequency));
        before=t_guest_cpu_timing->join_count;QueryPerformanceCounter(&begin);
        for(unsigned i=0;i<10000;i++){critical_call(277);critical_call(294);}
        QueryPerformanceCounter(&end);uint64_t pair_joins=t_guest_cpu_timing->join_count-before;
        fprintf(stderr,"actual recursive enters:2 joins=%llu; releases:2 joins=%llu; critical pairs:10000 joins=%llu ns/pair=%g\n",(unsigned long long)fast_joins,(unsigned long long)leave_joins,(unsigned long long)pair_joins,(double)(end.QuadPart-begin.QuadPart)*1e9/frequency.QuadPart/10000);
        xbox_KfRaiseIrql(DISPATCH_LEVEL);before=t_guest_cpu_timing->join_count;critical_call(277);critical_call(294);
        assert(t_guest_held && xbox_CurrentIrql()==DISPATCH_LEVEL && t_guest_cpu_timing->join_count==before);xbox_KfLowerIrql(PASSIVE_LEVEL);
        assert(fast_joins==0 && leave_joins==0 && pair_joins==0);guest_cpu_part();
    } else if(strcmp(argv[1],"event_idle")==0) {
        assert(ResetEvent(g_guest_cpu_acquired));
        LARGE_INTEGER frequency,begin,end;assert(QueryPerformanceFrequency(&frequency));
        QueryPerformanceCounter(&begin);
        for(unsigned i=0;i<100000;i++){guest_cpu_part();guest_cpu_join();}
        QueryPerformanceCounter(&end);
        DWORD state=WaitForSingleObject(g_guest_cpu_acquired,0);
        fprintf(stderr,"idle CPU joins:100000 ns/join=%g event=%lu\n",(double)(end.QuadPart-begin.QuadPart)*1e9/frequency.QuadPart/100000,state);
        assert(state==WAIT_TIMEOUT);guest_cpu_part();
    } else if(strcmp(argv[1],"handoff_stress")==0) {
        HANDLE workers[8];for(unsigned i=0;i<8;i++){workers[i]=CreateThread(0,0,handoff_stress_worker,0,0,0);assert(workers[i]);}
        ULONGLONG deadline=GetTickCount64()+5000;
        while(InterlockedCompareExchange(&g_guest_cpu_waiters,0,0)<8){assert(GetTickCount64()<deadline);Sleep(0);}
        assert(stress_ready==8);t_guest_yield_tick=0;guest_cpu_yield();guest_cpu_part();
        assert(WaitForMultipleObjects(8,workers,TRUE,10000)==WAIT_OBJECT_0);
        assert(stress_done==4000 && !stress_inside && !ReadAcquire(&g_guest_cpu_handoff_waiters));
        fprintf(stderr,"actual CPU handoffs:4000 peak outstanding waiters=%ld\n",stress_peak);
        for(unsigned i=0;i<8;i++)CloseHandle(workers[i]);
    } else if(strcmp(argv[1],"pcr")==0) {
        mode=1;
        assert(xbox_KfRaiseIrql(DISPATCH_LEVEL)==PASSIVE_LEVEL);
        HANDLE t=bridge_spawn_thread(probe_guest,0,0,xbox_AllocThreadStack());
        assert(t);
        /* Worker copies main PCR before waiting to enter the existing CPU. */
        Sleep(100);
        xbox_KfLowerIrql(PASSIVE_LEVEL); guest_cpu_part();
        assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
        CloseHandle(t);
    } else if(strcmp(argv[1],"dpc")==0) {
        mode=3;
        BRIDGE_MEM32(0x2000+12)=0x20000;
        call_slot(160,bridge_KfRaiseIrql,DISPATCH_LEVEL);
        HANDLE t=CreateThread(0,0,dpc_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        ownership_probe();
        call_slot(161,bridge_KfLowerIrql,PASSIVE_LEVEL);
        wait_at_guest_backedge();
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0 && changed==1);
        CloseHandle(t);
    } else if(strcmp(argv[1],"dpc_priority")==0) {
        fixture_prepare_dpc_stack();
        mode=3;
        BRIDGE_MEM32(0x2000+12)=0x20000;
        call_slot(160,bridge_KfRaiseIrql,DISPATCH_LEVEL);
        HANDLE t=CreateThread(0,0,dpc_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        ownership_probe();
        call_slot(161,bridge_KfLowerIrql,PASSIVE_LEVEL);
        xbox_CpuCli(); ownership_probe();
        xbox_CpuSti(); ownership_probe();
        xbox_CpuCompleteStiShadow();
        /* Keep an ordinary quantum out of this window: an already waiting
         * DISPATCH_LEVEL routine must be admitted at an eligible backedge. */
        t_guest_yield_tick=GetTickCount64();
        guest_cpu_yield();
        assert(WaitForSingleObject(entered,100)==WAIT_OBJECT_0 && changed==1);
        assert(t_guest_held && g_dpc_cpu_waiters==0);
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
        CloseHandle(t);
    } else if(strcmp(argv[1],"apu_idle")==0) {
        MCPXAPUState *apu=calloc(1,sizeof(*apu));assert(apu);
        qemu_mutex_init(&apu->lock);qemu_cond_init(&apu->cond);g_state=apu;
        mcpx_ac97_init();
        assert(!mcpx_ac97_irq_pending());
        HANDLE t=CreateThread(0,0,apu_worker,0,0,0);assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        /* No original device level needs guest execution. */
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
        assert(t_guest_held && !ReadAcquire(&g_dpc_cpu_waiters));
        CloseHandle(t);g_state=0;free(apu);
    } else if(strcmp(argv[1],"apu_irq")==0) {
        mode=5;
        MCPXAPUState *apu=calloc(1,sizeof(*apu)); assert(apu);
        qemu_mutex_init(&apu->lock); qemu_cond_init(&apu->cond); g_state=apu;
        g_connected_isr[5]=0x2000; BRIDGE_MEM32(0x2000)=0x20000;
        BRIDGE_MEM32(0x2008)=5;BRIDGE_MEM32(0x200c)=22;BRIDGE_MEM8(0x2010)=1;
        mcpx_apu_mmio_write(apu,NV_PAPU_IEN,0x11,4);
        mcpx_apu_mmio_write(apu,NV_PAPU_FECTL,0x1FEF,4);
        apu->regs[NV_PAPU_ISTS]=NV_PAPU_ISTS_FETINTSTS;
        mcpx_apu_mmio_write(apu,NV_PAPU_ISTS,0,4);
        retire_started=CreateEventA(0,1,0,0); assert(retire_started);
        HANDLE t=CreateThread(0,0,apu_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        assert(WaitForSingleObject(entered,30)==WAIT_TIMEOUT && changed==0);
        wait_at_guest_backedge();
        assert(changed==1 && g_dpc_cpu_waiters==0);
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
        assert(WaitForSingleObject(retire_thread,5000)==WAIT_OBJECT_0 && apu_retired);
        assert(!(mcpx_apu_mmio_read(apu,NV_PAPU_ISTS,4)&0x11));
        CloseHandle(t); CloseHandle(retire_thread); CloseHandle(retire_started);
        g_state=0; free(apu);
    } else if(strcmp(argv[1],"quantum")==0) {
        assert(SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST));
        HANDLE t=CreateThread(0,0,cpu_worker,0,0,0); assert(t);
        assert(SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL));
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        call_slot(160,bridge_KfRaiseIrql,DISPATCH_LEVEL);
        ownership_probe();
        call_slot(161,bridge_KfLowerIrql,PASSIVE_LEVEL);
        xbox_CpuCli(); ownership_probe();
        xbox_CpuSti(); ownership_probe();
        xbox_CpuCompleteStiShadow();
        wait_at_guest_backedge();
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0 && changed==1);
        CloseHandle(t);
    } else if(strcmp(argv[1],"resume")==0 || strcmp(argv[1],"kernel_resume")==0) {
        mode=4;
        HANDLE t=CreateThread(0,0,cpu_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        if(strcmp(argv[1],"resume")==0)
            wait_at_guest_backedge();
        else
            call_slot(175,wait_ownership_probe,0);
        uint64_t before=t_guest_cpu_timing->yield_count;
        if(strcmp(argv[1],"resume")==0) assert(before>0);
        /* Time spent waiting for another owner is not guest execution time. */
        guest_cpu_yield();
        fprintf(stderr,"resumed backedge: yields %llu -> %llu\n",(unsigned long long)before,(unsigned long long)t_guest_cpu_timing->yield_count);
        assert(t_guest_cpu_timing->yield_count==before);
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0 && changed==1);
        CloseHandle(t);
    } else if(strcmp(argv[1],"raise")==0) {
        HANDLE t=CreateThread(0,0,cpu_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        call_slot(160,raise_ownership_probe,DISPATCH_LEVEL);
        assert(t_guest_held);
        call_slot(161,bridge_KfLowerIrql,PASSIVE_LEVEL);
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0 && changed==1);
        CloseHandle(t);
    } else if(strcmp(argv[1],"irql")==0) {
        call_slot(160,bridge_KfRaiseIrql,DISPATCH_LEVEL);
        HANDLE t=CreateThread(0,0,cpu_worker,0,0,0); assert(t);
        assert(WaitForSingleObject(started,5000)==WAIT_OBJECT_0);
        call_slot(103,ownership_probe,0);
        assert(t_guest_held);
        call_slot(161,bridge_KfLowerIrql,PASSIVE_LEVEL);
        guest_cpu_part();
        assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0 && changed==1);
        CloseHandle(t);
    } else if(strcmp(argv[1],"checkpoint_backedge")==0) {
        RecompContFrame frame;recomp_cont_enter(&frame,0x10000,NULL,0);
        recomp_cont_site(&frame,0x200010000ULL);
        xbox_kernel_set_checkpoint_hook(backedge_checkpoint);
        t_guest_yield_tick=GetTickCount64();guest_cpu_yield();
        assert(changed==1 && t_guest_held && recomp_cont_current()==&frame);
        xbox_kernel_set_checkpoint_hook(NULL);recomp_cont_leave(&frame);guest_cpu_part();
    } else if(strcmp(argv[1],"wait_objects")==0) {
        wait_objects_probe();guest_cpu_part();
    } else if(strcmp(argv[1],"register_context")==0 || strcmp(argv[1],"context_save")==0 ||
              strcmp(argv[1],"context_load")==0) {
        context_probe(argc==3?argv[2]:NULL,strcmp(argv[1],"context_load")==0);
        guest_cpu_part();
    } else {
        mode=2;
        memset(XBOX_TO_NATIVE(g_esp),0,40);
        BRIDGE_MEM32(g_esp+36)=0x20000;
        call_slot(255,bridge_PsCreateSystemThreadEx,0);
        guest_cpu_part();
    }
    CloseHandle(entered); CloseHandle(started); return 0;
}
'''
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        (work/"memory.c").write_text(memory, encoding="utf-8")
        (work/"threads.c").write_text(program, encoding="utf-8")
        libraries = list((root.parents[1]/"build/native/xboxrecomp/src").glob("*/*.lib"))
        assert len(libraries)==8, "requires the existing native runtime build"
        dsp_jit = root.parents[1]/"build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib"
        assert dsp_jit.is_file(), "requires the same native DSP JIT library as production"
        libraries.append(dsp_jit)
        result = subprocess.run([compiler,"/nologo","/O2","/Gy","/MD","/utf-8","/DWIN32_LEAN_AND_MEAN",
            "/DNOMINMAX","/I"+str(root/"src"),str(work/"memory.c"),
            str(work/"threads.c"),str(root/"src/kernel/kernel_hal.c"),str(root/"src/kernel/recomp_continuation.c"),
            str(root/"src/kernel/kernel_path.c"),
            str(root/"src/usb/ohci.c"),str(root/"src/kernel/recomp_trace.c"),
            "/Fe:"+str(work/"threads.exe"),"/link","/OPT:REF",*[str(p) for p in libraries],
            "kernel32.lib","user32.lib","gdi32.lib","advapi32.lib","ole32.lib","shell32.lib",
            "d3d11.lib","d3d12.lib","dxgi.lib","dxguid.lib","d3dcompiler.lib","winmm.lib",
            "xaudio2_8.lib","xinput.lib","bcrypt.lib","ntdll.lib","userenv.lib","ws2_32.lib"],
            cwd=work,capture_output=True,text=True,errors="replace")
        assert result.returncode==0,result.stdout+result.stderr
        yield work/"threads.exe"


@pytest.mark.parametrize("mode",["yield_idle","pcr_layout","critical_leave","event_idle","handoff_stress","pcr","raise","dpc","dpc_priority","apu_idle","apu_irq","quantum","resume","kernel_resume","irql","inline","register_context","wait_objects","checkpoint_backedge"])
def test_original_thread_and_thunk_ownership(native_thread_probe,mode):
    subprocess.run([str(native_thread_probe),mode],check=True,timeout=12)


def test_production_register_context_survives_new_process(native_thread_probe,tmp_path):
    snapshot=tmp_path/'registers.bin'
    subprocess.run([str(native_thread_probe),'context_save',str(snapshot)],check=True,timeout=12)
    original=snapshot.read_bytes()
    subprocess.run([str(native_thread_probe),'context_load',str(snapshot)],check=True,timeout=12)
    assert snapshot.read_bytes()==original


@pytest.mark.parametrize("enabled",[False,True],ids=["off","on"])
def test_original_dispatch_diagnostic_budget(native_thread_probe,enabled):
    result=subprocess.run([str(native_thread_probe),"diagnostic_summary_"+("on" if enabled else "off")],
                          capture_output=True,text=True,errors="replace",timeout=12)
    assert result.returncode==0,result.stdout+result.stderr
    assert "[DIAGNOSTIC_WATCH]" in result.stderr and "[KWATCH]" in result.stderr
    assert ("[KERNEL] summary:" in result.stderr)==enabled


@pytest.mark.parametrize("enabled",[False,True],ids=["off","on"])
def test_original_file_load_diagnostic_timing(native_thread_probe,tmp_path,enabled):
    root=Path(__file__).resolve().parents[2]
    xbe=root/"tools/conformance/test.xbe"
    assert xbe.is_file(), "requires original conformance XBE"
    result=subprocess.run([str(native_thread_probe),"diagnostic_load_"+("on" if enabled else "off"),str(xbe),str(tmp_path)],
                          capture_output=True,text=True,errors="replace",timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    assert "[DIAGNOSTIC_LOAD]" in result.stderr
    assert ("[OPEN TIME]" in result.stderr)==enabled
    assert ("[READ TIME]" in result.stderr)==enabled


@pytest.mark.parametrize("enabled",[False,True],ids=["off","on"])
def test_original_file_diagnostic_logging(native_thread_probe,tmp_path,enabled):
    root=Path(__file__).resolve().parents[2]
    xbe=root/"tools/conformance/test.xbe"
    assert xbe.is_file(), "requires original conformance XBE"
    result=subprocess.run([str(native_thread_probe),"diagnostic_file_"+("on" if enabled else "off"),str(xbe),str(tmp_path)],
                          cwd=tmp_path,capture_output=True,text=True,errors="replace",timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    assert "data=NINE status=0 bytes=4" in result.stderr
    assert ("[FILE] ->" in result.stderr)==enabled
    assert "[PATH] " not in result.stderr
    assert f"initial_logging={int(enabled)}" in result.stderr
    log=(tmp_path/"xbox_kernel.log").read_text(encoding="utf8",errors="replace")
    assert ("input.bin" in log)==enabled


def test_original_apc_early_guards_keep_guest_state(native_thread_probe):
    result=subprocess.run([str(native_thread_probe),"apc_identity"],capture_output=True,text=True,errors="replace")
    assert result.returncode==0,result.stdout+result.stderr
    assert "[APC_IDENTITY] cases=36 guest_bytes=432 ram_bytes=33554432" in result.stdout
