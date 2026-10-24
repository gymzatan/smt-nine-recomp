static void verify_dot_texture_stage(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    /* Standing Y16 has no effect on stages which do not sample it. */
    method(NV097_SET_TEXTURE_OFFSET+64,0xa0000);method(NV097_SET_TEXTURE_FORMAT+64,(2u<<4)|(D3DFMT_LIN_L16<<8)|5);method(NV097_SET_TEXTURE_IMAGE_RECT+64,(2u<<16)|2);method(NV097_SET_TEXTURE_CONTROL1+64,8u<<16);method(NV097_SET_TEXTURE_CONTROL0+64,NV_PGRAPH_TEXCTL0_0_ENABLE|1);
    const unsigned modes[]={4,5,0},expected[]={0x00ff00ff,0,0xff000000};float passthru[4]={1,0,1,0};bump_coordinates(1,passthru);method(NV097_SET_COMBINER_COLOR_ICW,(9u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(25u<<24)|0x200000);
    for(unsigned i=0;i<3;i++){method(NV097_SET_SHADER_STAGE_PROGRAM,4|(modes[i]<<5));uint32_t pixel=shadow_draw_for_test();if(pixel!=expected[i])fprintf(stderr,"[DOT STANDING] mode=%u pixel=%08x expected=%08x\n",modes[i],pixel,expected[i]);assert(pixel==expected[i]);}
    const float inputs[4][4]={{.25f,.75f,.5f,1},{192.f/255,64.f/255,0,1},{127.f/255,128.f/255,0,1},{127.f/255,129.f/255,0,1}};
    const unsigned expected_index[]={13,10,15,15};uint32_t colors[16];for(unsigned i=0;i<16;i++)colors[i]=0xff000000|((i*16)<<16)|((15-i)*16<<8)|0x20;
    for(unsigned layout=0;layout<2;layout++)for(unsigned variant=0;variant<3;variant++)for(unsigned map=0;map<4;map++) {
        unsigned stage=variant?3:2,source=variant==2?1:0,first=stage-1,offset=stage*64;
        method(NV097_SET_SHADER_OTHER_STAGE_INPUT,(source<<16)|(source<<20));method(NV097_SET_DOT_RGBMAPPING,map|(map<<4)|(map<<8));assert((nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]&0xfff)==(map|(map<<4)|(map<<8)));
        method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|((layout?D3DFMT_LIN_A8R8G8B8:D3DFMT_A8R8G8B8)<<8)|(2u<<20)|(2u<<24)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        uint32_t *data=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)data[layout?y*4+x:swizzle_offset(x,y,4,4)]=colors[y*4+x];
        bump_coordinates(source,inputs[map]);float first_tc[4]={layout?4.f:1.f,0,0,1},last_tc[4]={0,(map?-1.f:1.f)*(layout?4.f:1.f),0,1};bump_coordinates(first,first_tc);bump_coordinates(stage,last_tc);
        unsigned program=4|(4u<<5);if(stage==3 && !source){float zero[4]={0,0,0,1};bump_coordinates(1,zero);program=4|(17u<<5);}program=(program&~(31u<<(first*5)))|(17u<<(first*5));program|=9u<<(stage*5);method(NV097_SET_SHADER_STAGE_PROGRAM,program);
        method(NV097_SET_COMBINER_COLOR_ICW,((8u+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((24u+stage)<<24)|0x200000);uint32_t pixel=shadow_draw_for_test();if(pixel!=colors[expected_index[map]])fprintf(stderr,"[DOT FAIL] layout=%u variant=%u map=%u actual=%08x expected=%08x\n",layout,variant,map,pixel,colors[expected_index[map]]);assert(pixel==colors[expected_index[map]]);
        method(NV097_SET_COMBINER_COLOR_ICW,((8u+first)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((24u+first)<<24)|0x200000);assert(shadow_draw_for_test()==0);
    }
    FILE *saved=tmpfile();assert(saved && nv2a_gpu_save_checkpoint(saved));method(NV097_SET_DOT_RGBMAPPING,0);method(NV097_SET_SHADER_OTHER_STAGE_INPUT,0);rewind(saved);assert(nv2a_gpu_load_checkpoint(saved));fclose(saved);assert((nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADERCTL]&0x110333)==0x110333);method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);assert(shadow_draw_for_test()==colors[15]);
    fprintf(stderr,"[DOT STAGE] originalmap0..3 andselector,DOTPRODUCTzero,ephemeraldots→DOT_ST2D,linear/swizzledsampling,standingY16non-sampling passed\n");
}
