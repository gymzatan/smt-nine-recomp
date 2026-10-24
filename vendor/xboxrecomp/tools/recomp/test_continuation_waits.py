"""Persist the real kernel-thunk wait and resume through its original bridge."""
from pathlib import Path
import json
import shutil
import subprocess

import pytest

from tools.recomp.test_continuations import translate


@pytest.fixture(scope='module',params=['dpc_wait', 'delay', 'multiple_any', 'multiple_all', 'signal_wait', 'apc_wait', 'apc_post', 'apc_loop', 'irp_dispatch', 'irp_complete', 'irp_invalid', 'sync_fsd', 'sync_ioctl', 'sync_pending', 'queue_timeout', 'completion_timeout', 'queue_ready', 'completion_ready', 'terminate_apc', 'terminate_irp', 'natural_apc', 'natural_irp', 'device_delete', 'device_delete_freed', 'control_dispatch', 'control_lock', 'control_pending', 'native_control_complete', 'file_device_raw', 'file_device_shadow', 'file_device_reference', 'file_device_apc', 'file_device_error', 'file_device_cancel', 'file_device_control', 'file_device_cleanup', 'file_device_close', 'file_device_cleanup_pending', 'file_device_close_pending', 'file_device_exit', 'file_device_nested_exit', 'file_device_jump', 'exception_wait', 'exception_nested', 'exception_divide', 'file_create_dispatch', 'file_create_pending', 'file_create_exit', 'unwind_wait', 'unwind_collision', 'unwind_all', 'unwind_synthetic', 'unwind_nested'])
def native_waits(tmp_path_factory,request):
    root=Path(__file__).resolve().parents[2]
    work=tmp_path_factory.mktemp('continuation_wait')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    seh_base=0x810000
    dpc=request.param=='dpc_wait'
    unwind=request.param.startswith('unwind_')
    unwind_collision=request.param=='unwind_collision'
    unwind_all=request.param=='unwind_all'
    unwind_synthetic=request.param=='unwind_synthetic'
    exception=request.param.startswith('exception_')
    exception_nested=request.param=='exception_nested'
    exception_divide=request.param=='exception_divide'
    stack_top=0x800000 if dpc or unwind or exception else 0x80000
    signal_wait=request.param=='signal_wait'
    apc_wait=request.param.startswith('apc_')
    apc_post=request.param=='apc_post'
    apc_loop=request.param=='apc_loop'
    file_create=request.param.startswith('file_create_')
    create_pending=request.param=='file_create_pending'
    create_exit=request.param=='file_create_exit'
    file_device=request.param.startswith('file_device_')
    file_shadow=file_device and request.param!='file_device_raw'
    file_reference=request.param=='file_device_reference'
    file_packet=request.param in ('file_device_apc','file_device_error','file_device_cancel')
    file_error=request.param=='file_device_error'
    file_cancel=request.param=='file_device_cancel'
    file_nested_exit=request.param=='file_device_nested_exit'
    file_exit=request.param=='file_device_exit' or file_nested_exit
    file_jump=request.param=='file_device_jump'
    file_control=request.param=='file_device_control' or file_exit or file_jump
    file_lifecycle=request.param.startswith(('file_device_cleanup','file_device_close'))
    file_cleanup=file_lifecycle and 'cleanup' in request.param
    file_pending=file_lifecycle and request.param.endswith('_pending')
    device_delete=request.param.startswith('device_delete')
    device_freed=request.param=='device_delete_freed'
    control_native=request.param=='native_control_complete'
    control=request.param.startswith('control_') or control_native
    control_dispatch=request.param=='control_dispatch'
    control_lock=request.param=='control_lock'
    control_pending=request.param=='control_pending'
    control_external=control_lock or control_pending
    irp=request.param.startswith('irp_')
    irp_invalid=request.param=='irp_invalid'
    irp_complete=request.param in ('irp_complete','irp_invalid')
    sync=request.param.startswith('sync_')
    sync_ioctl=request.param=='sync_ioctl'
    sync_pending=request.param=='sync_pending'
    queue=request.param.startswith(('queue_','completion_'))
    completion=request.param.startswith('completion_')
    queue_ready=queue and request.param.endswith('_ready')
    natural_exit=request.param.startswith('natural_')
    terminate=natural_exit or request.param.startswith('terminate_')
    terminate_irp=terminate and request.param.endswith('_irp')
    multiple=request.param.startswith('multiple_')
    wait_all=request.param=='multiple_all'
    args=(bytes.fromhex('68800002006a006a006a')+bytes([0 if wait_all else 1])+
          bytes.fromhex('68900002006a02')) if multiple else bytes.fromhex('68800002006a006a00')
    if signal_wait:args=bytes.fromhex('68800002006a006a00ff3594000200ff3590000200')
    if apc_wait:args=bytes.fromhex('68800002006a016a01')
    if apc_post:args=bytes.fromhex('68880002006a006a00')
    program={0x11000:bytes.fromhex('5589e5ff0500000200')+args+
             bytes.fromhex('ff1520000200a304000200ff0508000200c9c3')}
    if apc_wait:
        program[0x11200]=bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff0510000200c9c20c00')
    if apc_wait:
        # The APC changes its own environment before waiting; its saved caller
        # must retain the original raw slots and control state across recovery.
        program[0x11200]=program[0x11200][:3]+bytes.fromhex("dbe3")+program[0x11200][3:]
    if apc_loop:
        program[0x11000]=bytes.fromhex('5589e5ff050000020031c0b9020000004975fda304000200ff0508000200c9c3')
    if exception:
        program[0x11000]=bytes.fromhex('5589e5ff0500000200b82a0000006800020200ff1524000200a304000200ff0508000200c9c3')
        routine=bytearray.fromhex('5589e5ff050c000200')
        if exception_nested:
            routine+=bytes.fromhex('8b45088138111111117515')
            routine+=bytes.fromhex('6880020200ff1524000200ff051400020031c0c9c3')
            # Save the nested flag observed by the original handler itself.
            routine+=bytes.fromhex('8b45088b4004a318000200')
        routine+=bytes.fromhex('68800002006a006a00ff1520000200ff051000020031c0c9c3')
        if exception_divide:
            program[0x11000]=bytes.fromhex('5589e5ff0500000200b81100000031d231c9f7f1a304000200ff0508000200c9c3')
            routine=routine[:-4]+bytes.fromhex('8b4510c780180200000300000031c0c9c3')
        program[0x11200]=bytes(routine)
    if dpc:
        program[0x11000]=bytes.fromhex('5589e5ff0500000200ff1524000200a304000200ff0508000200c9c3')
        program[0x11200]=bytes.fromhex('5589e5ff050c00020064a15c000000a31400020064a158000000a31800020031c9ff152800020068800002006a006a00ff1520000200ff0510000200c9c21000')
    if unwind:
        target=0xffffffff if unwind_all else seh_base+0x40 if unwind_collision else seh_base+0x20
        program[0x11000]=bytes.fromhex('5589e5ff05000002006878563412')+b'\x68'+(0 if unwind_synthetic else 0x20200).to_bytes(4,'little')+bytes.fromhex('68efbeadde')+b'\x68'+target.to_bytes(4,'little')+bytes.fromhex('ff1524000200a304000200ff0508000200c9c3')
        routine=bytes.fromhex('5589e5ff050c000200')
        if request.param=='unwind_nested':routine+=bytes.fromhex('6878563412ff75086a0068')+(seh_base+0x20).to_bytes(4,'little')+bytes.fromhex('ff1524000200')
        routine+=bytes.fromhex('dbe368800002006a006a00ff1520000200ff0510000200')
        routine+=(bytes.fromhex('8b4514c700')+(seh_base+0x20).to_bytes(4,'little')+bytes.fromhex('b803000000')) if unwind_collision else bytes.fromhex('b801000000')
        program[0x11200]=routine+bytes.fromhex('c9c3')
        program[0x11300]=bytes.fromhex('5589e5ff0514000200b801000000c9c3')
    if irp:
        registers='8b0d9800020031d2' if irp_complete else 'b9008002008b1598000200'
        program[0x11000]=bytes.fromhex('5589e5ff0500000200'+registers+'ff1524000200a304000200ff0508000200c9c3')
        if irp_invalid:program[0x11000]=bytes.fromhex('5589e5ff0500000200ff35980002006800800200ff1524000200a304000200ff0508000200c9c3')
        program[0x11200]=bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff0510000200c9c2'+('0c00' if irp_complete else '0800'))
    if device_delete:
        program[0x11000]=bytes.fromhex('5589e5ff0500000200ff3598000200ff1524000200a304000200ff0508000200c9c3')
        release='8b4d08ff1528000200'
        wait='68800002006a006a00ff1520000200ff0510000200'
        program[0x11200]=bytes.fromhex('5589e5ff050c000200'+(release+wait if device_freed else wait+release)+'c9c20400')
    if file_device:
        invoke='8b0dc0000200' if file_reference else 'ff359c000200'
        program[0x11000]=bytes.fromhex('5589e5ff0500000200'+invoke+'ff1524000200'+('31c0' if file_reference else '')+'a304000200ff0508000200c9c3')
        program[0x11200]=bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff05100002008b4d08ff1528000200c9c20400')
        if file_packet:
            program[0x11000]=bytes.fromhex('5589e5ff05000002008b0d9800020031d2ff152400020031c0a304000200ff0508000200c9c3')
        if file_control:
            args='6a006a006a006a006814d0040068b00002006a006a006a00ff359c000200'
            program[0x11000]=bytes.fromhex('5589e5ff0500000200'+args+'ff1524000200a304000200ff0508000200c9c3')
            program[0x11300]=bytes.fromhex('5589e5ff05d00002008b450ca398000200ff359c000200ff152c0002008b4d0cc7411000000000c741142a00000031d2ff153000020031c0c9c20800')

    if file_jump:
        from tools.recomp.test_continuation_nonlocal import PROGRAM
        program[0x14000]=PROGRAM[0x14000]
        program[0x15000]=PROGRAM[0x15000]
        routine=bytearray.fromhex('5589e5ff05000002006800020200')
        def call(target,base):
            return b'\xe8'+(target-base-5).to_bytes(4,'little',signed=True)
        routine+=call(0x14000,0x11000+len(routine))
        routine+=bytes.fromhex('83c40485c00f85');jump=len(routine);routine+=bytes(4)
        routine+=bytes.fromhex(args+'ff1524000200')
        routine[jump:jump+4]=(len(routine)-jump-4).to_bytes(4,'little')
        routine+=bytes.fromhex('a304000200ff0508000200c9c3')
        program[0x11000]=bytes(routine)
        # Finish the IRP's APC first, leaving the active control call as the
        # last file owner. Its abandoned cleanup enters the real delete wait.
        routine=bytearray(program[0x11300][:-6])
        routine+=bytes.fromhex('c70580020200f0d8ffffc70584020200ffffffff68800202006a006a00ff15200002006a076800020200')
        routine+=call(0x15000,0x11300+len(routine));routine+=b'\xcc'
        program[0x11300]=bytes(routine)

    if file_exit:
        program[0x11300]=program[0x11300][:-6]+bytes.fromhex('6856040000ff1534000200cc')

    if file_nested_exit:
        # Complete the outer request, then issue a nested control call. Both
        # calls still own a file reference when the inner driver exits.
        routine=bytearray.fromhex('5589e5ff05d0000200833dd0000200010f85')
        jump=len(routine);routine+=bytes(4)
        routine+=bytes.fromhex('8b4d0c890de0000200c7411000000000c741142a00000031d2ff1530000200')
        routine+=bytes.fromhex('6a006a006a006a006814d0040068b00002006a006a006a00ff359c000200ff1524000200cc')
        routine[jump:jump+4]=(len(routine)-jump-4).to_bytes(4,'little')
        # Skip the inner callback's shared prologue/counter increment.
        routine+=program[0x11300][9:]
        program[0x11300]=bytes(routine)

    if file_device:
        # The native file is deliberately assigned a guest driver. Supply both
        # original lifecycle dispatches instead of relying on missing entries.
        routine=bytearray.fromhex('5589e58b450ca3dc0002008b505c0fb60a83f90d7508ff05d4000200eb06ff05d8000200')
        if file_lifecycle:
            program[0x11200]=bytes.fromhex('5589e58b4d08ff1528000200c9c20400')
            routine+=bytes.fromhex('83f9')+bytes([13 if file_cleanup else 1])+bytes.fromhex('75')
            skip=len(routine);routine+=b'\0'
            routine+=bytes.fromhex('ff050c000200')
            if file_pending:
                routine+=bytes.fromhex('804a0301b803010000c9c20800')
            else:
                routine+=bytes.fromhex('68800002006a006a00ff1520000200ff0510000200')
            routine[skip]=len(routine)-skip-1
        routine+=bytes.fromhex('8b4d0cc7411000000000c741140000000031d2ff153000020031c0c9c20800')
        program[0x11400]=bytes(routine)

    if sync:
        args=('6a0068a00002006a006a006a006a0068008002006a04' if sync_ioctl else
              '68880002006a006a0068008002006a02')
        program[0x11000]=bytes.fromhex('5589e5ff0500000200'+args+'ff1524000200a304000200ff0508000200c9c3')
        callback='5589e5ff050c0002008b450ca3980002008b4020a3a4000200'
        if not sync_pending:
            callback+='68800002006a006a00ff1520000200ff0510000200'
            callback+='8b4d0cc7411000000000c741142a00000031d2ff1528000200'
        program[0x11200]=bytes.fromhex(callback+'b803010000c9c20800')
    if control:
        args='6a006a006a006a006814d0040068b00002006a006a006a00ff359c000200'
        program[0x11000]=bytes.fromhex('5589e5ff0500000200'+args+'ff1524000200a304000200ff0508000200c9c3')
        callback='5589e5ff050c0002008b450ca398000200'
        if control_pending:
            callback+='8b405c80480301b803010000'
        else:
            if control_dispatch:callback+='68800002006a006a00ff1520000200'
            callback+='ff05100002008b4d0cc7411000000000c741142a00000031d2ff152800020031c0'
        program[0x11200]=bytes.fromhex(callback+'c9c20800')
    if control_native:
        program[0x11000]=bytes.fromhex('5589e5ff05000002008b0dc40002008b1598000200ff1524000200a304000200ff0508000200c9c3')
        program[0x11200]=bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff051000020031c0c9c20c00')
    if queue:
        args=('688000020068b000020068ac00020068a8000200ff359c000200' if completion else
              '68800002006a006800800200')
        program[0x11000]=bytes.fromhex('5589e5ff0500000200'+args+'ff1524000200a304000200ff0508000200c9c3')
    if terminate:
        program[0x11000]=bytes.fromhex('5589e5ff05000002006856040000ff1524000200ff0508000200c9c3')
        program[0x11200]=bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff0510000200c9c20400')
    if natural_exit:program[0x11000]=bytes.fromhex('5589e5ff0500000200b856040000c9c3')
    if file_create:
        push=lambda value:b'\x68'+value.to_bytes(4,'little')
        args=b''.join(push(x) for x in [0x20,1,7,0,0,0x200b0,0x200e0,0x100000,0x2009c])
        program[0x11000]=bytes.fromhex('5589e5ff0500000200')+args+bytes.fromhex('ff1524000200a304000200ff0508000200c9c3')
        callback='5589e5ff050c0002008b450ca398000200'
        if create_exit:
            callback+='8b4860890dc00002008b4d0cc7411000000000c741142a00000031d2ff15280002006856040000ff152c000200cc'
        elif create_pending:callback+='8b405c80480301b803010000'
        else:
            callback+='68800002006a006a00ff1520000200ff0510000200'
            callback+='8b4d0cc7411000000000c741142a00000031d2ff152800020031c0'
        program[0x11200]=bytes.fromhex(callback+'c9c20800')
        lifecycle=bytearray.fromhex('5589e58b450c8b505c0fb60a83f90d7508ff05d4000200eb06ff05d8000200')
        if create_exit:
            lifecycle+=bytes.fromhex('83f90d7515'+'68800002006a006a00ff1520000200ff0510000200')
        lifecycle+=bytes.fromhex('8b4d0cc7411000000000c741140000000031d2ff152800020031c0c9c20800')
        program[0x11400]=bytes(lifecycle)
    prefix=r'''
#include "kernel/kernel_bridge.c"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
void sub_00011000(void);
#if FILE_JUMP
void sub_00014000(void);void sub_00015000(void);
#endif
#if FILE_DEVICE || FILE_CREATE
void sub_00011400(void);
#endif
#if FILE_CONTROL || UNWIND_WAIT
void sub_00011300(void);
#endif
#if DPC_WAIT || APC_WAIT || UNWIND_WAIT || EXCEPTION_WAIT || IRP_CALLBACK || SYNC_REQUEST || TERMINATE || DEVICE_DELETE || CONTROL_REQUEST || FILE_DEVICE || FILE_CREATE
void sub_00011200(void);
#endif
static void fixture_memory_init(void) {
    FILE *f=fopen(TEST_XBE,"rb");assert(f);fseek(f,0,SEEK_END);
    long size=ftell(f);rewind(f);void *xbe=malloc(size);assert(xbe);
    assert(fread(xbe,size,1,f)==1 && fclose(f)==0 && xbox_MemoryLayoutInit(xbe,size));free(xbe);
}
recomp_func_t recomp_lookup(uint32_t va) {
#if FILE_DEVICE || FILE_CREATE
    if(va==0x11400)return sub_00011400;
#endif
#if FILE_CONTROL || UNWIND_WAIT
    if(va==0x11300)return sub_00011300;
#endif
#if DPC_WAIT || APC_WAIT || UNWIND_WAIT || EXCEPTION_WAIT || IRP_CALLBACK || SYNC_REQUEST || TERMINATE || DEVICE_DELETE || CONTROL_REQUEST || FILE_DEVICE || FILE_CREATE
    if(va==0x11200)return sub_00011200;
#endif
    (void)va;return 0;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return 0; }
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr,"bad target %08x\n",va);exit(5); }
void recomp_icall_not_code_log(uint32_t va) { recomp_icall_fail_log(va); }
static const char *snapshot;
static int saving,cold;
static DWORD saved_remaining;
static ULONGLONG began;
static DWORD WINAPI complete_pending(void *unused) {
    (void)unused;Sleep(1000);guest_cpu_join();
    if(QUEUE_WAIT) {
        uint32_t queue=MEM32(0x200b8);
        if(COMPLETION_PORT)assert(!xbox_IoSetIoCompletion(XBOX_TO_NATIVE(queue),XBOX_TO_NATIVE(0xabc),XBOX_TO_NATIVE(0xdef),0,42));
        else ke_insert_queue(queue,0x28200,0);
        guest_cpu_part();return 0;
    }
    if(CONTROL_LOCK) {
        uint32_t file=MEM32(0x200c0);assert(MEM32(file+0x24)==1 && !MEM32(0x2000c));
        --MEM32(file+0x24);io_signal_guest_event(file+0x28);guest_cpu_part();return 0;
    }
    uint32_t irp=MEM32(FILE_LIFECYCLE_PENDING?0x200dc:0x20098);assert(irp && MEM32(0x2000c)==1);
    MEM32(irp+0x10)=0;MEM32(irp+0x14)=42;MEM32(0x20010)++;
    xbox_IofCompleteRequest(XBOX_TO_NATIVE(irp),0);guest_cpu_part();return 0;
}
static void checkpoint(void) {
    if(saving && GetTickCount64()-began>=400) {
        if(FILE_EXIT || FILE_JUMP || FILE_CREATE_EXIT)assert(t_kernel_wait && t_dispatch_depth>=(FILE_CREATE_EXIT?2:1));
        else assert(t_kernel_wait && t_dispatch_depth==APC_DEPTH);
        if(NATURAL_EXIT)assert(recomp_cont_root_function()==KERNEL_CONT_THREAD_EXIT);
        if(FILE_DEVICE) {
            HANDLE retired=bridge_resolve_handle(MEM32(0x2009c));
            if(FILE_LIFECYCLE_PENDING) {
                /* Pending completion already reused the retired public token
                 * for the private wait alias of its own event. */
                assert(retired && s_handle_private[MEM32(0x2009c)&BRIDGE_HANDLE_MASK]);
                assert(checkpoint_native.compare(retired,ke_shadow_lookup(MEM32(MEM32(0x200dc)+0x20)))==0);
            } else assert(!retired);
            if(FILE_LIFECYCLE) {
                uint32_t file=MEM32(0x200c0),irp=MEM32(0x200dc);
                assert(ke_object_slot(file) && MEM32(file+0x24)==0 && (MEM8(file+3)&0x20));
                assert(MEM32(irp+0x60)==file && MEM32(irp+4)==0x44);
                assert(MEM32(ke_current_thread()+0x134)==irp+8);
                assert(MEM32(0x200d4)==1 && MEM32(0x200d8)==!FILE_CLEANUP);
            }
            HANDLE replacement=CreateEventW(NULL,TRUE,TRUE,NULL);assert(replacement);
            MEM32(0x200c8)=bridge_handle_token(replacement);
            assert((MEM32(0x200c8)==MEM32(0x2009c))==!FILE_LIFECYCLE_PENDING);
        }
        saved_remaining=t_kernel_wait->remaining_ms;
        if(SYNC_PENDING || CONTROL_EXTERNAL || FILE_LIFECYCLE_PENDING || FILE_CREATE_PENDING)assert(saved_remaining==INFINITE);
        else if(TERMINATE_IRP)assert(saved_remaining<=100);
        else if(QUEUE_READY)assert(saved_remaining>1000 && saved_remaining<1800);
        else assert(saved_remaining>200 && saved_remaining<800);
        XboxGuestRegisters state;xbox_CaptureGuestRegisters(&state);
        FILE *f=fopen(snapshot,"wb");assert(f);
        assert(fwrite(&saved_remaining,sizeof saved_remaining,1,f)==1);
        assert(fwrite(&state,sizeof state,1,f)==1);
        /* This single-thread fixture is stopped inside the wait boundary. Use
         * the real RAM/allocator and object serializers along with the frame. */
        t_checkpoint_controller=1;
        assert(xbox_SavePhysicalMemory(f) && recomp_cont_save(f) && xbox_CheckpointWriteObjects(f));
        if(QUEUE_WAIT){xbox_RuntimeClockPause();assert(xbox_SaveRuntimeClock(f));xbox_RuntimeClockResume();}
        t_checkpoint_controller=0;
        assert(fclose(f)==0);saving=0;if(cold)exit(23);
    }
}
static void load(void) {
    XboxGuestRegisters state;FILE *f=fopen(snapshot,"rb");assert(f);
    assert(fread(&saved_remaining,sizeof saved_remaining,1,f)==1);
    assert(fread(&state,sizeof state,1,f)==1);
    t_checkpoint_controller=1;assert(checkpoint_clear_objects());
    assert(xbox_LoadPhysicalMemory(f) && xbox_RestoreGuestRegisters(&state) && recomp_cont_load(f));
    assert(xbox_CheckpointLoadObjects(f,NULL,0));t_checkpoint_controller=0;
    if(QUEUE_WAIT) {
        t_checkpoint_controller=1;xbox_RuntimeClockPause();void *clock=xbox_PrepareRuntimeClock(f);assert(clock);
        xbox_ApplyRuntimeClock(clock);free(clock);xbox_RuntimeClockResume();t_checkpoint_controller=0;
    }
    assert(fclose(f)==0);
}
'''
    suffix=r'''
static void run_guest(int restored) {
    if(NATURAL_EXIT && restored)xbox_RunGuestRoot(checkpoint_lookup_root(KERNEL_CONT_THREAD_EXIT));
    else {
        xbox_RunGuestRoot(sub_00011000);
        if(NATURAL_EXIT)xbox_RunGuestRoot(bridge_finish_thread);
    }
}
static void apc_float_state(int initialize) {
    XboxGuestRegisters state;xbox_CaptureGuestRegisters(&state);
    if(initialize) {
        state.fp_control_word=0x27f;state.fp_status_word=0x21;state.fp_cc=0x4100;
        state.fp_top=6;state.fp_cmp=-1;state.fp_tag_word=0;state.fp_opcode=0x456;
        state.fp_code_selector=8;state.fp_data_selector=0x10;
        state.fp_instruction_pointer=0x111234;state.fp_data_pointer=0x221234;state.mxcsr=0x3f80;
        for(unsigned i=0;i<8;i++) {
            uint64_t significand=UINT64_C(0x8000000000000001)+i;uint16_t exponent=0x3fff;double value=1;
            memcpy(state.fp_raw[i],&significand,8);memcpy(state.fp_raw[i]+8,&exponent,2);
            memcpy(&state.fp_stack[i],&value,8);state.mm[i]=significand;memset(state.xmm[i],0xa0+i,16);
        }
        assert(xbox_RestoreGuestRegisters(&state));return;
    }
    assert(state.fp_control_word==0x27f && state.fp_status_word==0x21 && state.fp_cc==0x4100);
    assert(state.fp_top==6 && state.fp_tag_word==0 && state.fp_opcode==0x456);
    assert(state.fp_code_selector==8 && state.fp_data_selector==0x10);
    assert(state.fp_instruction_pointer==0x111234 && state.fp_data_pointer==0x221234 && state.mxcsr==0x3f80);
    for(unsigned i=0;i<8;i++) {
        uint64_t significand;uint16_t exponent;
        memcpy(&significand,state.fp_raw[i],8);memcpy(&exponent,state.fp_raw[i]+8,2);
        assert(significand==UINT64_C(0x8000000000000001)+i && exponent==0x3fff && state.mm[i]==significand);
        for(unsigned j=0;j<16;j++)assert(state.xmm[i][j]==0xa0+i);
    }
}
static void fixture_dpc_wait_call(void) {
    assert(kernel_run_dpc(0x200c0,0,0));g_eax=0;
}
static void check(void) {
    if(DPC_WAIT) {
        assert(MEM32(0x2000c)==1 && MEM32(0x20010)==1 && MEM32(0x20014)>0x90000 && MEM32(0x20018)==MEM32(0x20014)-8);
        assert(MEM32(XBOX_FS_BASE)==TEST_SEH_BASE && !MEM32(XBOX_FS_BASE+0x58) && MEM32(XBOX_FS_BASE+0x5c)==MEM32(0x20014));
        assert(xbox_CurrentIrql()==0);
    }
    if(APC_WAIT || UNWIND_WAIT)apc_float_state(0);
    if(UNWIND_WAIT) {
        assert(MEM32(0x2000c)==1 && MEM32(0x20010)==1);
        assert(MEM32(0x20014)==UNWIND_ALL);
        assert(MEM32(XBOX_FS_BASE)==(UNWIND_ALL?0xffffffffu:UNWIND_COLLISION?(TEST_SEH_BASE+0x40):(TEST_SEH_BASE+0x20)));
    }
    if(EXCEPTION_WAIT) {
        assert(MEM32(0x2000c)==1+EXCEPTION_NESTED && MEM32(0x20010)==1);
        assert(BRIDGE_MEM32(XBOX_FS_BASE)==TEST_SEH_BASE);
        if(EXCEPTION_NESTED)assert(MEM32(0x20014)==1 && (MEM32(0x20018)&0x10));
    }
    assert(MEM32(0x20000)==1);
    if(TERMINATE || FILE_EXIT || FILE_CREATE_EXIT)assert(!MEM32(0x20008) && t_guest_terminated && g_eax==0x456 && MEM32(ke_current_thread()+0x120)==0x456);
    else assert(g_esp==TEST_STACK_TOP && MEM32(0x20008)==1 && MEM32(0x20004)==EXPECTED_STATUS);
    assert(!recomp_cont_active() && !t_kernel_wait && t_dispatch_depth==0);
    if(APC_WAIT || IRP_CALLBACK || SYNC_REQUEST || TERMINATE || DEVICE_DELETE || CONTROL_REQUEST || FILE_DEVICE || FILE_CREATE)assert(MEM32(0x2000c)==1 && MEM32(0x20010)==1);
    if(DEVICE_DELETE)assert(!ke_object_slot(MEM32(0x20098)));
    if(FILE_DEVICE) {
        assert(!ke_object_slot(MEM32(0x200c4)));
        if(FILE_SHADOW) {
            assert(!ke_object_slot(MEM32(0x200c0)));
            assert(MEM32(0x200d4)==!(FILE_REFERENCE || FILE_PACKET) && MEM32(0x200d8)==1);
            assert(!MEM16(MEM32(0x200dc)));
            assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        }
        if(FILE_PACKET || FILE_CONTROL) {
            uint32_t head=ke_current_thread()+0x134;
            assert(!MEM16(MEM32(0x20098)) && MEM32(head)==head);
            assert(!MEM32(0x200cc) || !ke_object_slot(MEM32(0x200cc)));
            if(FILE_ERROR || FILE_CANCEL)assert(MEM32(0x200b0)==0xabcdef);
            else assert(!MEM32(0x200b0) && MEM32(0x200b4)==42);
            if(FILE_CONTROL)assert(MEM32(0x200d0)==1+FILE_NESTED_EXIT);
            if(FILE_NESTED_EXIT)assert(MEM32(0x200e0) && !MEM16(MEM32(0x200e0)));
        }
        if(MEM32(0x200c8))assert(WaitForSingleObject(bridge_read_handle(0x200c8),0)==WAIT_OBJECT_0);
        else assert(!bridge_resolve_handle(MEM32(0x2009c)));
    }
    if(FILE_CREATE && !FILE_CREATE_EXIT) {
        uint32_t token=MEM32(0x2009c),file=ke_object_for_handle(bridge_resolve_handle(token)),device=MEM32(0x200c4);
        assert(file && ke_object_slot(file)->in_use==4 && MEM32(file+4)==device && MEM32(device+4)==1);
        assert(!MEM16(MEM32(0x20098)) && !MEM32(0x200b0) && MEM32(0x200b4)==42);
        assert(MEM32(ke_current_thread()+0x134)==ke_current_thread()+0x134);
        assert(!xbox_NtClose(bridge_resolve_handle(token)));
        assert(!ke_object_slot(file) && !MEM32(device+4) && MEM32(0x200d4)==1 && MEM32(0x200d8)==1);
    }
    if(FILE_CREATE_EXIT) {
        uint32_t file=MEM32(0x200c0),device=MEM32(0x200c4);
        assert(file && !ke_object_slot(file) && !MEM32(device+4));
        assert(!MEM32(0x2009c) && !MEM16(MEM32(0x20098)) && MEM32(0x200d4)==1 && MEM32(0x200d8)==1);
    }
    if(IRP_CALLBACK)assert(BRIDGE_MEM8(MEM32(0x20098)+0x19)==(IRP_COMPLETE?3:1));
    if(IRP_INVALID)assert(MEM32(0x10)==0x12345678);
    if(CONTROL_NATIVE) {
        uint32_t device=MEM32(0x200c4),irp=MEM32(0x20098),head=ke_current_thread()+0x134;
        assert(MEM8(device+0x1f) && !MEM32(MEM32(device+0x18)));
        assert(MEM16(irp)==6 && !MEM32(0x200b0) && !MEM32(0x200b4));
        ke_list_unlink(irp+8);xbox_IoFreeIrp(XBOX_TO_NATIVE(irp));assert(MEM32(head)==head);
    }
    if(CONTROL_REQUEST && !CONTROL_NATIVE) {
        uint32_t file=MEM32(0x200c0),head=ke_current_thread()+0x134;
        assert(MEM32(file+0x24)==UINT32_MAX && ke_object_slot(file)->references==1);
        assert(!MEM16(MEM32(0x20098)) && MEM32(head)==head && !MEM32(0x200b0) && MEM32(0x200b4)==42);
    }
    if(SYNC_REQUEST) {
        assert(!ke_shadow_lookup(MEM32(0x200a4)));
        uint32_t head=ke_current_thread()+0x134;assert(MEM32(head)==head);
        if(SYNC_IOCTL)assert(MEM32(0x200a0)==42);
    }
    if(TERMINATE || FILE_EXIT) {
        uint32_t thread=ke_current_thread();assert(MEM32(thread+0x134)==thread+0x134 && MEM32(thread+0x3c)==thread+0x3c);
    }
    if(QUEUE_WAIT) {
        uint32_t queue=MEM32(0x200b8),thread=ke_current_thread();
        assert(MEM32(queue+0x18)==1 && MEM32(thread+0x78)==queue && MEM8(thread+0x2c)==2);
        assert(MEM32(queue+0x20)==thread+0x7c && MEM32(queue+0x24)==thread+0x7c);
        if(COMPLETION_PORT && QUEUE_READY)assert(MEM32(0x200a8)==0xabc && MEM32(0x200ac)==0xdef && !MEM32(0x200b0) && MEM32(0x200b4)==42);
    }
    if(SIGNAL_WAIT) {
        struct { LONG count,maximum; } state;assert(checkpoint_native_init());
        assert(checkpoint_native.semaphore(bridge_read_handle(0x20090),0,&state,sizeof state,NULL)==0);
        assert(state.count==1); /* Restoring a waiting call cannot signal twice. */
    }
}
int main(int argc,char **argv) {
    assert(argc==3);snapshot=argv[2];fixture_memory_init();
    if(DPC_WAIT || APC_WAIT || UNWIND_WAIT || EXCEPTION_WAIT || IRP_CALLBACK || SYNC_REQUEST || QUEUE_WAIT || TERMINATE || DEVICE_DELETE || CONTROL_REQUEST || FILE_DEVICE || FILE_CREATE) {_putenv_s("RECOMP_GUEST_LOCK","1");guest_cpu_join();}
    g_slot_ordinals[0]=WAIT_ORDINAL;g_slot_bridges[0]=WAIT_BRIDGE;g_slot_arg_bytes[0]=WAIT_BYTES;
    BRIDGE_MEM32(0x20020)=KERNEL_VA_BASE;
    *(LONGLONG *)XBOX_TO_NATIVE(0x20080)=-10000000;
    if(QUEUE_READY)*(LONGLONG *)XBOX_TO_NATIVE(0x20080)=-20000000;
    /* The original object serializer also retains the private wait aliases. */
    HANDLE first=SIGNAL_WAIT?CreateSemaphoreW(0,0,8,0):CreateEventA(0,1,0,0);
    HANDLE second=CreateEventA(0,1,0,0);assert(first && second);
    BRIDGE_MEM32(0x20090)=bridge_handle_token(first);
    BRIDGE_MEM32(0x20094)=bridge_handle_token(second);
    xbox_kernel_set_checkpoint_hook(checkpoint);
    int restored=!strcmp(argv[1],"load");
    if(IRP_CALLBACK) {
        g_slot_ordinals[1]=IRP_INVALID?74:IRP_COMPLETE?87:86;
        g_slot_bridges[1]=IRP_INVALID?bridge_IoInvalidDeviceRequest:IRP_COMPLETE?bridge_IofCompleteRequest:bridge_IofCallDriver;
        g_slot_arg_bytes[1]=IRP_INVALID?8:0;BRIDGE_MEM32(0x20024)=KERNEL_VA_BASE+4;
        if(!restored) {
            uint32_t irp=io_guest_address(xbox_IoAllocateIrp(1)),stack=irp+0x68;
            if(IRP_INVALID)MEM32(0x10)=0x12345678;
            MEM32(0x20098)=irp;MEM32(0x28008)=0x28100;MEM32(0x28100+12+2*4)=0x11200;
            MEM8(0x2801e)=1;MEM8(stack)=2;
            if(IRP_COMPLETE) {
                MEM32(irp+4)=0x80;MEM8(irp+0x19)=1;MEM32(irp+0x5c)=stack;
                MEM8(stack+3)=IRP_INVALID?0x80:0x40;MEM32(stack+0x1c)=0x11200;
            }
        }
    }
    if(FILE_CREATE) {
        g_slot_ordinals[3]=258;g_slot_bridges[3]=bridge_PsTerminateSystemThread;g_slot_arg_bytes[3]=4;MEM32(0x2002c)=KERNEL_VA_BASE+12;
        g_slot_ordinals[1]=190;g_slot_bridges[1]=bridge_NtCreateFile;g_slot_arg_bytes[1]=36;MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[2]=87;g_slot_bridges[2]=bridge_IofCompleteRequest;g_slot_arg_bytes[2]=0;MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            assert(!xbox_kernel_init_devices());
            XBOX_ANSI_STRING name={19,19,"\\Device\\CreateProbe"};PVOID body=NULL;
            assert(!xbox_IoCreateDevice(XBOX_TO_NATIVE(0x28100),0,&name,0x15,FALSE,&body));
            uint32_t device=io_guest_address(body);MEM32(0x200c4)=device;MEM32(device+0x14)&=~0x10u;
            MEM32(0x28100+12)=0x11200;MEM32(0x28100+12+1*4)=MEM32(0x28100+12+13*4)=0x11400;
            MEM32(0x200e0)=0;MEM32(0x200e4)=0x200f0;MEM32(0x200e8)=0x40;
            MEM16(0x200f0)=MEM16(0x200f2)=25;MEM32(0x200f4)=0x20100;
            memcpy(XBOX_TO_NATIVE(0x20100),"\\Device\\CreateProbe\\child",25);
        }
    }
    if(CONTROL_REQUEST) {
        xbox_path_init(TEST_ROOT,TEST_ROOT);
        g_slot_ordinals[1]=CONTROL_NATIVE?86:CONTROL_DISPATCH?200:196;
        g_slot_bridges[1]=CONTROL_NATIVE?bridge_IofCallDriver:CONTROL_DISPATCH?bridge_NtFsControlFile:bridge_NtDeviceIoControlFile;
        g_slot_arg_bytes[1]=CONTROL_NATIVE?0:40;MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[2]=87;g_slot_bridges[2]=bridge_IofCompleteRequest;g_slot_arg_bytes[2]=0;
        MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            assert(!xbox_kernel_init_devices());
            XBOX_ANSI_STRING name={14,14,"\\Device\\CdRom0"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};
            XBOX_IO_STATUS_BLOCK ios;HANDLE handle;
            assert(!xbox_NtOpenFile(&handle,0,&oa,&ios,7,0x20));
            uint32_t file=0;assert(!ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE,&file));
            MEM32(0x2009c)=bridge_handle_token(handle);MEM32(0x200c0)=file;
            uint32_t device=MEM32(file+4);MEM32(0x200c4)=device;
            if(CONTROL_NATIVE) {
                uint32_t irp=io_guest_address(xbox_IoBuildDeviceIoControlRequest(0x90020,XBOX_TO_NATIVE(device),NULL,0,NULL,0,FALSE,NULL,XBOX_TO_NATIVE(0x200b0)));
                assert(irp);uint32_t stack=MEM32(irp+0x5c)-XBOX_IO_STACK_BYTES;
                MEM32(irp+4)=0x44;MEM8(stack)=9;MEM32(stack+0x18)=file;
                MEM8(stack+3)=0x40;MEM32(stack+0x1c)=0x11200;MEM32(0x20098)=irp;
            } else {
                MEM32(device+8)=0x28100;MEM32(0x28100+12+9*4)=MEM32(0x28100+12+10*4)=0x11200;
                if(CONTROL_LOCK)MEM32(file+0x24)=0;
            }
        }
    }
    if(FILE_DEVICE) {
        xbox_path_init(TEST_ROOT,TEST_ROOT);
        g_slot_ordinals[1]=FILE_CONTROL?196:FILE_PACKET?87:FILE_REFERENCE?250:187;
        g_slot_bridges[1]=FILE_CONTROL?bridge_NtDeviceIoControlFile:FILE_PACKET?bridge_IofCompleteRequest:FILE_REFERENCE?bridge_ObfDereferenceObject:bridge_NtClose;
        g_slot_arg_bytes[1]=FILE_CONTROL?40:(FILE_REFERENCE || FILE_PACKET)?0:4;MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[3]=187;g_slot_bridges[3]=bridge_NtClose;g_slot_arg_bytes[3]=4;MEM32(0x2002c)=KERNEL_VA_BASE+12;
        g_slot_ordinals[5]=258;g_slot_bridges[5]=bridge_PsTerminateSystemThread;g_slot_arg_bytes[5]=4;MEM32(0x20034)=KERNEL_VA_BASE+20;
        g_slot_ordinals[4]=87;g_slot_bridges[4]=bridge_IofCompleteRequest;g_slot_arg_bytes[4]=0;MEM32(0x20030)=KERNEL_VA_BASE+16;
        g_slot_ordinals[2]=250;g_slot_bridges[2]=bridge_ObfDereferenceObject;g_slot_arg_bytes[2]=0;
        MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            assert(!xbox_kernel_init_devices());
            XBOX_ANSI_STRING name={14,14,"\\Device\\CdRom0"};XBOX_OBJECT_ATTRIBUTES oa={NULL,&name,0};
            XBOX_IO_STATUS_BLOCK ios;HANDLE handle;uint32_t file=0;
            /* Closing one's own synchronous handle from its locked control
             * dispatch deadlocks in the original kernel as well. */
            assert(!xbox_NtOpenFile(&handle,0,&oa,&ios,7,FILE_CONTROL?0:0x20));
            uint32_t device=xbox_FileHandleDevice(handle);MEM32(0x200c4)=device;
            MEM32(0x2009c)=bridge_handle_token(handle);
            if(FILE_SHADOW) {
                assert(!ke_reference_handle(handle,XBOX_KERNEL_DATA_BASE+KDATA_FILE_OBJ_TYPE,&file));MEM32(0x200c0)=file;
                if(FILE_REFERENCE || FILE_PACKET) {
                    g_esp=0x60000;MEM32(g_esp)=MEM32(0x2009c);bridge_NtClose();assert(!g_eax);
                } else ke_dereference_object(file);
            }
            if(FILE_PACKET) {
                uint32_t irp=io_guest_address(xbox_IoAllocateIrp(1));assert(irp);MEM32(0x20098)=irp;
                MEM8(irp+0x19)=1;MEM32(irp+0x5c)=irp+0x68;MEM32(irp+0x60)=file;
                MEM32(irp+0x10)=FILE_ERROR?0xc0000001:0;MEM32(irp+0x14)=42;
                MEM32(irp+0x1c)=0x200b0;MEM32(0x200b0)=0xabcdef;io_attach_request_thread(irp,1);
                HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);uint32_t event_object=0;assert(event);
                assert(!ke_reference_handle(event,XBOX_KERNEL_DATA_BASE+KDATA_EVENT_OBJ_TYPE,&event_object));
                assert(!xbox_NtClose(event));MEM32(irp+0x20)=event_object;MEM32(0x200cc)=event_object;
                if(FILE_CANCEL){MEM8(irp+0x1b)=1;io_disassociate_thread_irp(ke_current_thread());assert(!MEM32(irp+0x50));}
            }
            if(FILE_CONTROL)MEM32(0x28100+12+10*4)=0x11300;
            MEM32(device+8)=0x28100;MEM32(0x28104)=0x11200;
            MEM32(0x28100+12+1*4)=MEM32(0x28100+12+13*4)=0x11400;
            xbox_IoDeleteDevice(XBOX_TO_NATIVE(device));assert(MEM8(device+0x1f) && MEM32(device+4)==1);
        }
    }
    if(DEVICE_DELETE) {
        g_slot_ordinals[1]=68;g_slot_bridges[1]=bridge_IoDeleteDevice;g_slot_arg_bytes[1]=4;
        MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[2]=250;g_slot_bridges[2]=bridge_ObfDereferenceObject;g_slot_arg_bytes[2]=0;
        MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            MEM32(0x28104)=0x11200;PVOID device=NULL;
            assert(xbox_IoCreateDevice(XBOX_TO_NATIVE(0x28100),32,NULL,7,FALSE,&device)==0);
            MEM32(0x20098)=io_guest_address(device);
        }
    }
    if(SYNC_REQUEST) {
        g_slot_ordinals[1]=SYNC_IOCTL?84:85;
        g_slot_bridges[1]=SYNC_IOCTL?bridge_IoSynchronousDeviceIoControlRequest:bridge_IoSynchronousFsdRequest;
        g_slot_arg_bytes[1]=SYNC_IOCTL?32:20;MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[2]=87;g_slot_bridges[2]=bridge_IofCompleteRequest;g_slot_arg_bytes[2]=0;
        MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            MEM32(0x28008)=0x28100;MEM8(0x2801e)=1;
            MEM32(0x28100+12+(SYNC_IOCTL?10:2)*4)=0x11200;
        }
    }
    if(QUEUE_WAIT) {
        g_slot_ordinals[1]=COMPLETION_PORT?223:136;
        g_slot_bridges[1]=COMPLETION_PORT?bridge_NtRemoveIoCompletion:bridge_KeRemoveQueue;
        g_slot_arg_bytes[1]=COMPLETION_PORT?20:12;MEM32(0x20024)=KERNEL_VA_BASE+4;
        if(!restored) {
            uint32_t queue=0x28000;
            if(COMPLETION_PORT) {
                g_esp=0x60000;MEM32(g_esp)=0x2009c;MEM32(g_esp+4)=0x1f0003;MEM32(g_esp+8)=0;MEM32(g_esp+12)=1;
                bridge_NtCreateIoCompletion();assert(!g_eax);queue=ke_completion_for_handle(bridge_read_handle(0x2009c));
            } else assert(ke_initialize_queue(queue,1));
            MEM32(0x200b8)=queue;
        }
    }
    if(TERMINATE) {
        g_slot_ordinals[1]=258;g_slot_bridges[1]=bridge_PsTerminateSystemThread;
        g_slot_arg_bytes[1]=4;MEM32(0x20024)=KERNEL_VA_BASE+4;
        if(!restored) {
            if(TERMINATE_IRP) {
                uint32_t irp=io_guest_address(xbox_IoAllocateIrp(1));MEM32(0x20098)=irp;MEM32(0x2000c)=1;
                MEM8(irp+0x19)=1;MEM32(irp+0x5c)=irp+0x68;io_attach_request_thread(irp,1);
            } else {
                uint32_t apc=xbox_HeapAlloc(0x28,16);assert(apc);
                ke_initialize_apc(apc,ke_current_thread(),KERNEL_POOL_FREE_APC,0x11200,0x11200,1,0);
                assert(ke_insert_apc(apc,0,0));
            }
        }
    }
    if(DPC_WAIT) {
        g_slot_ordinals[1]=121;g_slot_bridges[1]=fixture_dpc_wait_call;g_slot_arg_bytes[1]=0;
        MEM32(0x20024)=KERNEL_VA_BASE+4;
        g_slot_ordinals[2]=161;g_slot_bridges[2]=bridge_KfLowerIrql;g_slot_arg_bytes[2]=0;
        MEM32(0x20028)=KERNEL_VA_BASE+8;
        if(!restored) {
            MEM32(XBOX_FS_BASE)=TEST_SEH_BASE;
            MEM32(0x200cc)=0x11200;MEM32(0x200d0)=0;
        }
    }
    if(UNWIND_WAIT) {
        g_slot_ordinals[1]=312;g_slot_bridges[1]=bridge_RtlUnwind;g_slot_arg_bytes[1]=16;
        MEM32(0x20024)=KERNEL_VA_BASE+4;
        if(!restored) {
            apc_float_state(1);
            MEM32(XBOX_FS_BASE)=TEST_SEH_BASE;
            MEM32(TEST_SEH_BASE)=(TEST_SEH_BASE+0x20);MEM32((TEST_SEH_BASE+4))=0x11200;
            MEM32((TEST_SEH_BASE+0x20))=UNWIND_ALL?0xffffffffu:(TEST_SEH_BASE+0x40);MEM32((TEST_SEH_BASE+0x24))=0x11300;
            MEM32((TEST_SEH_BASE+0x40))=0xffffffffu;MEM32((TEST_SEH_BASE+0x44))=0x11300;
            memset(XBOX_TO_NATIVE(0x20200),0,0x50);MEM32(0x20200)=0x11111111;
        }
    }
    if(EXCEPTION_WAIT) {
        g_slot_ordinals[1]=302;g_slot_bridges[1]=bridge_RtlRaiseException;g_slot_arg_bytes[1]=4;
        MEM32(0x20024)=KERNEL_VA_BASE+4;
        if(!restored) {
            MEM32(XBOX_FS_BASE)=TEST_SEH_BASE;
            MEM32(TEST_SEH_BASE)=0xffffffffu;MEM32((TEST_SEH_BASE+4))=0x11200;
            memset(XBOX_TO_NATIVE(0x20200),0,0x100);MEM32(0x20200)=0x11111111;MEM32(0x20280)=0x22222222;
        }
    }
    if(APC_WAIT && !restored) {
        apc_float_state(1);
        uint32_t apc=xbox_HeapAlloc(0x28,16);assert(apc);
        ke_initialize_apc(apc,ke_current_thread(),KERNEL_POOL_FREE_APC,KERNEL_POOL_FREE_APC,0x11200,APC_MODE,0);
        assert(ke_insert_apc(apc,0,0));
    }
    if(restored) load();else {g_esp=TEST_STACK_TOP;PUSH32(g_esp,0x76543210);}
    saving=!strcmp(argv[1],"save") || !strcmp(argv[1],"roundtrip");cold=!strcmp(argv[1],"save");
    HANDLE completing=(SYNC_PENDING || QUEUE_READY || TERMINATE_IRP || CONTROL_EXTERNAL || FILE_LIFECYCLE_PENDING || FILE_CREATE_PENDING)?CreateThread(NULL,0,complete_pending,NULL,0,NULL):NULL;
    began=GetTickCount64();run_guest(restored);check();
    if(completing){assert(WaitForSingleObject(completing,2000)==WAIT_OBJECT_0);CloseHandle(completing);}
    if(restored && !SYNC_PENDING && !QUEUE_READY && !TERMINATE_IRP && !CONTROL_EXTERNAL && !FILE_LIFECYCLE_PENDING && !FILE_CREATE_PENDING) {
        ULONGLONG elapsed=GetTickCount64()-began;
        assert(elapsed+20>=saved_remaining && elapsed<=saved_remaining+250);
    }
    if(!strcmp(argv[1],"roundtrip")) {
        load();completing=(SYNC_PENDING || QUEUE_READY || TERMINATE_IRP || CONTROL_EXTERNAL || FILE_LIFECYCLE_PENDING || FILE_CREATE_PENDING)?CreateThread(NULL,0,complete_pending,NULL,0,NULL):NULL;
        began=GetTickCount64();run_guest(1);check();
        if(completing){assert(WaitForSingleObject(completing,2000)==WAIT_OBJECT_0);CloseHandle(completing);}
        ULONGLONG elapsed=GetTickCount64()-began;
        if(!SYNC_PENDING && !QUEUE_READY && !TERMINATE_IRP && !CONTROL_EXTERNAL && !FILE_LIFECYCLE_PENDING && !FILE_CREATE_PENDING)assert(elapsed+20>=saved_remaining && elapsed<=saved_remaining+250);
    }
    printf("%08x %u %u %u\n",g_esp,MEM32(0x20000),MEM32(0x20004),MEM32(0x20008));
    t_checkpoint_controller=1;assert(checkpoint_clear_objects());t_checkpoint_controller=0;return 0;
}
'''
    defines=(f'#define WAIT_ORDINAL {230 if signal_wait else 235 if multiple else 99}\n'
             f'#define WAIT_BYTES {20 if signal_wait else 24 if multiple else 12}\n'
             f'#define WAIT_BRIDGE {"bridge_NtSignalAndWaitForSingleObjectEx" if signal_wait else "bridge_NtWaitForMultipleObjectsEx" if multiple else "bridge_KeDelayExecutionThread"}\n'
             f'#define SIGNAL_WAIT {int(signal_wait)}\n'
             f'#define APC_WAIT {int(apc_wait)}\n'
             f'#define DPC_WAIT {int(dpc)}\n'
             f'#define UNWIND_WAIT {int(unwind)}\n'
             f'#define UNWIND_COLLISION {int(unwind_collision)}\n'
             f'#define UNWIND_ALL {int(unwind_all)}\n'
             f'#define EXCEPTION_WAIT {int(exception)}\n'
             f'#define EXCEPTION_NESTED {int(exception_nested)}\n'
             f'#define DEVICE_DELETE {int(device_delete)}\n'
             f'#define FILE_DEVICE {int(file_device)}\n'
             f'#define FILE_CREATE {int(file_create)}\n'
             f'#define FILE_CREATE_PENDING {int(create_pending)}\n'
             f'#define FILE_CREATE_EXIT {int(create_exit)}\n'
             f'#define FILE_SHADOW {int(file_shadow)}\n'
             f'#define FILE_REFERENCE {int(file_reference)}\n'
             f'#define FILE_PACKET {int(file_packet)}\n'
             f'#define FILE_ERROR {int(file_error)}\n'
             f'#define FILE_CANCEL {int(file_cancel)}\n'
             f'#define FILE_CONTROL {int(file_control)}\n'
             f'#define FILE_EXIT {int(file_exit)}\n'
             f'#define FILE_JUMP {int(file_jump)}\n'
             f'#define FILE_NESTED_EXIT {int(file_nested_exit)}\n'
             f'#define FILE_LIFECYCLE {int(file_lifecycle)}\n'
             f'#define FILE_CLEANUP {int(file_cleanup)}\n'
             f'#define FILE_LIFECYCLE_PENDING {int(file_pending)}\n'
             f'#define CONTROL_REQUEST {int(control)}\n'
             f'#define CONTROL_NATIVE {int(control_native)}\n'
             f'#define CONTROL_DISPATCH {int(control_dispatch)}\n'
             f'#define CONTROL_LOCK {int(control_lock)}\n'
             f'#define CONTROL_PENDING {int(control_pending)}\n'
             f'#define CONTROL_EXTERNAL {int(control_external)}\n'
             f'#define IRP_CALLBACK {int(irp)}\n'
             f'#define IRP_COMPLETE {int(irp_complete)}\n'
             f'#define IRP_INVALID {int(irp_invalid)}\n'
             f'#define SYNC_REQUEST {int(sync)}\n'
             f'#define SYNC_IOCTL {int(sync_ioctl)}\n'
             f'#define SYNC_PENDING {int(sync_pending)}\n'
             f'#define QUEUE_WAIT {int(queue)}\n'
             f'#define QUEUE_READY {int(queue_ready)}\n'
             f'#define COMPLETION_PORT {int(completion)}\n'
             f'#define TERMINATE {int(terminate)}\n'
             f'#define TERMINATE_IRP {int(terminate_irp)}\n'
             f'#define NATURAL_EXIT {int(natural_exit)}\n'
             f'#define APC_DEPTH {(2 if dpc or unwind else 1 if exception_divide else 3 if exception_nested else 2 if exception else 1 if create_pending or file_pending or file_packet and not file_cancel else int(not terminate_irp) if natural_exit else 2 if request.param=="apc_wait" or device_delete or file_device or file_create or control_dispatch or control_native or irp or sync and not sync_pending or terminate and not terminate_irp else 1)}\n'
             f'#define APC_MODE {0 if apc_post or apc_loop else 1}\n'
             f'#define EXPECTED_STATUS {0x12345678 if unwind else 5 if exception_divide else 42 if exception else 7 if file_jump else 0xc0000010 if irp_invalid else 192 if request.param=="apc_wait" else (0 if completion else 0x28200) if queue_ready else 258 if multiple or signal_wait or queue else 0}\n')
    defines+=f'#define TEST_STACK_TOP 0x{stack_top:x}u\n#define TEST_SEH_BASE 0x{seh_base:x}u\n'
    defines+='#define TEST_XBE '+json.dumps(str(root/'tools/conformance/test.xbe'))+'\n'
    defines+='#define TEST_ROOT '+json.dumps(str(work))+'\n'
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    monkeypatch=pytest.MonkeyPatch()
    try:
        for mode,enabled in [('original',False),('continuation',True)]:
            source=work/(mode+'.c')
            # Thread termination unwinds the real generated frame chain even
            # in the uninterrupted reference run; it never returns to its caller.
            source.write_text(defines+prefix+translate(monkeypatch,enabled or terminate or file_exit or create_exit or file_jump,program,**({"setjmp_fn":0x14000,"longjmp_fn":0x15000} if file_jump else {}))+suffix,encoding='utf-8')
            result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
                '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'src'),'/I'+str(root.parents[1]/'config'),str(source),str(root/'src/kernel/xbox_memory_layout.c'),str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/kernel_sync.c'),str(root/'src/kernel/recomp_continuation.c'),
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
                            capture_output=True,text=True,errors="replace",check=True,timeout=20)
    if resume=='load':
        saved=subprocess.run([str(native_waits/'continuation.exe'),'save',str(snapshot)],
                             capture_output=True,text=True,errors="replace",timeout=20)
        assert saved.returncode==23,saved.stdout+saved.stderr
    restored=subprocess.run([str(native_waits/'continuation.exe'),resume,str(snapshot)],
                            capture_output=True,text=True,errors="replace",timeout=20)
    assert restored.returncode==0,restored.stdout+restored.stderr
    assert restored.stdout==expected.stdout
