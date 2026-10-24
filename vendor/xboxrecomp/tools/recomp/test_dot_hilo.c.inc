static void verify_dot_hilo_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[16]={0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040,0xff903050,0xffa06070,0xffb09090,0xffc0c0b0,0xffd03010,0xffe06020,0xfff09030,0xff00c040};
    const uint32_t packed[]={0xff00ff00,0x00400000,0x80008000,0x4000c000,0x004080c0,0,0x00010000,0x00000001,0x00010001};const unsigned indices[]={15,0,10,13,8,0,2,8,10};
    method(NV097_SET_TEXTURE_OFFSET,0xa0000);method(NV097_SET_TEXTURE_FORMAT,(2u<<4)|(D3DFMT_A8R8G8B8<<8)|(1u<<20)|(1u<<24)|1);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);float origin[4]={.5f,.5f,0,1};bump_coordinates(0,origin);
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);method(NV097_SET_DOT_RGBMAPPING,4|(4<<4));method(NV097_SET_SHADER_STAGE_PROGRAM,1|(17u<<5)|(9u<<10));method(NV097_SET_COMBINER_COLOR_ICW,(10u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(26u<<24)|0x200000);
    for(unsigned layout=0;layout<2;layout++) {
        unsigned offset=2*64;method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|((layout?D3DFMT_LIN_A8R8G8B8:D3DFMT_A8R8G8B8)<<8)|(2u<<20)|(2u<<24)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)data[layout?y*4+x:swizzle_offset(x,y,4,4)]=colors[y*4+x];
        for(unsigned code=0;code<9;code++) {
            uint32_t *source=(uint32_t *)(xbox_GetMemoryOffset()+0x800a0000u);for(unsigned i=0;i<4;i++)source[i]=packed[code];
            float rgba[4]={((packed[code]>>16)&255)/255.f,((packed[code]>>8)&255)/255.f,(packed[code]&255)/255.f,(packed[code]>>24)/255.f},axis[4]={1,0,0,0};float hi=(float)(((packed[code]>>24)<<8)|((packed[code]>>16)&255))/65535.f,lo=(float)(packed[code]&65535)/65535.f;assert(texture_dot_value(4,rgba,axis)==hi);axis[0]=0;axis[1]=1;assert(texture_dot_value(4,rgba,axis)==lo);axis[1]=0;axis[2]=1;assert(texture_dot_value(4,rgba,axis)==1);
            float scale=(layout?4.f:1.f)*(code>=5?32768.f:1.f),first[4]={scale,0,0,1},second[4]={0,scale,0,1};bump_coordinates(1,first);bump_coordinates(2,second);uint32_t pixel=shadow_draw_for_test();if(pixel!=colors[indices[code]])fprintf(stderr,"[DOT HILO FAIL] layout=%u code=%u actual=%08x expected=%08x\n",layout,code,pixel,colors[indices[code]]);assert(pixel==colors[indices[code]]);
            unsigned count=combiner_test_shader_count();first[0]=second[1]=0;first[2]=(layout?4.f:1.f)*.3f;second[2]=(layout?4.f:1.f)*.8f;bump_coordinates(1,first);bump_coordinates(2,second);assert(shadow_draw_for_test()==colors[13]);assert(combiner_test_shader_count()==count);
        }
    }
    fprintf(stderr,"[DOT HILO] primarymap4ARGBpackhi/lo/one,exactsoftware16bitvalues,actualresourcebyte1influence,thirdcomponentone,originalDOTSTsampler/cache,linear/Morton passed\n");
}
