static void verify_depth_raw_comparisons(void) {
    method(NV097_SET_STENCIL_TEST_ENABLE,0);method(NV097_SET_CONTROL0,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);method(NV097_SET_SURFACE_PITCH,32u|(32u<<16));
    method(NV097_SET_SURFACE_ZETA_OFFSET,0x30000);method(NV097_SET_COLOR_CLEAR_VALUE,0xff334455);
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
    for(unsigned format=1;format<=2;format++) {
        unsigned maximum=format==1?65535:16777215;
        method(NV097_SET_SURFACE_FORMAT,8|(format<<4));polygon_offset_float(NV097_SET_CLIP_MAX,(float)maximum);
        const unsigned codes[]={0,1,511,format==1?0x6543:0x654321,maximum-1,maximum};
        for(unsigned c=0;c<sizeof codes/sizeof codes[0];c++)for(unsigned cmp=0;cmp<3;cmp++)for(int delta=-1;delta<=1;delta++) {
            unsigned code=codes[c];if((!code && delta<0)||(code==maximum && delta>0))continue;
            unsigned incoming=(unsigned)((int)code+delta);
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?code:code<<8);method(NV097_CLEAR_SURFACE,0xf1);
            struct {float x,y,z,w;uint32_t color;} data[3]={{0,0,(float)incoming,1,0xffff0000},{8,0,(float)incoming,1,0xffff0000},{0,8,(float)incoming,1,0xffff0000}};
            memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
            method(NV097_SET_DEPTH_FUNC,cmp==0?0x202:cmp==1?0x203:0x206);
            method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
            int pass=cmp==0?incoming==code:cmp==1?incoming<=code:incoming>=code;
            uint32_t actual=BRIDGE_MEM32(s_gpu.color_base);if(actual!=(pass?0xffff0000u:0xff334455u))fprintf(stderr,"depth compare fmt=%u code=%x in=%x cmp=%u actual=%x\n",format,code,incoming,cmp,actual);
            assert(actual==(pass?0xffff0000u:0xff334455u));
        }
    }
    method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x201);method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);
}
static uint32_t depth_raw_for_test(HwDepth *d,unsigned x,unsigned y) {
    const uint8_t *p=d->memory->host_view+d->memory_offset+hw_depth_offset(d,x,y);
    return d->format==1?*(const uint16_t *)p:*(const uint32_t *)p;
}
static void verify_depth_ram_coherence(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    for(unsigned swizzled=0;swizzled<2;swizzled++)for(unsigned format=1;format<=2;format++) {
        unsigned bpp=format==1?2:4,pitch=swizzled?8*bpp:48;
        uint32_t address=0x800c0000u+(swizzled*2+format)*0x10000;
        method(NV097_SET_SURFACE_ZETA_OFFSET,address-0x80000000u);
        method(NV097_SET_SURFACE_PITCH,32u|(48u<<16));
        method(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);
        method(NV097_SET_SURFACE_FORMAT,8|(format<<4)|(swizzled?0x03030200:0));
        uint8_t *guest=(uint8_t *)xbox_GetMemoryOffset()+address;
        memset(guest,0xa5,4096);
        HwDepth *d=hw_depth();assert(d->pitch==pitch && d->width==8 && d->height==8);
        uint8_t *mem=d->memory->host_view+d->memory_offset;
        /* Test exact encoded endpoints before restoring the RAM pattern. */
        const uint32_t codes[]={0,1,511,format==1?0x6543:0x654321,format==1?0xfffe:0xfffffe,format==1?0xffff:0xffffff};
        pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
        for(unsigned code=0;code<sizeof codes/sizeof codes[0];code++) {
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?codes[code]:(codes[code]<<8)|0x59);method(NV097_CLEAR_SURFACE,format==1?1:3);
            if(format==1){uint16_t raw=*(volatile uint16_t *)guest;assert(raw==codes[code]);}
            else {uint32_t raw=*(volatile uint32_t *)guest;assert(raw==((codes[code]<<8)|0x59));}
        }
        int held_reset=nv2a_pb_host_write_begin(guest,4096);memset(guest,0xa5,4096);nv2a_pb_host_write_end(held_reset);
        /* Partial clear must retain initial RAM bits, row padding and stencil. */
        pg->regs[NV_PGRAPH_CLEARRECTX]=(5u<<16)|2;pg->regs[NV_PGRAPH_CLEARRECTY]=(6u<<16)|1;
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?0x1357:0x2468ac7b);
        method(NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_Z);
        assert(d->dirty);uint64_t reads=*(volatile uint64_t *)&s_gpu.hardware_cpu_read_faults;
        volatile uint8_t observed=*(volatile uint8_t *)guest;(void)observed;MemoryBarrier();
        assert(s_gpu.hardware_cpu_read_faults==reads+1 && !d->dirty);
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            uint32_t expected=x>=2&&x<=5&&y>=1&&y<=6?(format==1?0x1357:0x2468aca5):(format==1?0xa5a5:0xa5a5a5a5);
            assert(depth_raw_for_test(d,x,y)==expected);
        }
        if(!swizzled)for(unsigned y=0;y<8;y++)for(unsigned k=8*bpp;k<pitch;k++)assert(mem[y*pitch+k]==0xa5);
        /* CPU alias writes invalidate this same image; masked clear consumes them. */
        uint8_t *alias=(uint8_t *)d->memory->views.cpu_views[d->memory->views.view_count-1]+d->memory_offset;
        if(format==1)*(volatile uint16_t *)alias=0x4321;else *(volatile uint32_t *)alias=0x76543219;
        MemoryBarrier();assert(d->upload_needed);d=hw_depth();assert(!d->upload_needed);
        uint32_t *pixels=malloc((size_t)8*8*d3d8_RenderScale()*d3d8_RenderScale()*8);assert(pixels && hw_depth_transfer(d,pixels,0,1));
        float z;memcpy(&z,pixels,4);float maximum=format==1?65536.0f:16777216.0f;
        assert(fabsf(z-(float)(format==1?0x4321:0x765432)/maximum)<1e-7f);
        if(format==2)assert((pixels[1]&255)==0x19);free(pixels);
        if(format==2) {
            pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xffffff63);method(NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_STENCIL);
            volatile uint32_t raw=*(volatile uint32_t *)guest;assert(raw==0x76543263);
        }
        /* Same-address growth preserves existing guest bytes, loads new rows. */
        if(!swizzled) {
            unsigned oldheight=d->height;
            int held=nv2a_pb_host_write_begin(guest+oldheight*pitch,4*pitch);memset(guest+oldheight*pitch,0x3c,4*pitch);nv2a_pb_host_write_end(held);
            uint32_t first=depth_raw_for_test(d,0,0);
            method(NV097_SET_SURFACE_CLIP_VERTICAL,12u<<16);d=hw_depth();assert(d->height==12 && depth_raw_for_test(d,0,0)==first);
            method(NV097_SET_SURFACE_CLIP_VERTICAL,4u<<16);assert(hw_depth()==d && d->height==12);
            pixels=malloc((size_t)8*12*d3d8_RenderScale()*d3d8_RenderScale()*8);assert(pixels && hw_depth_transfer(d,pixels,0,1));
            size_t k=(size_t)8*d3d8_RenderScale()*8*d3d8_RenderScale()*2;memcpy(&z,pixels+k,4);
            assert(fabsf(z-(float)(format==1?0x3c3c:0x3c3c3c)/maximum)<1e-7f);free(pixels);
        }
        if(format==2) {
            /* The original semaphore writer shares depth ownership, including
             * bytes adjacent to a dirty depth image on the same guarded page. */
            pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=3u<<16;
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x654321ff);method(NV097_CLEAR_SURFACE,3);
            size_t at=hw_depth_offset(d,1,0);hw_defer_output(guest+at,0x2468ac10);
            uint32_t written=*(volatile uint32_t *)(guest+at);MemoryBarrier();assert(written==0x2468ac10);
            assert(depth_raw_for_test(d,0,0)==0x654321ff);
            d=hw_depth();assert(!d->upload_needed);
            /* Sampling the same RAM routes through the original texture cache. */
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x1234569a);method(NV097_CLEAR_SURFACE,3);
            s_gpu.texs[0]=(Texture){.offset=d->address,.width=8,.height=4,.pitch=d->pitch,.color=swizzled?6:0x12,.levels=1,.valid=1};
            s_gpu.rc.stage_program=1;hw_texture_bind(0,hw_color(0));
            HwTexture *entry=s_hw_textures;assert(entry && *(uint32_t *)entry->raw==0x1234569a && !d->dirty);
            s_gpu.rc.stage_program=0;
        }
        if(!swizzled) {
            uint32_t first=depth_raw_for_test(d,0,0);
            method(NV097_SET_SURFACE_PITCH,64u|(64u<<16));d=hw_depth();assert(d->width==16 && d->pitch==64 && depth_raw_for_test(d,0,0)==first);
            method(NV097_SET_SURFACE_PITCH,32u|(48u<<16));d=hw_depth();assert(d->width==8 && d->pitch==48 && depth_raw_for_test(d,0,0)==first);
        }
        FILE *snapshot=tmpfile();assert(snapshot && nv2a_gpu_save_checkpoint(snapshot));
        rewind(snapshot);assert(nv2a_gpu_load_checkpoint(snapshot));fclose(snapshot);
        d=hw_depth();assert(d->memory && !d->upload_needed && d->pitch==pitch);
    }
    verify_depth_raw_comparisons();
    FILE *bad=tmpfile();assert(bad && hw_save_depths(bad));rewind(bad);uint32_t count;HwDepthCheckpoint header;
    assert(fread(&count,4,1,bad)==1 && count && fread(&header,sizeof header,1,bad)==1);
    header.normalization=header.format==1?65535:16777215;assert(!fseek(bad,4,SEEK_SET) && fwrite(&header,sizeof header,1,bad)==1);rewind(bad);
    HwDepth *rejected=NULL;HwColor *rejected_colors=NULL;assert(!hw_prepare_depths(bad,&rejected,&rejected_colors) && !rejected && !rejected_colors);fclose(bad);
    /* Actual drawing publishes the depth produced by the existing raster path. */
    method(NV097_SET_SURFACE_FORMAT,8|(2<<4));method(NV097_SET_SURFACE_PITCH,32u|(32u<<16));
    method(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);method(NV097_SET_SURFACE_ZETA_OFFSET,0x30000);
    pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
    method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xffffff00);method(NV097_CLEAR_SURFACE,3);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x30000);HwColor *alias_color=hw_color(0);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x20000);HwColor *target=hw_color(0);
    method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x11223344);method(NV097_CLEAR_SURFACE,3);
    s_gpu.texs[0]=(Texture){.offset=0x80030000,.width=8,.height=8,.pitch=32,.color=0x12,.levels=1,.valid=1};
    s_gpu.rc.stage_program=1;hw_texture_bind(0,target);
    assert(!hw_depth()->dirty && !alias_color->upload_needed);
    uint32_t alias_pixels[64];
    if(alias_color->native_surface)assert(nvk_image_read(alias_color->native_surface,alias_pixels,32));
    else {D3DLOCKED_RECT lock;assert(SUCCEEDED(alias_color->surface->lpVtbl->LockRect(alias_color->surface,&lock,NULL,D3DLOCK_READONLY)));for(unsigned y=0;y<8;y++)memcpy(alias_pixels+y*8,(uint8_t *)lock.pBits+y*lock.Pitch,32);alias_color->surface->lpVtbl->UnlockRect(alias_color->surface);}
    assert(alias_pixels[0]==0x11223344);s_gpu.rc.stage_program=0;
    method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xffffff00);method(NV097_CLEAR_SURFACE,3);
    vertices(0.25f);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    HwDepth *d=hw_depth();assert(d->dirty);
    volatile uint32_t raw=*(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset()+d->address+2*32+2*4);assert((raw>>8)>=0x3fffff && (raw>>8)<=0x400001);
    fprintf(stderr,"[DEPTH RAM] actual pitched/swizzled Z16/Z24S8, masked clears, CPU alias upload, growth/shrink, checkpoint and draw writeback passed\n");
}







static void verify_depth_write_guards(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();vertices(0.25f);
    HwDepth *d=hw_depth();assert(!d->dirty);uint32_t original=depth_raw_for_test(d,0,0);
    /* A distinct colour surface occupies the same physical page. */
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x30400);hw_color(0);method(NV097_SET_SURFACE_COLOR_OFFSET,0x20000);
    depth_guard_watch=(uintptr_t)d->memory->views.cpu_views[0]+d->memory_offset;
    method(NV097_SET_DEPTH_FUNC,0x207);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_STENCIL_FUNC,0x207);method(NV097_SET_STENCIL_OP_FAIL,0x1e00);method(NV097_SET_STENCIL_OP_ZFAIL,0x1e00);
    for(unsigned mode=0;mode<4;mode++) {
        method(NV097_SET_STENCIL_TEST_ENABLE,mode!=0);method(NV097_SET_CONTROL0,mode==3?0:NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
        method(NV097_SET_STENCIL_MASK,mode==1?0:255);method(NV097_SET_STENCIL_OP_ZPASS,mode==2?0x1e00:0x1e01);
        unsigned reads=depth_transfer_test_reads;depth_guard_test_calls=0;
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        assert(!d->dirty && !depth_guard_test_calls);nv2a_pb_checkpoint_publish();
        assert(depth_transfer_test_reads==reads && depth_raw_for_test(d,0,0)==original);
    }
    method(NV097_SET_STENCIL_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,1);
    depth_guard_test_calls=0;
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    assert(d->dirty && depth_guard_test_calls);
    unsigned reads=depth_transfer_test_reads;depth_guard_test_calls=0;
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x30400);
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    assert(d->dirty && !depth_guard_test_calls && reads==depth_transfer_test_reads);
    for(unsigned i=0;i<d->memory->views.view_count;i++){MEMORY_BASIC_INFORMATION page;assert(VirtualQuery((uint8_t *)d->memory->views.cpu_views[i]+d->memory_offset,&page,sizeof page) && page.Protect==PAGE_NOACCESS);}
    /* Use the same owner-publication boundary as NtProtectVirtualMemory and
     * MmSetAddressProtect, before the real host permission change. */
    int host_write=nv2a_pb_host_write_begin((void *)depth_guard_watch,hw_depth_bytes(d));
    assert(!d->dirty && depth_transfer_test_reads==reads+1);
    DWORD previous;assert(VirtualProtect((void *)depth_guard_watch,hw_depth_bytes(d),PAGE_READONLY,&previous));
    nv2a_pb_host_write_end(host_write);
    depth_guard_test_calls=0;
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    assert(d->dirty && depth_guard_test_calls);nv2a_pb_checkpoint_publish();
    assert(!d->dirty && depth_transfer_test_reads==reads+2);
    method(NV097_SET_DEPTH_MASK,0);method(NV097_SET_STENCIL_TEST_ENABLE,1);
    method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);method(NV097_SET_STENCIL_MASK,255);method(NV097_SET_STENCIL_OP_ZPASS,0x1e01);
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);assert(d->dirty);
    nv2a_pb_checkpoint_publish();assert(depth_transfer_test_reads==reads+3);
    depth_guard_watch=0;
    fprintf(stderr,"[DEPTH GUARD] read-only depth and KEEP/mask/write-disabled stencil preserve RAM without readback; writable depth/stencil and original-view protection recovery passed\n");
}
