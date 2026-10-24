static void verify_tile_blit_guard(void) {
    NV2AState *gpu=nv2a_get_state();gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);seed();method(NV097_SET_BEGIN_END,0);
    uint8_t *source=gpu->vram_ptr+0xa0000,*destination=gpu->vram_ptr+0xb0000;for(unsigned i=0;i<16384;i++)source[i]=(uint8_t)(i*13+7);
    /* Primary BlitBeyondWidth disables tile-limit error interrupts. */
    nv2a_mmio_write(gpu,0x400880,0x28c3ff,4);
    for(unsigned tile=0;tile<8;tile++)nv2a_mmio_write(gpu,0x100000+NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(tile),0,4);
    for(unsigned active=0;active<2;active++)for(unsigned partial=0;partial<2;partial++) {
        unsigned pitch=partial?40:32,limit=0xb0000+4096-1;memset(destination,0xcc,8192);
        nv2a_mmio_write(gpu,0x100000+NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(3),0xb0000|active,4);nv2a_mmio_write(gpu,0x100000+NV_PFB_TILE_LIMIT(3),limit,4);nv2a_mmio_write(gpu,0x100000+NV_PFB_TILE_PITCH(3),pitch,4);
        image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0xa0000,0xb0000,64,pitch,0,0,0,0,16,140);
        for(unsigned i=0;i<8192;i++){unsigned row=i/pitch,x=i%pitch;int copied=row<140 && (!active || 0xb0000+i<=limit);assert(destination[i]==(copied?source[row*64+x]:0xcc));}
    }
    /* An active tile before the destination does not constrain this span. */
    nv2a_mmio_write(gpu,0x100000+NV_PFB_TILE_LIMIT(3),0xaffff,4);memset(destination,0xcc,160);image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0xa0000,0xb0000,64,32,0,0,0,0,16,3);for(unsigned i=0;i<96;i++)assert(destination[i]==source[(i/32)*64+i%32]);
    fprintf(stderr,"[TILE BLIT] active inclusive limit, partial finalrow, disabledtile and outside transfer guard passed\n");
}
