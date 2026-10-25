
extern unsigned combiner_test_input_groups,combiner_test_output_groups;
static void combiner_parse_work(unsigned expected) {
    if(combiner_test_input_groups!=expected*2 || combiner_test_output_groups!=expected*2) {
        fprintf(stderr,"[COMBINER PREP] actual input/output groups %u/%u expected %u/%u\n",combiner_test_input_groups,combiner_test_output_groups,expected*2,expected*2);exit(5);
    }
}
static void verify_combiner_active_parser(void) {
    DWORD rs[256]={0};
    for(unsigned i=0;i<8;i++) {
        rs[D3DRS_PSRGBINPUTS0+i]=0x01020304u+i*0x1020304u;
        rs[D3DRS_PSALPHAINPUTS0+i]=0x04030201u+i*0x1010101u;
        rs[D3DRS_PSRGBOUTPUTS0+i]=0xc00u+i*0x1000;
        rs[D3DRS_PSALPHAOUTPUTS0+i]=0xc00u+i*0x2000;
        rs[D3DRS_PSCONSTANT0_0+i]=0xff102030u+i;
        rs[D3DRS_PSCONSTANT1_0+i]=0xff405060u+i;
    }
    rs[D3DRS_PSFINALCOMBINERINPUTSABCD]=0x01020304;rs[D3DRS_PSFINALCOMBINERINPUTSEFG]=0x04030201;
    const unsigned counts[]={0,1,2,8};char a[32768],b[32768];
    for(unsigned k=0;k<4;k++) {
        unsigned count=counts[k],parsed=count?count:1;
        NV2ACombinerState actual={0},full={0};actual.num_stages=count;full.num_stages=8;
        combiner_test_input_groups=combiner_test_output_groups=0;
        d3d8_combiners_from_render_states(rs,&actual);combiner_parse_work(parsed);
        assert(actual.num_stages==(int)parsed);
        d3d8_combiners_from_render_states(rs,&full);
        assert(!memcmp(actual.stages,full.stages,parsed*sizeof actual.stages[0]));
        assert(!memcmp(actual.final_input,full.final_input,sizeof actual.final_input));
        assert(!memcmp(actual.c0,full.c0,sizeof actual.c0) && !memcmp(actual.c1,full.c1,sizeof actual.c1));
        assert(actual.final_c0==actual.c0[parsed-1] && actual.final_c1==actual.c1[parsed-1]);
        actual.num_stages=full.num_stages=count;actual.native_coordinates=full.native_coordinates=1;
        full.final_c0=actual.final_c0;full.final_c1=actual.final_c1;
        for(unsigned final=0;final<2;final++) {
            actual.final_enabled=full.final_enabled=final;
            int bytes=d3d8_combiners_generate_hlsl(&actual,a,sizeof a);
            assert(bytes>0 && d3d8_combiners_generate_hlsl(&full,b,sizeof b)==bytes && !memcmp(a,b,(size_t)bytes+1));
        }
    }
    const unsigned tokens[]={0,1,2,8,15};
    for(unsigned k=0;k<5;k++) {
        NV2ACombinerState actual={0};unsigned token=tokens[k],parsed=token<1?1:token>8?8:token;
        combiner_test_input_groups=combiner_test_output_groups=0;
        d3d8_combiners_parse_token(token,rs,&actual);combiner_parse_work(parsed);
        assert(actual.num_stages==(int)parsed && actual.native_coordinates==0);
        assert(actual.final_c0==actual.c0[parsed-1] && actual.final_c1==actual.c1[parsed-1]);
        assert(actual.final_input[0].reg==NV2A_REG_V0);
        assert(d3d8_combiners_generate_hlsl(&actual,a,sizeof a)>0);
        /* Non-native final is still emitted with final_enabled zero. */
        actual.final_input[3].mapping=NV2A_MAP_UNSIGNED_INVERT;
        assert(d3d8_combiners_generate_hlsl(&actual,b,sizeof b)>0 && strcmp(a,b));
    }
    fprintf(stderr,"[COMBINER PREP] active0/1/2/8, token0/1/2/8/15, constants/final and same original HLSL passed\n");
}
extern unsigned combiner_test_shader_count(void);
static void *combiner_effective_shader(const NV2ACombinerState *state) {
    void *shader=d3d8_IsNativeVulkan()?(void *)d3d8_combiners_vulkan_shader(state):(void *)d3d8_combiners_get_shader(state);
    assert(shader);return shader;
}
static void combiner_effective_pixel(uint32_t expected) {
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff010203);method(NV097_CLEAR_SURFACE,0xf0);
    raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
    uint32_t actual=BRIDGE_MEM32(s_gpu.color_base+9*4);
    if(actual!=expected){fprintf(stderr,"[COMBINER EFFECTIVE] pixel=%08x expected=%08x\n",actual,expected);exit(5);}
}
static void verify_combiner_effective_key(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    verify_combiner_active_parser();
    NV2ACombinerState state={0};state.num_stages=1;state.native_coordinates=1;
    for(unsigned i=0;i<4;i++)state.tex_mode[i]=NV2A_TEXMODE_NONE;
    state.stages[0].rgb_input[0].reg=state.stages[0].alpha_input[0].reg=NV2A_REG_V0;
    state.stages[0].alpha_input[0].alpha_rep=1;
    state.stages[0].rgb_input[1].mapping=state.stages[0].alpha_input[1].mapping=NV2A_MAP_UNSIGNED_INVERT;
    state.stages[0].rgb_output.sum_dst=state.stages[0].alpha_output.sum_dst=NV2A_REG_R0;
    char original[16384],candidate[16384];
    int bytes=d3d8_combiners_generate_hlsl(&state,original,sizeof original);assert(bytes>0);
    void *first=combiner_effective_shader(&state);unsigned initial=combiner_test_shader_count();
    for(unsigned v=0;v<16;v++) {
        NV2ACombinerState other=state;
        for(unsigned i=1;i<8;i++) {
            other.stages[i].rgb_input[0].reg=(v&1)?NV2A_REG_V0:NV2A_REG_V1;
            other.stages[i].rgb_input[0].mapping=(NV2AInputMapping)(v&7);
            other.stages[i].alpha_input[3].alpha_rep=v&1;
            other.stages[i].rgb_output.sum_dst=NV2A_REG_R1;
            other.stages[i].alpha_output.output_map=(NV2AOutputMapping)(v%NV2A_OUT_COUNT);
        }
        for(unsigned i=0;i<7;i++){other.final_input[i].reg=NV2A_REG_V1;other.final_input[i].mapping=(NV2AInputMapping)(v&7);}
        other.final_flags=v*17;
        int length=d3d8_combiners_generate_hlsl(&other,candidate,sizeof candidate);
        assert(length==bytes && !memcmp(original,candidate,(size_t)bytes+1));
        void *same=combiner_effective_shader(&other);unsigned count=combiner_test_shader_count();
        if(same!=first || count!=initial){fprintf(stderr,"[COMBINER EFFECTIVE] inactive variant=%u shaders=%u expected=%u same=%u\n",v,count,initial,same==first);exit(5);}
    }
    NV2ACombinerState two=state;two.num_stages=2;
    two.stages[1].rgb_output.sum_dst=two.stages[1].alpha_output.sum_dst=NV2A_REG_R0;
    assert(d3d8_combiners_generate_hlsl(&two,candidate,sizeof candidate)>0 && strcmp(original,candidate));
    assert(combiner_effective_shader(&two)!=first);
    NV2ACombinerState final=state;final.final_enabled=1;
    final.final_input[3].mapping=final.final_input[6].mapping=NV2A_MAP_UNSIGNED_INVERT;
    assert(d3d8_combiners_generate_hlsl(&final,candidate,sizeof candidate)>0 && strcmp(original,candidate));
    assert(combiner_effective_shader(&final)!=first);
    /* Non-native final is consumed even when final_enabled is zero. */
    NV2ACombinerState legacy=state;legacy.native_coordinates=0;
    legacy.final_input[3].reg=legacy.final_input[6].reg=NV2A_REG_V0;legacy.final_input[6].alpha_rep=1;
    assert(d3d8_combiners_generate_hlsl(&legacy,original,sizeof original)>0);
    void *legacy_first=combiner_effective_shader(&legacy);
    legacy.final_input[3].mapping=NV2A_MAP_UNSIGNED_INVERT;
    assert(d3d8_combiners_generate_hlsl(&legacy,candidate,sizeof candidate)>0 && strcmp(original,candidate));
    assert(combiner_effective_shader(&legacy)!=legacy_first);
    /* Same original Kelvin producers -> original native draw -> actual RAM pixels. */
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0);
    method(NV097_SET_COMBINER_CONTROL,1);combiner_effective_pixel(0xffff0000);
    unsigned before=combiner_test_shader_count();
    method(NV097_SET_COMBINER_COLOR_ICW+7*4,0x05200000);
    method(NV097_SET_COMBINER_COLOR_OCW+7*4,0xc00);
    combiner_effective_pixel(0xffff0000);assert(combiner_test_shader_count()==before);
    method(NV097_SET_COMBINER_COLOR_ICW+4,0);method(NV097_SET_COMBINER_ALPHA_ICW+4,0);
    method(NV097_SET_COMBINER_COLOR_OCW+4,0xc00);method(NV097_SET_COMBINER_ALPHA_OCW+4,0xc00);
    method(NV097_SET_COMBINER_CONTROL,2);combiner_effective_pixel(0);
    for(unsigned i=2;i<8;i++) {
        method(NV097_SET_COMBINER_COLOR_ICW+i*4,0);method(NV097_SET_COMBINER_ALPHA_ICW+i*4,0);
        method(NV097_SET_COMBINER_COLOR_OCW+i*4,0xc00);method(NV097_SET_COMBINER_ALPHA_OCW+i*4,0xc00);
    }
    method(NV097_SET_COMBINER_CONTROL,8);combiner_effective_pixel(0);
    method(NV097_SET_COMBINER_CONTROL,0);combiner_effective_pixel(0xff000000);

    method(NV097_SET_COMBINER_CONTROL,1);
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0x20);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0x2000);
    combiner_effective_pixel(0xffffffff);
    fprintf(stderr,"[COMBINER EFFECTIVE] byte-identical unused HLSL/cache identity; active stage/final shader and red/black/white pixels; non-native final retained\n");
}
