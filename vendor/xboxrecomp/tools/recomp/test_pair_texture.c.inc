static void verify_pair_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const unsigned formats[]={D3DFMT_G8B8,D3DFMT_R8B8,D3DFMT_LIN_G8B8};
    const uint16_t words[]={0x0000,0xffff,0x7f7f,0x817f,0x0080,0xff01};
    for(unsigned f=0;f<3;f++)for(unsigned k=0;k<6;k++) {
        unsigned format=formats[f];uint16_t *data=(uint16_t *)(xbox_GetMemoryOffset()+0x80080000u);for(unsigned i=0;i<32;i++)data[i]=words[k];
        method(NV097_SET_TEXTURE_OFFSET,0x80000);method(NV097_SET_TEXTURE_FORMAT,(2u<<4)|(format<<8)|(2u<<20)|(2u<<24)|1);method(NV097_SET_TEXTURE_IMAGE_RECT,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1,12u<<16);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);method(NV097_SET_SHADER_STAGE_PROGRAM,1);
        unsigned lo=words[k]&255,hi=words[k]>>8;uint32_t expected=(hi<<24)|((format==D3DFMT_R8B8?hi:lo)<<16)|((format==D3DFMT_R8B8?lo:hi)<<8)|lo;
        uint32_t decoded;float value[4];assert(sample_tex_value(&s_gpu.texs[0],0,3,2,0,0,&decoded,value) && decoded==expected);assert(value[0]==((expected>>16)&255)/255.f && value[1]==((expected>>8)&255)/255.f && value[2]==lo/255.f && value[3]==hi/255.f);
        method(NV097_SET_COMBINER_COLOR_ICW,(8u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(24u<<24)|0x200000);shadow_coordinates_for_test(0,0,1);assert(shadow_draw_for_test()==expected);
        /* The decoded B/G channels feed the original BUMP remap, not a second signed-format decoder. */
        unsigned offset=64;method(NV097_SET_TEXTURE_OFFSET+offset,0x90000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_LIN_A8R8G8B8<<8)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        uint32_t *target=(uint32_t *)(xbox_GetMemoryOffset()+0x80090000u);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)target[y*4+x]=0xff000000|((x*60)<<16)|((y*60)<<8)|0x20;
        float tc[4]={1.5f,1.5f,0,1};bump_coordinates(1,tc);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset,1);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+4,0);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+8,1);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+12,0);
        method(NV097_SET_SHADER_STAGE_PROGRAM,1|(6u<<5));method(NV097_SET_COMBINER_COLOR_ICW,(9u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(25u<<24)|0x200000);
        float ds=(float)(int8_t)lo/127.f,dt=(float)(int8_t)((expected>>8)&255)/127.f;int x=(int)floorf(1.5f+ds),y=(int)floorf(1.5f+dt);if(x<0)x=0;if(x>3)x=3;if(y<0)y=0;if(y>3)y=3;assert(shadow_draw_for_test()==target[y*4+x]);
    }
    fprintf(stderr,"[PAIR TEXTURE] exactUNORMG8B8/R8B8swizzles,linear/swizzledlayout,alpha,cacheinvalidation,BUMPinput passed\n");
}
