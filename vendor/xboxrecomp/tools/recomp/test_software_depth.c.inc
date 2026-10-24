static void verify_software_depth_ram(void) {
    assert(!hw_enabled());nv2a_get_state()->vram_ptr=(uint8_t *)xbox_GetMemoryOffset()+XBOX_CONTIG_BASE;seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    for(unsigned swizzled=0;swizzled<2;swizzled++)for(unsigned format=1;format<=2;format++) {
        method(NV097_SET_SURFACE_ZETA_OFFSET,0xc0000+(swizzled*2+format)*0x10000);
        method(NV097_SET_SURFACE_FORMAT,8|(format<<4)|(swizzled?0x03030200:0));method(NV097_SET_SURFACE_PITCH,32u|(48u<<16));
        ZetaSurface surface=zeta_surface();memset(surface.memory,0xa5,4096);
        pg->regs[NV_PGRAPH_CLEARRECTX]=(5u<<16)|2;pg->regs[NV_PGRAPH_CLEARRECTY]=(6u<<16)|1;
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?0x1357:0x2468ac7b);method(NV097_CLEAR_SURFACE,1);
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            uint8_t *p=zeta_pixel(&surface,x,y);uint32_t raw=surface.format==1?*(uint16_t *)p:*(uint32_t *)p;
            uint32_t expected=x>=2&&x<=5&&y>=1&&y<=6?(format==1?0x1357:0x2468aca5):(format==1?0xa5a5:0xa5a5a5a5);assert(raw==expected);
        }
        if(!swizzled)for(unsigned y=0;y<8;y++)for(unsigned k=8*(format==1?2:4);k<surface.pitch;k++)assert(surface.memory[y*surface.pitch+k]==0xa5);
        if(format==2) {
            pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x1234569a);method(NV097_CLEAR_SURFACE,3);
            s_gpu.texs[0]=(Texture){.offset=surface.address,.width=8,.height=8,.pitch=surface.pitch,.color=swizzled?6:0x12,.levels=1,.valid=1};uint32_t texel=0;assert(sample_texture(0,0,&texel) && texel==0x1234569a);memset(s_gpu.texs,0,sizeof s_gpu.texs);
            assert(nv_dma_span(nv2a_get_state(),0x100,surface.address-XBOX_CONTIG_BASE,4)==surface.memory);method(NV097_SET_CONTEXT_DMA_SEMAPHORE,0x100);method(NV097_SET_SEMAPHORE_OFFSET,surface.address-0x80000000u);method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x87654321);assert(*(uint32_t *)surface.memory==0x87654321);
        }
    }
    /* Raw CPU changes and both formats feed the original EQ/LE/GE consumers. */
    method(NV097_SET_SURFACE_FORMAT,8|(2<<4));method(NV097_SET_SURFACE_PITCH,32u|(32u<<16));
    verify_depth_raw_comparisons();
    /* Exercise actual worker partitioning on a large existing triangle. */
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_SURFACE_ZETA_OFFSET,0x180000);method(NV097_SET_SURFACE_COLOR_OFFSET,0x140000);
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,128u<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,128u<<16);method(NV097_SET_SURFACE_PITCH,512u|(512u<<16));
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,127u<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,127u<<16);
    pg->regs[NV_PGRAPH_CLEARRECTX]=127u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=127u<<16;method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xffffffa5);method(NV097_CLEAR_SURFACE,3);
    ZetaSurface surface=zeta_surface();
    /* CPU alias writes must affect the very next existing comparison. */
    XboxSharedMemoryViews views;assert(xbox_GetSharedMemoryViews(surface.memory,(size_t)surface.pitch*surface.height,&views));uint8_t *alias=(uint8_t *)views.cpu_views[views.view_count-1]+views.offset;
    *(uint32_t *)alias=0x000000a5;
    struct {float x,y,z,w;uint32_t color;} data[3]={{0,0,1,1,0xffff0000},{128,0,1,1,0xffff0000},{0,128,1,1,0xffff0000}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
    method(NV097_SET_DEPTH_FUNC,0x201);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    assert(*(uint32_t *)surface.memory==0x000000a5);assert(*(uint32_t *)zeta_pixel(&surface,16,16)==0x000001a5);assert(*(uint32_t *)zeta_pixel(&surface,127,127)==0xffffffa5);
    assert(s_pool.n==(unsigned)atoi(getenv("RECOMP_RASTER_THREADS")));
    FILE *gpu=tmpfile(),*memory=tmpfile();assert(gpu && memory && nv2a_gpu_save_checkpoint(gpu) && xbox_SavePhysicalMemory(memory));
    *(uint32_t *)surface.memory=0xdeadbeef;rewind(memory);assert(xbox_LoadPhysicalMemory(memory));rewind(gpu);assert(nv2a_gpu_load_checkpoint(gpu));assert(*(uint32_t *)surface.memory==0x000000a5);
    rewind(gpu);assert(fwrite("XBGPUC",6,1,gpu)==1);rewind(gpu);uint32_t before=s_gpu.clear_color;assert(!nv2a_gpu_load_checkpoint(gpu) && s_gpu.clear_color==before);fclose(gpu);fclose(memory);
    fprintf(stderr,"[SOFTWARE DEPTH RAM] pitched/Morton raw clears, aspect/padding, RAM texture/semaphore, CPU alias compare, original workers and paired checkpoint with old-cache rejection passed\n");
}

