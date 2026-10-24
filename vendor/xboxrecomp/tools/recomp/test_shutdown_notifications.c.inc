#include "kernel/kernel_bridge.c"
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
void sub_00011000(void);void sub_00011200(void);
static unsigned mutations,native_entry;
static const char *snapshot;
static int saving,cold;
static ULONGLONG began;
static void shutdown_probe(void) {
    uint32_t record=MEM32(g_esp+4),head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
    assert(guest_cpu_is_held());assert(MEM32(head)!=record+8);
    unsigned count=MEM32(0x20100);MEM32(0x20110+count*4)=record;MEM32(0x20100)=count+1;
    assert(xbox_CurrentIrql()==(mutations?1:0));
    if(mutations && record==0x28000) {
        xbox_HalRegisterShutdownNotification(XBOX_TO_NATIVE(0x28040),FALSE);
        xbox_HalRegisterShutdownNotification(XBOX_TO_NATIVE(0x28080),TRUE);
    }
    g_esp+=8;
}
static void shutdown_final(void) {
    assert(MEM32(g_esp+4)==0x28020 && MEM32(0x2000c)==1 && MEM32(0x20010)==1);
    uint32_t head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
    assert(MEM32(head)==head && MEM32(head+4)==head && xbox_CurrentIrql()==0);
    fprintf(stdout,"shutdown complete once\n");fflush(stdout);g_esp+=8;
}
static void native_shutdown(void) {xbox_HalReturnToFirmware(2);}
static void native_notifications(void) {xbox_HalNotifyShutdown();}
recomp_func_t recomp_lookup(uint32_t va) {
    if(va==0x11000)return sub_00011000;if(va==0x11200)return sub_00011200;
    if(va==0x11300)return shutdown_probe;if(va==0x11400)return shutdown_final;if(va==0x11500)return native_shutdown;
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va){return va==0x11500?native_shutdown:va==0x11600?native_notifications:NULL;}
void recomp_icall_fail_log(uint32_t va){fprintf(stderr,"bad target %08x\n",va);exit(5);}
void recomp_icall_not_code_log(uint32_t va){recomp_icall_fail_log(va);}
static void register_record(uint32_t record,uint32_t callback,int32_t priority,int imported) {
    MEM32(record)=callback;MEM32(record+4)=(uint32_t)priority;
    if(imported){MEM32(g_esp)=record;MEM32(g_esp+4)=1;bridge_HalRegisterShutdownNotification();}
    else xbox_HalRegisterShutdownNotification(XBOX_TO_NATIVE(record),TRUE);
}
static void checkpoint(void) {
    if(saving && GetTickCount64()-began>=200) {
        assert(t_kernel_wait && MEM32(0x2000c)==1 && !MEM32(0x20010));
        uint32_t head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;assert(MEM32(head)==0x28028);
        XboxGuestRegisters registers;xbox_CaptureGuestRegisters(&registers);FILE *f=fopen(snapshot,"wb");assert(f);
        assert(fwrite(&registers,sizeof registers,1,f)==1);t_checkpoint_controller=1;
        assert(xbox_SavePhysicalMemory(f) && recomp_cont_save(f) && xbox_CheckpointWriteObjects(f));
        t_checkpoint_controller=0;assert(!fclose(f));saving=0;if(cold)exit(23);
    }
}
