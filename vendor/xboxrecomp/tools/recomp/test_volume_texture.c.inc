static void volume_coordinates(float u,float v,float w) {
    float source[4]={1,0,0,1},first[4]={u,0,0,5},second[4]={v,0,0,7},third[4]={w,0,0,11};
    bump_coordinates(0,source);bump_coordinates(1,first);bump_coordinates(2,second);bump_coordinates(3,third);
}
static void volume_expect(uint32_t expected) {
    uint32_t actual=shadow_draw_for_test();
    fprintf(stderr,"[VOLUME PIXEL] actual=%08x expected=%08x\n",actual,expected);assert(actual==expected);
}
static void verify_standing_volume_w(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    unsigned stage=3*64;method(NV097_SET_TEXTURE_OFFSET+stage,0x80000);method(NV097_SET_TEXTURE_FORMAT+stage,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);method(NV097_SET_TEXTURE_CONTROL0+stage,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(15<<16));
    method(NV097_SET_SHADER_STAGE_PROGRAM,4u<<15);method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);float color[4]={.25f,.5f,.75f,1};bump_coordinates(3,color);uint32_t control=shadow_draw_for_test();
    method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);assert(shadow_draw_for_test()==control);
    if(hw_enabled()){fprintf(stderr,"[STANDING W] host=%u raw=%u\n",d3d8_GetTSS(3)[D3DTSS_NV2AADDRESSW],texture_address_w(3));assert(d3d8_GetTSS(3)[D3DTSS_NV2AADDRESSW]==D3DTADDRESS_WRAP);}
    fprintf(stderr,"[STANDING VOLUME W] nonsampling FORMAT3 keeps PASSTHRU color and original WRAP without consuming W15\n");
}
static void verify_volume_missing_palette(unsigned linear) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    unsigned stage=3*64;method(NV097_SET_TEXTURE_OFFSET+stage,0x80000);method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_P8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|(1u<<28)|1);method(NV097_SET_TEXTURE_CONTROL0+stage,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(3<<16));method(NV097_SET_TEXTURE_FILTER+stage,linear?0x02020000:0x01010000);
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_DOT_RGBMAPPING,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(13u<<15));volume_coordinates(.25f,.25f,.25f);shadow_draw_for_test();assert(!"missing palette must stop");
}
static void verify_volume_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_DOT_RGBMAPPING,0);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(17u<<5)|(17u<<10)|(13u<<15));method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);
    const unsigned stage=3*64;const uint32_t colors[8]={0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040};
    uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);
    method(NV097_SET_TEXTURE_OFFSET+stage,0x80000);method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|(1u<<28)|1);method(NV097_SET_TEXTURE_CONTROL0+stage,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(3<<16));method(NV097_SET_TEXTURE_FILTER+stage,0x01010000);
    assert(texture_volume(&s_gpu.texs[3]) && texture_depth(&s_gpu.texs[3])==2 && texture_volume_bytes(&s_gpu.texs[3])==32);
    assert(texture_address_w(3)==3);xbox_swizzle_box(data,colors,2,2,2,4);
    for(unsigned z=0;z<2;z++)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++) {
        volume_coordinates(x?.75f:.25f,y?.75f:.25f,z?.75f:.25f);volume_expect(colors[z*4+y*2+x]);
    }
    unsigned uploads=s_gpu.hardware_texture_uploads,shaders=combiner_test_shader_count();
    data[xbox_swizzle_offset_3d(1,1,1,2,2,2)]=0xffc05020;volume_expect(0xffc05020);
    if(hw_enabled()){assert(s_gpu.hardware_texture_uploads==uploads+1);assert(combiner_test_shader_count()==shaders);}
    /* W addressing is standing NV097 state, not a second sampler identity. */
    for(unsigned address=1;address<=5;address++) {
        method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(address<<16));method(NV097_SET_TEXTURE_BORDER_COLOR+stage,0xffa0b0c0);
        volume_coordinates(.25f,.25f,-.25f);volume_expect(address==1?colors[4]:address==4?0xffa0b0c0:colors[0]);
    }
    /* P=0 is still a 3D texture: linear W taps include border at w=0. */
    method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);
    uint32_t white[4]={0xffffffff,0xffffffff,0xffffffff,0xffffffff};xbox_swizzle_box(data,white,2,2,1,4);
    method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(4<<16));method(NV097_SET_TEXTURE_BORDER_COLOR+stage,0);method(NV097_SET_TEXTURE_FILTER+stage,0x02020000);volume_coordinates(.25f,.25f,0);
    float filtered[4];rc_texel(&s_gpu.texs[3],0,.25f,.25f,0,4,filtered);for(unsigned i=0;i<4;i++)assert(filtered[i]==.5f);
    uint32_t mixed=shadow_draw_for_test();
    /* Same original 2D sampler/output conversion control, without assuming host UNORM rounding. */
    method(NV097_SET_TEXTURE_FORMAT+stage,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);method(NV097_SET_TEXTURE_ADDRESS+stage,4|(3<<8));volume_coordinates(0,.25f,0);assert(shadow_draw_for_test()==mixed);
    method(NV097_SET_TEXTURE_FILTER+stage,0x01010000);method(NV097_SET_TEXTURE_FORMAT+stage,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);volume_coordinates(.25f,.25f,0);volume_expect(0xffffffff);
    method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(4<<16));method(NV097_SET_TEXTURE_FILTER+stage,0x02020000);assert(shadow_draw_for_test()==mixed);
    FILE *state=tmpfile();assert(state && nv2a_gpu_save_checkpoint(state));method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(1<<16));rewind(state);assert(nv2a_gpu_load_checkpoint(state));fclose(state);assert(texture_address_w(3)==4 && shadow_draw_for_test()==mixed);
    /* Equal old key fields, including byte span128: 2D and volume must keep different views. */
    uint32_t slices[32];for(unsigned i=0;i<32;i++)slices[i]=colors[i/4];xbox_swizzle_box(data,slices,2,2,8,4);
    method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8)|(3<<16));method(NV097_SET_TEXTURE_FILTER+stage,0x01010000);
    for(unsigned pass=0;pass<2;pass++) {
        method(NV097_SET_TEXTURE_FORMAT+stage,(3u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|(3u<<28)|1);volume_coordinates(.25f,.25f,.9375f);volume_expect(colors[7]);
        method(NV097_SET_TEXTURE_FORMAT+stage,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<16)|(1u<<20)|(1u<<24)|1);volume_expect(colors[0]);
    }
    fprintf(stderr,"[VOLUME TEXTURE] original FORMAT.P/TEXADDRESS.W, MortonXYZ, DOT_STR spatial output, z-write invalidation, W modes, P0 border filter, 2Didentity and checkpoint passed\n");
}

