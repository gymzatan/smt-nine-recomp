static void verify_dot_remap_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[2][16]={{0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040,0xff903050,0xffa06070,0xffb09090,0xffc0c0b0,0xffd03010,0xffe06020,0xfff09030,0xff00c040},{0xff102010,0xff204020,0xff306030,0xff408040,0xff502050,0xff604060,0xff706070,0xff808080,0xff903090,0xffa060a0,0xffb090b0,0xffc0c0c0,0xffd030d0,0xffe060e0,0xfff090f0,0xff00c000}};
    const float directions[7][3]={{1,.3f,-.8f},{-1,.8f,.3f},{.8f,1,-.3f},{.3f,-1,.8f},{.3f,.8f,1},{-.8f,.3f,-1},{2,2,1}};const unsigned indices[]={7,13,7,13,13,7,15};
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);
    for(unsigned layout=0;layout<2;layout++) {
        for(unsigned stage=2;stage<4;stage++) {
            unsigned offset=stage*64,address=0x80000+(stage-2)*0x10000;method(NV097_SET_TEXTURE_OFFSET+offset,address);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|((layout?D3DFMT_LIN_A8R8G8B8:D3DFMT_A8R8G8B8)<<8)|(2u<<20)|(2u<<24)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
            uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80000000u+address);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)data[layout?y*4+x:swizzle_offset(x,y,4,4)]=colors[stage-2][y*4+x];
        }
        for(unsigned variant=0;variant<3;variant++)for(unsigned map=0;map<4;map++) {
            method(NV097_SET_DOT_RGBMAPPING,map|(map<<4)|(map<<8));float source[4]={map>=2?127.f/255:1,0,0,1};bump_coordinates(0,source);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|((variant==2?11u:17u)<<10)|((variant?12u:14u)<<15));
            for(unsigned output=variant==2?2:3;output<4;output++) {
                method(NV097_SET_COMBINER_COLOR_ICW,((8u+output)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((24u+output)<<24)|0x200000);unsigned cache_count=0;
                for(unsigned direction=0;direction<7;direction++) {
                    for(unsigned stage=1;stage<4;stage++){float coordinate[4]={directions[direction][stage-1],0,0,directions[direction][stage-1]};bump_coordinates(stage,coordinate);}
                    uint32_t expected=colors[output-2][indices[direction]],pixel=shadow_draw_for_test();if(pixel!=expected)fprintf(stderr,"[DOT REMAP FAIL] layout=%u variant=%u map=%u output=%u direction=%u actual=%08x expected=%08x\n",layout,variant,map,output,direction,pixel,expected);assert(pixel==expected);if(direction)assert(combiner_test_shader_count()==cache_count);else cache_count=combiner_test_shader_count();
                }
            }
        }
    }
    fprintf(stderr,"[DOT REMAP] primarynoncube2Ddirectionformula,STR/SPEC/DIFF,6axesandtie,maps0..3,linear/swizzlednormalizedunits,originalsampler/cache passed\n");
}
