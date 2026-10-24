static void verify_bump_luma_order(void) {
    const unsigned formats[]={D3DFMT_LIN_L16,D3DFMT_LIN_A8};for(unsigned kind=0;kind<2;kind++) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    unsigned offset=64;uint16_t *data=(uint16_t *)(xbox_GetMemoryOffset()+0x80080000u);data[0]=data[1]=data[4]=data[5]=65535;
    method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(formats[kind]<<8)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(2u<<16)|2);method(NV097_SET_TEXTURE_CONTROL1+offset,8u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE|NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
    float input[4]={1,0,0,1},coordinate[4]={.5f,.5f,0,1};bump_coordinates(0,input);bump_coordinates(1,coordinate);method(NV097_SET_SHADER_STAGE_PROGRAM,4|(7u<<5));for(unsigned c=0;c<4;c++)bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+c*4,0);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE+offset,.5f);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET+offset,0);
    float previous[4][4]={{1,0,0,1}},sample[4];RcStageDots dots;assert(rc_stage_fetch(1,coordinate,previous,&dots,sample));for(unsigned c=0;c<4;c++)assert(sample[c]==.5f);
    method(NV097_SET_COMBINER_COLOR_ICW,(9u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(25u<<24)|0x200000);uint32_t pixel=shadow_draw_for_test();
    float *colors=(float *)(xbox_GetMemoryOffset()+0x8006e000u);for(unsigned v=0;v<3;v++)for(unsigned c=0;c<4;c++)colors[v*4+c]=.5f;method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+12,0x6e000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+12,0x1042);method(NV097_SET_COMBINER_COLOR_ICW,(4u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(20u<<24)|0x200000);uint32_t control=shadow_draw_for_test();fprintf(stderr,"[BUMP LUMA ORDER] format=%x actual=%08x floatcontrol=%08x\n",formats[kind],pixel,control);if(hw_enabled())assert(pixel==control);else assert(pixel==0x80808080);
    method(NV097_SET_COMBINER_COLOR_ICW,(9u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(25u<<24)|0x200000);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE+offset,0);assert(shadow_draw_for_test()==0xff334455);
    }
    fprintf(stderr,"[BUMP LUMA ORDER] decodedRRR1/alpha-only precedes allRGBA LUM; originalalpha-kill followsmodulation\n");
}
