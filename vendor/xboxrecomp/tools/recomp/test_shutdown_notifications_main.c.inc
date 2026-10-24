#undef esp
static void load_shutdown(void) {
    XboxGuestRegisters state;FILE *f=fopen(snapshot,"rb");assert(f);assert(fread(&state,sizeof state,1,f)==1);
    t_checkpoint_controller=1;assert(checkpoint_clear_objects());
    assert(xbox_LoadPhysicalMemory(f) && xbox_RestoreGuestRegisters(&state) && recomp_cont_load(f));
    assert(xbox_CheckpointLoadObjects(f,NULL,0));t_checkpoint_controller=0;assert(!fclose(f));
}
int main(int argc,char **argv) {
    assert(argc==5);snapshot=argv[3];native_entry=(unsigned)atoi(argv[4]);
    FILE *xbe=fopen(argv[2],"rb");assert(xbe);fseek(xbe,0,SEEK_END);long bytes=ftell(xbe);rewind(xbe);void *data=malloc(bytes);assert(data && fread(data,bytes,1,xbe)==1);fclose(xbe);assert(xbox_MemoryLayoutInit(data,bytes));free(data);
    _putenv_s("RECOMP_GUEST_LOCK","1");guest_cpu_join();kernel_data_init();g_esp=0x80000;
    uint32_t head=XBOX_KERNEL_DATA_BASE+KDATA_SHUTDOWN_HEAD;
    g_slot_ordinals[0]=99;g_slot_bridges[0]=bridge_KeDelayExecutionThread;g_slot_arg_bytes[0]=12;MEM32(0x20020)=KERNEL_VA_BASE;
    g_slot_ordinals[1]=49;g_slot_bridges[1]=native_entry?native_shutdown:bridge_HalReturnToFirmware;g_slot_arg_bytes[1]=4;MEM32(0x20024)=KERNEL_VA_BASE+4;
    if(!strcmp(argv[1],"order")) {
        mutations=1;xbox_KfRaiseIrql(1);
        register_record(0x28000,0x11300,10,0);register_record(0x28020,0x11300,10,1);
        register_record(0x28040,0x11300,5,0);register_record(0x28060,0x11300,-1,1);
        MEM32(0x28080)=0x11300;MEM32(0x28084)=20;
        assert(MEM32(head)==0x28008 && MEM32(0x28008)==0x28028 && MEM32(0x28028)==0x28048);
        xbox_HalRegisterShutdownNotification(XBOX_TO_NATIVE(0x28100),FALSE);assert(xbox_CurrentIrql()==1);
        xbox_HalNotifyShutdown();assert(xbox_CurrentIrql()==1 && MEM32(head)==head && MEM32(head+4)==head);
        const uint32_t order[]={0x28000,0x28080,0x28020,0x28060};assert(MEM32(0x20100)==4);
        for(unsigned i=0;i<4;i++)assert(MEM32(0x20110+i*4)==order[i]);
        xbox_HalNotifyShutdown();assert(MEM32(0x20100)==4);fprintf(stdout,"shutdown order/unregister/mutation/IRQL passed\n");return 0;
    }
    if(!strcmp(argv[1],"other")) {
        register_record(0x28000,0x11300,0,0);MEM32(g_esp)=4;bridge_HalReturnToFirmware();assert(0);
    }
    *(LONGLONG *)XBOX_TO_NATIVE(0x20080)=-7000000;
    if(strcmp(argv[1],"load")) {
        register_record(0x28000,0x11200,10,1);register_record(0x28020,0x11400,0,0);
        MEM32(g_esp)=0;g_esp+=4;
    } else load_shutdown();
    if(!strcmp(argv[1],"roundtrip"))g_slot_bridges[1]=native_notifications;
    xbox_kernel_set_checkpoint_hook(checkpoint);saving=!strcmp(argv[1],"save") || !strcmp(argv[1],"roundtrip");cold=!strcmp(argv[1],"save");began=GetTickCount64();
    xbox_RunGuestRoot(sub_00011000);
    if(!strcmp(argv[1],"roundtrip")) {
        assert(!recomp_cont_active() && !t_kernel_wait);load_shutdown();xbox_RunGuestRoot(sub_00011000);
        assert(!recomp_cont_active() && !t_kernel_wait && MEM32(head)==head);return 0;
    }
    assert(0);return 1;
}
