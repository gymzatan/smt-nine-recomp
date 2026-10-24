static void verify_dot_diffuse_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[2][6]={{0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,0xffff00ff,0xff00ffff},{0xff123456,0xff234567,0xff345678,0xff456789,0xff56789a,0xff6789ab}};
    const float directions[6][3]={{1,.25f,.125f},{-1,.25f,.125f},{.125f,1,.25f},{.125f,-1,.25f},{.25f,.125f,1},{.25f,.125f,-1}};
    for(unsigned stage=2;stage<4;stage++) {
        unsigned offset=stage*64,address=0x80000+(stage-2)*0x10000;method(NV097_SET_TEXTURE_OFFSET+offset,address);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|5);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        unsigned stride=tex_face_stride(&s_gpu.texs[stage]);assert(stride==128);for(unsigned face=0;face<6;face++){uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80000000u+address+face*stride);for(unsigned i=0;i<4;i++)data[i]=colors[stage-2][face];}
    }
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(11u<<10)|(12u<<15));
    for(unsigned map=0;map<4;map++) {
        method(NV097_SET_DOT_RGBMAPPING,map|(map<<4)|(map<<8));float source[4]={map>=2?127.f/255:1,0,0,1};bump_coordinates(0,source);
        for(unsigned target=0;target<2;target++) {
            method(NV097_SET_COMBINER_COLOR_ICW,((10u+target)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((26u+target)<<24)|0x200000);unsigned cache_count=0;
            for(unsigned face=0;face<6;face++) {
                for(unsigned stage=1;stage<4;stage++){float coordinate[4]={directions[face][stage-1],0,0,stage==3?1.f:0.f};bump_coordinates(stage,coordinate);}
                unsigned expected_face=target?(face<4?5:4):face;uint32_t expected=colors[target][expected_face],pixel=shadow_draw_for_test();if(pixel!=expected)fprintf(stderr,"[DOT DIFFUSE FAIL] map=%u target=%u face=%u actual=%08x expected=%08x\n",map,target,face,pixel,expected);assert(pixel==expected);if(face)assert(combiner_test_shader_count()==cache_count);else cache_count=combiner_test_shader_count();
            }
        }
    }
    fprintf(stderr,"[DOT DIFFUSE] primaryDIFFlookahead/SPECpair,6faces/maps0..3,stage3coordinateonce,originalcube sampler/cache passed\n");
}
