"""Restore original token aliases, file cursor and native synchronization owners."""
from pathlib import Path
import shutil
import struct
import subprocess

import re
import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module')
def native_objects(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_objects')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    push=lambda value:b'\x68'+struct.pack('<I',value)
    call=lambda pointer:b'\xff\x15'+struct.pack('<I',pointer)
    take=push(0)+push(0)+bytes.fromhex('ff353c000200')+call(0x21000)
    enter=push(0x200a0)+call(0x21004);leave=push(0x200a0)+call(0x21008)
    release=push(0)+bytes.fromhex('ff353c000200')+call(0x2100c)
    program={0x11000:bytes.fromhex('5589e5ff0500000200')+take*2+enter*2+
             bytes.fromhex('ff0504000200a12000020085c074f1')+leave*2+release*2+
             bytes.fromhex('ff0508000200c9c20800')}
    program[0x11100]=bytes.fromhex('5589e5')+push(0)+push(0)+bytes.fromhex('ff3570000200')+call(0x21000)+bytes.fromhex('a374000200ff0578000200c9c20800')
    prefix=r'''
/* Pause after the native wait consumes an event, before the bridge resumes.
 * This fixes the interleaving, without relying on thread timing. */
#define xbox_KeWaitForSingleObject fixture_native_single_wait
#define xbox_KeWaitForMultipleObjects fixture_native_multiple_wait
#include "kernel/kernel_bridge.c"
#undef xbox_KeWaitForSingleObject
#undef xbox_KeWaitForMultipleObjects
/* Compile the actual file backend into this original fixture, not the archive copy. */
#include "file_policy_counted.c"
extern NTSTATUS __stdcall xbox_KeWaitForSingleObject(PVOID,ULONG,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
extern NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(ULONG,PVOID *,ULONG,ULONG,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER,PVOID);
static HANDLE wait_consumed,wait_resume;
static RECOMP_TLS int pause_consumed;
static void pause_after_consumption(NTSTATUS result) {
    if(pause_consumed && result==0) {
        SetEvent(wait_consumed);
        if(WaitForSingleObject(wait_resume,5000)!=WAIT_OBJECT_0) abort();
    }
}
NTSTATUS __stdcall fixture_native_single_wait(PVOID object,ULONG reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout) {
    NTSTATUS result=xbox_KeWaitForSingleObject(object,reason,mode,alertable,timeout);
    pause_after_consumption(result);return result;
}
NTSTATUS __stdcall fixture_native_multiple_wait(ULONG count,PVOID *objects,ULONG type,ULONG reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout,PVOID blocks) {
    NTSTATUS result=xbox_KeWaitForMultipleObjects(count,objects,type,reason,mode,alertable,timeout,blocks);
    pause_after_consumption(result);return result;
}
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
extern int nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t,uint8_t *);
void sub_00011000(void);
void sub_00011100(void);
static volatile LONG dpc_calls;
static void dpc_probe(void) {
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==2);
    InterlockedIncrement(&dpc_calls);g_esp+=20;
}
static HANDLE timer_scan_fired;
static volatile LONG timer_scan_calls;
static void timer_scan_rearm_probe(void) {
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==2);
    uint32_t saved=g_esp;
    EnterCriticalSection(&g_timer_lock);
    assert(g_timer_scan_end==0); /* One-shot retired before its callback. */
    LeaveCriticalSection(&g_timer_lock);
    LONG count=InterlockedIncrement(&timer_scan_calls);
    if(count==1) {
        kernel_set_timer(0x52000,-200000,0,0x53000);
        assert(g_eax==0);
        EnterCriticalSection(&g_timer_lock);
        assert(g_timer_scan_end==1 && g_timers[0].timer_va==0x52000);
        LeaveCriticalSection(&g_timer_lock);
    } else {assert(count==2);SetEvent(timer_scan_fired);}
    g_esp=saved+20;
}
static DWORD WINAPI drain_probe(LPVOID unused) {
    (void)unused;g_esp=0x90000;kernel_drain_dpcs();return 0;
}
static void synchronize_probe(void) {
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==20);
    assert(BRIDGE_MEM32(g_esp+4)==0x1234);
    g_eax=0x5a;g_esp+=8;
}
static unsigned packet_calls,packet_recursive;
static uint32_t packet_order[16];
static void packet_probe(void) {
    uint32_t entry=g_esp,device=BRIDGE_MEM32(entry+4),irp=BRIDGE_MEM32(entry+8);
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==2);
    assert(device==0x27000 && BRIDGE_MEM32(device+0x10)==irp);
    assert(!BRIDGE_MEM8(irp+0x3c+12) && packet_calls<16);
    packet_order[packet_calls++]=irp;
    if(packet_recursive) xbox_IoStartNextPacket(XBOX_TO_NATIVE(device));
    assert(g_esp==entry);g_esp+=12;
}
static unsigned irp_dispatch_calls;
static uint32_t irp_dispatch_expect,irp_dispatch_device;
static void irp_dispatch_probe(void) {
    uint32_t entry=g_esp,device=BRIDGE_MEM32(entry+4),irp=BRIDGE_MEM32(entry+8);
    assert(guest_cpu_is_held() && device==irp_dispatch_device && irp==irp_dispatch_expect);
    uint32_t stack=BRIDGE_MEM32(irp+0x5c);
    assert(BRIDGE_MEM32(stack+0x14)==device && BRIDGE_MEM8(irp+0x19)==1);
    assert(BRIDGE_MEM8(stack)==10 && BRIDGE_MEM32(stack+8)==0x29000);
    ++irp_dispatch_calls;g_eax=0x103;g_esp+=12;
}

static unsigned apc_kernel_calls,apc_normal_calls,apc_order[16],apc_count;
static uint32_t expected_apc_thread;
static void apc_kernel_probe(void) {
    uint32_t apc=BRIDGE_MEM32(g_esp+4),normal=BRIDGE_MEM32(g_esp+8),context=BRIDGE_MEM32(g_esp+12);
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==1 && ke_current_thread()==expected_apc_thread);
    assert(!BRIDGE_MEM8(apc+3));++apc_kernel_calls;apc_order[apc_count++]=apc;
    if(BRIDGE_MEM32(apc+0x18)) {
        assert(BRIDGE_MEM32(normal)==0x17000);BRIDGE_MEM32(context)=0x1234;
        BRIDGE_MEM32(BRIDGE_MEM32(g_esp+16))=0x5678;BRIDGE_MEM32(BRIDGE_MEM32(g_esp+20))=0x9abc;
    }
    g_eax=0xdeadbeef;g_esp+=24;
}
static void apc_normal_probe(void) {
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==0 && ke_current_thread()==expected_apc_thread);
    assert(BRIDGE_MEM32(g_esp+4)==0x1234 && BRIDGE_MEM32(g_esp+8)==0x5678 && BRIDGE_MEM32(g_esp+12)==0x9abc);
    ++apc_normal_calls;g_esp+=16;
}
static void apc_worker_probe(void) {
    assert(guest_cpu_is_held() && ke_current_thread()==expected_apc_thread && g_fs_base!=XBOX_TIB_MAIN);
    LARGE_INTEGER zero;zero.QuadPart=0;
    assert(ke_remove_queue(0x28000,KernelMode,&zero)==0x28100);
    assert(xbox_checkpoint_wait(0,NULL,FALSE,UserMode,TRUE,0)==XBOX_WAIT_USER_APC);
    assert(apc_normal_calls==1);
    uint32_t abandoned=xbox_HeapAlloc(0x28,16);
    ke_initialize_apc(abandoned,ke_current_thread(),KERNEL_POOL_FREE_APC,0,0x17000,1,0x1234);
    assert(ke_insert_apc(abandoned,0x5678,0x9abc));
    g_esp+=12;
}

static unsigned completion_calls,completion_hold,completion_free;
static uint32_t completion_contexts[8];
static void irp_completion_probe(void) {
    uint32_t irp=BRIDGE_MEM32(g_esp+8),context=BRIDGE_MEM32(g_esp+12);
    assert(guest_cpu_is_held());completion_contexts[completion_calls++]=context;
    if(completion_hold) {
        if(completion_free)xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));g_eax=0xc0000016u;
    } else g_eax=0;
    g_esp+=16;
}
static void irp_sync_driver(void) {
    uint32_t irp=BRIDGE_MEM32(g_esp+8),stack=BRIDGE_MEM32(irp+0x5c);
    BRIDGE_MEM32(irp+0x10)=0;BRIDGE_MEM32(irp+0x14)=17;BRIDGE_MEM8(stack+3)|=1;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);g_eax=0x103;g_esp+=12;
}

static unsigned control_calls,control_pending,control_error,control_fs,control_terminate;
static uint32_t control_file,control_irp;
static void control_dispatch_probe(void) {
    uint32_t device=BRIDGE_MEM32(g_esp+4),irp=BRIDGE_MEM32(g_esp+8),stack=BRIDGE_MEM32(irp+0x5c);
    assert(guest_cpu_is_held() && device==BRIDGE_MEM32(control_file+4));
    assert(BRIDGE_MEM8(stack)==(control_fs?9:10) && BRIDGE_MEM32(stack+0x18)==control_file);
    assert(BRIDGE_MEM32(stack+0x10)==0xabcdef04 && BRIDGE_MEM32(stack+8)==0x28100 && BRIDGE_MEM32(stack+0xc)==4);
    assert(BRIDGE_MEM32(stack+4)==4 && BRIDGE_MEM32(irp+0x30)==0x28200 && BRIDGE_MEM32(irp+0x1c)==0x29200);
    assert(BRIDGE_MEM32(irp+0x60)==control_file && BRIDGE_MEM32(irp+0x50)==ke_current_thread());
    assert(!!(BRIDGE_MEM32(irp+4)&0x80)==!!control_fs);
    control_irp=irp;++control_calls;
    if(control_pending)BRIDGE_MEM8(stack+3)|=1;
    BRIDGE_MEM32(irp+0x10)=control_error?0xc0000001u:0;BRIDGE_MEM32(irp+0x14)=control_error?0:4;
    if(!control_error)BRIDGE_MEM32(0x28200)=BRIDGE_MEM32(0x28100)+1;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);
    if(control_terminate) {
        xbox_DeliverThreadApcs(FALSE);g_esp-=4;BRIDGE_MEM32(g_esp)=0x456;
        bridge_PsTerminateSystemThread();assert(0);
    }
    g_eax=control_pending?0x103:control_error?0xc0000001u:0;g_esp+=12;
}

static void control_termination_probe(void) {
    uint32_t marker=1;RecompContFrame frame;const RecompContLocal locals[]={{"marker",&marker,4}};
    assert(!recomp_cont_enter(&frame,0x1c100,locals,1));
    bridge_control_file(0);assert(0);
}

static unsigned file_cleanup_calls,file_close_calls,file_lifecycle_pending;
static uint32_t lifecycle_file,lifecycle_irp;
static void file_lifecycle_probe(void) {
    uint32_t device=MEM32(g_esp+4),irp=MEM32(g_esp+8),stack=MEM32(irp+0x5c);
    unsigned major=MEM8(stack);assert(major==1 || major==13);
    assert(device==MEM32(lifecycle_file+4) && ke_object_slot(lifecycle_file));
    assert(MEM32(device+4)==1 && MEM32(irp+4)==0x44);
    assert(MEM32(irp+0x60)==lifecycle_file && MEM32(stack+0x18)==lifecycle_file);
    assert(MEM32(irp+0x50)==ke_current_thread() && (MEM8(lifecycle_file+3)&0x20));
    assert(MEM32(ke_current_thread()+0x134)==irp+8 && MEM32(lifecycle_file+0x3c)==0);
    if(MEM8(lifecycle_file+3)&1)assert(MEM32(lifecycle_file+0x24)==0);
    if(major==13){assert(!file_close_calls);++file_cleanup_calls;}
    else {assert(file_cleanup_calls==2);++file_close_calls;}
    lifecycle_irp=irp;MEM32(irp+0x10)=0;MEM32(irp+0x14)=0;
    if(file_lifecycle_pending)MEM8(stack+3)|=1;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);
    g_eax=file_lifecycle_pending?0x103:0;g_esp+=12;
}

static void check_float_context(uint32_t context) {
    uint32_t fp=context+4;uint64_t significand;memcpy(&significand,XBOX_TO_NATIVE(fp+32),8);
    assert(BRIDGE_MEM16(fp)==0x27f && BRIDGE_MEM16(fp+2)==0x1901 && BRIDGE_MEM8(fp+4)==8);
    assert(significand==UINT64_C(0xa000000000000000) && BRIDGE_MEM16(fp+40)==0x4000);
    assert(BRIDGE_MEM16(fp+6)==0x123 && BRIDGE_MEM32(fp+8)==0x223344 && BRIDGE_MEM32(fp+16)==0x556677);
    assert(BRIDGE_MEM32(fp+24)==0x3f80 && BRIDGE_MEM8(fp+160)==0x90 && BRIDGE_MEM8(fp+287)==0x97);
}
static void edit_float_context(uint32_t context) {
    uint32_t fp=context+4;uint64_t significand=UINT64_C(0x8000000000000000);
    BRIDGE_MEM16(fp)=0x37f;BRIDGE_MEM16(fp+2)=0x6820;BRIDGE_MEM8(fp+4)=32;
    memcpy(XBOX_TO_NATIVE(fp+32),&significand,8);BRIDGE_MEM16(fp+40)=0xc001;
    BRIDGE_MEM32(fp+24)=0x5f80;memset(XBOX_TO_NATIVE(fp+160),0x78,16);
}
static unsigned create_calls,create_cleanup,create_close,create_pending,create_error;
static uint32_t create_device,create_file,create_irp;
static const char *create_name="\\child";
static uint64_t create_expected_allocation;
static void generic_create_probe(void) {
    uint32_t device=MEM32(g_esp+4),irp=MEM32(g_esp+8),stack=MEM32(irp+0x5c);
    unsigned major=MEM8(stack);assert(device==create_device && MEM32(device+4));
    if(major==0) {
        ++create_calls;create_file=MEM32(stack+0x18);create_irp=irp;
        assert(MEM32(irp+4)==0x8c && MEM32(irp+0x60)==create_file);
        assert(MEM32(create_file+4)==device && MEM16(create_file)==5);
        assert(MEM32(stack+8)==0x01000020 && MEM16(stack+0xe)==7);
        assert(*(uint64_t *)XBOX_TO_NATIVE(MEM32(stack+4))==create_expected_allocation);
        uint32_t name=MEM32(stack+0x10);
        assert(MEM16(name)==strlen(create_name) && !memcmp(XBOX_TO_NATIVE(MEM32(name+4)),create_name,strlen(create_name)));
        MEM32(create_file+8)=0xfeed;
    } else {
        assert(major==1 || major==13);assert(MEM32(irp+4)==0x44);
        if(major==13)++create_cleanup;else ++create_close;
    }
    MEM32(irp+0x10)=create_error && major==0?0xc0000022u:0;MEM32(irp+0x14)=major==0?2:0;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);
    assert(MEM16(irp)==6); /* Create and lifecycle IRPs remain caller-owned. */
    g_eax=create_pending?0x103:(create_error && major==0?0xc0000022u:0);g_esp+=12;
}

static unsigned seh_calls,seh_mode;
static uint32_t seh_address,seh_stack;
static void seh_probe(void) {
    uint32_t record=BRIDGE_MEM32(g_esp+4),registration=BRIDGE_MEM32(g_esp+8);
    uint32_t context=BRIDGE_MEM32(g_esp+12),dispatcher=BRIDGE_MEM32(g_esp+16);
    assert(guest_cpu_is_held() && BRIDGE_MEM32(context)==0x10007);
    ++seh_calls;
    if(seh_mode==3) {
        if(BRIDGE_MEM32(record)==0xc0000025u) {
            assert(BRIDGE_MEM32(record+4)==1 && BRIDGE_MEM32(record+8));g_eax=1;
        } else g_eax=0;
        g_esp+=20;return;
    }
    assert(BRIDGE_MEM32(record+12)==seh_address && BRIDGE_MEM32(context+0x224)==seh_address);
    assert(BRIDGE_MEM32(context+0x230)==seh_stack && BRIDGE_MEM32(context+0x21c)==123);
    assert(BRIDGE_MEM32(context+0x210)==0xabcdef && BRIDGE_MEM32(context+0x220)==0x810000);
    assert(BRIDGE_MEM32(context+0x22c)==0x202);check_float_context(context);
    if(seh_mode==2 && registration==0x820000) {
        BRIDGE_MEM32(dispatcher)=0x820020;g_eax=2;
    } else {
        if(seh_mode==2)assert(BRIDGE_MEM32(record+4)&0x10);
        BRIDGE_MEM32(context+0x21c)=456;BRIDGE_MEM32(context+0x218)=7;
        BRIDGE_MEM32(context+0x22c)=0x246;edit_float_context(context);
        if(!seh_mode)BRIDGE_MEM32(context+0x224)+=2;
        g_eax=0;
    }
    g_esp+=20;
}


static unsigned dpc_stack_calls,dpc_unwind_calls,dpc_dispatch_calls;
static void dpc_dispatch_stack_probe(void) {
    assert(MEM32(g_esp+8)==MEM32(XBOX_FS_BASE+0x5c)-0x80);
    ++dpc_dispatch_calls;g_eax=0;g_esp+=20;
}
static void dpc_unwind_stack_probe(void) {
    uint32_t registration=MEM32(g_esp+8);
    assert(MEM32(MEM32(g_esp+4)+4)==2);
    assert(registration==MEM32(XBOX_FS_BASE+0x5c)-0x80);
    assert(bridge_unwind_registration_valid(registration));
    ++dpc_unwind_calls;g_eax=1;g_esp+=20;
}
static void dpc_stack_contract_probe(void) {
    uint32_t top=MEM32(XBOX_FS_BASE+0x5c),saved=g_esp;
    assert(top && g_esp==top-28 && MEM32(XBOX_FS_BASE)==0xffffffffu);
    assert(MEM32(XBOX_FS_BASE+0x58)==top-8 && MEM32(top-4)==0x800000 && xbox_CurrentIrql()==2);
    bridge_KeIsExecutingDpc();assert(g_eax==1);
    assert(MEM32(g_esp+4)==0x29000 && MEM32(g_esp+8)==0x1234);
    assert(MEM32(g_esp+12)==0x5678 && MEM32(g_esp+16)==0x9abc);
    assert(!bridge_unwind_registration_valid(top-0x3004));
    assert(bridge_unwind_registration_valid(top-0x3000));
    assert(!bridge_unwind_registration_valid(top-4));
    MEM32(XBOX_FS_BASE+0x58)=0;assert(!bridge_unwind_registration_valid(top-0x80));
    MEM32(XBOX_FS_BASE+0x58)=top-8;
    xbox_IrqlLeaveInterrupt(0);assert(!bridge_unwind_registration_valid(top-0x80));
    xbox_IrqlEnterInterrupt(2);
    MEM32(top-0x80)=0xffffffffu;MEM32(top-0x7c)=0x1b200;MEM32(XBOX_FS_BASE)=top-0x80;
    g_esp=top-0x100;MEM32(g_esp)=0xffffffffu;MEM32(g_esp+4)=0;
    MEM32(g_esp+8)=0x60000;MEM32(g_esp+12)=0;
    MEM32(0x60000)=0x11111111;MEM32(0x60004)=0;MEM32(g_esp-4)=0x12345;
    bridge_RtlUnwind();assert(MEM32(XBOX_FS_BASE)==0xffffffffu && dpc_unwind_calls==dpc_stack_calls+1);
    g_esp=top-0x100;MEM32(top-0x7c)=0x1b300;MEM32(XBOX_FS_BASE)=top-0x80;
    XboxGuestRegisters registers;xbox_CaptureGuestRegisters(&registers);
    bridge_capture_context(0x61000,&registers,0x12345,g_esp);
    MEM32(0x60004)=0;assert(bridge_dispatch_exception(0x60000,0x61000));
    assert(dpc_dispatch_calls==dpc_stack_calls+1 && MEM32(XBOX_FS_BASE)==top-0x80);
    ++dpc_stack_calls;g_esp=saved+20;
}
static unsigned unwind_mode,unwind_calls;
static void unwind_contract_probe(void) {
    uint32_t record=MEM32(g_esp+4),registration=MEM32(g_esp+8);
    uint32_t context=MEM32(g_esp+12),dispatcher=MEM32(g_esp+16),code=MEM32(record);
    if(code==0xc0000026u || code==0xc0000028u || code==0xc0000029u) {
        assert(MEM32(record+4)&1u);fprintf(stderr,"UNWIND_ERROR %08X\n",code);g_eax=1;g_esp+=20;return;
    }
    assert(code==(unwind_mode==1?0xc0000027u:0x11111111u));
    uint32_t guard=MEM32(XBOX_FS_BASE);assert(guard!=registration && MEM32(guard)==registration);
    assert(MEM32(guard+4)==KERNEL_UNWIND_COLLISION_GUARD && MEM32(guard+8)==registration);
    assert(MEM32(record+4)==(unwind_mode==7?6:2));
    assert(context && dispatcher && MEM32(context)==0x10007 && MEM32(context+0x21c)==0x456);
    assert(MEM32(context+0x224)==0x12345 && MEM32(context+0x230)==0x800010);
    if(!unwind_calls)check_float_context(context);
    uint32_t guard_args[]={record,guard,context,dispatcher};
    assert(ke_call_guest(KERNEL_UNWIND_COLLISION_GUARD,guard_args,4,1)==3);
    assert(MEM32(dispatcher)==registration && MEM32(XBOX_FS_BASE)==guard);
    uint32_t flags=MEM32(record+4);MEM32(record+4)=0;MEM32(dispatcher)=0xabcdef;
    assert(ke_call_guest(KERNEL_UNWIND_COLLISION_GUARD,guard_args,4,1)==1);
    assert(MEM32(dispatcher)==0xabcdef);MEM32(record+4)=flags;
    if(unwind_mode==10) {
        uint32_t top=g_esp;g_esp-=16;MEM32(g_esp-4)=0x12345;
        MEM32(g_esp)=0x820020;MEM32(g_esp+4)=0;MEM32(g_esp+8)=record;MEM32(g_esp+12)=0x789;
        bridge_RtlUnwind();g_esp+=16;assert(g_esp==top && MEM32(XBOX_FS_BASE)==0x820020);
    }
    ++unwind_calls;MEM32(context+0x210)=0x98765;MEM32(context+0x218)=9;
    MEM32(context+0x22c)=0x246;edit_float_context(context);
    if(unwind_mode==2) {assert(unwind_calls==1);MEM32(dispatcher)=0x820020;g_eax=3;}
    else g_eax=unwind_mode==5?0:1;
    g_esp+=20;
}

static unsigned owned_stack_dispatch_calls,owned_stack_unwind_calls;
static uint32_t owned_stack_registration;
static void owned_stack_probe(void) {
    uint32_t record=MEM32(g_esp+4),registration=MEM32(g_esp+8);
    assert(registration==owned_stack_registration && bridge_unwind_registration_valid(registration));
    assert(MEM32(g_esp+12) && MEM32(g_esp+16));
    if(MEM32(record+4)&2u){++owned_stack_unwind_calls;g_eax=1;}
    else {++owned_stack_dispatch_calls;g_eax=0;}
    g_esp+=20;
}

static unsigned irp_user_apc_calls;
static void irp_user_apc_probe(void) {
    assert(BRIDGE_MEM32(g_esp+4)==0x1234 && BRIDGE_MEM32(g_esp+8)==0x29200);
    assert(!BRIDGE_MEM32(g_esp+12));++irp_user_apc_calls;g_esp+=16;
}
static void timer_apc_probe(void);
static unsigned device_delete_calls;
static void device_delete_probe(void) {
    uint32_t device=MEM32(g_esp+4);assert(MEM8(device+0x1f) && !MEM32(device+4));
    ++device_delete_calls;xbox_ObfDereferenceObject(XBOX_TO_NATIVE(device));g_esp+=8;
}
recomp_func_t recomp_lookup(uint32_t va) {
    if(va==0x1b500)return owned_stack_probe;
    if(va==0x1b400)return timer_scan_rearm_probe;
    if(va==0x1b100)return dpc_stack_contract_probe;
    if(va==0x1b200)return dpc_unwind_stack_probe;
    if(va==0x1b300)return dpc_dispatch_stack_probe;
    if(va==0x1b000)return unwind_contract_probe;
    if(va==0x17600)return device_delete_probe;
    if(va==0x17200)return timer_apc_probe;
    if(va==0x17400)return irp_user_apc_probe;
    if(va==0x17700)return control_dispatch_probe;
    if(va==0x17800)return file_lifecycle_probe;
    if(va==0x17900)return generic_create_probe;
    return va==0x11100?sub_00011100:va==0x11000?sub_00011000:va==0x12000?synchronize_probe:va==0x13000?dpc_probe:va==0x14000?packet_probe:va==0x15000?irp_dispatch_probe:va==0x16000?apc_kernel_probe:va==0x17000?apc_normal_probe:va==0x18000?irp_completion_probe:va==0x19000?irp_sync_driver:va==0x1a000?seh_probe:NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
'''
    suffix=r'''
#undef esp
static void fixture_import(uint32_t ordinal) {
    uint32_t entry=g_esp,bytes=stdcall_args_for_ordinal(ordinal);
    int was_held=guest_cpu_is_held();if(!was_held)guest_cpu_join();
    /* Imports keep a fixed slot while another guest thread is waiting. */
    g_slot_ordinals[ordinal]=ordinal;g_slot_arg_bytes[ordinal]=bytes;
    g_slot_bridges[ordinal]=bridge_for_ordinal(ordinal);assert(g_slot_bridges[ordinal]);
    g_kernel_dispatch_slot=(int)ordinal;g_esp-=4;BRIDGE_MEM32(g_esp)=0x12345;
    kernel_thunk_dispatch();assert(guest_cpu_is_held() && g_esp==entry+bytes);
    g_esp=entry;if(!was_held)guest_cpu_part();
}
#define bridge_NtQueueApcThread() fixture_import(206)
#define bridge_IoCreateDevice() fixture_import(65)
#define bridge_IoDeleteDevice() fixture_import(68)
#define bridge_NtCreateIoCompletion() fixture_import(191)
#define bridge_NtSetIoCompletion() fixture_import(227)
#define bridge_NtRemoveIoCompletion() fixture_import(223)
#define bridge_KeInitializeQueue() fixture_import(111)
#define bridge_KeInsertQueue() fixture_import(117)
#define bridge_KeInsertHeadQueue() fixture_import(116)
#define bridge_KeRemoveQueue() fixture_import(136)
#define bridge_KeRundownQueue() fixture_import(141)
#define bridge_KeInitializeApc() fixture_import(105)
#define bridge_KeInsertQueueApc() fixture_import(118)
#define bridge_KeGetCurrentThread() fixture_import(104)
#define bridge_KeEnterCriticalRegion() fixture_import(101)
#define bridge_KeLeaveCriticalRegion() fixture_import(122)
#define bridge_IofCompleteRequest() fixture_import(87)
#define bridge_IoSynchronousDeviceIoControlRequest() fixture_import(84)
#define bridge_IoSynchronousFsdRequest() fixture_import(85)
#define bridge_IoAllocateIrp() fixture_import(59)
#define bridge_IoBuildAsynchronousFsdRequest() fixture_import(60)
#define bridge_IoBuildDeviceIoControlRequest() fixture_import(61)
#define bridge_IoBuildSynchronousFsdRequest() fixture_import(62)
#define bridge_IoFreeIrp() fixture_import(72)
#define bridge_IoInitializeIrp() fixture_import(73)
#define bridge_IofCallDriver() fixture_import(86)
#define bridge_IoStartPacket() fixture_import(83)
#define bridge_IoStartNextPacket() fixture_import(81)
#define bridge_IoStartNextPacketByKey() fixture_import(82)
#define bridge_KeInsertDeviceQueue() fixture_import(115)
#define bridge_KeInsertByKeyDeviceQueue() fixture_import(114)
#define bridge_KeRemoveDeviceQueue() fixture_import(134)
#define bridge_KeRemoveByKeyDeviceQueue() fixture_import(133)
#define bridge_KeRemoveEntryDeviceQueue() fixture_import(135)
#define bridge_HalGetInterruptVector() fixture_import(44)
#define bridge_IoCreateDevice() fixture_import(65)
#define bridge_IoDeleteDevice() fixture_import(68)
#define bridge_KeCancelTimer() fixture_import(97)
#define bridge_KeConnectInterrupt() fixture_import(98)
#define bridge_KeDisconnectInterrupt() fixture_import(100)
#define bridge_KeInitializeDeviceQueue() fixture_import(106)
#define bridge_KeInitializeDpc() fixture_import(107)
#define bridge_KeInitializeEvent() fixture_import(108)
#define bridge_KeInitializeInterrupt() fixture_import(109)
#define bridge_KeInitializeTimerEx() fixture_import(113)
#define bridge_KeInsertQueueDpc() fixture_import(119)
#define bridge_KePulseEvent() fixture_import(123)
#define bridge_KeRemoveQueueDpc() fixture_import(137)
#define bridge_KeResetEvent() fixture_import(138)
#define bridge_KeSetEvent() fixture_import(145)
#define bridge_KeSetTimer() fixture_import(149)
#define bridge_KeSetTimerEx() fixture_import(150)
#define bridge_KeSynchronizeExecution() fixture_import(153)
#define bridge_KeWaitForMultipleObjects() fixture_import(158)
#define bridge_KeWaitForSingleObject() fixture_import(159)
static void event_wait_call(int multiple,uint32_t stack) {
    g_esp=stack;
    if(multiple) {
        BRIDGE_MEM32(0x26400)=0x26000;
        BRIDGE_MEM32(g_esp)=1;BRIDGE_MEM32(g_esp+4)=0x26400;
        BRIDGE_MEM32(g_esp+8)=1;BRIDGE_MEM32(g_esp+12)=0;
        BRIDGE_MEM32(g_esp+16)=0;BRIDGE_MEM32(g_esp+20)=0;
        BRIDGE_MEM32(g_esp+24)=0x26200;BRIDGE_MEM32(g_esp+28)=0;
        bridge_KeWaitForMultipleObjects();
    } else {
        BRIDGE_MEM32(g_esp)=0x26000;BRIDGE_MEM32(g_esp+4)=0;
        BRIDGE_MEM32(g_esp+8)=0;BRIDGE_MEM32(g_esp+12)=0;
        BRIDGE_MEM32(g_esp+16)=0x26200;bridge_KeWaitForSingleObject();
    }
}
static DWORD WINAPI event_wait_probe(LPVOID multiple) {
    pause_consumed=1;event_wait_call((int)(uintptr_t)multiple,0x90000);
    assert(g_eax==0);return 0;
}
static void setup(const char *path,const char *directory,int with_tls) {
    FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);
    long size=ftell(file);rewind(file);void *xbe=malloc(size);assert(xbe);
    assert(fread(xbe,size,1,file)==1 && fclose(file)==0);
    if(with_tls) {
        /* Existing conformance XBE leaves this header area unused. The real
         * loader rounds the16-byte template into its20-byte TLS block. */
        uint32_t *header=xbe;assert(size>=0x1000 && header[0x104/4]==0x10000 && header[0x108/4]>=0x1000);
        assert(!header[0x12c/4]);
        for(unsigned at=0x700;at<0x764;at++)assert(!((uint8_t *)xbe)[at]);
        header[0x12c/4]=0x10700;
        uint32_t tls[]={0x10740,0x10750,0x10760,0,0,0};
        memcpy((uint8_t *)xbe+0x700,tls,sizeof tls);
        memset((uint8_t *)xbe+0x740,0,16);
    }
    assert(xbox_MemoryLayoutInit(xbe,size));free(xbe);
    xbox_path_init(directory,directory);_putenv_s("RECOMP_GUEST_LOCK","1");assert(guest_cpu_enabled());
    assert(xbox_kernel_init_devices()==0);
    XBOX_ANSI_STRING drive={6,6,"\\??\\D:"},device={14,14,"\\Device\\CdRom0"};
    assert(xbox_IoCreateSymbolicLink(&drive,&device)==0);
    void (*bridges[])(void)={bridge_NtWaitForSingleObject,bridge_RtlEnterCriticalSection,
                            bridge_RtlLeaveCriticalSection,bridge_NtReleaseMutant};
    unsigned sizes[]={12,4,4,8};unsigned ordinals[]={233,277,294,221};
    for(unsigned i=0;i<4;i++) { g_slot_bridges[i]=bridges[i];g_slot_arg_bytes[i]=sizes[i];
        g_slot_ordinals[i]=ordinals[i];MEM32(0x21000+i*4)=KERNEL_VA_BASE+i*4; }
}
static void duplicate(uint32_t source,uint32_t target,uint32_t options) {
    g_esp=0x80000;MEM32(g_esp)=source;MEM32(g_esp+4)=target;MEM32(g_esp+8)=options;
    bridge_NtDuplicateObject();assert(g_eax==0);
}
static void make_objects(void) {
    HANDLE handle;assert(xbox_NtCreateEvent(&handle,NULL,0,FALSE)==0);bridge_write_handle(0x20030,handle);
    assert(xbox_NtCreateEvent(&handle,NULL,1,TRUE)==0);bridge_write_handle(0x20034,handle);
    assert(xbox_NtCreateSemaphore(&handle,NULL,2,7)==0);bridge_write_handle(0x20038,handle);
    assert(xbox_NtCreateMutant(&handle,NULL,FALSE)==0);bridge_write_handle(0x2003c,handle);
    duplicate(MEM32(0x20030),0x20040,DUPLICATE_SAME_ACCESS);
    ke_shadow_insert(0x20090,CreateEventW(NULL,TRUE,TRUE,NULL));
    xbox_RtlInitializeCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    XBOX_ANSI_STRING name={12,13,"D:\\asset.bin"};XBOX_OBJECT_ATTRIBUTES attrs={0,&name,0};XBOX_IO_STATUS_BLOCK ios;
    assert(xbox_NtCreateFile(&handle,XBOX_GENERIC_READ|XBOX_SYNCHRONIZE,&attrs,&ios,NULL,0,7,XBOX_FILE_OPEN,0)==0);
    bridge_write_handle(0x20050,handle);unsigned char data[4];DWORD got;
    assert(ReadFile(handle,data,4,&got,NULL) && got==4 && data[3]==3);
    duplicate(MEM32(0x20050),0x20054,DUPLICATE_SAME_ACCESS);
    /* Deliberately distinct saved per-handle marks; duplication inheritance is
     * covered separately by closed_source_probe. */
    bridge_note_async_handle(MEM32(0x20050));
}
static void verify_objects(void) {
    HANDLE manual=bridge_read_handle(0x20030),alias=bridge_read_handle(0x20040);
    assert(MEM32(0x20030)!=MEM32(0x20040) && checkpoint_native.compare(manual,alias)==0);
    assert(WaitForSingleObject(manual,0)==WAIT_TIMEOUT);assert(SetEvent(alias));
    assert(WaitForSingleObject(manual,0)==WAIT_OBJECT_0 && WaitForSingleObject(alias,0)==WAIT_OBJECT_0);
    assert(ResetEvent(manual) && WaitForSingleObject(alias,0)==WAIT_TIMEOUT);
    HANDLE automatic=bridge_read_handle(0x20034);
    assert(WaitForSingleObject(automatic,0)==WAIT_OBJECT_0 && WaitForSingleObject(automatic,0)==WAIT_TIMEOUT);
    HANDLE sem=bridge_read_handle(0x20038);
    assert(WaitForSingleObject(sem,0)==WAIT_OBJECT_0 && WaitForSingleObject(sem,0)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(sem,0)==WAIT_TIMEOUT);LONG previous;
    assert(ReleaseSemaphore(sem,7,&previous) && previous==0 && !ReleaseSemaphore(sem,1,NULL));
    HANDLE file=bridge_read_handle(0x20050),file_alias=bridge_read_handle(0x20054);unsigned char byte;DWORD got;
    assert(bridge_handle_is_async(MEM32(0x20050)) && !bridge_handle_is_async(MEM32(0x20054)));
    assert(checkpoint_native.compare(file,file_alias)==0);
    assert(ReadFile(file,&byte,1,&got,NULL) && got==1 && byte==4);
    assert(ReadFile(file_alias,&byte,1,&got,NULL) && got==1 && byte==5);
    assert(WaitForSingleObject(ke_shadow_lookup(0x20090),0)==WAIT_OBJECT_0);
}
static void cleanup(void) {
    for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) if(s_handle_table[i]) { xbox_NtClose(s_handle_table[i]);s_handle_table[i]=NULL; }
    ke_shadow_remove(0x20090);xbox_MemoryLayoutShutdown();
}
static volatile LONG termination_returned;
static int termination_timer;
static uint32_t timer_set_call(uint32_t,int64_t,uint32_t,uint32_t);
static void terminate_probe(void) {
    uint32_t marker=0x456;
    RecompContFrame frame;
    const RecompContLocal locals[]={{"marker",&marker,sizeof marker}};
    assert(!recomp_cont_enter(&frame,0x1b000,locals,1));
    if(termination_timer) {
        uint32_t stack=g_esp;g_esp-=32;
        MEM32(g_esp)=0x20070;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;fixture_import(194);assert(g_eax==0);
        uint32_t token=MEM32(0x20070);assert(timer_set_call(token,-100000000,0x17200,1000)==0);
        MEM32(0x2007c)=ke_object_for_handle(bridge_resolve_handle(token));
        MEM32(g_esp)=token;fixture_import(187);assert(g_eax==0);g_esp=stack;
    }
    BRIDGE_MEM32(g_esp)=marker;
    fixture_import(258);
    InterlockedIncrement(&termination_returned);
    recomp_cont_leave(&frame);
}



static HANDLE wait_owner_ready;
static int wait_owner_hot;
static void close_owned_wait_source(void) {
    uint32_t token=MEM32(0x20070);
    if(!bridge_resolve_handle(token))return;
    assert(t_kernel_wait && t_kernel_wait->object_count==1);
    MEM32(0x2007c)=MEM32(t_kernel_wait->object_tokens_va);
    assert(MEM32(0x2007c)!=token && xbox_NtClose(bridge_take_handle(token))==0);
    if(wait_owner_hot)MEM32(0x20080)=t_kernel_wait->object_tokens_va;
    SetEvent(wait_owner_ready);
}
static uint32_t wait_close_token;
static HANDLE wait_close_alias;
static void close_wait_source(void) {
    if(!wait_close_token)return;
    HANDLE source=bridge_take_handle(wait_close_token);wait_close_token=0;
    assert(xbox_NtClose(source)==0 && SetEvent(wait_close_alias));
}

static unsigned timer_apc_count;
static void timer_apc_probe(void) {
    assert(guest_cpu_is_held() && xbox_CurrentIrql()==0 && MEM32(g_esp+4)==0xabc);
    uint64_t time=MEM32(g_esp+8)|((uint64_t)MEM32(g_esp+12)<<32);assert(time>10000000);
    ++timer_apc_count;g_esp+=16;
}
static uint32_t timer_set_call(uint32_t token,int64_t due,uint32_t routine,uint32_t period) {
    g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=0x20080;
    *(int64_t *)XBOX_TO_NATIVE(0x20080)=due;MEM32(g_esp+8)=routine;MEM32(g_esp+12)=0xabc;
    MEM32(g_esp+16)=1;MEM32(g_esp+20)=0;MEM32(g_esp+24)=period;MEM32(g_esp+28)=0x20088;
    MEM32(0x20088)=0xaabbccdd;fixture_import(229);return g_eax;
}
static uint32_t object_reference(uint32_t token,uint32_t type) {
    g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=type;MEM32(g_esp+8)=0x20060;
    MEM32(0x20060)=0xdeadbeef;fixture_import(246);assert(g_eax==0 && MEM32(0x20060));return MEM32(0x20060);
}
static void object_dereference(uint32_t address) {g_ecx=address;fixture_import(250);}
static void object_close(uint32_t token) {MEM32(g_esp)=token;fixture_import(187);assert(g_eax==0);}
static void object_roundtrip(void) {
    int held=guest_cpu_is_held();if(held)guest_cpu_part();
    assert(xbox_CheckpointBootstrapBegin());FILE *out=tmpfile();assert(out && xbox_CheckpointWriteObjects(out));
    assert(checkpoint_clear_objects());rewind(out);assert(xbox_CheckpointLoadObjects(out,NULL,0));
    fclose(out);xbox_CheckpointEnd();if(held)guest_cpu_join();
}
static void named_attributes(uint32_t root,const char *text,uint32_t attributes) {
    size_t length=strlen(text);assert(length<256);
    MEM32(0x29000)=root;MEM32(0x29004)=0x29020;MEM32(0x29008)=attributes;
    MEM16(0x29020)=(uint16_t)length;MEM16(0x29022)=(uint16_t)length;MEM32(0x29024)=0x29040;
    memcpy(XBOX_TO_NATIVE(0x29040),text,length);
}
static uint32_t named_create(uint32_t ordinal,uint32_t root,const char *name,uint32_t attributes,uint32_t first,uint32_t second) {
    named_attributes(root,name,attributes);MEM32(0x29180)=0xdeadbeef;
    MEM32(g_esp)=0x29180;MEM32(g_esp+4)=ordinal==191?0:0x29000;
    MEM32(g_esp+8)=ordinal==191?0x29000:first;MEM32(g_esp+12)=ordinal==191?first:second;
    fixture_import(ordinal);return MEM32(0x29180);
}
static uint32_t named_open(uint32_t ordinal,uint32_t root,const char *name,uint32_t type) {
    named_attributes(root,name,0);MEM32(0x29180)=0xdeadbeef;
    MEM32(g_esp)=ordinal==201?0x29180:0x29000;MEM32(g_esp+4)=ordinal==201?0x29000:type;
    MEM32(g_esp+8)=0;MEM32(g_esp+12)=0x29180;
    fixture_import(ordinal);return MEM32(0x29180);
}
extern void (*g_xbox_path_hook)(const char *);
static unsigned path_notifications;
static void path_notification(const char *path) {
    assert(!strcmp(path,"R:\\Media\\asset.bin"));++path_notifications;
}

static void verify_original_file_policy_bounds(void) {
    HANDLE unknown=CreateEventA(NULL,FALSE,FALSE,NULL);assert(unknown);
    XboxFileOpenState saved;file_policy_test_reads=0;
    assert(!xbox_FileHandleDevice(unknown));
    assert(!xbox_CaptureFileOpenState(unknown,&saved));
    xbox_FileHandleClosed(unknown);
    xbox_FileHandleDuplicated(unknown,unknown,0);
    fprintf(stderr,"[FILE POLICY] empty unknown actual-handle-reads=%llu\n",file_policy_test_reads);
    assert(file_policy_test_reads==0);assert(CloseHandle(unknown));
    HANDLE handles[4];
    for(unsigned i=0;i<3;i++) {
        handles[i]=CreateEventA(NULL,FALSE,FALSE,NULL);assert(handles[i]);
        assert(file_policy_store(handles[i],GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,0x100+i));
    }
    xbox_FileHandleClosed(handles[1]);assert(CloseHandle(handles[1]));
    /* An existing identity must be updated before filling an earlier hole. */
    assert(file_policy_store(handles[2],GENERIC_WRITE,1,FILE_ATTRIBUTE_NORMAL,0x22,0x202));
    assert(!s_file_open_policy[1].handle && s_file_open_policy[2].handle==handles[2]);
    handles[3]=CreateEventA(NULL,FALSE,FALSE,NULL);assert(handles[3]);
    assert(file_policy_store(handles[3],GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,0x303));
    assert(s_file_open_policy[1].handle==handles[3]);
    /* Close-source copies all policy metadata and retires the source slot. */
    HANDLE alias=NULL;assert(DuplicateHandle(GetCurrentProcess(),handles[0],GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS));
    xbox_FileHandleDuplicated(handles[0],alias,1);assert(CloseHandle(handles[0]));
    assert(!xbox_FileHandleDevice(handles[0]) && xbox_FileHandleDevice(alias)==0x100);
    xbox_FileHandleClosed(alias);assert(CloseHandle(alias));
    xbox_FileHandleClosed(handles[2]);assert(CloseHandle(handles[2]));
    xbox_FileHandleClosed(handles[3]);assert(CloseHandle(handles[3]));
    file_policy_test_reads=0;assert(!xbox_FileHandleDevice((HANDLE)(uintptr_t)0x12345));
    assert(file_policy_test_reads==0); /* Observe tail retirement through the real query. */
    HANDLE *full=calloc(16384,sizeof *full);assert(full);
    for(unsigned i=0;i<16384;i++) {
        full[i]=CreateEventA(NULL,FALSE,FALSE,NULL);assert(full[i]);
        assert(file_policy_store(full[i],GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,i+1));
        assert(s_file_open_policy[i].handle==full[i]);
    }
    HANDLE overflow=CreateEventA(NULL,FALSE,FALSE,NULL);assert(overflow);
    assert(!file_policy_store(overflow,GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,0));
    assert(file_policy_store(full[16383],GENERIC_WRITE,1,FILE_ATTRIBUTE_NORMAL,0x22,0xabcdef));
    assert(xbox_FileHandleDevice(full[16383])==0xabcdef);
    xbox_FileHandleClosed(full[8192]);assert(CloseHandle(full[8192]));
    assert(file_policy_store(full[16383],GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,0xfedcba));
    assert(!s_file_open_policy[8192].handle && s_file_open_policy[16383].handle==full[16383]);
    assert(file_policy_store(overflow,GENERIC_READ,3,FILE_ATTRIBUTE_NORMAL,0x20,0x123456));
    assert(s_file_open_policy[8192].handle==overflow);full[8192]=overflow;
    /* Retire descending tails; no fabricated direct reset of the derived bound. */
    for(unsigned i=16384;i;i--) {xbox_FileHandleClosed(full[i-1]);assert(CloseHandle(full[i-1]));}
    free(full);file_policy_test_reads=0;
    assert(!xbox_FileHandleDevice((HANDLE)(uintptr_t)0x12345));assert(file_policy_test_reads==0);
    fprintf(stderr,"[FILE POLICY] holes/unique/close-source/tail/full16384/overflow passed\n");
}

int main(int argc,char **argv) {
    assert(argc==7);setup(argv[2],argv[6],!strcmp(argv[1],"tls_owned_stack"));
    if(!strcmp(argv[1],"file_policy_bounds")) {verify_original_file_policy_bounds();cleanup();return 0;}


    if(!strcmp(argv[1],"volume_dismount_optical") || !strcmp(argv[1],"volume_dismount_disk")) {
        guest_cpu_join();g_esp=0x80000;
        int disk=!strcmp(argv[1],"volume_dismount_disk");
        XBOX_ANSI_STRING name={0,0,disk?"\\Device\\Harddisk0\\Partition1":"D:"};
        name.Length=name.MaximumLength=(USHORT)strlen(name.Buffer);
        XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};XBOX_IO_STATUS_BLOCK ios;HANDLE volume,fresh,file;
        assert(xbox_NtOpenFile(&volume,0,&oa,&ios,7,0x20)==0);
        uint32_t old=xbox_FileHandleDevice(volume),physical=MEM32(MEM32(old+0x18));
        uint32_t references=ke_object_slot(physical)->references;
        HANDLE signal;assert(xbox_NtCreateEvent(&signal,NULL,0,FALSE)==0);
        uint32_t token=bridge_handle_token(volume),event=bridge_handle_token(signal);
        uint32_t object=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
        MEM32(g_esp)=token;MEM32(g_esp+4)=event;MEM32(g_esp+8)=0x17400;MEM32(g_esp+12)=0x1234;
        MEM32(g_esp+16)=0x29200;MEM32(g_esp+20)=0x90020;
        for(unsigned i=6;i<10;i++)MEM32(g_esp+i*4)=0;
        fixture_import(200);
        assert(!g_eax && !MEM32(0x29200) && !MEM32(0x29204));
        assert(WaitForSingleObject(bridge_resolve_handle(event),0)==WAIT_OBJECT_0);
        assert(!irp_user_apc_calls && xbox_DeliverThreadApcs(TRUE)==1 && irp_user_apc_calls==1);
        assert(MEM8(old+0x1f) && !MEM32(MEM32(old+0x18)) && !MEM32(physical+0xc));
        assert(ke_object_slot(physical)->references==references-1 && MEM32(old+4)==1);
        assert(xbox_NtFsControlFile(volume,NULL,NULL,NULL,XBOX_TO_NATIVE(0x29220),0x90020,NULL,0,NULL,0)==(NTSTATUS)0xc000026eu);
        /* An immediate dispatch error leaves the caller IOSB unchanged. */
        char byte='z';assert(xbox_NtReadFile(volume,NULL,NULL,NULL,&ios,&byte,1,NULL)==(NTSTATUS)0xc000026eu && byte=='z');
        assert(xbox_NtWriteFile(volume,NULL,NULL,NULL,&ios,&byte,1,NULL)==(NTSTATUS)0xc000026eu);
        assert(xbox_NtFlushBuffersFile(volume,&ios)==(NTSTATUS)0xc000026eu);
        XBOX_ANSI_STRING relative={9,9,"asset.bin"};XBOX_OBJECT_ATTRIBUTES relative_oa={volume,&relative,0};
        assert(xbox_NtOpenFile(&file,XBOX_GENERIC_READ,&relative_oa,&ios,7,0)==(NTSTATUS)0xc000026eu);
        XBOX_FILE_NETWORK_OPEN_INFORMATION attributes;
        assert(xbox_NtQueryFullAttributesFile(&relative_oa,&attributes)==(NTSTATUS)0xc000026eu);
        assert(xbox_NtDeleteFile(&relative_oa)==(NTSTATUS)0xc000026eu);
        assert(xbox_NtOpenFile(&fresh,0,&oa,&ios,7,0)==0);
        uint32_t mounted=xbox_FileHandleDevice(fresh),fresh_token=bridge_handle_token(fresh);
        assert(mounted!=old && MEM32(physical+0xc)==mounted && MEM32(mounted+4)==1);
        assert(xbox_NtFsControlFile(fresh,NULL,NULL,NULL,XBOX_TO_NATIVE(0x29220),0x90000,NULL,0,NULL,0)==STATUS_INVALID_DEVICE_REQUEST);
        object_roundtrip();fresh=bridge_resolve_handle(fresh_token);
        assert(xbox_kernel_file_volume_status(bridge_resolve_handle(token))==(NTSTATUS)0xc000026eu);
        object_close(token);object_dereference(object);
        assert(!ke_object_slot(old) && MEM32(physical+0xc)==mounted && MEM32(mounted+4)==1);
        relative_oa.RootDirectory=fresh;
        assert(xbox_NtOpenFile(&file,XBOX_GENERIC_READ,&relative_oa,&ios,7,0)==0);
        assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,&byte,1,NULL)==0 && byte=='a');
        assert(xbox_NtClose(file)==0);object_close(fresh_token);object_close(event);
        assert(!MEM32(mounted+4));guest_cpu_part();cleanup();return 0;
    }

    if(!strncmp(argv[1],"control_request_",16)) {
        guest_cpu_join();g_esp=0x80000;
        control_fs=argv[1][16]=='1';int sync=argv[1][17]=='1';control_pending=argv[1][18]=='1';control_error=argv[1][19]=='1';
        XBOX_ANSI_STRING name={2,2,"D:"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};XBOX_IO_STATUS_BLOCK ios;
        HANDLE handle,signal;assert(xbox_NtOpenFile(&handle,0,&oa,&ios,7,sync?0x20:0)==0);
        assert(xbox_NtCreateEvent(&signal,NULL,0,TRUE)==0);
        uint32_t token=bridge_handle_token(handle),event=bridge_handle_token(signal);
        control_file=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
        uint32_t device=MEM32(control_file+4);MEM32(device+8)=0x26000;
        MEM32(0x26000+12+9*4)=MEM32(0x26000+12+10*4)=0x17700;
        MEM32(0x28100)=41;MEM32(0x28200)=0xcccccccc;MEM32(0x29200)=MEM32(0x29204)=0xcccccccc;
        MEM32(g_esp)=token;MEM32(g_esp+4)=event;MEM32(g_esp+8)=0x17400;MEM32(g_esp+12)=0x1234;
        MEM32(g_esp+16)=0x29200;MEM32(g_esp+20)=0xabcdef04;MEM32(g_esp+24)=0x28100;MEM32(g_esp+28)=4;
        MEM32(g_esp+32)=0x28200;MEM32(g_esp+36)=4;
        if(argv[1][20]=='t') {
            control_terminate=1;
            xbox_RunGuestRoot(control_termination_probe);
            assert(t_guest_terminated && !recomp_cont_active() && !t_kernel_wait && !t_dispatch_depth);
            assert(MEM32(control_file+0x24)==UINT32_MAX && ke_object_slot(control_file)->references==1);
            assert(!MEM16(control_irp));MEM32(device+8)=0;
            object_close(event);object_close(token);object_dereference(control_file);
            assert(!MEM32(device+4));guest_cpu_part();cleanup();return 0;
        }
        if(argv[1][20]=='n')g_eax=(uint32_t)(control_fs?xbox_NtFsControlFile:xbox_NtDeviceIoControlFile)(
            bridge_resolve_handle(token),bridge_resolve_handle(event),(PIO_APC_ROUTINE)XBOX_TO_NATIVE(0x17400),XBOX_TO_NATIVE(0x1234),
            XBOX_TO_NATIVE(0x29200),0xabcdef04,XBOX_TO_NATIVE(0x28100),4,XBOX_TO_NATIVE(0x28200),4);
        else fixture_import(control_fs?200:196);
        assert(control_calls==1 && g_eax==(control_pending && !sync?0x103u:control_error?0xc0000001u:0));
        int published=!control_error || (control_pending && !sync);
        assert(MEM32(0x29200)==(published?(control_error?0xc0000001u:0):0xcccccccc));
        assert(MEM32(0x29204)==(published?(control_error?0:4):0xcccccccc));
        assert(MEM32(0x28200)==(control_error?0xcccccccc:42));
        assert(WaitForSingleObject(bridge_resolve_handle(event),0)==(published?WAIT_OBJECT_0:WAIT_TIMEOUT));
        assert(!irp_user_apc_calls && xbox_DeliverThreadApcs(TRUE)==published && irp_user_apc_calls==published);
        assert(!MEM16(control_irp) && ke_object_slot(control_file)->references==1);
        if(sync)assert(MEM32(control_file+0x24)==UINT32_MAX);
        assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        MEM32(device+8)=0;object_close(event);object_close(token);object_dereference(control_file);
        assert(!MEM32(device+4));guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"optical_file_control")) {
        guest_cpu_join();g_esp=0x80000;
        XBOX_ANSI_STRING name={14,14,"\\Device\\CdRom0"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0x40};
        XBOX_IO_STATUS_BLOCK ios;HANDLE file;
        assert(xbox_NtOpenFile(&file,0x80100000,&oa,&ios,1,0x20)==0);
        uint32_t token=bridge_handle_token(file),mounted=xbox_FileHandleDevice(file);
        memset(XBOX_TO_NATIVE(0x28000),0,44);memset(XBOX_TO_NATIVE(0x28100),0xcc,29);
        MEM16(0x28000)=44;MEM8(0x28008)=1;MEM32(0x2800c)=28;MEM32(0x28014)=0x28100;
        MEM8(0x2801c)=0x5a;MEM8(0x2801e)=0x3e;MEM16(0x28023)=0x1c00;
        MEM32(g_esp)=token;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0;
        MEM32(g_esp+16)=0x29200;MEM32(g_esp+20)=0x4d014;MEM32(g_esp+24)=0x28000;
        MEM32(g_esp+28)=44;MEM32(g_esp+32)=0;MEM32(g_esp+36)=0;
        fixture_import(196);assert(!g_eax && !MEM32(0x29200) && MEM32(0x29204)==44);
        assert(MEM8(0x2810a)==1 && MEM8(0x2810b)==1 && MEM8(0x2810c)==1 && MEM8(0x2811c)==0xcc);
        memset(XBOX_TO_NATIVE(0x28100),0xcc,28);MEM8(0x2801e)=0x3f;MEM32(0x29204)=0xabcdef;
        fixture_import(196);assert(g_eax==(uint32_t)STATUS_INVALID_DEVICE_REQUEST && MEM8(0x28100)==0xcc);
        MEM8(0x2801e)=0x3e;MEM32(g_esp+20)=0xdeadbeef;
        fixture_import(196);assert(g_eax==(uint32_t)STATUS_INVALID_DEVICE_REQUEST && MEM32(0x29204)==0xabcdef);
        object_close(token);assert(!MEM32(mounted+4));
        MEM32(g_esp)=token;fixture_import(196);assert(g_eax==(uint32_t)STATUS_INVALID_HANDLE);
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"file_device_ownership")) {
        guest_cpu_join();g_esp=0x80000;
        XBOX_ANSI_STRING name={2,2,"D:"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};
        XBOX_IO_STATUS_BLOCK ios;HANDLE volume,handle,alias,second;
        uint32_t physical=xbox_LookupSymbolicLinkTarget("\\??\\D:");
        uint32_t references=ke_object_slot(physical)->references;
        assert(xbox_NtOpenFile(&volume,0,&oa,&ios,7,0)==0);
        uint32_t mounted=xbox_FileHandleDevice(volume);
        assert(mounted && mounted!=physical && MEM32(physical+0xc)==mounted);
        assert(MEM8(mounted+0x1c)==3 && MEM32(MEM32(mounted+0x18))==physical);
        assert(ke_object_slot(physical)->references==references+1 && MEM32(mounted+4)==1);
        assert(xbox_kernel_file_device_type(volume)==2);
        name=(XBOX_ANSI_STRING){28,28,"\\Device\\Harddisk0\\Partition1"};
        name.Length=(USHORT)strlen(name.Buffer);HANDLE directory;
        assert(xbox_NtOpenFile(&directory,0,&oa,&ios,7,0)==0);
        uint32_t disk=xbox_FileHandleDevice(directory);assert(disk!=mounted && MEM32(disk+4)==1);
        name=(XBOX_ANSI_STRING){12,12,"D:\\asset.bin"};oa.RootDirectory=directory;
        assert(xbox_NtOpenFile(&second,XBOX_GENERIC_READ,&oa,&ios,7,0)==0);
        assert(xbox_FileHandleDevice(second)==mounted && MEM32(mounted+4)==2 && MEM32(disk+4)==1);
        assert(xbox_NtClose(second)==0 && xbox_NtClose(directory)==0 && MEM32(mounted+4)==1 && !MEM32(disk+4));
        name=(XBOX_ANSI_STRING){9,9,"asset.bin"};oa.RootDirectory=volume;
        assert(xbox_NtOpenFile(&handle,XBOX_GENERIC_READ,&oa,&ios,7,0)==0);
        assert(xbox_FileHandleDevice(handle)==mounted && MEM32(mounted+4)==2);
        assert(xbox_NtDuplicateObject(handle,&alias,0)==0 && MEM32(mounted+4)==2);
        assert(xbox_NtClose(handle)==0 && MEM32(mounted+4)==2);
        assert(xbox_NtOpenFile(&second,XBOX_GENERIC_READ,&oa,&ios,7,0)==0 && MEM32(mounted+4)==3);
        assert(xbox_NtClose(second)==0 && MEM32(mounted+4)==2);
        name=(XBOX_ANSI_STRING){7,7,"missing"};
        assert(xbox_NtOpenFile(&second,XBOX_GENERIC_READ,&oa,&ios,7,0)<0 && MEM32(mounted+4)==2);
        uint32_t token=bridge_handle_token(alias),object=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
        assert(MEM32(object+4)==mounted && MEM32(mounted+4)==2);
        assert(xbox_NtClose(volume)==0 && MEM32(mounted+4)==1);
        object_roundtrip();XboxFileOpenState state;
        assert(xbox_CaptureFileOpenState(bridge_resolve_handle(token),&state) && state.device==mounted);
        assert(MEM32(mounted+4)==1 && ke_object_slot(physical)->references==references+1);
        object_close(token);assert(MEM32(mounted+4)==1 && ke_object_slot(object)->references==1);
        xbox_IoDeleteDevice(XBOX_TO_NATIVE(mounted));
        assert(MEM8(mounted+0x1f)==1 && MEM32(mounted+4)==1 && ke_object_slot(mounted));
        object_roundtrip();object_dereference(object);
        assert(!ke_object_slot(object) && !ke_object_slot(mounted));
        assert(!MEM32(physical+0xc) && ke_object_slot(physical)->references==references);
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"native_raw_volume_controls")) {
        guest_cpu_join();g_esp=0x80000;
        XBOX_ANSI_STRING name={28,28,"\\Device\\Harddisk0\\Partition1"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};XBOX_IO_STATUS_BLOCK opened;
        HANDLE handle=NULL;assert(!xbox_NtCreateFile(&handle,0,&oa,&opened,NULL,0,7,1,0x20));
        for(unsigned kind=0;kind<2;kind++) {
            memset(XBOX_TO_NATIVE(0x29000),0xcd,64);MEM32(0x29100)=0xabcdef;MEM32(0x29104)=0xabcdef;
            NTSTATUS status=xbox_NtDeviceIoControlFile(handle,NULL,NULL,NULL,XBOX_TO_NATIVE(0x29100),kind?0x74004:0x70000,NULL,0,XBOX_TO_NATIVE(0x29000),kind?32:24);
            assert(status==STATUS_INVALID_DEVICE_REQUEST && MEM32(0x29100)==0xabcdef && MEM32(0x29104)==0xabcdef);
            for(unsigned i=0;i<64;i++)assert(MEM8(0x29000+i)==0xcd);
        }
        const uint32_t offsets[]={0x10,0xfff,0x1000,0,0xffffffffu};
        const uint32_t lengths[]={0x40,1,0,0x1001,2};
        for(unsigned write=0;write<2;write++)for(unsigned i=0;i<5;i++) {
            MEM32(0x29200)=offsets[i];MEM32(0x29204)=lengths[i];MEM32(0x29208)=0x29000;
            NTSTATUS status=xbox_NtFsControlFile(handle,NULL,NULL,NULL,XBOX_TO_NATIVE(0x29100),write?0x98120:0x9411c,XBOX_TO_NATIVE(0x29200),12,NULL,0);
            assert(status==(i<2?STATUS_INVALID_DEVICE_REQUEST:STATUS_INVALID_PARAMETER));
            assert(MEM32(0x29100)==0xabcdef && MEM32(0x29104)==0xabcdef);
            for(unsigned j=0;j<64;j++)assert(MEM8(0x29000+j)==0xcd);
        }
        assert(xbox_NtFsControlFile(handle,NULL,NULL,NULL,XBOX_TO_NATIVE(0x29100),0x9411c,XBOX_TO_NATIVE(0x29200),11,NULL,0)==STATUS_INVALID_PARAMETER);
        assert(!xbox_NtClose(handle));guest_cpu_part();cleanup();return 0;
    }

    if(!strncmp(argv[1],"generic_create_",15)) {
        guest_cpu_join();g_esp=0x80000;create_pending=strstr(argv[1],"pending")!=NULL;create_error=strstr(argv[1],"error")!=NULL;
        XBOX_ANSI_STRING device_name={19,19,"\\Device\\CreateProbe"};PVOID body=NULL;
        assert(!xbox_IoCreateDevice(XBOX_TO_NATIVE(0x26000),0,&device_name,0x15,FALSE,&body));
        create_device=io_guest_address(body);MEM32(create_device+0x14)&=~0x10u;
        MEM32(0x26000+12)=MEM32(0x26000+12+1*4)=MEM32(0x26000+12+13*4)=0x17900;
        HANDLE handle=NULL;XBOX_IO_STATUS_BLOCK ios={0};
        XBOX_ANSI_STRING name={25,25,"\\Device\\CreateProbe\\child"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0x40};
        NTSTATUS status;
        if(strstr(argv[1],"import")) {
            create_expected_allocation=0x1234567800000020ull;
            MEM32(0x29000)=0;MEM32(0x29004)=0x29020;MEM32(0x29008)=0x40;
            MEM16(0x29020)=MEM16(0x29022)=name.Length;MEM32(0x29024)=0x29040;
            memcpy(XBOX_TO_NATIVE(0x29040),name.Buffer,name.Length);memcpy(XBOX_TO_NATIVE(0x29040+name.Length),"junk",4);
            *(uint64_t *)XBOX_TO_NATIVE(0x20090)=create_expected_allocation;
            const uint32_t args[]={0x20070,XBOX_GENERIC_READ|XBOX_SYNCHRONIZE,0x29000,0x20080,0x20090,0,7,1,0x20};
            for(unsigned i=0;i<9;i++)MEM32(g_esp+i*4)=args[i];fixture_import(190);
            status=(NTSTATUS)g_eax;handle=bridge_read_handle(0x20070);ios.Status=(NTSTATUS)MEM32(0x20080);ios.Information=MEM32(0x20084);
        } else status=xbox_NtCreateFile(&handle,XBOX_GENERIC_READ|XBOX_SYNCHRONIZE,&oa,&ios,NULL,0,7,1,0x20);
        assert(create_calls==1 && !MEM16(create_irp));
        if(create_error) {
            assert(status==(NTSTATUS)0xc0000022u && ios.Status==status && !ios.Information && !handle);
            assert(!MEM32(create_device+4) && !create_cleanup && !create_close);
        } else {
            assert(!status && !ios.Status && ios.Information==2 && handle);
            uint32_t token=bridge_handle_token(handle),file=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
            assert(file==create_file && MEM32(file+8)==0xfeed && MEM32(create_device+4)==1);
            if(strstr(argv[1],"relative")) {
                HANDLE child=NULL;XBOX_ANSI_STRING child_name={3,3,"kid"};XBOX_OBJECT_ATTRIBUTES child_oa={handle,&child_name,0};
                create_name="kid";XBOX_IO_STATUS_BLOCK child_ios;
                assert(!xbox_NtCreateFile(&child,0,&child_oa,&child_ios,NULL,0,7,1,0x20));
                uint32_t child_file=ke_object_for_handle(child);assert(child_file && MEM32(child_file+0x1c)==file && MEM32(create_device+4)==2);
                assert(!xbox_NtClose(child));assert(MEM32(create_device+4)==1 && create_cleanup==1 && create_close==1);
                create_cleanup=create_close=0;create_name="\\child";
            }
            if(strstr(argv[1],"exclusive")) {
                MEM32(create_device+0x14)|=2;HANDLE rejected=NULL;XBOX_IO_STATUS_BLOCK denied;
                assert(xbox_NtCreateFile(&rejected,0,&oa,&denied,NULL,0,7,1,0x20)==STATUS_ACCESS_DENIED);
                assert(!rejected && MEM32(create_device+4)==1 && create_calls==1);MEM32(create_device+0x14)&=~2u;
            }
            assert(xbox_NtSetEvent(handle,NULL)==(NTSTATUS)0xc0000024u);
            PVOID wrong=(PVOID)1;assert(xbox_ObReferenceObjectByHandle(handle,xbox_ExEventObjectType,&wrong)==(NTSTATUS)0xc0000024u && !wrong);
            HANDLE alias=NULL;assert(!xbox_NtDuplicateObject(handle,&alias,0));uint32_t second=bridge_handle_token(alias);
            object_roundtrip();assert(MEM32(file+8)==0xfeed && ke_object_slot(file)->in_use==4);
            object_close(token);assert(!create_cleanup && !create_close);
            object_close(second);assert(create_cleanup==1 && !create_close);
            MEM32(g_esp)=file;MEM32(g_esp+4)=XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE;MEM32(g_esp+8)=0x29200;
            fixture_import(244);assert(!g_eax);object_close(MEM32(0x29200));
            assert(create_cleanup==2 && !create_close);
            object_dereference(file);assert(create_close==1 && !ke_object_slot(file) && !MEM32(create_device+4));
        }
        xbox_IoDeleteDevice(body);guest_cpu_part();cleanup();return 0;
    }

    if(!strncmp(argv[1],"file_lifecycle_",15)) {
        guest_cpu_join();g_esp=0x80000;
        int sync=argv[1][15]=='1';file_lifecycle_pending=argv[1][16]=='1';
        XBOX_ANSI_STRING name={2,2,"D:"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};XBOX_IO_STATUS_BLOCK ios;
        HANDLE handle,alias,owned;assert(!xbox_NtOpenFile(&handle,0,&oa,&ios,7,sync?0x20:0));
        assert(!xbox_NtDuplicateObject(handle,&alias,0));
        uint32_t token=bridge_handle_token(handle),other=bridge_handle_token(alias),wait_token=0;
        lifecycle_file=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
        uint32_t device=MEM32(lifecycle_file+4);MEM32(device+8)=0x26000;
        MEM32(0x26000+12+1*4)=MEM32(0x26000+12+13*4)=0x17800;
        assert(!xbox_kernel_wait_acquire(1,&handle,&owned,&wait_token));
        uint32_t port=named_create(191,0xfffffffcu,"FileCompletion",0x80,1,0);assert(port);
        uint32_t queue=object_reference(port,XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE);
        uint32_t completion=xbox_HeapAlloc(8,4);assert(completion);
        MEM32(completion)=queue;MEM32(completion+4)=42;MEM32(lifecycle_file+0x20)=completion;object_close(port);
        object_close(token);assert(!file_cleanup_calls && !file_close_calls);
        object_close(other);assert(file_cleanup_calls==1 && !file_close_calls);
        assert(ke_object_slot(lifecycle_file) && !MEM16(lifecycle_irp));
        assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        if(sync)assert(MEM32(lifecycle_file+0x24)==UINT32_MAX);
        object_roundtrip();assert(MEM8(lifecycle_file+3)&0x20);
        MEM32(g_esp)=lifecycle_file;MEM32(g_esp+4)=XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE;MEM32(g_esp+8)=0x29200;
        fixture_import(244);assert(!g_eax && MEM32(0x29200));
        object_close(MEM32(0x29200));assert(file_cleanup_calls==2 && !file_close_calls);
        xbox_kernel_wait_release_objects(1,&wait_token);assert(file_cleanup_calls==2 && !file_close_calls);
        object_dereference(lifecycle_file);assert(file_cleanup_calls==2 && file_close_calls==1);
        assert(!ke_object_slot(lifecycle_file) && !ke_object_slot(queue) && !xbox_HeapBlockSize(completion));
        assert(!MEM32(device+4) && !MEM16(lifecycle_irp));
        assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        MEM32(device+8)=0;guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"file_open_options")) {
        guest_cpu_join();g_esp=0x80000;
        const uint32_t options[]={0,0x10,0x20,0x24,0x28,0x820};
        const uint8_t flags[]={0,3,1,9,5,0x41};
        XBOX_ANSI_STRING name={12,12,"D:\\asset.bin"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};
        for(unsigned i=0;i<6;i++) {
            HANDLE handle,alias;XBOX_IO_STATUS_BLOCK ios;
            assert(xbox_NtOpenFile(&handle,XBOX_GENERIC_READ|XBOX_SYNCHRONIZE,&oa,&ios,7,options[i])==0);
            assert(xbox_NtDuplicateObject(handle,&alias,0)==0);assert(xbox_NtClose(handle)==0);
            uint32_t token=bridge_handle_token(alias),object=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE);
            assert(MEM8(object+3)==flags[i]);
            assert(MEM32(object+0x24)==(options[i]&0x30?UINT32_MAX:0));
            HANDLE lock=ke_shadow_lookup(object+0x28);assert(!!lock==!!(options[i]&0x30));
            if(lock)assert(MEM8(object+0x28)==1 && MEM32(object+0x2c)==0 && WaitForSingleObject(lock,0)==WAIT_TIMEOUT);
            assert(MEM8(object+0x38)==0 && MEM32(object+0x3c)==0);
            object_roundtrip();XboxFileOpenState state;
            assert(xbox_CaptureFileOpenState(bridge_resolve_handle(token),&state) && state.options==options[i]);
            assert(MEM8(object+3)==flags[i] && !!ke_shadow_lookup(object+0x28)==!!(options[i]&0x30));
            object_close(token);object_dereference(object);assert(!ke_object_slot(object));
        }
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"device_paths")) {
        guest_cpu_join();g_esp=0x80000;
        XBOX_ANSI_STRING cd={14,14,"\\Device\\CdRom0"};PVOID native=NULL;
        assert(xbox_ObReferenceObjectByName(&cd,0x40,xbox_IoDeviceObjectType,NULL,&native)==0);
        uint32_t device=io_guest_address(native);assert(device==xbox_LookupSymbolicLinkTarget("\\??\\D:"));
        assert(ke_object_slot(device)->path_rule && !(MEM32(device+0x14)&0x10));
        xbox_ObfDereferenceObject(native);
        char media[MAX_PATH],user[MAX_PATH];sprintf_s(media,MAX_PATH,"%s\\MediaRoot",argv[6]);
        sprintf_s(user,MAX_PATH,"%s\\user",argv[6]);assert(xbox_path_set_user_directory(user));
        xbox_path_set_language_media("English",media);
        XBOX_ANSI_STRING q={6,6,"\\??\\Q:"},r={6,6,"\\??\\R:"},d={6,6,"\\??\\D:"};
        assert(xbox_IoCreateSymbolicLink(&q,&cd)==0 && xbox_IoCreateSymbolicLink(&r,&q)==0);
        assert(xbox_LookupSymbolicLinkTarget(r.Buffer)==device);
        WCHAR host[MAX_PATH],expected[MAX_PATH];
        assert(xbox_translate_path("\\Device\\CdRom0",host,MAX_PATH));
        swprintf_s(expected,MAX_PATH,L"%hs",argv[6]);assert(!_wcsicmp(host,expected));
        assert(xbox_translate_path("\\Device\\Harddisk0\\Partition1",host,MAX_PATH));
        swprintf_s(expected,MAX_PATH,L"%hs",user);assert(!_wcsicmp(host,expected));
        assert(xbox_translate_path("\\Device\\Harddisk0\\Partition2",host,MAX_PATH));
        swprintf_s(expected,MAX_PATH,L"%hs\\SystemData",argv[6]);assert(!_wcsicmp(host,expected));
        g_xbox_path_hook=path_notification;
        assert(xbox_translate_path("R:\\Media\\asset.bin",host,MAX_PATH));g_xbox_path_hook=NULL;
        assert(path_notifications==1 && !wcscmp(xbox_LastHostPath(),host));
        swprintf_s(expected,MAX_PATH,L"%hs\\Media\\asset.bin",media);assert(!_wcsicmp(host,expected));
        XBOX_ANSI_STRING path={18,18,"R:\\Media\\asset.bin"};path.Length=(USHORT)strlen(path.Buffer);
        XBOX_OBJECT_ATTRIBUTES oa={NULL,&path,0};XBOX_IO_STATUS_BLOCK ios;HANDLE handle;
        assert(xbox_NtOpenFile(&handle,XBOX_GENERIC_READ,&oa,&ios,7,0)==0);
        char byte;DWORD got;assert(ReadFile(handle,&byte,1,&got,NULL) && got==1 && byte=='e');assert(xbox_NtClose(handle)==0);
        assert(xbox_IoDeleteSymbolicLink(&d)==0 && !xbox_translate_path("D:\\asset.bin",host,MAX_PATH));
        XBOX_ANSI_STRING partition={28,28,"\\Device\\Harddisk0\\Partition1"};partition.Length=(USHORT)strlen(partition.Buffer);
        assert(xbox_IoCreateSymbolicLink(&d,&partition)==0);
        assert(xbox_translate_path("d:\\asset.bin",host,MAX_PATH));
        swprintf_s(expected,MAX_PATH,L"%hs\\asset.bin",user);assert(!_wcsicmp(host,expected));
        xbox_IoDeleteDevice(native);assert(!ke_object_slot(device)->name_address);
        object_roundtrip();assert(xbox_LookupSymbolicLinkTarget(q.Buffer)==device && ke_object_slot(device)->path_rule);
        assert(xbox_translate_path("Q:\\Media\\asset.bin",host,MAX_PATH));
        swprintf_s(expected,MAX_PATH,L"%hs\\Media\\asset.bin",media);assert(!_wcsicmp(host,expected));
        path=(XBOX_ANSI_STRING){15,15,"\\Device\\CdRom01"};
        assert(xbox_NtOpenFile(&handle,0,&oa,&ios,7,XBOX_FILE_DIRECTORY_FILE)<0);
        uint32_t mounted=MEM32(device+0xc);assert(mounted && !MEM32(mounted+4));
        assert(xbox_IoDeleteSymbolicLink(&q)==0 && xbox_IoDeleteSymbolicLink(&r)==0);
        assert(ke_object_slot(device)->references==1); /* The mounted filesystem owns the physical device. */
        xbox_IoDeleteDevice(XBOX_TO_NATIVE(mounted));
        assert(!ke_object_slot(mounted) && !ke_object_slot(device));
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"device_object_contract")) {
        guest_cpu_join();g_esp=0x80000;memset(XBOX_TO_NATIVE(0x26000),0,64);
        XBOX_ANSI_STRING name={15,15,"\\Device\\fixture"};PVOID native=NULL,same=NULL;
        assert(xbox_IoCreateDevice(XBOX_TO_NATIVE(0x26000),0x171,&name,7,FALSE,&native)==0);
        uint32_t device=io_guest_address(native);
        assert(device && MEM16(device)==3 && MEM16(device+2)==0x1b9 && xbox_HeapBlockSize(device)>=0x1c0);
        assert(ke_object_slot(device)->references==1 && ke_object_slot(device)->object_type==XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE);
        assert(xbox_ObReferenceObjectByName(&name,0x40,xbox_IoDeviceObjectType,NULL,&same)==0 && same==native);
        uint32_t token=named_open(243,0,"\\Device\\fixture",XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE);assert(!g_eax);
        assert(object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_IO_DEVICE_TYPE)==device);object_dereference(device);
        XBOX_ANSI_STRING alias={6,6,"\\??\\Q:"};assert(xbox_IoCreateSymbolicLink(&alias,&name)==0);
        object_roundtrip();assert(ke_object_slot(device)->references==3);
        MEM32(g_esp)=device;fixture_import(68);
        assert(MEM8(device+0x1f)==1 && ke_object_slot(device)->references==2 && ke_object_slot(device)->name_address);
        object_close(token);assert(!ke_object_slot(device)->name_address && ke_shadow_lookup(device+0x34));
        object_dereference(device);assert(ke_object_slot(device)->references==1);
        assert(xbox_IoDeleteSymbolicLink(&alias)==0 && !ke_object_slot(device) && !ke_shadow_lookup(device+0x34));
        assert(xbox_IoCreateDevice(XBOX_TO_NATIVE(0x26000),0,&name,0x22,FALSE,&native)==0);
        device=io_guest_address(native);MEM32(device+4)=1;
        xbox_IoDeleteDevice(native);assert(MEM8(device+0x1f)==1 && !ke_object_slot(device)->name_address && ke_object_slot(device)->references==1);
        object_roundtrip();assert(MEM32(device+4)==1 && MEM8(device+0x1f)==1);
        MEM32(device+4)=0;io_delete_device(device,0);assert(!ke_object_slot(device));
        MEM32(0x26004)=0x17600;
        assert(xbox_IoCreateDevice(XBOX_TO_NATIVE(0x26000),0,NULL,0x22,FALSE,&native)==0);
        device=io_guest_address(native);xbox_IoDeleteDevice(native);
        assert(device_delete_calls==1 && !ke_object_slot(device));
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"save_directory_alias")) {
        guest_cpu_join();g_esp=0x80000;
        assert(xbox_path_set_user_directory(argv[6]));
        const char *directories[]={"\\Device\\Harddisk0\\Partition1\\UDATA",
            "\\Device\\Harddisk0\\Partition1\\UDATA\\41540002",
            "\\Device\\Harddisk0\\Partition1\\UDATA\\41540002\\5498112384B2"};
        XBOX_IO_STATUS_BLOCK ios;HANDLE handle;
        XBOX_ANSI_STRING name={0};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0x40};
        for(unsigned i=0;i<3;i++) {
            name.Buffer=(PCHAR)directories[i];name.Length=(USHORT)strlen(name.Buffer);name.MaximumLength=name.Length+1;
            assert(!xbox_NtCreateFile(&handle,0x100001,&oa,&ios,NULL,0x80,3,XBOX_FILE_OPEN_IF,0x4021));
            assert(!xbox_NtClose(handle));
        }
        XBOX_ANSI_STRING drive={6,6,"\\??\\X:"},target={(USHORT)strlen(directories[1]),(USHORT)strlen(directories[1]),(PCHAR)directories[1]};
        assert(!xbox_IoCreateSymbolicLink(&drive,&target));
        const char *file="X:\\5498112384B2\\\\megaten9.sav";
        name.Buffer=(PCHAR)file;name.Length=(USHORT)strlen(file);name.MaximumLength=name.Length+1;
        NTSTATUS opened=xbox_NtCreateFile(&handle,0xC0100000,&oa,&ios,NULL,0x80,0,XBOX_FILE_OPEN_IF,0x60);
        assert(!opened);
        const char payload[]="original save alias";LARGE_INTEGER offset={0};
        assert(!xbox_NtWriteFile(handle,NULL,NULL,NULL,&ios,(PVOID)payload,sizeof payload,&offset));
        assert(ios.Information==sizeof payload && !xbox_NtClose(handle));
        name.Buffer="X:\\5498112384B2";name.Length=(USHORT)strlen(name.Buffer);name.MaximumLength=name.Length+1;
        HANDLE directory;assert(!xbox_NtOpenFile(&directory,0x100001,&oa,&ios,3,0x21));
        name.Buffer="megaten9.sav";name.Length=(USHORT)strlen(name.Buffer);name.MaximumLength=name.Length+1;
        oa.RootDirectory=directory;
        assert(!xbox_NtOpenFile(&handle,0x80100000,&oa,&ios,0,0x60));
        char actual[sizeof payload];
        assert(!xbox_NtReadFile(handle,NULL,NULL,NULL,&ios,actual,sizeof actual,&offset));
        assert(ios.Information==sizeof payload && !memcmp(actual,payload,sizeof payload));
        assert(!xbox_NtClose(handle) && !xbox_NtClose(directory));
        assert(!xbox_IoDeleteSymbolicLink(&drive));
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"symbolic_link_contract")) {
        guest_cpu_join();g_esp=0x80000;
        const char *text="\\Device\\Harddisk0\\Partition1\\checkpoint";
        char counted_target[160];sprintf_s(counted_target,sizeof counted_target,"%s\\ignored-tail",text);
        XBOX_ANSI_STRING link={6,6,"\\??\\U:"},target={(USHORT)strlen(text),(USHORT)strlen(text),counted_target};
        assert(xbox_IoCreateSymbolicLink(&link,&target)==0);
        named_attributes(0,"\\??\\u:",0);MEM32(g_esp)=0x29200;MEM32(g_esp+4)=0x29000;
        fixture_import(203);assert(!g_eax);uint32_t token=MEM32(0x29200);
        uint32_t object=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_SYM_LINK_TYPE);
        for(unsigned maximum=0;maximum<=target.Length+1;maximum++) {
            memset(XBOX_TO_NATIVE(0x29300),0xcc,256);MEM16(0x29220)=0xdddd;MEM16(0x29222)=(uint16_t)maximum;
            MEM32(0x29224)=0x29300;MEM32(0x29228)=0xdeadbeef;
            MEM32(g_esp)=token;MEM32(g_esp+4)=0x29220;MEM32(g_esp+8)=0x29228;fixture_import(215);
            unsigned copied=maximum<target.Length?maximum:target.Length;
            assert(g_eax==(maximum<target.Length?0xc0000023u:0));
            assert(MEM16(0x29220)==copied && MEM32(0x29228)==target.Length);
            assert(!memcmp(XBOX_TO_NATIVE(0x29300),text,copied) && MEM8(0x29300+copied)==0xcc);
        }
        uint32_t directory=MEM32(object);assert(directory && ke_object_slot(directory)->references==1);
        object_roundtrip();assert(ke_object_for_handle(bridge_resolve_handle(token))==object);
        assert(MEM32(object)==directory && ke_object_slot(directory)->references==1);
        assert(!strcmp(xbox_LookupSymbolicLink("\\??\\u:"),text));
        char original[MAX_PATH],renamed[MAX_PATH],replacement[MAX_PATH];
        sprintf_s(original,MAX_PATH,"%s\\checkpoint",argv[6]);
        sprintf_s(renamed,MAX_PATH,"%s\\renamed",argv[6]);
        assert(MoveFileA(original,renamed) && CreateDirectoryA(original,NULL));
        sprintf_s(replacement,MAX_PATH,"%s\\asset.bin",original);
        FILE *new_file=fopen(replacement,"wb");assert(new_file && fputc('b',new_file)=='b' && fclose(new_file)==0);
        XBOX_ANSI_STRING file_name={12,12,"U:\\asset.bin"};XBOX_OBJECT_ATTRIBUTES file_attrs={NULL,&file_name,0x40};
        HANDLE file_handle=NULL;XBOX_IO_STATUS_BLOCK file_ios;char byte=0;DWORD read=0;
        assert(xbox_NtOpenFile(&file_handle,XBOX_GENERIC_READ,&file_attrs,&file_ios,7,0)==0);
        assert(ReadFile(file_handle,&byte,1,&read,NULL) && read==1 && byte=='a');assert(xbox_NtClose(file_handle)==0);
        assert(xbox_IoDeleteSymbolicLink(&link)==0);
        assert(xbox_LookupSymbolicLink("\\??\\U:")); /* An open handle retains a temporary name. */
        object_close(token);assert(!xbox_LookupSymbolicLink("\\??\\U:") && ke_shadow_lookup(object));
        object_dereference(object);assert(!ke_shadow_lookup(object) && !ke_shadow_lookup(directory));
        named_attributes(0,"\\??\\U:",0);MEM32(g_esp)=0x29200;MEM32(g_esp+4)=0x29000;
        fixture_import(203);assert(g_eax==0xc0000034u && !MEM32(0x29200));
        guest_cpu_part();cleanup();return 0;
    }


    if(!strcmp(argv[1],"symbolic_object_targets")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t directory=named_create(188,0xfffffffc,"Original",0x80,0,0);assert(!g_eax);
        uint32_t directory_body=ke_object_for_handle(bridge_resolve_handle(directory));
        uint32_t event=named_create(189,directory,"Signal",0x80,0,1);assert(!g_eax);
        uint32_t event_body=ke_object_for_handle(bridge_resolve_handle(event));
        XBOX_ANSI_STRING alias={24,24,"\\Win32NamedObjects\\Alias"};
        XBOX_ANSI_STRING target={27,27,"\\Win32NamedObjects\\Original"};
        assert(xbox_IoCreateSymbolicLink(&alias,&target)==0);
        XBOX_ANSI_STRING second={25,25,"\\Win32NamedObjects\\Second"};
        assert(xbox_IoCreateSymbolicLink(&second,&alias)==0);
        XBOX_ANSI_STRING event_alias={29,29,"\\Win32NamedObjects\\EventAlias"};
        XBOX_ANSI_STRING event_target={31,31,"\\Win32NamedObjects\\Alias\\Signal"};
        assert(xbox_IoCreateSymbolicLink(&event_alias,&event_target)==0);
        uint32_t opened=named_open(243,0,"\\Win32NamedObjects\\EventAlias",XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE);
        assert(!g_eax && ke_object_for_handle(bridge_resolve_handle(opened))==event_body);object_close(opened);
        assert(xbox_IoDeleteSymbolicLink(&alias)==0);object_close(directory);
        assert(!ke_object_slot(directory_body)->name_address);
        object_roundtrip();
        opened=named_open(243,0,"\\Win32NamedObjects\\Second\\Signal",XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE);
        assert(!g_eax && ke_object_for_handle(bridge_resolve_handle(opened))==event_body);object_close(opened);
        object_close(event);assert(!ke_object_slot(event_body)->name_address && ke_object_slot(event_body)->references==1);
        assert(xbox_IoDeleteSymbolicLink(&event_alias)==0 && !ke_object_slot(event_body));
        assert(xbox_IoDeleteSymbolicLink(&second)==0 && !ke_object_slot(directory_body));
        XBOX_ANSI_STRING missing={24,24,"\\Win32NamedObjects\\Ghost"};
        assert(xbox_IoCreateSymbolicLink(&alias,&missing)==STATUS_INVALID_PARAMETER);
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"symbolic_full_ansi_target")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t event=named_create(189,0xfffffffc,"Go",0x80,0,1);assert(!g_eax);
        uint32_t body=ke_object_for_handle(bridge_resolve_handle(event));
        XBOX_ANSI_STRING root={18,18,"\\Win32NamedObjects"},again={24,24,"\\Win32NamedObjects\\Again"};
        assert(xbox_IoCreateSymbolicLink(&again,&root)==0);
        char *text=malloc(65536);assert(text);memcpy(text,root.Buffer,18);
        unsigned used=18;while(used+6+3<=65535){memcpy(text+used,"\\Again",6);used+=6;}
        memcpy(text+used,"\\Go",3);used+=3;text[used]=0;assert(used==65535);
        XBOX_ANSI_STRING name={6,6,"\\??\\L:"},target={65535,65535,text};
        assert(xbox_IoCreateSymbolicLink(&name,&target)==0);
        uint32_t opened=named_open(243,0,"\\??\\L:",XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE);
        assert(!g_eax && ke_object_for_handle(bridge_resolve_handle(opened))==body);object_close(opened);
        XBOX_OBJECT_ATTRIBUTES attributes={NULL,&name,0x40};HANDLE link;
        assert(xbox_NtOpenSymbolicLinkObject(&link,&attributes)==0);
        bridge_write_handle(0x29200,link);uint32_t token=MEM32(0x29200);
        object_roundtrip();
        for(unsigned maximum=65534;maximum<=65535;maximum++) {
            memset(XBOX_TO_NATIVE(0x30000),0xcc,65536);
            MEM16(0x29220)=0;MEM16(0x29222)=(uint16_t)maximum;MEM32(0x29224)=0x30000;MEM32(0x29228)=0;
            MEM32(g_esp)=token;MEM32(g_esp+4)=0x29220;MEM32(g_esp+8)=0x29228;fixture_import(215);
            assert(g_eax==(maximum==65535?0:0xc0000023u) && MEM16(0x29220)==maximum && MEM32(0x29228)==65535);
            assert(!memcmp(XBOX_TO_NATIVE(0x30000),text,maximum) && MEM8(0x30000+maximum)==0xcc);
        }
        assert(xbox_IoDeleteSymbolicLink(&name)==0);object_close(token);object_close(event);
        assert(xbox_IoDeleteSymbolicLink(&again)==0 && !ke_object_slot(body));
        free(text);guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"named_cold_save") || !strcmp(argv[1],"named_cold_load")) {
        guest_cpu_join();g_esp=0x80000;
        if(strstr(argv[1],"save")) {
            MEM32(0x29200)=named_create(188,0xfffffffcu,"SavedDirectory",0x80,0,0);assert(!g_eax);
            uint32_t token=named_create(189,MEM32(0x29200),"SavedEvent",0x80,0,1);assert(!g_eax);
            MEM32(0x29204)=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE);
            HANDLE event=bridge_resolve_handle(token),owned;uint32_t wait_token;
            assert(xbox_kernel_wait_acquire(1,&event,&owned,&wait_token)==0);MEM32(0x29208)=wait_token;
            object_close(token);
            MEM32(0x2920c)=named_create(189,MEM32(0x29200),"SavedEvent",0x80,1,0);assert(!g_eax);
            guest_cpu_part();assert(xbox_CheckpointBootstrapBegin());
            FILE *memory=fopen(argv[3],"wb"),*objects=fopen(argv[5],"wb");
            assert(memory && objects && xbox_SavePhysicalMemory(memory) && xbox_CheckpointWriteObjects(objects));
            fclose(memory);fclose(objects);xbox_CheckpointEnd();return 0;
        }
        guest_cpu_part();assert(xbox_CheckpointBootstrapBegin());
        FILE *memory=fopen(argv[3],"rb"),*objects=fopen(argv[5],"rb");
        assert(memory && objects && checkpoint_clear_objects() && xbox_LoadPhysicalMemory(memory) && xbox_CheckpointLoadObjects(objects,NULL,0));
        fclose(memory);fclose(objects);xbox_CheckpointEnd();guest_cpu_join();g_esp=0x80000;
        uint32_t old=MEM32(0x29204),wait_token=MEM32(0x29208);
        assert(s_handle_private[wait_token&BRIDGE_HANDLE_MASK] && ke_object_for_handle(bridge_resolve_handle(wait_token))==old);
        assert(!ke_object_slot(old)->name_address && ke_native_event_state(bridge_resolve_handle(wait_token)));
        uint32_t current=named_open(243,0,"\\Win32NamedObjects\\SavedDirectory\\SavedEvent",0);assert(!g_eax);
        assert(ke_object_for_handle(bridge_resolve_handle(current))!=old && !ke_native_event_state(bridge_resolve_handle(current)));
        assert(checkpoint_native.compare(bridge_resolve_handle(current),bridge_resolve_handle(MEM32(0x2920c)))==0);
        object_close(current);object_close(MEM32(0x2920c));object_close(MEM32(0x29200));
        xbox_kernel_wait_release_objects(1,&wait_token);object_dereference(old);assert(!ke_object_slot(old));
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"named_directory")) {
        guest_cpu_join();g_esp=0x80000;
        assert(stdcall_args_for_ordinal(201)==8 && stdcall_args_for_ordinal(244)==12);
        uint32_t directory=named_create(188,0,"\\Win32NamedObjects\\Session",0x80,0,0);assert(!g_eax && directory);
        uint32_t body=object_reference(directory,XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE);
        assert(xbox_HeapBlockSize(body)>=0x2c);
        uint32_t event=named_create(189,directory,"Signal",0x80,0,1);assert(!g_eax && event);
        uint32_t child=ke_object_for_handle(bridge_resolve_handle(event));assert(child && ke_object_slot(body)->references==2);
        object_roundtrip();
        uint32_t alias=named_open(201,0,"\\win32namedobjects\\session",0);assert(!g_eax && alias);
        assert(ke_object_for_handle(bridge_resolve_handle(alias))==body);
        uint32_t opened=named_open(243,alias,"sIGNAL",XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE);assert(!g_eax && opened);
        assert(ke_object_for_handle(bridge_resolve_handle(opened))==child);
        assert(!named_create(189,alias,"Signal",0,1,0) && g_eax==0xc0000035u);
        assert(!named_create(193,alias,"Signal",0,1,2) && g_eax==0xc0000035u);
        assert(!named_create(193,alias,"Signal",0x80,1,2) && g_eax==0xc0000024u);
        assert(!named_create(189,0,"relative",0x80,0,0) && g_eax==0xc0000033u);
        assert(!named_open(243,directory,"\\Signal",0) && g_eax==0xc0000033u);
        assert(!named_open(243,0,"\\Missing\\Signal",0) && g_eax==0xc000003au);
        assert(!named_open(243,directory,"Signal\\Child",0) && g_eax==0xc000003au);
        assert(!named_open(201,0,"\\Win32NamedObjects\\Session\\Signal",0) && g_eax==0xc0000024u);
        assert(!named_open(243,BRIDGE_HANDLE_TAG|15000,"Signal",0) && g_eax==0xc0000008u);
        MEM32(g_esp)=body;MEM32(g_esp+4)=XBOX_KERNEL_DATA_BASE+KDATA_OBJ_DIR_TYPE;MEM32(g_esp+8)=0x29190;
        fixture_import(244);assert(!g_eax && MEM32(0x29190));object_close(MEM32(0x29190));
        object_close(alias);object_close(directory);
        assert(!named_open(201,0,"\\Win32NamedObjects\\Session",0) && g_eax==0xc0000034u);
        assert(ke_object_slot(body) && ke_object_slot(body)->references==2);
        object_close(event);object_close(opened);assert(!ke_object_slot(child) && ke_object_slot(body)->references==1);
        object_dereference(body);assert(!ke_object_slot(body));guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"named_permanent") || !strcmp(argv[1],"named_native")) {
        guest_cpu_join();g_esp=0x80000;int native=!strcmp(argv[1],"named_native");
        HANDLE handle=NULL;uint32_t token=0;
        if(native) {
            XBOX_ANSI_STRING name={10,10,"NativeName"};XBOX_OBJECT_ATTRIBUTES oa={(HANDLE)(intptr_t)-4,&name,0x90};
            assert(xbox_NtCreateEvent(&handle,&oa,0,TRUE)==0 && handle);
        } else {token=named_create(189,0xfffffffcu,"NativeName",0x90,0,1);assert(!g_eax);handle=bridge_resolve_handle(token);}
        uint32_t object=0;assert(ke_reference_handle(handle,0,&object)==0);
        if(native)assert(xbox_NtClose(handle)==0);else object_close(token);
        object_roundtrip();token=named_open(243,0xfffffffcu,"NativeName",0);assert(!g_eax && token);
        assert(ke_object_for_handle(bridge_resolve_handle(token))==object && ke_native_event_state(bridge_resolve_handle(token)));
        named_attributes(0,"\\Win32NamedObjects\\NativeName",0);
        MEM32(g_esp)=0x29020;MEM32(g_esp+4)=0;MEM32(g_esp+8)=XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE;
        MEM32(g_esp+12)=0;MEM32(g_esp+16)=0x29190;fixture_import(247);
        assert(!g_eax && MEM32(0x29190)==object);object_dereference(object);
        object_close(token);MEM32(g_esp)=object;fixture_import(242);
        assert(!ke_object_slot(object)->name_address);
        assert(!named_open(243,0xfffffffcu,"NativeName",0) && g_eax==0xc0000034u);
        object_dereference(object);assert(!ke_object_slot(object));guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"named_wait_lifetime")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t token=named_create(189,0xfffffffcu,"WaitName",0x80,0,0);assert(!g_eax);
        HANDLE event=bridge_resolve_handle(token),owned=NULL;uint32_t wait_token=0,object=ke_object_for_handle(event);
        assert(xbox_kernel_wait_acquire(1,&event,&owned,&wait_token)==0 && s_handle_private[wait_token&BRIDGE_HANDLE_MASK]);
        object_roundtrip();assert(s_handle_private[wait_token&BRIDGE_HANDLE_MASK]);
        object_close(token);assert(ke_object_slot(object) && !ke_object_slot(object)->name_address);
        assert(!named_open(243,0xfffffffcu,"WaitName",0) && g_eax==0xc0000034u);
        assert(SetEvent(bridge_resolve_handle(wait_token)) && WaitForSingleObject(bridge_resolve_handle(wait_token),0)==WAIT_OBJECT_0);
        xbox_kernel_wait_release_objects(1,&wait_token);assert(!ke_object_slot(object));guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"named_completion")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t token=named_create(191,0xfffffffcu,"CompletionName",0x80,2,0);assert(!g_eax && token);
        uint32_t queue=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_IO_COMPLETION_TYPE);
        uint32_t alias=named_create(191,0xfffffffcu,"CompletionName",0x80,4,0);assert(g_eax==0x40000000u);
        assert(ke_completion_for_handle(bridge_resolve_handle(alias))==queue && MEM32(queue+0x1c)==2);
        object_roundtrip();object_close(token);object_close(alias);
        assert(ke_object_slot(queue) && !ke_object_slot(queue)->name_address);
        object_dereference(queue);assert(!ke_object_slot(queue));guest_cpu_part();cleanup();return 0;
    }

    if(!strncmp(argv[1],"named_object_",13)) {
        uint32_t ordinal=!strcmp(argv[1]+13,"event")?189:!strcmp(argv[1]+13,"semaphore")?193:
            !strcmp(argv[1]+13,"mutant")?192:194;
        uint32_t type=XBOX_KERNEL_DATA_BASE+(ordinal==189?KDATA_EVENT_OBJ_TYPE:ordinal==193?KDATA_SEMAPHORE_OBJ_TYPE:
            ordinal==192?KDATA_MUTANT_OBJ_TYPE:KDATA_TIMER_OBJ_TYPE);
        g_esp=0x80000;MEM32(0x29000)=0xfffffffcu;MEM32(0x29004)=0x29020;MEM32(0x29008)=0x80;
        memcpy(XBOX_TO_NATIVE(0x29040),"OriginalName",12);MEM16(0x29020)=12;MEM16(0x29022)=12;MEM32(0x29024)=0x29040;
        MEM32(g_esp)=0x29100;MEM32(g_esp+4)=0x29000;
        MEM32(g_esp+8)=ordinal==193?3:ordinal==194?1:0;MEM32(g_esp+12)=ordinal==193?8:1;
        fixture_import(ordinal);assert(!g_eax);uint32_t first=MEM32(0x29100),object=object_reference(first,type);
        memcpy(XBOX_TO_NATIVE(0x29040),"oRIGINALnAME",12);
        MEM32(g_esp)=0x29104;MEM32(g_esp+4)=0x29000;MEM32(g_esp+8)=ordinal==192?1:0;MEM32(g_esp+12)=ordinal==193?1:0;
        fixture_import(ordinal);assert(g_eax==0x40000000u);uint32_t second=MEM32(0x29104);
        assert(checkpoint_native.compare(bridge_resolve_handle(first),bridge_resolve_handle(second))==0);
        if(ordinal==189)assert(ke_native_event_state(bridge_resolve_handle(first))==1);
        if(ordinal==193) {struct {LONG count,maximum;} state;
            assert(checkpoint_native.semaphore(bridge_resolve_handle(first),0,&state,sizeof state,NULL)==0 && state.count==3 && state.maximum==8);}
        if(ordinal==192) {struct {LONG count;BOOLEAN owned,abandoned;} state;
            assert(checkpoint_native.mutant(bridge_resolve_handle(first),0,&state,sizeof state,NULL)==0 && !state.owned);}
        if(ordinal==194)assert(MEM8(object)==9);
        object_roundtrip();
        MEM32(g_esp)=0x29000;MEM32(g_esp+4)=type;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0x29108;
        fixture_import(243);assert(!g_eax);uint32_t opened=MEM32(0x29108);
        assert(checkpoint_native.compare(bridge_resolve_handle(first),bridge_resolve_handle(opened))==0);
        MEM32(g_esp+4)=XBOX_KERNEL_DATA_BASE+KDATA_THREAD_OBJ_TYPE;MEM32(0x29108)=0xdeadbeef;
        fixture_import(243);assert(g_eax==0xc0000024u && !MEM32(0x29108));
        object_close(first);object_close(second);object_close(opened);
        assert(ke_object_slot(object)); /* An explicit reference retains the body, not its temporary name. */
        MEM32(g_esp)=0x29000;MEM32(g_esp+4)=type;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0x29108;
        fixture_import(243);assert(g_eax==0xc0000034u && !MEM32(0x29108));
        object_dereference(object);assert(!ke_object_slot(object));cleanup();return 0;
    }





    if(!strcmp(argv[1],"wait_owner_save") || !strcmp(argv[1],"wait_owner_load") || !strcmp(argv[1],"wait_owner_hot_save") ||
       !strcmp(argv[1],"wait_timer_save") || !strcmp(argv[1],"wait_timer_load")) {
        HANDLE worker;int timer_wait=strstr(argv[1],"timer")!=NULL;
        wait_owner_hot=!strcmp(argv[1],"wait_owner_hot_save");
        if(strstr(argv[1],"save")) {
            if(timer_wait) {
                g_esp=0x80000;MEM32(g_esp)=0x20070;MEM32(g_esp+4)=0;MEM32(g_esp+8)=1;fixture_import(194);assert(g_eax==0);
                assert(timer_set_call(MEM32(0x20070),-10000000,0,0)==0);
            } else {
                HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);assert(event);
                MEM32(0x20070)=bridge_handle_token(event);
            }
            MEM32(0x20074)=0xdeadbeef;
            wait_owner_ready=CreateEventW(NULL,TRUE,FALSE,NULL);assert(wait_owner_ready);
            xbox_kernel_set_checkpoint_hook(close_owned_wait_source);
            worker=bridge_spawn_thread(sub_00011100,0,0,xbox_AllocThreadStack());assert(worker);
            bridge_write_handle(0x20044,worker);
            assert(WaitForSingleObject(wait_owner_ready,5000)==WAIT_OBJECT_0);
            assert(xbox_CheckpointBegin(3000));
            FILE *memory=fopen(argv[3],"wb"),*threads=fopen(argv[4],"wb"),*objects=fopen(argv[5],"wb");
            assert(memory && threads && objects && xbox_SavePhysicalMemory(memory) &&
                   xbox_CheckpointWriteThreads(threads) && xbox_CheckpointWriteObjects(objects));
            if(timer_wait)assert(xbox_SaveKernelScheduler(objects));
            fclose(memory);fclose(threads);fclose(objects);xbox_CheckpointEnd();
            if(!strcmp(argv[1],"wait_owner_hot_save")) {
                xbox_kernel_set_checkpoint_hook(NULL);
                assert(xbox_CheckpointBegin(3000));
                memory=fopen(argv[3],"rb");threads=fopen(argv[4],"rb");objects=fopen(argv[5],"rb");
                assert(memory && threads && objects);
                void *prepared=xbox_CheckpointPrepareThreads(threads,2);assert(prepared);
                assert(xbox_CheckpointQuiesceThreads(prepared));
                assert(xbox_LoadPhysicalMemory(memory));
                XboxCheckpointRoot roots[2];unsigned count;
                assert(xbox_CheckpointApplyThreads(prepared,roots,&count) && count==1);
                xbox_CheckpointFreeThreads(prepared);
                assert(xbox_CheckpointLoadObjects(objects,roots,count));
                fclose(memory);fclose(threads);fclose(objects);
                assert(xbox_HeapBlockSize(MEM32(0x20080))>=4);
                worker=bridge_read_handle(0x20044);CloseHandle(roots[0].handle);
                xbox_CheckpointEnd();
            }
            xbox_kernel_set_checkpoint_hook(NULL);CloseHandle(wait_owner_ready);
        } else {
            if(timer_wait)xbox_CheckpointStartKernelDevices();
            assert(xbox_CheckpointBootstrapBegin());
            FILE *memory=fopen(argv[3],"rb"),*threads=fopen(argv[4],"rb"),*objects=fopen(argv[5],"rb");
            assert(memory && threads && objects && xbox_LoadPhysicalMemory(memory));fclose(memory);
            XboxCheckpointRoot roots[2];unsigned count;
            assert(xbox_CheckpointLoadThreads(threads,roots,2,&count) && count==1);fclose(threads);
            assert(xbox_CheckpointLoadObjects(objects,roots,count));
            if(timer_wait)assert(xbox_LoadKernelScheduler(objects));fclose(objects);
            worker=bridge_read_handle(0x20044);CloseHandle(roots[0].handle);xbox_CheckpointEnd();
        }
        assert(!bridge_resolve_handle(MEM32(0x20070)) && MEM32(0x20074)==0xdeadbeef);
        if(!timer_wait)assert(SetEvent(bridge_resolve_handle(MEM32(0x2007c))));
        assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && MEM32(0x20074)==0 && MEM32(0x20078)==1);
        assert(!bridge_resolve_handle(MEM32(0x2007c)));
        if(wait_owner_hot)assert(!xbox_HeapBlockSize(MEM32(0x20080)));
        cleanup();return 0;
    }


    if(!strcmp(argv[1],"timer_thread_rundown")) {
        termination_timer=1;HANDLE worker=bridge_spawn_thread(terminate_probe,0,0,xbox_AllocThreadStack());assert(worker);
        assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && !termination_returned);
        assert(MEM32(0x2007c) && !ke_shadow_lookup(MEM32(0x2007c)));
        for(unsigned i=0;i<g_timer_capacity;i++)assert(g_timers[i].timer_va!=MEM32(0x2007c));
        CloseHandle(worker);cleanup();return 0;
    }
    if(!strcmp(argv[1],"timer_handle_contract")) {
        guest_cpu_join();g_esp=0x80000;
        for(unsigned type=0;type<2;type++) {
            MEM32(g_esp)=0x20070;MEM32(g_esp+4)=0;MEM32(g_esp+8)=type;fixture_import(194);
            assert(g_eax==0);uint32_t token=MEM32(0x20070);assert(token);
            assert(timer_set_call(token,-200000,0,0)==0 && MEM32(0x20088)==0xaabbcc00);
            LARGE_INTEGER due;due.QuadPart=-5000000;
            assert(xbox_NtWaitForSingleObject(bridge_resolve_handle(token),FALSE,&due)==0);
            due.QuadPart=0;
            assert(xbox_NtWaitForSingleObject(bridge_resolve_handle(token),FALSE,&due)==(type?0x102:0));
            g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=0x20088;MEM32(0x20088)=0xaabbccdd;
            fixture_import(185);assert(g_eax==0 && MEM32(0x20088)==(type?0xaabbcc00:0xaabbcc01));
            uint32_t object=object_reference(token,XBOX_KERNEL_DATA_BASE+KDATA_TIMER_OBJ_TYPE);
            assert(MEM8(object)==8+type && MEM8(object+2)==10);object_dereference(object);
            assert(timer_set_call(token,-200000,0x17200,0)==0);
            due.QuadPart=-5000000;assert(xbox_NtWaitForSingleObject(bridge_resolve_handle(token),FALSE,&due)==0);
            assert(xbox_DeliverThreadApcs(TRUE)==1 && timer_apc_count==type+1);
            assert(MEM32(ke_current_thread()+0x124)==ke_current_thread()+0x124);
            /* Cancel removes a queued callback and preserves signalled state. */
            assert(timer_set_call(token,-200000,0x17200,200)==0);
            due.QuadPart=-5000000;assert(xbox_NtWaitForSingleObject(bridge_resolve_handle(token),FALSE,&due)==0);
            g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=0;fixture_import(185);assert(g_eax==0);
            assert(xbox_DeliverThreadApcs(TRUE)==0 && timer_apc_count==type+1);
            assert(timer_set_call(token,-10000000,0,0xffffffffu)==0xc000000du);
            assert(timer_set_call(token,-10000000,0x17200,200)==0);
            assert(stdcall_args_for_ordinal(216)==8);
            g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=0x20100;
            memset(XBOX_TO_NATIVE(0x20100),0xcc,16);fixture_import(216);assert(g_eax==0);
            int64_t remaining;memcpy(&remaining,XBOX_TO_NATIVE(0x20100),8);
            assert(remaining>5000000 && remaining<=10000000 && !MEM8(0x20108) && MEM8(0x20109)==0xcc);
            object_roundtrip();assert(ke_object_for_handle(bridge_resolve_handle(token))==object);
            assert(ke_object_slot(object)->references==1 && MEM8(object+0x78));
            g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=0;fixture_import(185);assert(g_eax==0);
            assert(!MEM8(object+0x78) && !ke_object_slot(object)->references);
            assert(xbox_NtSetEvent(bridge_resolve_handle(token),NULL)==(NTSTATUS)0xc0000024u);
            g_esp=0x80000;MEM32(g_esp)=token;MEM32(g_esp+4)=token;MEM32(g_esp+8)=0;
            MEM32(g_esp+12)=0;MEM32(g_esp+16)=0x20080;*(int64_t *)XBOX_TO_NATIVE(0x20080)=0;
            fixture_import(230);assert(g_eax==0xc0000024u);
            object_close(token);assert(!ke_shadow_lookup(object));
        }
        g_esp=0x80000;MEM32(g_esp)=0x20070;MEM32(g_esp+4)=0;MEM32(g_esp+8)=2;
        fixture_import(194);assert(g_eax==0xc000000du);
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"signal_wait_contract")) {
        guest_cpu_join();g_esp=0x80000;
        HANDLE signal=CreateSemaphoreW(NULL,0,2,NULL),event=CreateEventW(NULL,FALSE,FALSE,NULL);assert(signal && event);
        uint32_t source=bridge_handle_token(signal),target=bridge_handle_token(event);
        for(unsigned iteration=0;iteration<2;iteration++) {
            MEM32(g_esp)=source;MEM32(g_esp+4)=target;MEM32(g_esp+8)=0;
            MEM32(g_esp+12)=0;MEM32(g_esp+16)=0x20080;*(int64_t *)XBOX_TO_NATIVE(0x20080)=-400000;
            if(iteration)assert(SetEvent(event));
            fixture_import(230);assert(g_eax==(iteration?0:0x102));
            assert(WaitForSingleObject(signal,0)==WAIT_OBJECT_0);
            assert(WaitForSingleObject(signal,0)==WAIT_TIMEOUT);
            assert(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
        }
        /* A bad wait handle must not perform the signal operation. */
        MEM32(g_esp)=source;MEM32(g_esp+4)=0;fixture_import(230);assert(g_eax==0xc0000008u);
        assert(WaitForSingleObject(signal,0)==WAIT_TIMEOUT);
        /* A mutant may be released only by its owning thread. */
        HANDLE mutant=CreateMutexW(NULL,FALSE,NULL);assert(mutant);
        uint32_t mutex=bridge_handle_token(mutant);MEM32(g_esp)=mutex;MEM32(g_esp+4)=target;
        fixture_import(230);assert(g_eax==0xc0000046u);
        assert(WaitForSingleObject(mutant,0)==WAIT_OBJECT_0 && SetEvent(event));
        fixture_import(230);assert(g_eax==0);
        assert(WaitForSingleObject(mutant,0)==WAIT_OBJECT_0 && ReleaseMutex(mutant));
        object_close(source);object_close(target);object_close(mutex);guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"wait_closed_source")) {
        guest_cpu_join();g_esp=0x80000;
        HANDLE source=CreateEventW(NULL,FALSE,FALSE,NULL);assert(source);
        assert(xbox_NtDuplicateObject(source,&wait_close_alias,0)==0);
        wait_close_token=bridge_handle_token(source);
        RecompContFrame frame;assert(!recomp_cont_enter(&frame,0x11100,NULL,0));
        xbox_kernel_set_checkpoint_hook(close_wait_source);
        MEM32(g_esp)=wait_close_token;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;
        fixture_import(233);assert(g_eax==0 && !wait_close_token);
        xbox_kernel_set_checkpoint_hook(NULL);recomp_cont_leave(&frame);
        for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++)assert(!s_handle_table[i]);
        assert(WaitForSingleObject(wait_close_alias,0)==WAIT_TIMEOUT);
        CloseHandle(wait_close_alias);guest_cpu_part();return 0;
    }
    if(!strcmp(argv[1],"object_reference_event")) {
        uint32_t initial_objects[KE_SHADOW_SIZE];
        for(unsigned i=0;i<KE_SHADOW_SIZE;i++)initial_objects[i]=g_ke_shadow[i].guest_va;
        guest_cpu_join();g_esp=0x80000;
        uint32_t event_type=ke_object_type_for_kind(CHECKPOINT_EVENT);
        for(unsigned pass=0;pass<96;pass++) {
            HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);assert(event);uint32_t token=bridge_handle_token(event);
            uint32_t object=object_reference(token,event_type);assert(object_reference(token,0)==object);
            assert(MEM8(object)==0 && MEM8(object+2)==4 && MEM32(object+8)==object+8);
            assert(!ke_thread_for_handle(event));
            duplicate(token,0x20064,0);uint32_t alias=MEM32(0x20064);
            assert(object_reference(alias,event_type)==object);
            MEM32(g_esp)=token;MEM32(g_esp+4)=ke_object_type_for_kind(CHECKPOINT_THREAD);MEM32(g_esp+8)=0x20060;
            fixture_import(246);assert(g_eax==0xc0000024u && !MEM32(0x20060));
            assert(SetHandleInformation(event,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE));
            MEM32(g_esp)=token;fixture_import(187);
            assert(g_eax==0xc0000235u && bridge_resolve_handle(token)==event);
            assert(SetHandleInformation(event,HANDLE_FLAG_PROTECT_FROM_CLOSE,0));
            object_close(token);object_close(alias);
            MEM32(g_esp)=object;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;fixture_import(145);
            assert(!g_eax && MEM32(object+4)==1 && WaitForSingleObject(ke_shadow_lookup(object),0)==WAIT_OBJECT_0);
            g_ecx=object;fixture_import(251);assert(ke_object_slot(object)->references==4);
            if(pass==0)object_roundtrip();
            for(unsigned i=0;i<4;i++)object_dereference(object);
            assert(!ke_shadow_lookup(object) && !xbox_HeapBlockSize(object));
            MEM32(g_esp)=BRIDGE_HANDLE_TAG|15000;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0x20060;fixture_import(246);
            assert(g_eax==0xc0000008u && !MEM32(0x20060));
        }
        for(unsigned i=0;i<KE_SHADOW_SIZE;i++)assert(g_ke_shadow[i].guest_va==initial_objects[i]);
        guest_cpu_part();return 0;
    }
    if(!strcmp(argv[1],"object_reference_thread")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t main=object_reference(0xfffffffeu,ke_object_type_for_kind(CHECKPOINT_THREAD));
        assert(main==ke_current_thread());
        MEM32(g_esp)=main;MEM32(g_esp+4)=1;fixture_import(143);assert(g_eax==0);
        MEM32(g_esp)=main;fixture_import(124);assert(g_eax==1);
        MEM32(g_esp)=main;MEM32(g_esp+4)=1;fixture_import(144);assert(!g_eax);
        MEM32(g_esp)=main;MEM32(g_esp+4)=0;fixture_import(144);assert(g_eax==1);
        object_dereference(main);
        HANDLE worker=bridge_spawn_thread(terminate_probe,0,0,xbox_AllocThreadStack());assert(worker);
        uint32_t token=bridge_handle_token(worker),thread=object_reference(token,ke_object_type_for_kind(CHECKPOINT_THREAD));
        uint32_t tib=ke_object_slot(thread)->allocation;assert(tib && tib!=g_fs_base);
        guest_cpu_part();assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);guest_cpu_join();
        assert(!xbox_CheckpointGuestRootCount() && ke_thread_for_handle(worker)==thread);
        assert(object_reference(token,0)==thread && MEM32(thread+0x120)==0x456);
        /* The current fixture has no checkpoint root. Drop its process-owned
         * main-thread shadow so this archive contains the completed worker. */
        ke_shadow_remove(main);
        object_roundtrip();worker=bridge_resolve_handle(token);
        DWORD code;assert(WaitForSingleObject(worker,0)==WAIT_OBJECT_0 && GetExitCodeThread(worker,&code) && code==0x456);
        assert(ke_thread_for_handle(worker)==thread && ke_object_slot(thread)->references==2);
        object_close(token);assert(ke_shadow_lookup(thread));object_dereference(thread);object_dereference(thread);
        assert(!ke_shadow_lookup(thread) && !xbox_HeapBlockSize(tib));guest_cpu_part();return 0;
    }

    if(!strncmp(argv[1],"irp_owners_",11)) {
        guest_cpu_join();g_esp=0x80000;make_objects();
        int detached=strstr(argv[1],"detach")!=NULL;
        int cancel=detached || strstr(argv[1],"cancel")!=NULL,error=strstr(argv[1],"error")!=NULL;
        int create=strstr(argv[1],"create")!=NULL,sync=strstr(argv[1],"sync")!=NULL;
        int user_apc=strstr(argv[1],"apc")!=NULL;
        uint32_t file=object_reference(MEM32(0x20050),ke_object_type_for_kind(CHECKPOINT_FILE));
        uint32_t event=sync?0x29100:object_reference(MEM32(0x20030),ke_object_type_for_kind(CHECKPOINT_EVENT));
        if(sync)assert(ke_initialize_event(event,0,0));
        object_close(MEM32(0x20050));object_close(MEM32(0x20054));
        object_close(MEM32(0x20030));object_close(MEM32(0x20040));
        uint32_t irp=io_guest_address(xbox_IoAllocateIrp(1));assert(irp);
        MEM32(irp+4)=(create?8:0)|(sync?4:0);
        MEM32(irp+0x60)=file;MEM32(irp+0x20)=event;MEM32(irp+0x1c)=0x29200;
        MEM32(irp+0x10)=cancel?0xc0000120u:error?0xc0000001u:0;
        MEM32(irp+0x14)=17;MEM32(irp+0x5c)=irp+0x68;MEM8(irp+0x19)=1;
        MEM8(irp+0x1b)=cancel;
        if(!cancel || detached)io_attach_request_thread(irp,1);
        if(detached) {
            uint32_t thread=ke_current_thread(),head=thread+0x134;
            MEM8(irp+0x19)=3;io_disassociate_thread_irp(thread);
            assert(MEM32(head)==irp+8 && MEM32(irp+0x50)==thread);
            MEM8(irp+0x19)=1;io_disassociate_thread_irp(thread);
            assert(MEM32(head)==head && !MEM32(irp+0x50));
            assert(MEM16(irp)==6 && ke_shadow_lookup(file) && ke_shadow_lookup(event));
        }
        if(user_apc){MEM32(irp+0x28)=0x17400;MEM32(irp+0x2c)=0x1234;}
        if(strstr(argv[1],"checkpoint"))object_roundtrip();
        g_ecx=irp;g_edx=0;fixture_import(87);xbox_DeliverThreadApcs(FALSE);
        assert(create?ke_shadow_lookup(file)!=NULL:ke_shadow_lookup(file)==NULL);
        assert(sync?ke_shadow_lookup(event)!=NULL:ke_shadow_lookup(event)==NULL);
        if(create){assert(ke_object_slot(file)->references==1);object_dereference(file);}
        if(sync)ke_shadow_remove(event);
        if(user_apc) {
            assert(!irp_user_apc_calls && MEM16(irp)==6);
            assert(xbox_DeliverThreadApcs(TRUE)==1 && irp_user_apc_calls==1);
        }
        assert(!MEM16(irp));
        if(!cancel && !error)assert(MEM32(0x29200)==0 && MEM32(0x29204)==17);
        assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_queue_owner")) {
        guest_cpu_join();g_esp=0x80000;
        uint32_t irp=io_guest_address(xbox_IoAllocateIrp(1)),thread=0x29000,head=thread+0x134;
        MEM32(head)=MEM32(head+4)=head;MEM32(irp+0x50)=thread;
        MEM32(g_esp)=irp;fixture_import(77);
        assert(MEM32(head)==irp+8 && MEM32(head+4)==irp+8 && MEM32(irp+8)==head && MEM32(irp+12)==head);
        assert(MEM32(irp+0x50)==thread);
        ke_list_unlink(irp+8);xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"object_reference_file")) {
        guest_cpu_join();g_esp=0x80000;make_objects();
        uint32_t token=MEM32(0x20050),alias=MEM32(0x20054);
        uint32_t object=object_reference(token,ke_object_type_for_kind(CHECKPOINT_FILE));
        assert(object_reference(alias,0)==object && MEM16(object)==5 && MEM32(object+0x14)==4);
        assert(!ke_shadow_lookup(object+0x28) && ke_shadow_lookup(object+0x38));
        object_close(token);object_close(alias);object_roundtrip();
        XboxFileOpenState state;assert(xbox_CaptureFileOpenState(ke_shadow_lookup(object),&state) && state.position==4);
        object_dereference(object);object_dereference(object);
        assert(!ke_shadow_lookup(object) && !ke_shadow_lookup(object+0x28) && !ke_shadow_lookup(object+0x38));
        assert(!xbox_HeapBlockSize(object));guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"terminate_root") || !strcmp(argv[1],"terminate_worker")) {
        guest_cpu_join();g_esp=0x80000;
        if(!strcmp(argv[1],"terminate_root")) {
            xbox_RunGuestRoot(terminate_probe);
            assert(g_eax==0x456 && t_guest_terminated && !t_checkpoint_root_valid);
            assert(!recomp_cont_active() && !t_kernel_wait && !t_dispatch_depth);
            assert(guest_cpu_is_held() && BRIDGE_MEM8(ke_current_thread()+0x2c)==4);
        } else {
            HANDLE worker=bridge_spawn_thread(terminate_probe,0,0,xbox_AllocThreadStack());assert(worker);
            guest_cpu_part();assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);
            DWORD status;assert(GetExitCodeThread(worker,&status) && status==0x456);CloseHandle(worker);
            guest_cpu_join();assert(!xbox_CheckpointGuestRootCount());
        }
        assert(!termination_returned);guest_cpu_part();return 0;
    }
    if(!strcmp(argv[1],"transfer_wait_owner")) {
        guest_cpu_join();g_esp=0x80000;
        RecompContFrame root,owner,helper;GuestKernelWait wait;
        uint32_t root_ebp=0x9000,owner_ebp=0x8000;
        const RecompContLocal root_locals[]={{"ebp",&root_ebp,4}};
        const RecompContLocal owner_locals[]={{"ebp",&owner_ebp,4}};
        assert(!recomp_cont_enter(&root,0x11000,root_locals,1));recomp_cont_site(&root,(1ULL<<32)|0x11010);
        kernel_wait_enter(&wait,0);recomp_cont_site(&wait.frame,(8ULL<<32)|wait.frame.function);
        HANDLE event=CreateEventW(NULL,TRUE,TRUE,NULL),owned;uint32_t token=0;assert(event);
        assert(!xbox_kernel_wait_acquire(1,&event,&owned,&token));
        uint32_t storage=wait.object_tokens_va;assert(storage && BRIDGE_MEM32(storage)==token);
        assert(!xbox_NtClose(event));
        assert(!recomp_cont_enter(&owner,0x12000,owner_locals,1));recomp_cont_site(&owner,(1ULL<<32)|0x12010);
        assert(!recomp_cont_enter(&helper,0x13000,NULL,0));recomp_cont_site(&helper,(1ULL<<32)|0x13010);
        recomp_cont_prepare_transfer(owner_ebp,0x14000);
        assert(!recomp_cont_active() && !t_kernel_wait && bridge_resolve_handle(token)==owned);
        assert(WaitForSingleObject(owned,0)==WAIT_OBJECT_0 && BRIDGE_MEM32(storage)==token);
        assert(recomp_cont_enter(&root,0x11000,root_locals,1)==((1ULL<<32)|0x11010));
        kernel_wait_enter(&wait,0);assert(wait.restoring==3 && wait.object_tokens_va==storage);
        assert(recomp_cont_enter(&owner,0x12000,owner_locals,1)==((7ULL<<32)|0x14000));
        HANDLE ignored=NULL,recovered=NULL;uint32_t recovered_token=0;
        assert(!xbox_kernel_wait_acquire(1,&ignored,&recovered,&recovered_token));
        assert(recovered==owned && recovered_token==token && WaitForSingleObject(recovered,0)==WAIT_OBJECT_0);
        recomp_cont_leave(&owner);recomp_cont_leave(&wait.frame);recomp_cont_leave(&root);
        assert(!bridge_resolve_handle(token) && !t_kernel_wait && !recomp_cont_active());
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"tls_owned_stack")) {
        guest_cpu_join();XboxGuestRegisters original;xbox_CaptureGuestRegisters(&original);
        uint32_t main_tls=MEM32(XBOX_FS_BASE+4);assert(main_tls==0x770014u);
        assert(MEM32(XBOX_FS_BASE+8)==XBOX_STACK_BASE && !g_is_spawned_thread);
        uint32_t worker_top=xbox_AllocThreadStack(),peer_top=xbox_AllocThreadStack();
        uint32_t worker_tib=xbox_AllocThreadTib();assert(worker_top && peer_top && worker_tib);
        uint32_t worker_tls=MEM32(worker_tib+4);assert(worker_tls==worker_tib+0x60+20);
        for(unsigned worker=0;worker<2;worker++) {
            if(worker){g_fs_base=worker_tib;g_is_spawned_thread=1;g_thread_stack_top=worker_top;}
            uint32_t bottom=worker?worker_top+16-512u*1024u:XBOX_STACK_BASE;
            uint32_t top=worker?worker_top:XBOX_STACK_TOP;
            uint32_t tls=worker?worker_tls:main_tls;
            assert(MEM32(XBOX_FS_BASE+4)==tls);
            uint32_t registration=top-0x200;
            int valid=bridge_unwind_registration_valid(registration);
            fprintf(stderr,"[TLS_STACK] worker=%u fs4=%08X fs8=%08X owned=%08X..%08X registration=%08X valid=%d\n",
                    worker,MEM32(XBOX_FS_BASE+4),MEM32(XBOX_FS_BASE+8),bottom,top,registration,valid);
            assert(valid);
            assert(bridge_unwind_registration_valid(bottom));
            assert(bridge_unwind_registration_valid(top-8));
            assert(!bridge_unwind_registration_valid(bottom-4));
            assert(!bridge_unwind_registration_valid(top-4));
            assert(!bridge_unwind_registration_valid(top-7));
            assert(!bridge_unwind_registration_valid(peer_top-0x100));
            assert(!bridge_unwind_registration_valid(worker?0x820000u:worker_top-0x100));
            if(worker) {
                g_thread_stack_top=0;assert(!bridge_unwind_registration_valid(worker_top-0x100));
                g_thread_stack_top=16;assert(!bridge_unwind_registration_valid(worker_top-0x100));
                g_thread_stack_top=worker_top;
            }
            g_esp=bottom+0x1000;g_ebp=g_seh_ebp=bottom+0x1800;
            owned_stack_registration=top-0x200;
            MEM32(owned_stack_registration)=0xffffffffu;MEM32(owned_stack_registration+4)=0x1b500;
            MEM32(XBOX_FS_BASE)=owned_stack_registration;
            memset(XBOX_TO_NATIVE(0x60000),0,0x50);MEM32(0x60000)=0xc0000094u;
            XboxGuestRegisters registers;xbox_CaptureGuestRegisters(&registers);
            bridge_capture_context(0x61000,&registers,0x12345,g_esp);
            assert(bridge_dispatch_exception(0x60000,0x61000));
            assert(owned_stack_dispatch_calls==worker+1 && MEM32(XBOX_FS_BASE)==owned_stack_registration);
            MEM32(0x60000)=0x11111111;MEM32(0x60004)=0;
            MEM32(g_esp-4)=0x12345;MEM32(g_esp)=0xffffffffu;MEM32(g_esp+4)=0;
            MEM32(g_esp+8)=0x60000;MEM32(g_esp+12)=0;
            bridge_RtlUnwind();assert(owned_stack_unwind_calls==worker+1 && MEM32(XBOX_FS_BASE)==0xffffffffu);
            assert(MEM32(XBOX_FS_BASE+4)==tls);
        }
        g_is_spawned_thread=0;g_thread_stack_top=0;assert(xbox_RestoreGuestRegisters(&original));
        assert(MEM32(XBOX_FS_BASE+4)==main_tls);
        xbox_FreeThreadStack(worker_top);xbox_FreeThreadStack(peer_top);xbox_HeapFree(worker_tib);
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"dpc_stack_contract")) {
        guest_cpu_join();g_esp=0x800000;g_ebp=0x810000;g_seh_ebp=0x810000;
        MEM32(XBOX_FS_BASE)=0x820000;
        MEM32(XBOX_FS_BASE+0x58)=0;
        uint32_t standing=MEM32(XBOX_FS_BASE+0x5c);assert(standing);
        MEM32(0x2900c)=0x1b100;MEM32(0x29010)=0x1234;
        for(unsigned i=0;i<2;i++) {
            assert(kernel_run_dpc(0x29000,0x5678,0x9abc));
            assert(g_esp==0x800000 && g_ebp==0x810000 && g_seh_ebp==0x810000);
            assert(MEM32(XBOX_FS_BASE)==0x820000 && !MEM32(XBOX_FS_BASE+0x58) && MEM32(XBOX_FS_BASE+0x5c)==standing);
            assert(xbox_CurrentIrql()==0);bridge_KeIsExecutingDpc();assert(!g_eax);
        }
        assert(dpc_stack_calls==2 && dpc_unwind_calls==2);
        FILE *memory=tmpfile();assert(memory);t_checkpoint_controller=1;
        assert(xbox_SavePhysicalMemory(memory));rewind(memory);assert(fwrite("XBPHYS",6,1,memory)==1);
        rewind(memory);assert(!xbox_LoadPhysicalMemory(memory));
        assert(MEM32(XBOX_FS_BASE+0x5c)==standing && !MEM32(XBOX_FS_BASE+0x58));
        rewind(memory);assert(fwrite("XBPCRS",6,1,memory)==1);rewind(memory);assert(xbox_LoadPhysicalMemory(memory));
        assert(MEM32(XBOX_FS_BASE+0x5c)==standing);t_checkpoint_controller=0;fclose(memory);
        guest_cpu_part();cleanup();return 0;
    }
    if(!strncmp(argv[1],"unwind_",7)) {
        guest_cpu_join();g_esp=0x800000;g_ebp=0x810000;g_eax=123;g_ecx=7;g_ebx=0xabcdef;g_eflags=0x202;
        XboxGuestRegisters fp;xbox_CaptureGuestRegisters(&fp);double initial=2.5;
        memcpy(&fp.fp_stack[3],&initial,8);fp.fp_top=3;fp.fp_control_word=0x27f;
        fp.fp_status_word=1;fp.fp_cc=0x100;fp.fp_tag_word=0xff3f;fp.fp_opcode=0x123;
        fp.fp_instruction_pointer=0x223344;fp.fp_data_pointer=0x556677;fp.mxcsr=0x3f80;
        for(unsigned i=0;i<8;i++)memset(fp.xmm[i],0x90+i,16);assert(xbox_RestoreGuestRegisters(&fp));
        MEM32(XBOX_FS_BASE)=0x820000;
        MEM32(0x820000)=0x820020;MEM32(0x820004)=0x1b000;
        MEM32(0x820020)=0x820040;MEM32(0x820024)=0x1b000;
        MEM32(0x820040)=0xffffffffu;MEM32(0x820044)=0x1b000;
        memset(XBOX_TO_NATIVE(0x60000),0,0x50);MEM32(0x60000)=0x11111111;
        uint32_t target=0x820020;
        if(!strcmp(argv[1],"unwind_nested"))unwind_mode=10;
        if(!strcmp(argv[1],"unwind_synthetic"))unwind_mode=1;
        if(!strcmp(argv[1],"unwind_collision")){unwind_mode=2;target=0x820040;}
        if(!strcmp(argv[1],"unwind_bad_target")){unwind_mode=3;target=0x81fff0;}
        if(!strcmp(argv[1],"unwind_bad_stack")){unwind_mode=4;MEM32(XBOX_FS_BASE)=0x820002;}
        if(!strcmp(argv[1],"unwind_bad_disposition"))unwind_mode=5;
        if(!strcmp(argv[1],"unwind_missing_target")){unwind_mode=6;target=0x820010;}
        if(!strcmp(argv[1],"unwind_exit")){unwind_mode=7;target=0;MEM32(0x820020)=0xffffffffu;}
        if(!strcmp(argv[1],"unwind_all")){unwind_mode=8;target=0xffffffffu;MEM32(0x820020)=0xffffffffu;}
        if(!strcmp(argv[1],"unwind_empty")){unwind_mode=9;target=0xffffffffu;MEM32(XBOX_FS_BASE)=0xffffffffu;}
        MEM32(g_esp)=target;MEM32(g_esp+4)=0xdeadbeef;MEM32(g_esp+8)=unwind_mode==1?0:0x60000;
        MEM32(g_esp+12)=0x456;fixture_import(312);
        assert(unwind_mode<3 || unwind_mode>=8);
        assert(MEM32(XBOX_FS_BASE)==target && g_eax==0x456 && g_esp==0x800000);
        assert(unwind_calls==(unwind_mode==9?0:unwind_mode==8?2:1));
        if(unwind_mode!=9) {
            xbox_CaptureGuestRegisters(&fp);double changed;memcpy(&changed,&fp.fp_stack[5],8);
            assert(g_ebx==0x98765 && g_ecx==9 && g_eflags==0x246);
            assert(changed==-4.0 && fp.fp_top==5 && fp.fp_control_word==0x37f && fp.mxcsr==0x5f80);
        }
        guest_cpu_part();return 0;
    }

    if(!strcmp(argv[1],"cpu_exception") || !strcmp(argv[1],"raise_exception") ||
       !strcmp(argv[1],"nested_exception") || !strcmp(argv[1],"noncontinuable_exception") ||
       !strcmp(argv[1],"capture_context")) {
        guest_cpu_join();g_esp=0x800000;g_ebp=0x810000;g_eax=123;g_ebx=0xabcdef;g_eflags=0x202;
        XboxGuestRegisters fp;xbox_CaptureGuestRegisters(&fp);double initial=2.5;
        memcpy(&fp.fp_stack[3],&initial,8);fp.fp_top=3;fp.fp_control_word=0x27f;
        fp.fp_status_word=1;fp.fp_cc=0x100;fp.fp_tag_word=0xff3f;fp.fp_opcode=0x123;
        fp.fp_instruction_pointer=0x223344;fp.fp_data_pointer=0x556677;fp.mxcsr=0x3f80;
        for(unsigned i=0;i<8;i++)memset(fp.xmm[i],0x90+i,16);
        assert(xbox_RestoreGuestRegisters(&fp));
        BRIDGE_MEM32(XBOX_FS_BASE)=0x820000;
        MEM32(0x820000)=0xffffffffu;MEM32(0x820004)=0x1a000;
        if(!strcmp(argv[1],"capture_context")) {
            MEM32(0x810000)=0x830000;MEM32(0x810004)=0x123456;
            memset(XBOX_TO_NATIVE(0x60000),0xaa,0x250);MEM32(g_esp)=0x60000;
            fixture_import(265);
            assert(MEM32(0x6021c)==123 && MEM32(0x60210)==0xabcdef);
            assert(MEM32(0x60220)==0x830000 && MEM32(0x60224)==0x123456 && MEM32(0x60230)==0x810008);
            check_float_context(0x60000);assert(MEM32(0x60240)==0xaaaaaaaa);guest_cpu_part();return 0;
        }
        seh_address=0x12345;seh_stack=0x800000;
        if(!strcmp(argv[1],"raise_exception")) {
            seh_mode=1;seh_stack=0x800004;
            memset(XBOX_TO_NATIVE(0x60000),0,0x50);MEM32(0x60000)=0xc0000094u;
            MEM32(g_esp)=0x60000;fixture_import(302);
            assert(seh_calls==1 && g_eax==456 && g_ecx==7 && g_eflags==0x246);
        } else {
            if(!strcmp(argv[1],"nested_exception")) {
                seh_mode=2;MEM32(0x820000)=0x820020;MEM32(0x820020)=0xffffffffu;MEM32(0x820024)=0x1a000;
            }
            if(!strcmp(argv[1],"noncontinuable_exception")) {
                seh_mode=3;memset(XBOX_TO_NATIVE(0x60000),0,0x50);
                MEM32(0x60000)=0xc0000001u;MEM32(0x60004)=1;
                bridge_raise_record(0x60000,seh_address,g_ebp,g_esp);assert(0);
            }
            uint32_t resumed=xbox_CpuException(0xc0000094u,seh_address,g_ebp);
            assert(resumed==seh_address+(seh_mode==2?0:2) && seh_calls==(seh_mode==2?2:1));
            assert(g_esp==seh_stack && g_eax==456 && g_ecx==7 && g_eflags==0x246);
        }
        xbox_CaptureGuestRegisters(&fp);double changed;memcpy(&changed,&fp.fp_stack[5],8);
        assert(changed==-4.0 && fp.fp_top==5 && fp.fp_control_word==0x37f && fp.fp_cc==0x4000);
        assert(fp.fp_status_word==0x20 && fp.mxcsr==0x5f80 && fp.xmm[0][15]==0x78);
        guest_cpu_part();return 0;
    }

    if(!strcmp(argv[1],"completion_port") || !strcmp(argv[1],"completion_checkpoint")) {
        g_esp=0x80000;MEM32(0x28004)=0x1234abcd;
        MEM32(g_esp)=0x28000;MEM32(g_esp+4)=0x1f0003;MEM32(g_esp+8)=0;MEM32(g_esp+12)=1;
        bridge_NtCreateIoCompletion();assert(!g_eax && MEM32(0x28004)==0x1234abcd);
        uint32_t token=MEM32(0x28000),queue=ke_completion_for_handle(bridge_resolve_handle(token));assert(queue);
        assert(BRIDGE_MEM8(queue)==4 && BRIDGE_MEM8(queue+2)==10 && MEM32(queue+0x1c)==1);
        for(unsigned i=0;i<2;i++) {
            MEM32(g_esp)=token;MEM32(g_esp+4)=0xabc000+i;MEM32(g_esp+8)=0xdef000+i;
            MEM32(g_esp+12)=i?0:0xc0000005;MEM32(g_esp+16)=128+i;bridge_NtSetIoCompletion();assert(!g_eax);
        }
        duplicate(token,0x28008,DUPLICATE_SAME_ACCESS);uint32_t alias=MEM32(0x28008);
        if(!strcmp(argv[1],"completion_checkpoint")) {
            guest_cpu_part();assert(xbox_CheckpointBootstrapBegin());FILE *packet=tmpfile();assert(packet);
            assert(xbox_CheckpointWriteObjects(packet));rewind(packet);assert(checkpoint_clear_objects());
            assert(xbox_CheckpointLoadObjects(packet,NULL,0));fclose(packet);xbox_CheckpointEnd();
            assert(ke_completion_for_handle(bridge_resolve_handle(alias))==queue && MEM32(queue+4)==2);
        }
        MEM32(g_esp)=token;bridge_NtClose();assert(!g_eax && ke_shadow_lookup(queue));
        MEM32(0x29300)=0;MEM32(0x29304)=0;
        for(unsigned i=0;i<3;i++) {
            MEM32(g_esp)=alias;MEM32(g_esp+4)=0x29000;MEM32(g_esp+8)=0x29004;
            MEM32(g_esp+12)=0x29008;MEM32(g_esp+16)=0x29300;
            bridge_NtRemoveIoCompletion();
            if(i<2) {
                assert(!g_eax && MEM32(0x29000)==0xabc000+i && MEM32(0x29004)==0xdef000+i);
                assert(MEM32(0x29008)==(i?0:0xc0000005) && MEM32(0x2900c)==128+i);
            } else assert(g_eax==0x102 && MEM32(0x29000)==0xabc001 && MEM32(0x2900c)==129);
        }
        guest_cpu_join();assert(!ke_rundown_queue(queue));guest_cpu_part();
        MEM32(g_esp)=alias;bridge_NtClose();assert(!g_eax && !ke_shadow_lookup(queue));
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"queue_order")) {
        uint32_t queue=0x28000;g_esp=0x80000;MEM32(g_esp)=queue;MEM32(g_esp+4)=2;
        bridge_KeInitializeQueue();assert(stdcall_args_for_ordinal(136)==12);
        for(unsigned i=0;i<3;i++) {
            MEM32(g_esp)=queue;MEM32(g_esp+4)=0x28100+16*i;
            if(i==2)bridge_KeInsertHeadQueue();else bridge_KeInsertQueue();assert(g_eax==i);
        }
        MEM32(0x29300)=0;MEM32(0x29304)=0;
        uint32_t order[]={0x28120,0x28100,0x28110};
        for(unsigned i=0;i<3;i++) {
            MEM32(g_esp)=queue;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0x29300;bridge_KeRemoveQueue();
            assert(g_eax==order[i] && !MEM32(order[i]) && MEM32(queue+0x18)==1);
        }
        guest_cpu_join();uint32_t released=xbox_QueueThreadWaitBegin();assert(released==queue && MEM32(queue+0x18)==0);
        xbox_QueueThreadWaitEnd(released);assert(MEM32(queue+0x18)==1);
        assert(!ke_rundown_queue(queue) && !MEM32(ke_current_thread()+0x78));guest_cpu_part();
        ke_shadow_remove(queue);cleanup();return 0;
    }


    if(!strcmp(argv[1],"apc_cross_thread")) {
        g_esp=0x80000;guest_cpu_join();uint32_t parent=ke_current_thread();
        assert(ke_initialize_queue(0x28000,1));ke_insert_queue(0x28000,0x28100,0);
        HANDLE worker=bridge_spawn_thread(apc_worker_probe,0,0,xbox_AllocThreadStack());assert(worker);
        bridge_write_handle(0x29200,worker);uint32_t token=MEM32(0x29200);
        expected_apc_thread=ke_thread_for_handle(worker);assert(expected_apc_thread && expected_apc_thread!=parent);
        MEM32(g_esp)=token;MEM32(g_esp+4)=0x17000;MEM32(g_esp+8)=0x1234;
        MEM32(g_esp+12)=0x5678;MEM32(g_esp+16)=0x9abc;bridge_NtQueueApcThread();
        assert(!g_eax && !apc_normal_calls);guest_cpu_part();
        assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && apc_normal_calls==1);
        assert(MEM32(0x28018)==0 && MEM32(0x28020)==0x28020);
        assert(!BRIDGE_MEM8(expected_apc_thread+0x4b) && BRIDGE_MEM8(expected_apc_thread+0x2c)==4);
        assert(MEM32(expected_apc_thread+0x3c)==expected_apc_thread+0x3c);
        bridge_NtQueueApcThread();assert(g_eax==0xc0000001u);
        MEM32(g_esp)=0x12345678;bridge_NtQueueApcThread();assert(g_eax==0xc0000008u);
        ke_shadow_remove(0x28000);cleanup();return 0;
    }

    if(!strcmp(argv[1],"apc_layout")) {
        uint32_t apc=0x28000,thread=MEM32(g_fs_base+0x28);g_esp=0x80000;
        assert(stdcall_args_for_ordinal(105)==28 && BRIDGE_MEM8(thread+0x4b));
        memset(XBOX_TO_NATIVE(apc),0xcd,0x30);
        MEM32(g_esp)=apc;MEM32(g_esp+4)=thread;MEM32(g_esp+8)=0x16000;
        MEM32(g_esp+12)=0x16100;MEM32(g_esp+16)=0x17000;MEM32(g_esp+20)=1;MEM32(g_esp+24)=0x123;
        bridge_KeInitializeApc();assert(BRIDGE_MEM16(apc)==0x12 && BRIDGE_MEM8(apc+2)==1 && !BRIDGE_MEM8(apc+3));
        assert(MEM32(apc+4)==thread && MEM32(apc+0x10)==0x16000 && MEM32(apc+0x14)==0x16100);
        assert(MEM32(apc+0x18)==0x17000 && MEM32(apc+0x1c)==0x123 && MEM32(apc+0x28)==0xcdcdcdcd);
        MEM32(g_esp+16)=0;bridge_KeInitializeApc();assert(!BRIDGE_MEM8(apc+2) && !MEM32(apc+0x1c));
        bridge_KeGetCurrentThread();assert(g_eax==thread);cleanup();return 0;
    }
    if(!strcmp(argv[1],"apc_order") || !strcmp(argv[1],"apc_user_wait")) {
        g_esp=0x80000;guest_cpu_join();uint32_t thread=ke_current_thread();expected_apc_thread=thread;
        if(!strcmp(argv[1],"apc_order")) {
            bridge_KeEnterCriticalRegion();assert(MEM32(thread+0x68)==0xffffffff);
            ke_initialize_apc(0x28000,thread,0x16000,0,0x17000,0,0);
            ke_initialize_apc(0x28100,thread,0x16000,0,0,0,0);
            assert(ke_insert_apc(0x28000,11,22) && ke_insert_apc(0x28100,33,44));
            assert(!ke_insert_apc(0x28100,55,66) && MEM32(0x28120)==55 && MEM32(0x28124)==66);
            g_eax=0x12345678;xbox_DeliverThreadApcs(FALSE);
            assert(g_eax==0x12345678 && g_esp==0x80000 && apc_kernel_calls==1 && !apc_normal_calls);
            assert(apc_order[0]==0x28100 && BRIDGE_MEM8(0x28003) && !BRIDGE_MEM8(0x28103));
            bridge_KeLeaveCriticalRegion();assert(apc_kernel_calls==2 && apc_normal_calls==1 && apc_order[1]==0x28000);
        } else {
            ke_initialize_apc(0x28000,thread,0x16000,0,0x17000,1,0);
            assert(ke_insert_apc(0x28000,11,22));
            assert(!xbox_DeliverThreadApcs(FALSE) && !apc_kernel_calls);
            assert(xbox_checkpoint_wait(0,NULL,FALSE,KernelMode,TRUE,0)==WAIT_TIMEOUT && !apc_kernel_calls);
            assert(xbox_checkpoint_wait(0,NULL,FALSE,UserMode,FALSE,0)==WAIT_TIMEOUT && !apc_kernel_calls);
            assert(xbox_checkpoint_wait(0,NULL,FALSE,UserMode,TRUE,0)==XBOX_WAIT_USER_APC);
            assert(apc_kernel_calls==1 && apc_normal_calls==1);
        }
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_unwind") || !strcmp(argv[1],"irp_hold") || !strcmp(argv[1],"irp_free_completion")) {
        g_esp=0x80000;guest_cpu_join();uint32_t irp=io_guest_address(xbox_IoAllocateIrp(2));
        MEM32(irp+4)=0x80;MEM32(irp+0x5c)=irp+0x68;BRIDGE_MEM8(irp+0x19)=1;
        for(unsigned i=0;i<2;i++) {
            uint32_t stack=irp+0x68+i*0x24;BRIDGE_MEM8(stack+3)=0x40;
            MEM32(stack+0x1c)=0x18000;MEM32(stack+0x20)=10+i;
        }
        completion_hold=strcmp(argv[1],"irp_unwind")!=0;completion_free=!strcmp(argv[1],"irp_free_completion");
        g_ecx=irp;g_edx=0;bridge_IofCompleteRequest();
        assert(completion_calls==(completion_hold?1u:2u) && completion_contexts[0]==10);
        if(!completion_hold)assert(completion_contexts[1]==11 && BRIDGE_MEM8(irp+0x19)==4);
        if(!completion_free)xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));
        guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_final_completion")) {
        g_esp=0x80000;guest_cpu_join();uint32_t device=0x27000,event=0x29200,iosb=0x29300;
        BRIDGE_MEM8(device+0x1e)=1;assert(ke_initialize_event(event,0,0));
        uint32_t irp=io_guest_address(xbox_IoBuildDeviceIoControlRequest(0, XBOX_TO_NATIVE(device),NULL,0,NULL,0,FALSE,
            XBOX_TO_NATIVE(event),XBOX_TO_NATIVE(iosb)));
        MEM32(irp+0x10)=0;MEM32(irp+0x14)=47;BRIDGE_MEM8(irp+0x19)=1;MEM32(irp+0x5c)=irp+0x68;
        uint32_t thread=ke_current_thread(),head=thread+0x134;assert(MEM32(head)==irp+8);
        xbox_KfRaiseIrql(2);g_ecx=irp;g_edx=0;bridge_IofCompleteRequest();
        assert(BRIDGE_MEM16(irp)==6 && !MEM32(event+4) && !MEM32(iosb+4));
        xbox_KfLowerIrql(0);xbox_DeliverThreadApcs(FALSE);
        assert(!BRIDGE_MEM16(irp) && MEM32(iosb)==0 && MEM32(iosb+4)==47 && MEM32(event+4)==1);
        assert(MEM32(head)==head && MEM32(head+4)==head);
        ke_shadow_remove(event);guest_cpu_part();cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_sync_call")) {
        g_esp=0x80000;guest_cpu_join();uint32_t device=0x27000;BRIDGE_MEM8(device+0x1e)=1;
        MEM32(device+8)=0x27100;MEM32(0x27100+12+10*4)=0x19000;
        MEM32(g_esp)=0;MEM32(g_esp+4)=device;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0;
        MEM32(g_esp+16)=0;MEM32(g_esp+20)=0;MEM32(g_esp+24)=0x29300;MEM32(g_esp+28)=0;
        bridge_IoSynchronousDeviceIoControlRequest();assert(g_eax==0 && MEM32(0x29300)==17);
        assert(g_esp==0x80000);guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"irp_layout")) {
        assert(stdcall_args_for_ordinal(59)==4 && stdcall_args_for_ordinal(60)==24);
        const uint32_t fixed=0x28000,bytes=0x68+3*0x24;
        memset(XBOX_TO_NATIVE(fixed-16),0xa5,bytes+32);
        g_esp=0x80000;MEM32(g_esp)=fixed;MEM32(g_esp+4)=bytes;MEM32(g_esp+8)=3;
        bridge_IoInitializeIrp();
        assert(BRIDGE_MEM16(fixed)==6 && BRIDGE_MEM16(fixed+2)==bytes);
        assert(BRIDGE_MEM8(fixed+0x18)==3 && BRIDGE_MEM8(fixed+0x19)==4);
        assert(MEM32(fixed+0x5c)==fixed+bytes && MEM32(fixed+8)==fixed+8 && MEM32(fixed+12)==fixed+8);
        for(unsigned i=0;i<16;i++)assert(BRIDGE_MEM8(fixed-16+i)==0xa5 && BRIDGE_MEM8(fixed+bytes+i)==0xa5);
        for(unsigned count=1;count<=4;count++) {
            MEM32(g_esp)=count;bridge_IoAllocateIrp();uint32_t irp=g_eax;
            assert(irp && xbox_HeapBlockSize(irp)>=0x68+count*0x24);
            assert(BRIDGE_MEM16(irp)==6 && MEM32(irp+0x5c)==irp+0x68+count*0x24);
            MEM32(g_esp)=irp;bridge_IoFreeIrp();assert(BRIDGE_MEM16(irp)==0);
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_ioctl") || !strcmp(argv[1],"irp_dispatch")) {
        const uint32_t device=0x27000,event=0x29200,iosb=0x29300,head=MEM32(g_fs_base+0x28)+0x134;
        assert(MEM32(head)==head && MEM32(head+4)==head);
        BRIDGE_MEM8(device+0x1e)=1;MEM32(device+8)=0x27100;MEM32(0x27100+12+10*4)=0x15000;
        g_esp=0x80000;MEM32(g_esp)=0x76543210;MEM32(g_esp+4)=device;
        MEM32(g_esp+8)=0x29000;MEM32(g_esp+12)=7;MEM32(g_esp+16)=0x29100;
        MEM32(g_esp+20)=13;MEM32(g_esp+24)=0;MEM32(g_esp+28)=event;MEM32(g_esp+32)=iosb;
        MEM32(iosb)=0xa5a5a5a5;MEM32(iosb+4)=0xbabababa;bridge_IoBuildDeviceIoControlRequest();
        uint32_t irp=g_eax,stack=irp+0x68;
        assert(irp && MEM32(irp+0x5c)==stack+0x24 && BRIDGE_MEM8(stack)==10);
        assert(MEM32(stack+4)==13 && MEM32(stack+8)==0x29000 && MEM32(stack+12)==7 && MEM32(stack+16)==0x76543210);
        assert(MEM32(irp+0x30)==0x29100 && MEM32(irp+0x20)==event && MEM32(irp+0x1c)==iosb);
        assert(MEM32(irp+0x50)==MEM32(g_fs_base+0x28));
        assert(MEM32(head)==irp+8 && MEM32(head+4)==irp+8 && MEM32(irp+8)==head && MEM32(irp+12)==head);
        assert(MEM32(iosb)==0xa5a5a5a5 && MEM32(iosb+4)==0xbabababa);
        if(!strcmp(argv[1],"irp_dispatch")) {
            irp_dispatch_expect=irp;irp_dispatch_device=device;g_ecx=device;g_edx=irp;
            bridge_IofCallDriver();assert(g_eax==0x103 && irp_dispatch_calls==1);
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"irp_fsd")) {
        const uint32_t device=0x27000;BRIDGE_MEM8(device+0x1e)=2;
        MEM32(0x29400)=0x12345678;MEM32(0x29404)=0x123;
        for(unsigned sync=0;sync<2;sync++) {
            g_esp=0x80000;MEM32(g_esp)=2;MEM32(g_esp+4)=device;MEM32(g_esp+8)=0x29000;
            MEM32(g_esp+12)=512;MEM32(g_esp+16)=0x29400;
            MEM32(g_esp+20)=sync?0x29200:0x29300;MEM32(g_esp+24)=0x29300;
            if(sync)bridge_IoBuildSynchronousFsdRequest();else bridge_IoBuildAsynchronousFsdRequest();
            uint32_t irp=g_eax,stack=irp+0x68+0x24;
            assert(irp && MEM32(irp+0x5c)==stack+0x24 && BRIDGE_MEM8(stack)==2);
            assert(MEM32(stack+4)==512 && MEM32(stack+12)==0x12345678 && MEM32(stack+16)==0x123);
            assert(MEM32(irp+0x1c)==0x29300 && MEM32(irp+0x30)==0x29000);
            assert(MEM32(irp+0x20)==(sync?0x29200:0));
            if(!sync)assert(MEM32(irp+8)==irp+8 && MEM32(irp+12)==irp+8);
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"device_packets_abi")) {
        assert(stdcall_args_for_ordinal(81)==4);
        assert(stdcall_args_for_ordinal(82)==8);
        assert(stdcall_args_for_ordinal(83)==12);
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"device_queues")) {
        const uint32_t queue=0x27028,a=0x28000,b=0x28100,c=0x28200,d=0x28300;
        g_esp=0x80000;MEM32(g_esp)=queue;bridge_KeInitializeDeviceQueue();
        MEM32(g_esp+4)=a;bridge_KeInsertDeviceQueue();assert(g_eax==0 && BRIDGE_MEM8(queue+3)==1 && !BRIDGE_MEM8(a+12));
        MEM32(g_esp+4)=b;MEM32(g_esp+8)=20;bridge_KeInsertByKeyDeviceQueue();assert(g_eax==1);
        MEM32(g_esp+4)=c;MEM32(g_esp+8)=10;bridge_KeInsertByKeyDeviceQueue();assert(g_eax==1);
        MEM32(g_esp+4)=d;MEM32(g_esp+8)=20;bridge_KeInsertByKeyDeviceQueue();assert(g_eax==1);
        assert(MEM32(queue+4)==c && MEM32(c)==b && MEM32(b)==d && MEM32(d)==queue+4);
        assert(MEM32(queue+8)==d && MEM32(d+4)==b && MEM32(b+4)==c && MEM32(c+4)==queue+4);
        MEM32(g_esp+4)=15;bridge_KeRemoveByKeyDeviceQueue();assert(g_eax==b && !BRIDGE_MEM8(b+12));
        MEM32(g_esp+4)=50;bridge_KeRemoveByKeyDeviceQueue();assert(g_eax==c);
        MEM32(g_esp+4)=d;bridge_KeRemoveEntryDeviceQueue();assert(g_eax==1);
        bridge_KeRemoveEntryDeviceQueue();assert(g_eax==0 && BRIDGE_MEM8(queue+3)==1);
        bridge_KeRemoveDeviceQueue();assert(!g_eax && !BRIDGE_MEM8(queue+3));
        MEM32(g_esp+4)=a;bridge_KeInsertDeviceQueue();assert(!g_eax);
        MEM32(g_esp+4)=b;bridge_KeInsertDeviceQueue();assert(g_eax==1);
        MEM32(g_esp+4)=c;bridge_KeInsertDeviceQueue();assert(g_eax==1);
        bridge_KeRemoveDeviceQueue();assert(g_eax==b);
        bridge_KeRemoveDeviceQueue();assert(g_eax==c);
        bridge_KeRemoveDeviceQueue();assert(g_eax==0 && !BRIDGE_MEM8(queue+3));
        cleanup();return 0;
    }
    if(!strncmp(argv[1],"device_packets_",15)) {
        const uint32_t device=0x27000;
        unsigned keyed=strstr(argv[1],"keyed")!=NULL;
        MEM32(device+8)=0x27100;MEM32(0x27100)=0x14000;
        g_esp=0x80000;MEM32(g_esp)=device+0x28;bridge_KeInitializeDeviceQueue();
        for(unsigned i=0;i<4;i++) {
            MEM32(g_esp)=device;MEM32(g_esp+4)=0x28000+i*0x100;
            MEM32(g_esp+8)=keyed?0x27200:0;MEM32(0x27200)=i==2?5:10;
            bridge_IoStartPacket();assert(packet_calls==1 && xbox_CurrentIrql()==0);
        }
        packet_recursive=strstr(argv[1],"recursive")!=NULL;
        for(unsigned i=0;i<(packet_recursive?1:4);i++) {
            MEM32(g_esp)=device;MEM32(g_esp+4)=7;
            if(keyed)bridge_IoStartNextPacketByKey();else bridge_IoStartNextPacket();
        }
        assert(packet_calls==4 && !MEM32(device+0x10) && !BRIDGE_MEM8(device+0x2b));
        assert(packet_order[0]==0x28000 && packet_order[1]==0x28100);
        assert(packet_order[2]==(keyed?0x28300:0x28200));
        assert(packet_order[3]==(keyed?0x28200:0x28300));
        assert(xbox_CurrentIrql()==0 && !xbox_IrqlBlocksInterrupts());cleanup();return 0;
    }





    if(!strncmp(argv[1],"event_header_reset_",19)) {
        int multiple=strstr(argv[1],"multiple")!=NULL;
        _putenv_s("RECOMP_TITLE_KEVENTS","1");MEM32(0x26200)=0;MEM32(0x26204)=0;
        for(unsigned type=0;type<2;type++) {
            ke_initialize_header(0x26000,(uint8_t)type,4,1);
            int actual_type;assert(ke_guest_event(0x26000,&actual_type));
            MEM32(0x26004)=0;event_wait_call(multiple,0x80000);assert(g_eax==0x102);
            g_esp=0x80000;MEM32(g_esp)=0x26000;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;
            bridge_KeSetEvent();assert(g_eax==0);
            event_wait_call(multiple,0x80000);assert(g_eax==0 && MEM32(0x26004)==!type);
            MEM32(0x26004)=0;event_wait_call(multiple,0x80000);assert(g_eax==0x102);
            ke_shadow_remove(0x26000);
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"event_native_previous")) {
        HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);assert(event);
        bridge_write_handle(0x26600,event);g_esp=0x80000;
        MEM32(g_esp)=MEM32(0x26600);MEM32(g_esp+4)=0x26604;
        fixture_import(225);assert(g_eax==0 && MEM32(0x26604)==0);
        fixture_import(225);assert(g_eax==0 && MEM32(0x26604)==1);
        fixture_import(205);assert(g_eax==0 && MEM32(0x26604)==1);
        fixture_import(205);assert(g_eax==0 && MEM32(0x26604)==0);
        assert(xbox_KeSetEvent(event,0,FALSE)==0 && xbox_KeSetEvent(event,0,FALSE)==1);
        MEM32(g_esp)=0;fixture_import(225);assert((int32_t)g_eax<0);
        fixture_import(205);assert((int32_t)g_eax<0);
        cleanup();return 0;
    }

    if(!strncmp(argv[1],"event_consumption_",18)) {
        int resignal=strstr(argv[1],"resignal")!=NULL;
        int multiple=strstr(argv[1],"multiple")!=NULL;
        int title=strstr(argv[1],"title")!=NULL;
        if(title) _putenv_s("RECOMP_TITLE_KEVENTS","1");
        wait_consumed=CreateEventW(NULL,TRUE,FALSE,NULL);
        wait_resume=CreateEventW(NULL,TRUE,FALSE,NULL);assert(wait_consumed && wait_resume);
        MEM32(0x26200)=0;MEM32(0x26204)=0;g_esp=0x80000;
        MEM32(g_esp)=0x26000;MEM32(g_esp+4)=1;MEM32(g_esp+8)=1;
        if(title) {
            ke_initialize_header(0x26000,1,4,1);
        } else bridge_KeInitializeEvent();
        HANDLE worker=CreateThread(NULL,0,event_wait_probe,(void *)(uintptr_t)multiple,0,NULL);assert(worker);
        assert(WaitForSingleObject(wait_consumed,3000)==WAIT_OBJECT_0);
        if(resignal) {
            g_esp=0x80000;MEM32(g_esp)=0x26000;MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;
            bridge_KeSetEvent();assert(g_eax==0);
        } else {
            event_wait_call(multiple,0x80000);
            assert(g_eax==0x102); /* One signal cannot release two waiters. */
        }
        SetEvent(wait_resume);assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0);
        event_wait_call(multiple,0x80000);assert(g_eax==(resignal?0:0x102));
        event_wait_call(multiple,0x80000);assert(g_eax==0x102);
        CloseHandle(worker);CloseHandle(wait_consumed);CloseHandle(wait_resume);
        ke_shadow_remove(0x26000);cleanup();return 0;
    }

    if(!strncmp(argv[1],"scheduler_capacity",18)) {
        int load=strstr(argv[1],"load")!=NULL,save=strstr(argv[1],"save")!=NULL;
        int timer_only=strstr(argv[1],"timer")!=NULL;
        g_esp=0x80000;kernel_start_timer_worker();assert(xbox_CheckpointBootstrapBegin());
        /* Distinct valid guest objects, through the actual import dispatcher. */
        if(!timer_only) for(unsigned i=0;i<160;i++) {
            uint32_t va=0x40000+i*32;
            MEM32(g_esp)=va;MEM32(g_esp+4)=0x13000;MEM32(g_esp+8)=0;
            bridge_KeInitializeDpc();MEM32(g_esp+4)=i;MEM32(g_esp+8)=i+1;
            if(load) {BRIDGE_MEM8(va+2)=i&1;continue;}
            fixture_import(119);assert(g_eax==1 && BRIDGE_MEM8(va+2));
            assert(MEM32(va+20)==i && MEM32(va+24)==i+1);
        }
        if(!load && !timer_only) {
            MEM32(g_esp)=0x40000;fixture_import(119);assert(g_eax==0);
            for(unsigned i=0;i<160;i+=2) {
                MEM32(g_esp)=0x40000+i*32;bridge_KeRemoveQueueDpc();assert(g_eax==1);
            }
        }
        for(unsigned i=0;i<80;i++) {
            uint32_t va=0x50000+i*40;
            MEM32(g_esp)=va;MEM32(g_esp+4)=0;bridge_KeInitializeTimerEx();
            if(load) {BRIDGE_MEM8(va+3)=(i!=17 && i!=79);continue;}
            MEM32(g_esp+4)=(uint32_t)-600000000;MEM32(g_esp+8)=0xffffffffu;
            MEM32(g_esp+12)=0;fixture_import(149);
            assert(g_eax==0 && BRIDGE_MEM8(va+3));
        }
        if(!load) {
            assert(g_timer_scan_end==80 && g_timer_capacity>=80);
            /* Keep a hole and retire the last slot before serializing. */
            assert(xbox_kernel_cancel_timer(0x50000+17*40));
            assert(xbox_kernel_cancel_timer(0x50000+79*40));
            assert(g_timer_scan_end==79);
            uint32_t capacity=g_timer_capacity;
            kernel_set_timer(0x54000,-600000000,0,0);
            assert(g_eax==0 && g_timer_scan_end==79 && g_timer_capacity==capacity);
            assert(g_timers[17].timer_va==0x54000);
            assert(xbox_kernel_cancel_timer(0x54000));
        }
        FILE *state=save ? fopen(argv[3],"wb") : load ? fopen(argv[3],"rb") : tmpfile();assert(state);
        if(!load) assert(xbox_SaveKernelScheduler(state));
        if(save) {assert(fclose(state)==0);xbox_CheckpointEnd();cleanup();return 0;}
        if(!load) {
            /* Discard host storage before Apply, leaving guest identities intact. */
            rewind(state);memset(g_timers,0,g_timer_capacity*sizeof *g_timers);g_timer_scan_end=0;
            memset(g_dpc_queue,0,g_dpc_capacity*sizeof *g_dpc_queue);g_dpc_head=g_dpc_tail=0;
        }
        assert(xbox_LoadKernelScheduler(state));fclose(state);
        assert(g_timer_scan_end==79 && g_timer_capacity>=80);
        for(unsigned i=0;i<80;i++)
            assert(xbox_kernel_cancel_timer(0x50000+i*40)==(i!=17 && i!=79));
        assert(g_timer_scan_end==0 && g_timer_capacity>=80);
        kernel_set_timer(0x54000,-600000000,0,0);
        assert(g_eax==0 && g_timer_scan_end==1 && g_timers[0].timer_va==0x54000);
        kernel_set_timer(0x54000,-600000000,0,0);
        assert(g_eax==1 && g_timer_scan_end==1);
        assert(xbox_kernel_cancel_timer(0x54000) && g_timer_scan_end==0);
        if(!timer_only) {kernel_drain_dpcs();assert(dpc_calls==80);}
        xbox_CheckpointEnd();cleanup();return 0;
    }

    if(!strcmp(argv[1],"timer_scan_worker_rearm")) {
        timer_scan_fired=CreateEvent(NULL,TRUE,FALSE,NULL);assert(timer_scan_fired);
        g_esp=0x80000;kernel_start_timer_worker();guest_cpu_join();
        MEM32(g_esp)=0x52000;MEM32(g_esp+4)=0;bridge_KeInitializeTimerEx();
        MEM32(g_esp)=0x53000;MEM32(g_esp+4)=0x1b400;MEM32(g_esp+8)=0;bridge_KeInitializeDpc();
        kernel_set_timer(0x52000,-200000,0,0x53000);
        assert(g_eax==0 && g_timer_scan_end==1);guest_cpu_part();
        assert(WaitForSingleObject(timer_scan_fired,5000)==WAIT_OBJECT_0);
        /* Serialize with the worker before inspecting its second retirement. */
        guest_cpu_join();EnterCriticalSection(&g_timer_lock);
        assert(timer_scan_calls==2 && g_timer_scan_end==0);
        assert(g_timer_capacity==XBOX_MAX_TIMERS && !BRIDGE_MEM8(0x52003));
        LeaveCriticalSection(&g_timer_lock);guest_cpu_part();
        CloseHandle(timer_scan_fired);cleanup();return 0;
    }

    if(!strcmp(argv[1],"dpc_cpu_ownership")) {
        uint32_t va=0x26000;g_esp=0x80000;
        MEM32(g_esp)=va;MEM32(g_esp+4)=0x13000;MEM32(g_esp+8)=0;
        bridge_KeInitializeDpc();MEM32(g_esp+4)=11;MEM32(g_esp+8)=22;
        guest_cpu_join();bridge_KeInsertQueueDpc();assert(g_eax==1);
        HANDLE worker=CreateThread(NULL,0,drain_probe,NULL,0,NULL);assert(worker);
        ULONGLONG until=GetTickCount64()+3000;
        while(!InterlockedCompareExchange(&g_dpc_cpu_waiters,0,0) && GetTickCount64()<until) Sleep(1);
        assert(InterlockedCompareExchange(&g_dpc_cpu_waiters,0,0)>0 && !dpc_calls);
        bridge_KeRemoveQueueDpc();assert(g_eax==1 && !BRIDGE_MEM8(va+2));
        guest_cpu_part();assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0 && !dpc_calls);
        CloseHandle(worker);
        guest_cpu_join();assert(kernel_run_dpc(va,11,22));
        assert(guest_cpu_is_held() && dpc_calls==1 && g_esp==0x80000);
        assert(MEM32(va+20)==11 && MEM32(va+24)==22);
        guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"dispatcher_state")) {
        uint32_t va=0x26000,second=va+0x100,timeout=va+0x200,array=va+0x220;
        MEM32(timeout)=0;MEM32(timeout+4)=0;g_esp=0x80000;
        for(unsigned type=0;type<2;type++) {
            MEM32(g_esp)=va;MEM32(g_esp+4)=type;MEM32(g_esp+8)=0;
            bridge_KeInitializeEvent();
            MEM32(g_esp+4)=0;bridge_KeSetEvent();assert(g_eax==0 && MEM32(va+4)==1);
            bridge_KeSetEvent();assert(g_eax==1 && MEM32(va+4)==1);
            bridge_KeResetEvent();assert(g_eax==1 && MEM32(va+4)==0);
            bridge_KeResetEvent();assert(g_eax==0);
            bridge_KeSetEvent();bridge_KePulseEvent();assert(g_eax==1 && MEM32(va+4)==0);
            bridge_KeSetEvent();MEM32(g_esp+12)=0;MEM32(g_esp+16)=timeout;
            bridge_KeWaitForSingleObject();assert(g_eax==0 && MEM32(va+4)==!type);
            bridge_KeWaitForSingleObject();assert(g_eax==(type?0x102:0));
            ke_shadow_remove(va);
        }
        /* Timer and event waits share reset semantics and retain guest state. */
        for(unsigned type=0;type<2;type++) {
            MEM32(g_esp)=va;MEM32(g_esp+4)=type;bridge_KeInitializeTimerEx();
            MEM32(va+4)=1;assert(SetEvent(ke_shadow_lookup(va)));
            MEM32(g_esp+4)=0;MEM32(g_esp+8)=0;MEM32(g_esp+12)=0;MEM32(g_esp+16)=timeout;
            bridge_KeWaitForSingleObject();assert(g_eax==0 && MEM32(va+4)==!type);
            bridge_KeWaitForSingleObject();assert(g_eax==(type?0x102:0));
            ke_shadow_remove(va);
        }
        for(unsigned i=0;i<2;i++) {
            uint32_t object=i?second:va;
            MEM32(g_esp)=object;MEM32(g_esp+4)=1;MEM32(g_esp+8)=i;
            bridge_KeInitializeEvent();MEM32(array+i*4)=object;
        }
        MEM32(g_esp)=2;MEM32(g_esp+4)=array;MEM32(g_esp+8)=1;
        MEM32(g_esp+12)=0;MEM32(g_esp+16)=0;MEM32(g_esp+20)=0;
        MEM32(g_esp+24)=timeout;MEM32(g_esp+28)=0;
        bridge_KeWaitForMultipleObjects();assert(g_eax==1 && !MEM32(va+4) && !MEM32(second+4));
        for(unsigned i=0;i<2;i++) {
            uint32_t object=i?second:va;MEM32(object+4)=1;
            assert(SetEvent(ke_shadow_lookup(object)));
        }
        MEM32(g_esp+8)=0;bridge_KeWaitForMultipleObjects();
        assert(g_eax==0 && !MEM32(va+4) && !MEM32(second+4));
        bridge_KeWaitForMultipleObjects();assert(g_eax==0x102);
        MEM32(g_esp)=65;bridge_KeWaitForMultipleObjects();assert(g_eax==(uint32_t)STATUS_INVALID_PARAMETER);
        ke_shadow_remove(va);ke_shadow_remove(second);cleanup();return 0;
    }
    if(!strcmp(argv[1],"dpc_layout")) {
        uint32_t va=0x26000;g_esp=0x80000;memset(XBOX_TO_NATIVE(va),0xcd,32);
        MEM32(g_esp)=va;MEM32(g_esp+4)=0x12000;MEM32(g_esp+8)=0x1234;
        bridge_KeInitializeDpc();
        assert(BRIDGE_MEM16(va)==0x13 && !BRIDGE_MEM8(va+2));
        assert(MEM32(va+12)==0x12000 && MEM32(va+16)==0x1234);
        assert(MEM32(va+28)==0xcdcdcdcd);
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"synchronize_irql")) {
        uint32_t va=0x26000;g_esp=0x80000;
        MEM32(g_esp)=va;MEM32(g_esp+4)=0x11000;MEM32(g_esp+8)=0;
        MEM32(g_esp+12)=0x37;MEM32(g_esp+16)=20;MEM32(g_esp+20)=1;MEM32(g_esp+24)=0;
        bridge_KeInitializeInterrupt();
        guest_cpu_join();
        MEM32(g_esp+4)=0x12000;MEM32(g_esp+8)=0x1234;
        for(unsigned level=0;level<=2;level+=2) {
            xbox_KfRaiseIrql((KIRQL)level);
            bridge_KeSynchronizeExecution();assert(g_eax==0x5a && g_esp==0x80000);
            assert(guest_cpu_is_held() && xbox_CurrentIrql()==level);
        }
        xbox_KfLowerIrql(0);guest_cpu_part();cleanup();return 0;
    }

    if(!strcmp(argv[1],"interrupt_layout")) {
        uint32_t va=0x26000,other=va+0x100,irql=va+0x200;
        g_esp=0x80000;
        MEM32(g_esp)=7;MEM32(g_esp+4)=irql;bridge_HalGetInterruptVector();
        assert(g_eax==0x37 && BRIDGE_MEM8(irql)==20);
        for(unsigned i=0;i<2;i++) {
            uint32_t object=i?other:va;memset(XBOX_TO_NATIVE(object),0xcd,116);
            MEM32(g_esp)=object;MEM32(g_esp+4)=0x11000;MEM32(g_esp+8)=0x1234;
            MEM32(g_esp+12)=0x37;MEM32(g_esp+16)=20;MEM32(g_esp+20)=1;MEM32(g_esp+24)=0;
            bridge_KeInitializeInterrupt();
            assert(MEM32(object)==0x11000 && MEM32(object+4)==0x1234);
            assert(MEM32(object+8)==7 && MEM32(object+12)==20);
            assert(BRIDGE_MEM8(object+16)==0 && BRIDGE_MEM8(object+18)==1);
            assert(MEM32(object+112)==0xcdcdcdcd);
        }
        for(unsigned cycle=0;cycle<64;cycle++) {
            MEM32(g_esp)=va;bridge_KeConnectInterrupt();
            assert(g_eax==1 && BRIDGE_MEM8(va+16)==1 && xbox_GetConnectedInterrupt(7)==va);
            bridge_KeConnectInterrupt();assert(g_eax==0);
            MEM32(g_esp)=other;bridge_KeConnectInterrupt();assert(g_eax==0);
            bridge_KeDisconnectInterrupt();assert(xbox_GetConnectedInterrupt(7)==va);
            MEM32(g_esp)=va;bridge_KeDisconnectInterrupt();
            assert(BRIDGE_MEM8(va+16)==0 && !xbox_GetConnectedInterrupt(7));
            assert(kernel_raise_interrupt(7)==0);
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"timer_cancel_layout")) {
        uint32_t va=0x26000;g_esp=0x80000;
        MEM32(g_esp)=va;MEM32(g_esp+4)=0;bridge_KeInitializeTimerEx();
        BRIDGE_MEM8(va+3)=1;
        MEM32(va+24)=va+24;MEM32(va+28)=va+24;
        unsigned char expected[40];memcpy(expected,XBOX_TO_NATIVE(va),40);expected[3]=0;
        InitializeCriticalSection(&g_timer_lock);g_timer_started=1;
        g_timer_capacity=XBOX_MAX_TIMERS;g_timers=calloc(g_timer_capacity,sizeof *g_timers);assert(g_timers);
        g_timers[0]=(XboxTimer){va,0,123456,10};g_timer_scan_end=1;
        bridge_KeCancelTimer();assert(g_eax==1 && !g_timers[0].timer_va);
        assert(!memcmp(expected,XBOX_TO_NATIVE(va),40));
        bridge_KeCancelTimer();assert(g_eax==0 && !memcmp(expected,XBOX_TO_NATIVE(va),40));
        ke_shadow_remove(va);cleanup();return 0;
    }
    if(!strcmp(argv[1],"guest_layout")) {
        uint32_t va=0x25000;
        g_esp=0x80000;
        /* Independent 32-bit Xbox ABI offsets; trailing bytes belong to callers. */
        for(unsigned type=0;type<2;type++) {
            memset(XBOX_TO_NATIVE(va),0xcd,48);
            MEM32(g_esp)=va;MEM32(g_esp+4)=type;bridge_KeInitializeTimerEx();
            assert(BRIDGE_MEM8(va)==8+type && BRIDGE_MEM8(va+2)==10);
            assert(MEM32(va+4)==0 && MEM32(va+8)==va+8 && MEM32(va+12)==va+8);
            assert(MEM32(va+16)==0 && MEM32(va+20)==0 && MEM32(va+36)==0);
            assert(MEM32(va+40)==0xcdcdcdcd);
            HANDLE timer=ke_shadow_lookup(va);assert(timer && WaitForSingleObject(timer,0)==WAIT_TIMEOUT);
            assert(SetEvent(timer) && WaitForSingleObject(timer,0)==WAIT_OBJECT_0);
            assert(WaitForSingleObject(timer,0)==(type?WAIT_TIMEOUT:WAIT_OBJECT_0));
            ke_shadow_remove(va);
            for(unsigned initial=0;initial<2;initial++) {
                memset(XBOX_TO_NATIVE(va),0xcd,20);
                MEM32(g_esp)=va;MEM32(g_esp+4)=type;MEM32(g_esp+8)=initial;
                bridge_KeInitializeEvent();
                assert(BRIDGE_MEM8(va)==type && BRIDGE_MEM8(va+2)==4);
                assert(MEM32(va+4)==initial && MEM32(va+8)==va+8 && MEM32(va+12)==va+8);
                assert(MEM32(va+16)==0xcdcdcdcd);
                HANDLE event=ke_shadow_lookup(va);assert(event);
                assert(WaitForSingleObject(event,0)==(initial?WAIT_OBJECT_0:WAIT_TIMEOUT));
                assert(WaitForSingleObject(event,0)==(initial&&!type?WAIT_OBJECT_0:WAIT_TIMEOUT));
                ke_shadow_remove(va);
            }
        }
        memset(XBOX_TO_NATIVE(va),0xcd,16);MEM32(g_esp)=va;bridge_KeInitializeDeviceQueue();
        assert(BRIDGE_MEM16(va)==0x14 && BRIDGE_MEM8(va+2)==12 && BRIDGE_MEM8(va+3)==0);
        assert(MEM32(va+4)==va+4 && MEM32(va+8)==va+4 && MEM32(va+12)==0xcdcdcdcd);
        unsigned types[]={2,7,0x3a,0x3b,0x22};
        memset(XBOX_TO_NATIVE(0x26000),0,64);
        for(unsigned t=0;t<5;t++) for(unsigned ext=0;ext<2;ext++) {
            uint32_t extension_size=ext?0x170:0;
            MEM32(g_esp)=0x26000;MEM32(g_esp+4)=extension_size;
            MEM32(va+0x100)=0x000c000b;MEM32(va+0x104)=va+0x110;
            memcpy(XBOX_TO_NATIVE(va+0x110),"\\Device\\dev",12);
            MEM32(g_esp+8)=ext?va+0x100:0;
            MEM32(g_esp+12)=types[t];MEM32(g_esp+16)=ext;MEM32(g_esp+20)=va;
            bridge_IoCreateDevice();assert(g_eax==0 && MEM32(va));uint32_t device=MEM32(va);
            assert(BRIDGE_MEM16(device)==3 && BRIDGE_MEM16(device+2)==0x48+extension_size);
            assert(MEM32(device+4)==0 && MEM32(device+8)==0x26000);
            assert(BRIDGE_MEM8(device+0x1c)==types[t] && BRIDGE_MEM8(device+0x1d)==0);
            assert(BRIDGE_MEM8(device+0x1e)==1 && BRIDGE_MEM8(device+0x1f)==0);
            assert(MEM32(device+0x14)==(ext?0x1a:0x10));
            assert(BRIDGE_MEM16(device+0x28)==0x14 && BRIDGE_MEM8(device+0x2a)==12);
            assert(MEM32(device+0x2c)==device+0x2c && MEM32(device+0x30)==device+0x2c);
            assert(MEM32(device+0x18)==(ext?device+0x48:0));
            if(ext) {
                for(unsigned j=0;j<extension_size;j++) assert(BRIDGE_MEM8(device+0x48+j)==0);
                /* The original memory-unit driver fills 0x170 extension bytes. */
                memset(XBOX_TO_NATIVE(MEM32(device+0x18)),0x5a,extension_size);
            }
            if(t<4) {
                assert(MEM32(device+12)==0 && BRIDGE_MEM8(device+0x34)==1);
                assert(BRIDGE_MEM8(device+0x36)==4 && MEM32(device+0x38)==1);
                assert(MEM32(device+0x3c)==device+0x3c && MEM32(device+0x40)==device+0x3c);
                HANDLE lock=ke_shadow_lookup(device+0x34);assert(lock);
                assert(WaitForSingleObject(lock,0)==WAIT_OBJECT_0 && WaitForSingleObject(lock,0)==WAIT_TIMEOUT);
            } else assert(MEM32(device+12)==device && !ke_shadow_lookup(device+0x34));
            assert(MEM32(device+0x44)==0);
            MEM32(g_esp)=device;bridge_IoDeleteDevice();assert(!ke_shadow_lookup(device+0x34));
        }
        cleanup();return 0;
    }
    if(!strcmp(argv[1],"closed_source_probe")) {
        HANDLE hole=CreateEventW(NULL,TRUE,FALSE,NULL);assert(hole);
        HANDLE handle;assert(xbox_NtCreateEvent(&handle,NULL,0,FALSE)==0);bridge_write_handle(0x20030,handle);CloseHandle(hole);
        uint32_t source=MEM32(0x20030);duplicate(source,0x20040,3);DWORD flags;
        printf("source_closed=%d table_live=%d\n",!GetHandleInformation(handle,&flags),bridge_resolve_handle(source)!=NULL);
        printf("native_reused=%d source_slot_closed=%d\n",handle==bridge_read_handle(0x20040),
               bridge_resolve_handle(source)!=NULL && !GetHandleInformation(bridge_resolve_handle(source),&flags));
        uint32_t current=MEM32(0x20040);bridge_note_async_handle(current);
        for(unsigned pass=0;pass<64;pass++) {
            duplicate(current,0x20040,1);current=MEM32(0x20040);
            unsigned live=0,asynchronous=0;
            for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++)live+=s_handle_table[i]!=NULL;
            for(unsigned i=0;i<BRIDGE_ASYNC_MAX;i++)asynchronous+=g_async_handles[i]!=0;
            assert(live==1 && asynchronous==1 && bridge_handle_is_async(current));
            assert(SetEvent(bridge_resolve_handle(current)) && WaitForSingleObject(bridge_resolve_handle(current),0)==WAIT_OBJECT_0);
        }
        MEM32(0x20040)=0xabcdef;g_esp=0x80000;MEM32(g_esp)=BRIDGE_HANDLE_TAG|12000;
        MEM32(g_esp+4)=0x20040;MEM32(g_esp+8)=0;bridge_NtDuplicateObject();
        assert(g_eax==0xc0000008u && !MEM32(0x20040));
        make_objects();duplicate(MEM32(0x20050),0x20058,3);
        assert(bridge_handle_is_async(MEM32(0x20058)));
        XboxFileOpenState file_state;assert(xbox_CaptureFileOpenState(bridge_read_handle(0x20058),&file_state) && file_state.position==4);
        assert(xbox_CheckpointBootstrapBegin());FILE *out=tmpfile();assert(out && xbox_CheckpointWriteObjects(out));fclose(out);
        xbox_CheckpointEnd();cleanup();return 0;
    }

    if(!strncmp(argv[1],"machine_",8)) {
        FILE *image=fopen(argv[2],"rb");assert(image);fseek(image,0,SEEK_END);long size=ftell(image);rewind(image);
        void *data=malloc(size);assert(data && fread(data,size,1,image)==1 && xbox_CheckpointSetGameImage(data,size));fclose(image);free(data);
        assert(xbox_ConnectNv2a() && xbox_ConnectApu());nv2a_get_state()->pfifo.method_consumer=nv2a_pb_exec_method;
        HANDLE worker;
        if(!strcmp(argv[1],"machine_save")) {
            XBOX_ANSI_STRING link={6,7,"\\??\\U:"},target={42,43,"\\Device\\Harddisk0\\Partition1\\checkpoint"};
            target.Length=(USHORT)strlen(target.Buffer);target.MaximumLength=target.Length+1;
            assert(xbox_IoCreateSymbolicLink(&link,&target)==0);
            MEM32(0x2400c)=xbox_LookupSymbolicLinkTarget("\\??\\U:");assert(MEM32(0x2400c));
            MEM32(0x24004)=0x13579bdf;
            MEM32(0x24008)=(uint32_t)xbox_MmGetPhysicalAddress((void *)(uintptr_t)0x24004);
            make_objects();RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);recomp_cont_site(&creator,0x100009000ULL);
            worker=bridge_spawn_thread(sub_00011000,0,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(worker);bridge_write_handle(0x20044,worker);
            ULONGLONG deadline=GetTickCount64()+3000;while(!MEM32(0x20004)){assert(GetTickCount64()<deadline);Sleep(1);}
            FILE *state=fopen(argv[3],"wb");assert(state && xbox_CheckpointSaveMachine(state,3000));fclose(state);
            assert(xbox_IoDeleteSymbolicLink(&link)==0);
            /* Loading resumes the worker before closing archive payloads, so
             * its live counter is not a stable observation of restored RAM. */
            MEM32(0x20000)=0xdeadbeef;MEM32(0x24004)=0xdeadbeef;
            state=fopen(argv[3],"rb");assert(state && xbox_CheckpointLoadMachine(state,0,3000));fclose(state);
            assert(MEM32(0x20000)==1);worker=bridge_read_handle(0x20044);
        } else {
            FILE *state=fopen(argv[3],"rb");assert(state);
            if(!strcmp(argv[1],"machine_bad")) {
                MEM32(0x20000)=0xdeadbeef;assert(!xbox_CheckpointLoadMachine(state,1,3000) && MEM32(0x20000)==0xdeadbeef);fclose(state);return 0;
            }
            assert(xbox_CheckpointLoadMachine(state,1,3000));fclose(state);assert(MEM32(0x20000)==1);worker=bridge_read_handle(0x20044);
        }
        assert(xbox_LookupSymbolicLink("\\??\\U:") && !strcmp(xbox_LookupSymbolicLink("\\??\\U:"),"\\Device\\Harddisk0\\Partition1\\checkpoint"));
        assert(xbox_LookupSymbolicLinkTarget("\\??\\U:")==MEM32(0x2400c));
        XBOX_ANSI_STRING linked_name={12,12,"U:\\asset.bin"};XBOX_OBJECT_ATTRIBUTES linked_attrs={NULL,&linked_name,0x40};
        HANDLE linked_handle;XBOX_IO_STATUS_BLOCK linked_ios;DWORD got;char linked_byte;
        assert(xbox_NtOpenFile(&linked_handle,XBOX_GENERIC_READ,&linked_attrs,&linked_ios,7,0)==0);
        assert(ReadFile(linked_handle,&linked_byte,1,&got,NULL) && got==1 && linked_byte=='k');
        assert(xbox_NtClose(linked_handle)==0);
        WCHAR restored_path[MAX_PATH],expected_path[MAX_PATH];
        assert(xbox_translate_path("D:\\asset.bin",restored_path,MAX_PATH));
        swprintf_s(expected_path,MAX_PATH,L"%hs\\asset.bin",argv[6]);assert(!_wcsicmp(restored_path,expected_path));
        assert(xbox_ResolvePhysicalBuffer(MEM32(0x24008),4)==XBOX_PTR(0x24004));
        assert(*(uint32_t *)xbox_ResolvePhysicalBuffer(MEM32(0x24008),4)==0x13579bdf);
        MEM32(0x20020)=1;assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0 && MEM32(0x20008)==1);
        cleanup();return 0;
    }
    uint32_t saved[3];HANDLE worker;
    if(!strcmp(argv[1],"save") || !strcmp(argv[1],"hot")) {
        make_objects();RecompContFrame creator;recomp_cont_enter(&creator,0x9000,NULL,0);
        recomp_cont_site(&creator,0x100009000ULL);
        worker=bridge_spawn_thread(sub_00011000,0,0,xbox_AllocThreadStack());recomp_cont_leave(&creator);assert(worker);
        bridge_write_handle(0x20044,worker);
        ULONGLONG deadline=GetTickCount64()+3000;
        while(!MEM32(0x20004)) { assert(GetTickCount64()<deadline);Sleep(1); }
        assert(xbox_CheckpointBegin(3000));saved[0]=MEM32(0x20004);
        XboxCriticalSectionState critical[4];unsigned n;
        assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2);
        saved[1]=critical[0].site;saved[2]=MEM32(0x2003c);
        FILE *memory=fopen(argv[3],"wb"),*threads=fopen(argv[4],"wb"),*objects=fopen(argv[5],"wb");assert(memory && threads && objects);
        assert(fwrite(saved,sizeof saved,1,memory)==1 && xbox_SavePhysicalMemory(memory));
        assert(xbox_CheckpointWriteThreads(threads) && xbox_CheckpointWriteObjects(objects));
        fclose(memory);fclose(threads);fclose(objects);verify_objects();xbox_CheckpointEnd();
        if(!strcmp(argv[1],"hot")) {
            Sleep(30);assert(MEM32(0x20004)>saved[0] && xbox_CheckpointBegin(3000));
            memory=fopen(argv[3],"rb");threads=fopen(argv[4],"rb");objects=fopen(argv[5],"rb");assert(memory && threads && objects);
            void *prepared=xbox_CheckpointPrepareThreads(threads,4);assert(prepared && xbox_CheckpointQuiesceThreads(prepared));
            assert(fread(saved,sizeof saved,1,memory)==1 && xbox_LoadPhysicalMemory(memory));fclose(memory);fclose(threads);
            XboxCheckpointRoot roots[4];unsigned count;
            assert(xbox_CheckpointApplyThreads(prepared,roots,&count) && count==1);xbox_CheckpointFreeThreads(prepared);worker=roots[0].handle;
            assert(xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);
            assert(GetThreadId(bridge_read_handle(0x20044))==GetThreadId(worker));
            XboxCriticalSectionState critical[4];unsigned n;
            assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2 && critical[0].site==saved[1]);
            assert(MEM32(0x20000)==1 && MEM32(0x20004)==saved[0]);
            verify_objects();xbox_CheckpointEnd();
        }

    } else {
        assert(xbox_CheckpointBootstrapBegin());
        FILE *memory=fopen(argv[3],"rb"),*threads=fopen(argv[4],"rb"),*objects=fopen(argv[5],"rb");assert(memory && threads && objects);
        assert(fread(saved,sizeof saved,1,memory)==1 && xbox_LoadPhysicalMemory(memory));fclose(memory);
        XboxCheckpointRoot roots[4];unsigned count;
        assert(xbox_CheckpointLoadThreads(threads,roots,4,&count) && count==1);fclose(threads);worker=roots[0].handle;
        if(!strcmp(argv[1],"reject")) {
            assert(!xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);
            for(unsigned i=1;i<BRIDGE_HANDLE_MAX;i++) assert(!s_handle_table[i]);
            assert(MEM32(0x20000)==1 && MEM32(0x20004)==saved[0]);return 0;
        }
        assert(xbox_CheckpointLoadObjects(objects,roots,count));fclose(objects);assert(checkpoint_native_init());
        assert(GetThreadId(bridge_read_handle(0x20044))==GetThreadId(worker) && MEM32(0x2003c)==saved[2]);
        struct { LONG count;BOOLEAN owned,abandoned; } mutant;
        struct { HANDLE process,thread; } owner;
        HANDLE handle=bridge_read_handle(0x2003c);
        assert(checkpoint_native.mutant(handle,0,&mutant,sizeof mutant,NULL)==0 && mutant.count==-1);
        assert(checkpoint_native.mutant(handle,1,&owner,sizeof owner,NULL)==0 && (DWORD)(uintptr_t)owner.thread==GetThreadId(worker));
        assert(WaitForSingleObject(handle,0)==WAIT_TIMEOUT);
        XboxCriticalSectionState critical[4];unsigned n;
        assert(xbox_CaptureCriticalSections(critical,4,&n) && n==1 && critical[0].depth==2 && critical[0].site==saved[1]);
        assert(critical[0].owner[1]==roots[0].stack_top && MEM32(0x20004)==saved[0]);
        verify_objects();xbox_CheckpointEnd();
    }
    Sleep(30);assert(MEM32(0x20000)==1 && MEM32(0x20004)>saved[0]);MEM32(0x20020)=1;
    assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0 && MEM32(0x20008)==1);
    HANDLE mutex=bridge_read_handle(0x2003c);assert(WaitForSingleObject(mutex,0)==WAIT_OBJECT_0 && ReleaseMutex(mutex));
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(0x200a0));
    if(strcmp(argv[1],"save")) CloseHandle(worker);cleanup();return 0;
}
'''
    patch=pytest.MonkeyPatch()
    try:body=translate(patch,True,program)
    finally:patch.undo()
    file_source=(root/'src/kernel/kernel_file.c').read_text(encoding='utf-8')
    file_source=re.sub(r's_file_open_policy\[([^\]]+)\]\.handle',r'file_policy_test_handle(\1)',file_source)
    marker='static FileOpenPolicy s_file_open_policy[16384];'
    assert file_source.count(marker)==1
    file_source=file_source.replace(marker,marker+'\nstatic unsigned long long file_policy_test_reads;\nstatic HANDLE file_policy_test_handle(unsigned i) {++file_policy_test_reads;return s_file_open_policy[i].handle;}')
    (work/'file_policy_counted.c').write_text(file_source,encoding='utf-8')
    (work/'objects.c').write_text(prefix+body+suffix,encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(root/'src'),'/I'+str(root/'src/kernel'),'/I'+str(root.parents[1]/'config'),str(work/'objects.c'),
            str(root/'src/kernel/xbox_memory_layout.c'),str(root/'src/kernel/recomp_continuation.c'),
            str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/kernel_sync.c'),
        '/Fe:'+str(work/'objects.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    return work/'objects.exe',root/'tools/conformance/test.xbe'


def test_original_object_aliases_counts_positions_and_recursive_owners(native_objects,tmp_path):
    executable,xbe=native_objects;memory=tmp_path/'memory.state';threads=tmp_path/'threads.state';objects=tmp_path/'objects.state'
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    def run(mode,path=objects):
        result=subprocess.run([str(executable),mode,str(xbe),str(memory),str(threads),str(path),str(tmp_path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr
        return result.stdout
    run('save');run('load')
    damaged=tmp_path/'damaged.state';data=objects.read_bytes()
    for contents in [data[:100],b'BROKEN'+data[6:],data[:-1]]:
        damaged.write_bytes(contents);run('reject',damaged)


def test_save_directory_alias_accepts_original_repeated_separators(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'save_directory_alias',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_duplicate_close_source_preserves_actual_native_identity_and_file_policy(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'closed_source_probe',str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'source_slot_closed=0' in result.stdout


def test_original_live_root_and_recursive_owners_resume_without_replaying_entry(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'hot',str(xbe),str(tmp_path/'memory.state'),
        str(tmp_path/'threads.state'),str(tmp_path/'objects.state'),str(tmp_path)],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_mandatory_machine_archive_same_fresh_process_and_integrity(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    (tmp_path/'checkpoint').mkdir()
    (tmp_path/'checkpoint/asset.bin').write_bytes(b'k')
    state=tmp_path/'machine.state'
    def run(mode,path=state):
        result=subprocess.run([str(executable),mode,str(xbe),str(path),'-','-',str(tmp_path)],capture_output=True,text=True,errors='replace',timeout=30)
        assert result.returncode==0,result.stdout+result.stderr
    run('machine_save');run('machine_load')
    bad=tmp_path/'bad-machine.state'
    data=bytearray(state.read_bytes());data[-1]^=1;bad.write_bytes(data);run('machine_bad',bad)


def test_guest_device_event_timer_and_queue_layouts(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'guest_layout',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['interrupt_layout','timer_cancel_layout'])
def test_guest_kernel_lifecycle_uses_the_registered_objects(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['dispatcher_state','dpc_layout','synchronize_irql','dpc_cpu_ownership'])
def test_guest_dispatcher_state_and_synchronized_callbacks(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['event_consumption_single','event_consumption_multiple',
                                'event_consumption_single_resignal','event_consumption_multiple_resignal',
                                'event_consumption_single_title','event_consumption_multiple_title',
                                'event_consumption_single_resignal_title','event_consumption_multiple_resignal_title'])
def test_guest_event_consumption_interleavings(native_objects,tmp_path,mode):
    exe,image=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(exe),mode,str(image),str(tmp_path/'objects.bin'),
                           str(tmp_path/'memory.bin'),str(tmp_path/'threads.bin'),str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['event_header_reset_single','event_header_reset_multiple','event_native_previous'])
def test_guest_event_header_reset_and_native_previous_state(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['device_packets_abi','device_queues','device_packets_fifo','device_packets_keyed','device_packets_recursive'])
def test_guest_device_queue_and_original_packet_entry_points(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['irp_layout','irp_ioctl','irp_fsd','irp_dispatch'])
def test_original_irp_layout_request_fields_and_driver_dispatch(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr

@pytest.mark.parametrize('mode',['apc_layout','apc_order','apc_user_wait','irp_unwind','irp_hold','irp_free_completion','irp_final_completion','irp_sync_call'])
def test_thread_apcs_and_original_irp_completion(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr

@pytest.mark.parametrize('mode',['completion_port','completion_checkpoint','queue_order'])
def test_original_completion_port_packets_and_queue_lifetime(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr

def test_original_cross_thread_apc_delivery_and_exit_cleanup(native_objects,tmp_path):
    exe,image=native_objects
    result=subprocess.run([str(exe),'apc_cross_thread',str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['cpu_exception','raise_exception','nested_exception','noncontinuable_exception','capture_context','transfer_wait_owner'])
def test_original_exception_context_dispatch_and_resume(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==(5 if mode=='noncontinuable_exception' else 0),result.stdout+result.stderr
    if mode=='noncontinuable_exception':assert '[STOP] unhandled guest exception' in result.stderr


@pytest.mark.parametrize('mode',['terminate_root','terminate_worker'])
def test_thread_termination_discards_guest_frames_and_returns_native_status(native_objects,tmp_path,mode):
    exe,image=native_objects
    result=subprocess.run([str(exe),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('kind',['event','thread','file'])
def test_object_reference_identity_lifetime_and_checkpoint(native_objects,tmp_path,kind):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'object_reference_'+kind,str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_wait_retains_object_after_source_handle_closes(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'wait_closed_source',str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_wait_closed_caller_handle_survives_cold_checkpoint(native_objects,tmp_path):
    executable,xbe=native_objects
    for mode in ['wait_owner_save','wait_owner_load']:
        result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'memory.state'),
            str(tmp_path/'threads.state'),str(tmp_path/'objects.state'),str(tmp_path)],
            capture_output=True,text=True,errors='replace',timeout=30)
        assert result.returncode==0,result.stdout+result.stderr


def test_signal_and_wait_uses_real_signal_wait_and_error_results(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'signal_wait_contract',str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize("mode",["timer_handle_contract","timer_thread_rundown"])
def test_timer_handles_share_scheduler_state_apcs_and_reference_lifetime(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'unused','unused','unused',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_timer_handle_wait_survives_close_and_cold_checkpoint(native_objects,tmp_path):
    executable,xbe=native_objects
    for mode in ['wait_timer_save','wait_timer_load']:
        result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'memory.state'),
            str(tmp_path/'threads.state'),str(tmp_path/'objects.state'),str(tmp_path)],
            capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['success','error','cancel','detach','create','sync','apc','checkpoint'])
def test_irp_completion_releases_owned_objects(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(bytes(range(32)))
    result=subprocess.run([str(executable),'irp_owners_'+mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_irp_queue_uses_recorded_issuing_thread(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'irp_queue_owner',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('kind',['event','semaphore','mutant','timer'])
def test_named_object_reuses_identity_and_initial_state(native_objects,tmp_path,kind):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'named_object_'+kind,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr

@pytest.mark.parametrize('mode',['named_directory','named_permanent','named_native','named_wait_lifetime','named_completion'])
def test_named_namespace_lifetime_and_directory_identity(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_named_namespace_cold_restore_preserves_new_name_and_old_wait_identity(native_objects,tmp_path):
    executable,xbe=native_objects
    for mode in ['named_cold_save','named_cold_load']:
        result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'memory.state'),'-',str(tmp_path/'objects.state'),str(tmp_path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr


def test_symbolic_link_handles_query_actual_target_and_original_buffer_contract(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'checkpoint').mkdir()
    (tmp_path/'checkpoint/asset.bin').write_bytes(b'a')
    result=subprocess.run([str(executable),'symbolic_link_contract',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_symbolic_link_resolves_and_retains_original_namespace_objects(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'symbolic_object_targets',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_symbolic_link_preserves_full_ansi_target_capacity(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'symbolic_full_ansi_target',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_device_namespace_references_pending_deletion_and_driver_callback(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'device_object_contract',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_builtin_device_paths_share_namespace_and_preserve_language_mapping(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'MediaRoot'/'Media').mkdir(parents=True)
    (tmp_path/'MediaRoot'/'Media'/'asset.bin').write_bytes(b'e')
    (tmp_path/'user').mkdir()
    result=subprocess.run([str(executable),'device_paths',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


def test_file_object_preserves_guest_open_options_and_lock_contract(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(b'file-options')
    result=subprocess.run([str(executable),'file_open_options',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


def test_file_device_ownership_survives_duplicates_restore_and_pending_delete(native_objects,tmp_path):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(b'ownership')
    result=subprocess.run([str(executable),'file_device_ownership',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('kind',['optical','disk'])
def test_volume_dismount_detaches_old_identity_and_remounts_without_reusing_handles(native_objects,tmp_path,kind):
    executable,xbe=native_objects
    (tmp_path/'asset.bin').write_bytes(b'asset')
    result=subprocess.run([str(executable),'volume_dismount_'+kind,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


def test_optical_authentication_uses_real_file_and_rejects_unperformed_controls(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'optical_file_control',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['0000','0001','0010','0011','0100','0101','0110','0111',
                                 '1000','1001','1010','1011','1100','1101','1110','1111'])
@pytest.mark.parametrize('native',[False,True])
def test_file_control_producer_preserves_original_dispatch_completion_and_lock_contract(native_objects,tmp_path,mode,native):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'control_request_'+mode+('n' if native else ''),str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


def test_control_thread_exit_releases_call_owned_file_lock_and_reference(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'control_request_0100t',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['00','01','10','11'])
def test_file_cleanup_close_and_completion_context_follow_original_lifetime(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'file_lifecycle_'+mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['scheduler_capacity','scheduler_capacity_timer'])
def test_scheduler_retains_more_than_initial_queue_and_timer_capacity(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr



def test_scheduler_expanded_storage_cold_restore(native_objects,tmp_path):
    executable,xbe=native_objects
    for mode in ['scheduler_capacity_save','scheduler_capacity_load']:
        result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'scheduler.state'),'-','-',str(tmp_path)],
                              capture_output=True,text=True,errors='replace',timeout=20)
        assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode',['generic_create_success','generic_create_pending','generic_create_error','generic_create_import','generic_create_relative','generic_create_exclusive'])
def test_generic_driver_create_preserves_file_irp_and_lifetime(native_objects,tmp_path,mode):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),mode,str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


def test_directory_backend_rejects_unperformed_raw_volume_controls(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'native_raw_volume_controls',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('mode,code',[
    ('unwind_nested',None),('unwind_target',None),('unwind_synthetic',None),('unwind_collision',None),
    ('unwind_all',None),('unwind_empty',None),('unwind_bad_target','C0000029'),
    ('unwind_missing_target','C0000029'),('unwind_bad_stack','C0000028'),
    ('unwind_bad_disposition','C0000026'),('unwind_exit','11111111')])
def test_original_rtl_unwind_context_chain_and_dispositions(native_objects,tmp_path,mode,code):
    executable,image=native_objects
    result=subprocess.run([str(executable),mode,str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace')
    assert result.returncode==(5 if code else 0),result.stdout+result.stderr
    if code:assert code in result.stderr,result.stderr


def test_original_tls_owned_stack_dispatch_and_unwind(native_objects,tmp_path):
    executable,image=native_objects
    result=subprocess.run([str(executable),'tls_owned_stack',str(image),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_original_dpc_stack_and_unwind_extension(native_objects,tmp_path):
    executable,image=native_objects
    result=subprocess.run([str(executable),'dpc_stack_contract',str(image),'-','-','-',str(tmp_path)],capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_timer_worker_trims_expired_tail_and_callback_rearms(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'timer_scan_worker_rearm',str(xbe),'-','-','-',str(tmp_path)],
                          capture_output=True,text=True,errors='replace',timeout=20)
    assert result.returncode==0,result.stdout+result.stderr

def test_wait_closed_caller_handle_survives_hot_checkpoint(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'wait_owner_hot_save',str(xbe),str(tmp_path/'memory.state'),
        str(tmp_path/'threads.state'),str(tmp_path/'objects.state'),str(tmp_path)],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_original_file_policy_reuses_holes_and_retires_empty_tail(native_objects,tmp_path):
    executable,xbe=native_objects
    result=subprocess.run([str(executable),'file_policy_bounds',str(xbe),'-','-','-',str(tmp_path)],
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FILE POLICY] holes/unique/close-source/tail/full16384/overflow passed' in result.stderr
