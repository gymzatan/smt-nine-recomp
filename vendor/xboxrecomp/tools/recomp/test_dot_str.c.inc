static void verify_dot_str_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[16]={0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040,0xff903050,0xffa06070,0xffb09090,0xffc0c0b0,0xffd03010,0xffe06020,0xfff09030,0xff00c040};
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(13u<<15));method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);
    for(unsigned layout=0;layout<2;layout++) {
        unsigned offset=3*64;method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|((layout?D3DFMT_LIN_A8R8G8B8:D3DFMT_A8R8G8B8)<<8)|(1u<<16)|(2u<<20)|(2u<<24)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)data[layout?y*4+x:swizzle_offset(x,y,4,4)]=colors[y*4+x];
        for(unsigned mapping=0;mapping<=4;mapping++) {
            method(NV097_SET_DOT_RGBMAPPING,mapping|(mapping<<4)|(mapping<<8));float source[4]={mapping==2||mapping==3?127.f/255:1,0,0,1};bump_coordinates(0,source);unsigned shader_count=0;
            for(unsigned swap=0;swap<2;swap++)for(unsigned third=0;third<2;third++) {
                float scale=layout?4.f:1.f,first[4]={scale*(swap?.8f:.3f),0,0,5},second[4]={scale*(swap?.3f:.8f),0,0,7},last[4]={third?100.f:.05f,0,0,11};bump_coordinates(1,first);bump_coordinates(2,second);bump_coordinates(3,last);
                unsigned index=swap?7:13;uint32_t pixel=shadow_draw_for_test();if(pixel!=colors[index])fprintf(stderr,"[DOT STR FAIL] layout=%u map=%u swap=%u third=%u actual=%08x expected=%08x\n",layout,mapping,swap,third,pixel,colors[index]);assert(pixel==colors[index]);if(swap||third)assert(combiner_test_shader_count()==shader_count);else shader_count=combiner_test_shader_count();
            }
        }
    }
    FILE *state=tmpfile();assert(state && nv2a_gpu_save_checkpoint(state));method(NV097_SET_SHADER_STAGE_PROGRAM,0);rewind(state);assert(nv2a_gpu_load_checkpoint(state));fclose(state);assert(shadow_draw_for_test()==colors[7]);
    fprintf(stderr,"[DOT STR] mode13dim2dot1/2xy,thirdandQnotprojected,linear/Mortonmaps0..4,originalmapper/sampler/cache andcheckpoint passed\n");
}
