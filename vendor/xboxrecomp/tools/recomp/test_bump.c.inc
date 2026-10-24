extern unsigned combiner_test_shader_count(void);
static void bump_float_method(unsigned command,float value){uint32_t raw;memcpy(&raw,&value,4);method(command,raw);}
static void bump_coordinates(unsigned stage,const float value[4]) {
    float *memory=(float *)(xbox_GetMemoryOffset()+0x80050000u+stage*0x100);for(unsigned i=0;i<3;i++)memcpy(memory+i*4,value,16);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+(9+stage)*4,0x50000+stage*0x100);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+(9+stage)*4,0x1042);
}
static void verify_bump_texture(void) {
    assert(offsetof(NV2APSConstants,bump_matrix)==368 && offsetof(NV2APSConstants,bump_lum_coord)==432 && sizeof(NV2APSConstants)==496);
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[16]={0xff102030,0xff204060,0xff306090,0xff4080c0,0xff502010,0xff604020,0xff706030,0xff808040,0xff903050,0xffa06070,0xffb09090,0xffc0c0b0,0xffd03010,0xffe06020,0xfff09030,0xff00c040};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),colors,sizeof colors);
    for(unsigned stage=1;stage<4;stage++) {
        unsigned offset=stage*64;
        method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2u<<4)|(D3DFMT_LIN_A8R8G8B8<<8)|1);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(4u<<16)|4);method(NV097_SET_TEXTURE_CONTROL1+offset,16u<<16);method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        method(NV097_SET_SHADER_OTHER_STAGE_INPUT,(1u<<16)|(1u<<20));
        unsigned source=stage==1?0:1;
        for(unsigned lum=0;lum<2;lum++)for(unsigned negative=0;negative<2;negative++) {
            float input[4]={1,negative?129.f/255:127.f/255,127.f/255,1},coordinate[4]={1.5f,1.5f,0,1};
            bump_coordinates(source,input);bump_coordinates(stage,coordinate);
            unsigned program=4|(4u<<5)|(4u<<10)|(4u<<15);program=(program&~(31u<<(stage*5)))|((6+lum)<<(stage*5));method(NV097_SET_SHADER_STAGE_PROGRAM,program);
            bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset,0);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+4,1);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+8,0);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+12,2);
            bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE+offset,1);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET+offset,0);
            struct PGRAPHState *pg=&nv2a_get_state()->pgraph;assert(pg->regs[NV_PGRAPH_BUMPMAT01+(stage-1)*4]==0x3f800000 && pg->regs[NV_PGRAPH_BUMPMAT10+(stage-1)*4]==0x40000000);
            method(NV097_SET_COMBINER_COLOR_ICW,((8u+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((24u+stage)<<24)|0x200000);
            uint32_t expected=colors[2*4+(negative?0:3)],pixel=shadow_draw_for_test();if(pixel!=expected)fprintf(stderr,"[BUMP FAIL] stage=%u lum=%u negative=%u actual=%08x expected=%08x\n",stage,lum,negative,pixel,expected);assert(pixel==expected);
            /* Dynamic matrix/scale values retain the same shader identity. */
            unsigned cache_count=combiner_test_shader_count();bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT+offset+12,0);assert(shadow_draw_for_test()==colors[2*4+1]);assert(combiner_test_shader_count()==cache_count);
            if(lum){bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE+offset,0);assert(shadow_draw_for_test()==0);bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET+offset,1);assert(shadow_draw_for_test()==colors[2*4+1]);assert(combiner_test_shader_count()==cache_count);}
        }
    }
    FILE *saved=tmpfile();assert(saved && nv2a_gpu_save_checkpoint(saved));bump_float_method(NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET+3*64,0);rewind(saved);assert(nv2a_gpu_load_checkpoint(saved));fclose(saved);assert(nv2a_get_state()->pgraph.regs[NV_PGRAPH_BUMPOFFSET1+8]==0x3f800000);assert(shadow_draw_for_test()==colors[2*4+1]);
    fprintf(stderr,"[BUMP] originalmethodswizzle,earlierstage,signedBG,matrix,LUMdynamicvalues,CombinerCBlayout andcheckpoint passed\n");
}
