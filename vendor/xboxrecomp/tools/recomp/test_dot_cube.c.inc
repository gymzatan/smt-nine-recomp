static void verify_dot_cube_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    unsigned offset=3*64;method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|5);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
    const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,0xffff00ff,0xff00ffff};const float directions[6][3]={{1,.25f,.125f},{-1,.25f,.125f},{.125f,1,.25f},{.125f,-1,.25f},{.25f,.125f,1},{.25f,.125f,-1}};
    unsigned stride=tex_face_stride(&s_gpu.texs[3]);assert(stride==128);for(unsigned face=0;face<6;face++){uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u+face*stride);for(unsigned i=0;i<4;i++)data[i]=colors[face];}
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(14u<<15));method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);
    for(unsigned map=0;map<4;map++) {
        method(NV097_SET_DOT_RGBMAPPING,map|(map<<4)|(map<<8));float source[4]={map>=2?127.f/255:1,0,0,1};bump_coordinates(0,source);unsigned cache_count=0;
        for(unsigned face=0;face<6;face++) {
            for(unsigned stage=1;stage<4;stage++){float coordinate[4]={directions[face][stage-1],0,0,1};bump_coordinates(stage,coordinate);}
            uint32_t pixel=shadow_draw_for_test();if(pixel!=colors[face])fprintf(stderr,"[DOT CUBE FAIL] map=%u face=%u actual=%08x expected=%08x\n",map,face,pixel,colors[face]);assert(pixel==colors[face]);if(face)assert(combiner_test_shader_count()==cache_count);else cache_count=combiner_test_shader_count();
        }
    }
    /* Original CUBEMAP and DOT_STR_CUBE share one existing face walk. */
    method(NV097_SET_SHADER_STAGE_PROGRAM,3u<<15);for(unsigned face=0;face<6;face++){float c[4]={directions[face][0],directions[face][1],directions[face][2],1};bump_coordinates(3,c);assert(shadow_draw_for_test()==colors[face]);}
    method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(14u<<15));
    uint32_t *first=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);for(unsigned i=0;i<4;i++)first[i]=0xff123456;for(unsigned stage=1;stage<4;stage++){float coordinate[4]={directions[0][stage-1],0,0,1};bump_coordinates(stage,coordinate);}assert(shadow_draw_for_test()==0xff123456);
    fprintf(stderr,"[DOT CUBE] originalcube sampler/cache,6faces/maps0..3,localdotchain,coordinatechangesreuse shader,realRAMinvalidation passed\n");
}
