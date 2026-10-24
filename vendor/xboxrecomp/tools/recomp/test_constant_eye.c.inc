static void constant_eye_set(const float eye[3]) {
    for(unsigned i=0;i<3;i++){uint32_t bits;memcpy(&bits,eye+i,4);method(NV097_SET_EYE_VECTOR+i*4,bits);assert(nv2a_get_state()->pgraph.regs[NV_PGRAPH_EYEVEC0+i*4]==bits);}
}
static void verify_constant_eye_texture(void) {
    assert(sizeof(NV2APSConstants)==496);seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[2][6]={{0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,0xffff00ff,0xff00ffff},{0xff123456,0xff234567,0xff345678,0xff456789,0xff56789a,0xff6789ab}};
    const float directions[6][3]={{1,.3f,.125f},{-1,.3f,.125f},{.125f,1,.3f},{.125f,-1,.3f},{.3f,.125f,1},{.3f,.125f,-1}};
    for(unsigned stage=2;stage<4;stage++){unsigned offset=stage*64,address=0x80000+(stage-2)*0x10000;method(NV097_SET_TEXTURE_OFFSET+offset,address);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|5);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);unsigned stride=tex_face_stride(&s_gpu.texs[stage]);for(unsigned face=0;face<6;face++){uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80000000u+address+face*stride);for(unsigned j=0;j<4;j++)data[j]=colors[stage-2][face];}}
    float source[4]={1,0,0,1};bump_coordinates(0,source);method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);
    for(unsigned pair=0;pair<2;pair++)for(unsigned map_variant=0;map_variant<2;map_variant++){
        unsigned mapping=map_variant?4:0;method(NV097_SET_DOT_RGBMAPPING,mapping|(mapping<<4)|(mapping<<8));
        method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|((pair?11u:17u)<<10)|(18u<<15));
        for(unsigned stage=1;stage<4;stage++){float coordinate[4]={stage==3?2.f:0,0,0,stage==3?-1.f:0};bump_coordinates(stage,coordinate);}
        unsigned shader_count=0;
        for(unsigned face=0;face<6;face++){
            float eye[3]={-directions[face][0],-directions[face][1],directions[face][2]};constant_eye_set(eye);
            method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);assert(shadow_draw_for_test()==colors[1][face]);if(face)assert(combiner_test_shader_count()==shader_count);else shader_count=combiner_test_shader_count();
        }
        if(pair){method(NV097_SET_COMBINER_COLOR_ICW,(10u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(26u<<24)|0x200000);assert(shadow_draw_for_test()==colors[0][4]);}
    }
    method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);method(NV097_SET_DOT_RGBMAPPING,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(18u<<15));float eye[3]={0,0,1};constant_eye_set(eye);assert(shadow_draw_for_test()==colors[1][4]);
    FILE *checkpoint=tmpfile();assert(checkpoint && nv2a_gpu_save_checkpoint(checkpoint));eye[2]=-1;constant_eye_set(eye);assert(shadow_draw_for_test()==colors[1][5]);rewind(checkpoint);assert(nv2a_gpu_load_checkpoint(checkpoint));fclose(checkpoint);assert(shadow_draw_for_test()==colors[1][4]);
    method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(12u<<15));assert(shadow_draw_for_test()==colors[1][5]);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(18u<<15));assert(shadow_draw_for_test()==colors[1][4]);
    unsigned offset=3*64;method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_LIN_A8R8G8B8<<8)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80090000u);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)data[y*4+x]=0xff000000|(x*64<<16)|(y*64<<8)|0x55;
    float remap_eye[3]={-.8f,-.3f,1};constant_eye_set(remap_eye);assert(shadow_draw_for_test()==0xffc04055);remap_eye[0]=-.3f;remap_eye[1]=-.8f;constant_eye_set(remap_eye);assert(shadow_draw_for_test()==0xff40c055);
    fprintf(stderr,"[CONSTANT EYE] original181C..1824standingregs,SPECsameformula,constEvsinterpolatedW,6facesmaps0/4,DIFFlookaheadpair,dynamicCB/cacheidentity,checkpointrestore,2Dremap passed\n");
}
