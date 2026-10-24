static void verify_project3d_volume(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);
    const uint32_t colors[8]={0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040};uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);xbox_swizzle_box(data,colors,2,2,2,4);
    for(unsigned stage=0;stage<4;stage++) {
        unsigned off=stage*64;method(NV097_SET_TEXTURE_OFFSET+off,0x80000);method(NV097_SET_TEXTURE_FORMAT+off,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|(1u<<28)|1);method(NV097_SET_TEXTURE_CONTROL0+off,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+off,3|(3<<8)|(3<<16));method(NV097_SET_TEXTURE_FILTER+off,0x01010000);
        method(NV097_SET_SHADER_STAGE_PROGRAM,2u<<(stage*5));method(NV097_SET_COMBINER_COLOR_ICW,((8+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((24+stage)<<24)|0x200000);
        for(unsigned corner=0;corner<8;corner++)for(unsigned variant=0;variant<3;variant++) {
            float x=(corner&1)?.75f:.25f,y=(corner&2)?.75f:.25f,z=(corner&4)?.75f:.25f;
            float q=variant==0?.5f:variant==1?2.0f:-2.0f,coordinate[4]={x*q,y*q,z*q,q};bump_coordinates(stage,coordinate);
            if(variant==0) {
                float *coords=(float *)(xbox_GetMemoryOffset()+0x80050000u+stage*0x100);
                const float varying_q[3]={.5f,2,4};for(unsigned v=0;v<3;v++){coords[v*4]=x*varying_q[v];coords[v*4+1]=y*varying_q[v];coords[v*4+2]=z*varying_q[v];coords[v*4+3]=varying_q[v];}
            }
            uint32_t actual=shadow_draw_for_test();assert(actual==colors[corner]);unsigned covered=0;
            const uint32_t *pixels=(const uint32_t *)(xbox_GetMemoryOffset()+s_gpu.color_base);for(unsigned i=0;i<64;i++)if(pixels[i]!=s_gpu.clear_color){assert(pixels[i]==colors[corner]);covered++;}assert(covered);
        }
    }
    fprintf(stderr,"[PROJECT3D VOLUME] four stages,eightXYZ corners,positive/negativeQ and per-vertex varyingQ,fullcoveredpixels passed\n");
}
