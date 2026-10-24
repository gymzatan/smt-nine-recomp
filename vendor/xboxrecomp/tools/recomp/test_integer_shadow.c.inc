static void shadow_coordinates_for_test(unsigned stage,float reference,float q) {
    float coordinates[3][4];for(unsigned vertex=0;vertex<3;vertex++){coordinates[vertex][0]=.5f*q;coordinates[vertex][1]=.5f*q;coordinates[vertex][2]=reference*q;coordinates[vertex][3]=q;}
    memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coordinates,sizeof coordinates);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+(9+stage)*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+(9+stage)*4,0x1042);
}
static uint32_t shadow_draw_for_test(void) {
    method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();return BRIDGE_MEM32(s_gpu.color_base);
}
static void verify_integer_shadow_textures(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_SHADOW_ZSLOPE_THRESHOLD,0x7f800000);
    for(unsigned bits=16;bits<=24;bits+=8)for(unsigned linear=0;linear<2;linear++) {
        unsigned format=bits==16?(linear?D3DFMT_LIN_D16:D3DFMT_D16):(linear?D3DFMT_LIN_D24S8:D3DFMT_D24S8);
        unsigned maximum=bits==16?65535:16777215;
        for(unsigned mode=1;mode<=2;mode++)for(unsigned function=0;function<8;function++) {
            unsigned stage=(function+mode+linear+(bits==24))&3,offset=stage*64;
            method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_CONTROL1+offset,8u<<16);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(2u<<16)|2);
            method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(format<<8)|(1u<<16)|(1u<<20)|(1u<<24));method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);
            method(NV097_SET_TEXTURE_ADDRESS+offset,3u|(3u<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
            method(NV097_SET_SHADER_STAGE_PROGRAM,mode<<(stage*5));method(NV097_SET_SHADOW_DEPTH_FUNC,function);
            method(NV097_SET_COMBINER_COLOR_ICW,((8+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((0x18+stage)<<24)|0x200000);
            const unsigned codes[]={0,511,maximum-1,maximum};
            for(unsigned c=0;c<sizeof codes/sizeof codes[0];c++) {
                uint8_t *ram=(uint8_t *)xbox_GetMemoryOffset()+0x80080000u;
                for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++) {
                    size_t at=linear?y*8+x*(bits==16?2:4):swizzle_offset(x,y,2,2)*(bits==16?2:4);
                    if(bits==16)*(uint16_t *)(ram+at)=(uint16_t)codes[c];else *(uint32_t *)(ram+at)=(codes[c]<<8)|0xa5;
                }
                for(int delta=mode==1?0:-1;delta<=1;delta++) {
                    float reference=mode==1?0:fminf((float)maximum,fmaxf(0,(float)codes[c]+delta));
                    shadow_coordinates_for_test(stage,reference,function&1?2:1);
                    uint32_t expected=compare_pass(0x200|function,(float)codes[c],reference)?0xffffffffu:0;
                    uint32_t actual=shadow_draw_for_test();if(actual!=expected)fprintf(stderr,"[SHADOW] bits=%u linear=%u mode=%u fn=%u raw=%x ref=%g actual=%08x expected=%08x\n",bits,linear,mode,function,codes[c],reference,actual,expected);
                    assert(actual==expected);
                }
            }
        }
    }
    /* The exact R32 upload also retains fractional filtered raw depth. */
    unsigned offset=0;method(NV097_SET_TEXTURE_FORMAT,(2u<<4)|(D3DFMT_D16<<8)|(1u<<16)|(1u<<20)|(1u<<24));
    method(NV097_SET_TEXTURE_OFFSET,0x80000);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x02020000);
    uint16_t *texels=(uint16_t *)(xbox_GetMemoryOffset()+0x80080000u);texels[0]=0;texels[1]=4;texels[2]=8;texels[3]=12;
    method(NV097_SET_SHADER_STAGE_PROGRAM,2);method(NV097_SET_SHADOW_DEPTH_FUNC,2);method(NV097_SET_COMBINER_COLOR_ICW,(8u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(24u<<24)|0x200000);
    shadow_coordinates_for_test(0,6,1);assert(shadow_draw_for_test()==0xffffffffu);
    /* Original post-sample kill/key actions consume the comparison result. */
    for(unsigned key=0;key<4;key++) {
        method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE|key);method(NV097_SET_COLOR_KEY_COLOR,0xffffffff);
        uint32_t actual=shadow_draw_for_test(),expected=key==0?0xffffffffu:key==1?0xffffffu:key==2?0:0xff334455;
        assert(actual==expected);
    }
    method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE|NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN);method(NV097_SET_SHADOW_DEPTH_FUNC,0);assert(shadow_draw_for_test()==0xff334455);
    fprintf(stderr,"[INTEGER SHADOW] real D16/D24 RAM in pitched/Morton layouts, PROJECT2D/3D, all8 stored-vs-reference comparisons, stages0..3, adjacent maxima, filtering, key/alpha-kill passed\n");
}
