/* Same original sixteen DWORD sites, through the existing two transports. */
typedef struct DirectGpuOp { uint32_t address,value; unsigned write; } DirectGpuOp;
static void direct_gpu_ops(DirectGpuOp op[16],uint32_t pattern) {
    DirectGpuOp authored[16]={
        {0xfd000140,0,0},{0xfd000140,0,1},{0xfd000100,0,0},
        {0xfd400720,0,1},{0xfd400100,0,0},{0xfd400704,0,0},
        {0xfd400108,0,0},{0xfd400100,pattern,1},{0xfd400708,0,0},
        {0xfd400720,1,1},{0xfd400100,0,0},{0xfd000140,pattern,1},
        {0xfd800044,0,0},{0xfd100410,0,0},
        {0xfd100410,0x10000,1},{0xfd100410,0,0}
    };
    memcpy(op,authored,sizeof authored);
}
static uint32_t direct_gpu_step(const DirectGpuOp *op,int direct) {
    if(direct) {
        if(op->write) {nv2a_guest_write32(op->address,op->value);return op->value;}
        return nv2a_guest_read32(op->address);
    }
    const unsigned char load[]={0x8b,0x10},store[]={0x89,0x10};
    CONTEXT context={0};const unsigned char *code=op->write?store:load;
    context.Rip=(DWORD64)code;context.Rax=op->address;
    context.Rdx=0xfedcba9800000000ull|op->value;context.EFlags=0xad7;
    assert(nv2a_hook_handle_mmio(&context,0,op->address,op->write));
    assert(context.Rip==(DWORD64)code+2 && context.EFlags==0xad7);
    if(op->write)assert(context.Rdx==(0xfedcba9800000000ull|op->value));
    else assert(context.Rdx<=UINT32_MAX);
    return (uint32_t)context.Rdx;
}
static void direct_gpu_save(NV2AState *gpu,FILE *file,uint32_t result) {
    assert(nv2a_gpu_save_checkpoint(file));
    XboxGuestRegisters guest;xbox_CaptureGuestRegisters(&guest);
    assert(fwrite(&guest,sizeof guest,1,file)==1);
    assert(fwrite(&result,sizeof result,1,file)==1);
    LONG kick=ReadAcquire(&gpu->pfifo.fifo_kick);
    assert(fwrite(&kick,sizeof kick,1,file)==1);
}
static void direct_gpu_equal_files(FILE *a,FILE *b) {
    assert(fflush(a)==0 && fflush(b)==0);long bytes=ftell(a);assert(bytes>=0 && ftell(b)==bytes);
    rewind(a);rewind(b);unsigned char left[16384],right[16384];
    while(bytes) {size_t count=bytes>sizeof left?sizeof left:(size_t)bytes;
        assert(fread(left,count,1,a)==1 && fread(right,count,1,b)==1);
        assert(!memcmp(left,right,count));bytes-=(long)count;
    }
}
static void direct_gpu_restore(NV2AState *gpu,FILE *saved,const uint8_t *ram,
                               const uint8_t *ramin,const XboxGuestRegisters *guest) {
    memcpy(gpu->vram_ptr,ram,0x4000000);memcpy(gpu->ramin_ptr,ramin,0x100000);
    rewind(saved);assert(nv2a_gpu_load_checkpoint(saved));assert(xbox_RestoreGuestRegisters(guest));
}
static void direct_gpu_sequence(NV2AState *gpu,const DirectGpuOp op[16],int direct,FILE *observed) {
    for(unsigned i=0;i<16;i++) {
        XboxGuestRegisters before,after;xbox_CaptureGuestRegisters(&before);
        uint32_t value=direct_gpu_step(&op[i],direct);
        xbox_CaptureGuestRegisters(&after);assert(!memcmp(&before,&after,sizeof before));
        if(observed)direct_gpu_save(gpu,observed,value);
    }
}
static void direct_gpu_seed(NV2AState *gpu,unsigned state,uint32_t pattern) {
    const uint32_t pending[]={0,NV_PGRAPH_INTR_NOTIFY,NV_PGRAPH_INTR_CONTEXT_SWITCH,
        NV_PGRAPH_INTR_NOTIFY|NV_PGRAPH_INTR_ERROR};
    gpu->pgraph.pending_interrupts=pending[state];
    gpu->pgraph.enabled_interrupts=0xffffffff;
    gpu->pgraph.waiting_for_nop=(pending[state]&NV_PGRAPH_INTR_ERROR)!=0;
    gpu->pgraph.regs[NV_PGRAPH_NSOURCE]=state==3?NV_PGRAPH_NSOURCE_NOTIFICATION:0;
    gpu->pgraph.regs[NV_PGRAPH_TRAPPED_ADDR]=pattern;
    gpu->pgraph.regs[NV_PGRAPH_TRAPPED_DATA_LOW]=~pattern;
    gpu->pfifo.regs[NV_PFIFO_MODE]=1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PUSH1]=0;
    WriteRelease((volatile LONG *)&gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],pattern&0x03fffffcu);
    nv2a_mmio_write(gpu,NV_PMC_INTR_EN_0,1,4);
}
static void direct_gpu_compare(NV2AState *gpu) {
    uint8_t *start=malloc(0x4000000),*reference=malloc(0x4000000);
    uint8_t *ramin=malloc(0x100000),*ramin_reference=malloc(0x100000);
    assert(start&&reference&&ramin&&ramin_reference);
    for(unsigned i=0;i<256;i++) {
        gpu->vram_ptr[0x3000000+i]=(uint8_t)(i*73+19);
        gpu->ramin_ptr[i]=(uint8_t)(i*31+7);
    }
    memcpy(start,gpu->vram_ptr,0x4000000);memcpy(ramin,gpu->ramin_ptr,0x100000);
    XboxGuestRegisters guest;xbox_CaptureGuestRegisters(&guest);
    guest.eax=0x12345678;guest.ecx=0x89abcdef;guest.edx=0x76543210;
    guest.ebx=0x13579bdf;guest.esi=0x2468ace0;guest.edi=0xfedcba98;
    guest.eflags=0x8d7;guest.fp_control_word=0x37f;guest.fp_status_word=0x45;
    guest.mxcsr=0x3f80;for(unsigned i=0;i<sizeof guest.xmm;i++)((uint8_t *)guest.xmm)[i]=(uint8_t)(i*31+7);
    assert(xbox_RestoreGuestRegisters(&guest));xbox_CaptureGuestRegisters(&guest);
    FILE *base=tmpfile();assert(base&&nv2a_gpu_save_checkpoint(base));
    const uint32_t patterns[]={0,0xffffffff,0x80000000,0x5aa5c33c};
    LARGE_INTEGER frozen,ending;xbox_RuntimePerformanceCounter(&frozen);
    unsigned cases=0;long compared_bytes=0;
    for(unsigned state=0;state<4;state++)for(unsigned p=0;p<4;p++) {
        DirectGpuOp op[16];direct_gpu_ops(op,patterns[p]);
        direct_gpu_restore(gpu,base,start,ramin,&guest);direct_gpu_seed(gpu,state,patterns[p]);
        FILE *saved=tmpfile(),*a=tmpfile(),*b=tmpfile();assert(saved&&a&&b);
        assert(nv2a_gpu_save_checkpoint(saved));
        direct_gpu_restore(gpu,saved,start,ramin,&guest);direct_gpu_sequence(gpu,op,0,a);
        memcpy(reference,gpu->vram_ptr,0x4000000);memcpy(ramin_reference,gpu->ramin_ptr,0x100000);
        direct_gpu_restore(gpu,saved,start,ramin,&guest);direct_gpu_sequence(gpu,op,1,b);
        compared_bytes+=ftell(a);direct_gpu_equal_files(a,b);
        assert(!memcmp(reference,gpu->vram_ptr,0x4000000));
        assert(!memcmp(ramin_reference,gpu->ramin_ptr,0x100000));
        fclose(saved);fclose(a);fclose(b);cases++;
    }
    xbox_RuntimePerformanceCounter(&ending);assert(frozen.QuadPart==ending.QuadPart);
    /* One fixed ABBA of the sixteen device accesses, without Windows AV dispatch. */
    double seconds[4];const int order[]={0,1,1,0};LARGE_INTEGER frequency,t0,t1;
    QueryPerformanceFrequency(&frequency);DirectGpuOp op[16];direct_gpu_ops(op,0x5aa5c33c);
    for(unsigned pass=0;pass<4;pass++) {
        direct_gpu_restore(gpu,base,start,ramin,&guest);direct_gpu_seed(gpu,3,0x5aa5c33c);
        for(unsigned warm=0;warm<64;warm++)for(unsigned i=0;i<16;i++)direct_gpu_step(&op[i],order[pass]);
        QueryPerformanceCounter(&t0);
        for(unsigned loop=0;loop<65536;loop++)for(unsigned i=0;i<16;i++)direct_gpu_step(&op[i],order[pass]);
        QueryPerformanceCounter(&t1);seconds[pass]=(double)(t1.QuadPart-t0.QuadPart)/frequency.QuadPart;
    }
    fclose(base);free(start);free(reference);free(ramin);free(ramin_reference);
    printf("DIRECT_GPU_RESULT {\"cases\":%u,\"operations_per_route\":%u,\"whole_ram_bytes_per_case\":67108864,\"whole_ramin_bytes_per_case\":1048576,\"guest_state_bytes\":%zu,\"serialized_bytes_per_route\":%ld,\"clock_exclusions\":0,\"abba_method_seconds\":[%.9f,%.9f,%.9f,%.9f]}\n",cases,cases*16,sizeof guest,compared_bytes,seconds[0],seconds[1],seconds[2],seconds[3]);
}
