
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


static void preparation_draw_check(const char *label,unsigned primitive,unsigned count,int program,int fixed,int output,uint32_t expected_pixel) {
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff010203);method(NV097_CLEAR_SURFACE,0xf0);
    method(NV097_CLEAR_REPORT_VALUE,0);method(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE,1);
    prep_batch_calls=prep_pixel_calls=prep_geometry_calls=prep_draw_calls=prep_geometry_draws=prep_stage_unpack_calls=prep_gs_cb_calls=prep_vs_cb_calls=0;
    prep_border_unpack_calls=0;prep_reverse_calls=0;
    layout_query_calls=layout_output_draws=0;layout_watch=1;
    prep_watch=1;raster_draw_for_test(primitive,0,count);nv2a_pb_checkpoint_publish();prep_watch=0;layout_watch=0;
    if(d3d8_IsNativeVulkan()) {
        unsigned expected=program&&!output?1:0;
        if(layout_query_calls!=expected){fprintf(stderr,"[OUTPUT LAYOUT] %s getter=%u expected=%u\n",label,layout_query_calls,expected);exit(5);}
        if(output)assert(layout_output_draws==prep_draw_calls);
    }
    unsigned border_expected=0;
    if(d3d8_IsNativeVulkan())for(unsigned st=0;st<4;st++)border_expected+=s_hw_vk_textures[st]!=NULL;
    if(prep_border_unpack_calls!=border_expected) {
        fprintf(stderr,"[SAMPLER PREPARATION] %s border-unpack=%u expected=%u\n",label,prep_border_unpack_calls,border_expected);exit(5);
    }
    assert(prep_batch_calls==1 && prep_draw_calls && prep_program==program && prep_fixed==fixed && prep_output==output);
    unsigned geometry=0,pixel=0;
    if(!d3d8_IsNativeVulkan()) {
        geometry=prep_geometry_draws+(program && !output?prep_batch_calls:0);
        pixel=program?prep_geometry_draws:prep_batch_calls;
    }
    if(prep_geometry_calls!=geometry || prep_pixel_calls!=pixel) {
        fprintf(stderr,"[PREPARATION COST] %s actual GS/PS %u/%u expected %u/%u finaldraws=%u\n",label,prep_geometry_calls,prep_pixel_calls,geometry,pixel,prep_draw_calls);exit(5);
    }
    unsigned active=s_gpu.rc.control&255;if(active>8)active=8;if(!active)active=1;
    if(prep_reverse_calls!=(2*active+2)*prep_batch_calls) {
        fprintf(stderr,"[DRAW STATE PREPARATION] %s reverse=%u expected=%u\n",label,prep_reverse_calls,(2*active+2)*prep_batch_calls);exit(5);
    }
    if(prep_stage_unpack_calls!=active*2*prep_batch_calls) {
        fprintf(stderr,"[PREPARATION COST] %s actual stage-unpack %u expected %u\n",label,prep_stage_unpack_calls,active*2*prep_batch_calls);exit(5);
    }
    if(!d3d8_IsNativeVulkan()) {
        unsigned gs_expected=prep_geometry_calls,vs_expected=fixed?1:program&&!output?1:0;
        if(prep_gs_cb_calls!=gs_expected || prep_vs_cb_calls!=vs_expected) {
            fprintf(stderr,"[BATCHED BINDINGS] %s actual GS/VS API %u/%u expected %u/%u\n",label,prep_gs_cb_calls,prep_vs_cb_calls,gs_expected,vs_expected);exit(5);
        }
    } else assert(!prep_gs_cb_calls && !prep_vs_cb_calls);
    if(expected_pixel!=0xdeadbeefu)assert(BRIDGE_MEM32(s_gpu.color_base+9*4)==(uint32_t)expected_pixel);
    assert(!s_hw_query_count);
    fprintf(stderr,"[PREPARATION COST] %s route=%d/%d/%d draws=%u GS/PS=%u/%u stage=%u GSCB/VSCB=%u/%u samples=%u\n",label,program,fixed,output,prep_draw_calls,prep_geometry_calls,prep_pixel_calls,prep_stage_unpack_calls,prep_gs_cb_calls,prep_vs_cb_calls,s_gpu.zpass_count);
}
static void verify_preparation_cost(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);method(NV097_SET_ALPHA_TEST_ENABLE,0);
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0);
    method(NV097_SET_FRONT_POLYGON_MODE,0x1b02);method(NV097_SET_BACK_POLYGON_MODE,0x1b02);
    preparation_draw_check("program",5,3,1,0,0,(int)0xffff0000);
    unsigned samples=s_gpu.zpass_count;assert(samples>0);
    const uint32_t colors[8]={0xffff0000,0xff00ff00,0xff0000ff,0xff00ffff,0xffff00ff,0xffffff00,0xff808080,0xffffffff};
    for(unsigned i=0;i<8;i++) {
        method(NV097_SET_COMBINER_FACTOR0+i*4,colors[i]);method(NV097_SET_COMBINER_FACTOR1+i*4,~colors[i]);
        method(NV097_SET_COMBINER_COLOR_ICW+i*4,0x01200000);method(NV097_SET_COMBINER_ALPHA_ICW+i*4,0x11200000);
        method(NV097_SET_COMBINER_COLOR_OCW+i*4,0xc00);method(NV097_SET_COMBINER_ALPHA_OCW+i*4,0xc00);
    }
    const unsigned stages[]={0,1,2,8,255};
    for(unsigned i=0;i<5;i++) {
        unsigned n=stages[i];method(NV097_SET_COMBINER_CONTROL,0x1000|n);
        preparation_draw_check("material",5,3,1,0,0,(int)(n?colors[(n>8?8:n)-1]:0xff000000));assert(s_gpu.zpass_count==samples);
    }
    method(NV097_SET_COMBINER_CONTROL,0x1001);method(NV097_SET_COMBINER_FACTOR0+7*4,0xff123456);
    preparation_draw_check("inactive-material",5,3,1,0,0,(int)colors[0]);
    method(NV097_SET_COMBINER_CONTROL,0x1008);preparation_draw_check("enabled-material",5,3,1,0,0,(int)0xff123456);
    method(NV097_SET_SPECULAR_FOG_FACTOR,0xff2468ac);method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,1);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0x1100);
    preparation_draw_check("final-material",5,3,1,0,0,(int)0xff2468ac);
    method(NV097_SET_COMBINER_CONTROL,1);method(NV097_SET_COMBINER_COLOR_ICW,0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    preparation_draw_check("fixed",5,3,1,1,0,0xdeadbeefu);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1442);
    preparation_draw_check("non-program",5,3,0,0,0,(int)0xffff0000);
    method(NV097_SET_FRONT_POLYGON_MODE,0x1b01);method(NV097_SET_BACK_POLYGON_MODE,0x1b01);method(NV097_SET_LINE_WIDTH,24);
    preparation_draw_check("output-polygon",5,3,1,0,1,0xdeadbeefu);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);
    preparation_draw_check("program-polygon",5,3,1,0,0,0xdeadbeefu);
    method(NV097_SET_FRONT_POLYGON_MODE,0x1b02);method(NV097_SET_BACK_POLYGON_MODE,0x1b02);method(NV097_SET_LINE_WIDTH,40*8);
    preparation_draw_check("line-copy-groups",2,2,1,0,0,0xdeadbeefu);
    method(NV097_SET_FRONT_POLYGON_MODE,0x1b00);method(NV097_SET_BACK_POLYGON_MODE,0x1b00);method(NV097_SET_POINT_SIZE,16);
    preparation_draw_check("polygon-points",5,3,1,0,0,0xdeadbeefu);
    method(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE,0);
    fprintf(stderr,"[PREPARATION COST] routes/counts/final pixels/query passed\n");
}

static void sampler_preparation_mask(unsigned mask,unsigned variant,uint32_t *expected) {
    unsigned program=0,selected=0;
    const uint32_t colors[4]={0xff123456,0xff2468ac,0xff4080c0,0xffa05020};
    for(unsigned st=0;st<4;st++) {
        unsigned offset=st*64;
        if(mask&(1u<<st)){program|=1u<<(st*5);selected=st;}
        method(NV097_SET_TEXTURE_CONTROL0+offset,(mask&(1u<<st))?NV_PGRAPH_TEXCTL0_0_ENABLE:0);
        method(NV097_SET_TEXTURE_OFFSET+offset,0x80000+st*0x1000);
        method(NV097_SET_TEXTURE_FORMAT+offset,(0x12<<8)|(1<<16));
        method(NV097_SET_TEXTURE_CONTROL1+offset,8<<16);method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(2<<16)|2);
        method(NV097_SET_TEXTURE_ADDRESS+offset,(variant?1:3)|((variant?1:3)<<8));
        method(NV097_SET_TEXTURE_FILTER+offset,variant?0x02020000:0x01010000);
        method(NV097_SET_TEXTURE_BORDER_COLOR+offset,0xff010203u+st+variant);
        uint32_t *pixels=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u+st*0x1000);
        for(unsigned j=0;j<4;j++)pixels[j]=colors[st];
    }
    method(NV097_SET_SHADER_STAGE_PROGRAM,program);
    method(NV097_SET_COMBINER_COLOR_ICW,mask?0x08200000|(selected<<24):0x04200000);
    method(NV097_SET_COMBINER_ALPHA_ICW,mask?0x18200000|(selected<<24):0x14200000);
    *expected=mask?colors[selected]:0xffff0000;
}
static void verify_sampler_preparation(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0);
    const unsigned masks[]={0,1,8,5,15,0,8};uint32_t expected=0;
    unsigned samples=0;
    for(unsigned test=0;test<sizeof masks/sizeof masks[0];test++) {
        unsigned mask=masks[test];sampler_preparation_mask(mask,test&1,&expected);
        preparation_draw_check("sampler-mask",5,3,1,0,0,expected);
        if(!test)samples=s_gpu.zpass_count;assert(samples && s_gpu.zpass_count==samples);
        if(d3d8_IsNativeVulkan()) {
            assert(native_texture_mask==mask);
            for(unsigned st=0;st<4;st++)if(mask&(1u<<st)) {
                float border[4];nv2a_rc_unpack(s_gpu.texs[st].border_color,border);
                assert(!memcmp(border,native_texture_params[st].border,sizeof border));
                assert(native_texture_params[st].filter==((test&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST));
                assert(native_texture_params[st].u==((test&1)?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
            }
        }
    }
    FILE *saved=tmpfile();assert(saved && nv2a_gpu_save_checkpoint(saved));
    uint32_t restored=expected;sampler_preparation_mask(0,0,&expected);
    preparation_draw_check("sampler-disabled",5,3,1,0,0,expected);
    rewind(saved);assert(nv2a_gpu_load_checkpoint(saved));fclose(saved);
    preparation_draw_check("sampler-restored",5,3,1,0,0,restored);
    assert(s_gpu.zpass_count==samples);
    if(d3d8_IsNativeVulkan())assert(native_texture_mask==8);
    method(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE,0);
    fprintf(stderr,"[SAMPLER PREPARATION] null/sparse/mixed/all/query/restore passed\n");
}

static void verify_texture_diagnostics(const char *mode) {
    extern void nv2a_pb_scan(uint32_t,uint32_t);
    if(!strcmp(mode,"legacy"))nv2a_pb_scan(0,0);
    s_gpu.texs[0]=(Texture){0};s_gpu.texs[0].valid=1;s_gpu.texs[0].width=s_gpu.texs[0].height=1;
    s_gpu.texs[0].color=0x12;s_gpu.texs[0].offset=0x80050000;
    *(uint32_t *)(xbox_GetMemoryOffset()+0x80050000)=0xff123456;
    diagnostic_probes=diagnostic_dump_count=0;
    note_texture_use(nv2a_pb_scan_exec_enabled());
    if(!strcmp(mode,"lateevery"))_putenv_s("RECOMP_TEX_DUMP_EVERY","2");
    for(unsigned i=0;i<100;i++)note_texture_use(nv2a_pb_scan_exec_enabled());
    if(!strcmp(mode,"default")) {
        if(s_tex_use_count || diagnostic_probes || diagnostic_dump_count){fprintf(stderr,"[DIAGNOSTIC] default actual count=%d probes=%u dumps=%u expected0\n",s_tex_use_count,diagnostic_probes,diagnostic_dump_count);exit(5);}
    } else {
        assert(s_tex_use_count==1 && s_tex_use[0].batches==101 && diagnostic_probes==100);
        unsigned repeated=(!strcmp(mode,"every2")||!strcmp(mode,"lateevery"))?40:0;
        assert(diagnostic_dump_count==1+repeated && diagnostic_indices[0]==0);
        for(unsigned i=0;i<repeated;i++)assert(diagnostic_indices[1+i]==1000+i);
        for(unsigned i=1;i<65;i++){s_gpu.texs[0].offset=0x80050000+i*4;note_texture_use(nv2a_pb_scan_exec_enabled());}
        assert(s_tex_use_count==64 && diagnostic_dump_count==64+repeated);
        for(unsigned i=0;i<64;i++)assert(s_tex_use[i].offset==0x80050000+i*4);
        s_gpu.texs[0].offset=0x80050000;note_texture_use(nv2a_pb_scan_exec_enabled());assert(s_tex_use[0].batches==102);
        unsigned before=diagnostic_probes;s_gpu.texs[0].offset=0x80050000+65*4;note_texture_use(nv2a_pb_scan_exec_enabled());assert(diagnostic_probes==before+64);
    }
    unsigned legacy=!strcmp(mode,"legacy");
    s_gpu.blend_npairs=1;s_gpu.blend_pairs[0]=0x12345678;s_gpu.xf_nfmt=1;s_gpu.xf_fmt[0]=0x12345678;
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_BLEND_ENABLE,1);
    raster_draw_for_test(5,0,3);
    assert(s_gpu.blend_npairs==(legacy?2:1));assert(s_gpu.xf_nfmt==(legacy?3:1));
    FILE *history=tmpfile();assert(history);assert(nv2a_gpu_save_checkpoint(history));
    s_gpu.blend_npairs=0;s_gpu.xf_nfmt=0;rewind(history);assert(nv2a_gpu_load_checkpoint(history));fclose(history);
    assert(s_gpu.blend_pairs[0]==0x12345678 && s_gpu.xf_fmt[0]==0x12345678);
    assert(s_gpu.blend_npairs==(legacy?2:1));assert(s_gpu.xf_nfmt==(legacy?3:1));
    fprintf(stderr,"[DIAGNOSTIC] %s count=%d probes=%u dumpAttempts=%u history/restore passed\n",mode,s_tex_use_count,diagnostic_probes,diagnostic_dump_count);
}
static void verify_dxt_block_work(void) {
    Texture t={0};t.valid=1;t.width=t.height=4;t.addr_u=t.addr_v=3;t.offset=0x80060000;
    uint8_t *mem=(uint8_t *)xbox_GetMemoryOffset();unsigned fills=0;
    for(unsigned fmt=0;fmt<3;fmt++) {
        t.color=(uint32_t[]){0xc,0xe,0xf}[fmt];
        for(unsigned rewrite=0;rewrite<5;rewrite++) {
            uint8_t *b=mem+t.offset;memset(b,0,16);unsigned c=fmt?8:0;
            b[c]=0;b[c+1]=rewrite&1?0xf8:0x07;b[c+2]=0x1f;b[c+3]=0;
            if(fmt==1)memset(b,0xff,8);if(fmt==2){b[0]=255;b[1]=0;}
            uint32_t expect=0xff000000;assert(d3d8_dxt_decode_texel(b,t.color,0,0,4,&expect));
            dxt_palette_work=0;
            for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){uint32_t actual=0;assert(sample_tex_value(&t,0,x,y,0,0,&actual,NULL));assert(actual==expect);}
            if(dxt_palette_work!=1){fprintf(stderr,"[DXT BLOCK] actual palette work=%u expected1\n",dxt_palette_work);exit(5);}++fills;
            uint32_t actual;dxt_palette_work=0;assert(sample_tex_value(&t,0,0,0,0,0,&actual,NULL));assert(!dxt_palette_work);
            t.addr_u=4;t.border_color=0xffabcdef;assert(sample_tex_value(&t,0,UINT32_MAX,0,0,0,&actual,NULL)&&actual==t.border_color);assert(!dxt_palette_work);t.addr_u=3;
        }
    }
    /* Same-slot address collision must displace/reload the original raw-key entry. */
    uint32_t bb=d3d8_format_dxt_block_bytes(t.color);uintptr_t key=(uintptr_t)(mem+t.offset);
    size_t slot=((key/bb)^(key>>16))&4095;uint32_t other=t.offset+16;
    for(;other<t.offset+0x200000;other+=16){uintptr_t k=(uintptr_t)(mem+other);if((((k/bb)^(k>>16))&4095)==slot)break;}
    assert(other<t.offset+0x200000);memcpy(mem+other,mem+t.offset,16);
    uint32_t first;assert(sample_tex_value(&t,0,0,0,0,0,&first,NULL));
    t.offset=other;dxt_palette_work=0;uint32_t actual;assert(sample_tex_value(&t,0,0,0,0,0,&actual,NULL)&&actual==first&&dxt_palette_work==1);
    t.offset=0x80060000;dxt_palette_work=0;assert(sample_tex_value(&t,0,0,0,0,0,&actual,NULL)&&actual==first&&dxt_palette_work==1);
    t.width=t.height=3;assert(sample_tex_value(&t,0,2,2,0,0,&actual,NULL)&&actual==first);
    /* Actual existing native upload/software fragment and original draw publication. */
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    method(NV097_SET_TEXTURE_OFFSET,0x80000);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);
    method(NV097_SET_SHADER_STAGE_PROGRAM,1);method(NV097_SET_COMBINER_COLOR_ICW,0x08200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x18200000);
    float coordinates[3][4]={{.5f,.5f,0,1},{.5f,.5f,0,1},{.5f,.5f,0,1}};
    memcpy(mem+0x80050000u,coordinates,sizeof coordinates);method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+9*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+9*4,0x1042);
    for(unsigned f=0;f<3;f++)for(unsigned rewrite=0;rewrite<2;rewrite++) {
        uint32_t fmt=(uint32_t[]){0xc,0xe,0xf}[f];uint8_t *b=mem+0x80080000u;memset(b,0,16);unsigned c=f?8:0;
        b[c+1]=rewrite?0xf8:0x07;b[c]=rewrite?0:0xe0;if(f==1)memset(b,0xff,8);if(f==2)b[0]=255;
        method(NV097_SET_TEXTURE_FORMAT,(fmt<<8)|(1<<16)|(2<<20)|(2<<24));
        method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
        uint32_t expected=rewrite?0xffff0000:0xff00ff00;
        assert(BRIDGE_MEM32(s_gpu.color_base)==expected);
    }
    fprintf(stderr,"[DXT BLOCK] %u format/rewrite fills one palette, hits/border/edge/collision and original native/software pixels passed\n",fills);
}

static HwVertexBatch *lru_append(unsigned identity,unsigned selected,unsigned visits) {
    s_gpu.idx_count=6;s_gpu.inline_active=0;
    for(unsigned i=0;i<6;i++)s_gpu.idx[i]=identity*8+i;
    int hit=-1;lru_selection_visits=0;HwVertexBatch *entry=hw_vertex_batch_begin(0,0,&hit);
    HwVertexBatch *expected=&s_hw_vertex_batches[selected/HW_VERTEX_CACHE_WAYS][selected%HW_VERTEX_CACHE_WAYS];
    if(entry!=expected || hit || lru_selection_visits!=visits){fprintf(stderr,"[VERTEX LRU] identity=%u selected=%td expected=%u visits=%u expected=%u hit=%d\n",identity,entry-&s_hw_vertex_batches[0][0],selected,lru_selection_visits,visits,hit);exit(5);}
    memset(entry->data+6*4,0,6*4);entry->valid=1;return entry;
}
static void verify_original_vertex_lru_selection(void) {
    hw_free_vertex_batches();
    lru_append(1,0,1); /* firstzero wins the original unsigned minimum tie. */
    hw_free_vertex_batches();
    for(unsigned i=0;i<123;i++)lru_append(i+1,i,i+1);
    lru_append(500,123,124); /* middle firstempty, laterzeros must not replace it. */
    hw_free_vertex_batches();
    for(unsigned i=0;i<512;i++)lru_append(i+1,i,i+1);
    for(unsigned i=0;i<512;i++)s_hw_vertex_batches[i/4][i%4].used=100+i;
    s_hw_vertex_batches[50][0].used=2;s_hw_vertex_batches[25][0].used=3;
    lru_append(600,200,512); /* full unsignedLRU still scans all. */
    for(unsigned i=0;i<512;i++)s_hw_vertex_batches[i/4][i%4].used=100+i;
    s_hw_vertex_batches[1][1].used=2;s_hw_vertex_batches[50][0].used=2;
    lru_append(601,5,512); /* strictless retains earliest equalminimum. */
    /* Clockwrap can create used0 with live data: preserve original choice. */
    s_hw_vertex_cache_clock=UINT64_MAX;
    int hit=0;lru_selection_visits=0;HwVertexBatch *entry=hw_vertex_batch_begin(0,0,&hit);
    assert(hit && entry==&s_hw_vertex_batches[1][1] && !entry->used && !lru_selection_visits);
    lru_append(602,5,6);
    hw_free_vertex_batches();assert(!s_hw_vertex_batches[0][0].used);
    fprintf(stderr,"[VERTEX LRU] cold/middle/full/tie/wrap/discard original selection and exact visit bounds passed\n");
}

static void cold_dxt_read(HwTexture *entry,const Texture *source,uint32_t *pixels) {
    unsigned faces=source->cube?6:1,width=source->width,height=source->height;
    if(entry->native)assert(nvk_image_read(entry->native,pixels,width*4));
    else {
        ID3D11Texture2D *image=source->cube?((D3D8CubeTexture *)entry->texture)->d3d11_texture:((D3D8Texture *)entry->texture)->d3d11_texture;
        D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(image,&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ID3D11Texture2D *staging=NULL;hw_check(ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(),&desc,NULL,&staging),"coldDXTtest staging");
        ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)image);
        for(unsigned face=0;face<faces;face++) {
            D3D11_MAPPED_SUBRESOURCE mapped;hw_check(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,face, D3D11_MAP_READ,0,&mapped),"coldDXTtest read");
            for(unsigned y=0;y<height;y++)memcpy(pixels+((size_t)face*height+y)*width,(uint8_t *)mapped.pData+(size_t)y*mapped.RowPitch,width*4);
            ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,face);
        }
        ID3D11Texture2D_Release(staging);
    }
}
static void verify_cold_dxt_upload(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();s_gpu.rc.stage_program=1;
    uint8_t *memory=(uint8_t *)xbox_GetMemoryOffset();unsigned cases=0;
    for(unsigned format=0;format<3;format++)for(unsigned cube=0;cube<2;cube++)for(unsigned edge=0;edge<2;edge++) {
        Texture texture={0};texture.valid=1;texture.width=edge?5:cube?16:8;texture.height=edge?(cube?5:3):(cube?16:4);
        texture.levels=3;texture.cube=cube;texture.color=(uint32_t[]){0xc,0xe,0xf}[format];texture.offset=0x80090000u+cases*0x10000;texture.addr_u=texture.addr_v=3;texture.dma_format=2u<<4;
        unsigned stride=tex_face_stride(&texture),faces=cube?6:1,bb=d3d8_format_dxt_block_bytes(texture.color),bw=(texture.width+3)/4,bh=(texture.height+3)/4;
        uint8_t *raw=memory+texture.offset;memset(raw,0xa5,stride*faces);
        for(unsigned face=0;face<faces;face++)for(unsigned block=0;block<bw*bh;block++) {
            uint8_t *b=raw+face*stride+block*bb;memset(b,0,bb);unsigned c=format?8:0;
            b[c]=0;b[c+1]=0xf8;b[c+2]=0x1f;b[c+3]=0;
            uint32_t selectors=0x1b1b1b1b ^(face*0x55555555u)^(block*0x01010101u);memcpy(b+c+4,&selectors,4);
            if(format==1)for(unsigned i=0;i<8;i++)b[i]=(uint8_t)(0x10*i+face);
            if(format==2){b[0]=face&1?0:255;b[1]=face&1?255:0;for(unsigned i=2;i<8;i++)b[i]=(uint8_t)(i*37+block);}
        }
        s_gpu.texs[0]=texture;
        for(unsigned phase=0;phase<3;phase++) {
            if(phase==2)raw[(cube?5:0)*stride+(format?8:0)+1]^=0xff; /* original guardedCPUwrite invalidates */
            uint64_t uploads=s_gpu.hardware_texture_uploads;cold_upload_samples=0;cold_upload_watch=1;
            hw_enter();HwColor *target=hw_color_target();hw_texture_bind(0,target);hw_leave();cold_upload_watch=0;
            if(cold_upload_samples){fprintf(stderr,"[COLD DXT] fmt=%x cube=%u edge=%u phase=%u sampler=%u expected0\n",texture.color,cube,edge,phase,cold_upload_samples);exit(5);}
            assert(s_gpu.hardware_texture_uploads==uploads+(phase!=1));
            HwTexture *entry=s_hw_textures;while(entry && entry->identity.offset!=texture.offset)entry=entry->next;assert(entry && entry->content_valid && !memcmp(entry->raw,raw,stride*faces));
            size_t count=(size_t)faces*texture.width*texture.height;uint32_t *pixels=malloc(count*4+32);assert(pixels);memset(pixels,0xcc,count*4+32);cold_dxt_read(entry,&texture,pixels);
            for(unsigned face=0;face<faces;face++)for(unsigned y=0;y<texture.height;y++)for(unsigned x=0;x<texture.width;x++) {
                uint32_t expected;assert(sample_tex(&texture,face*stride,x,y,&expected));assert(pixels[((size_t)face*texture.height+y)*texture.width+x]==expected);
            }
            for(unsigned i=0;i<32;i++)assert(((uint8_t *)pixels)[count*4+i]==0xcc);free(pixels);
        }
        ++cases;
    }
    /* Reuse the existing actualdraw path after testing upload-only cube/edge tuples. */
    verify_dxt_block_work();
    fprintf(stderr,"[COLD DXT] %u2D/cube regular/oddedge cases allfaces/mipsstride/rawrewrite/stable guards passed\n",cases);
}

static HwColor *owner_color(uint32_t address) {method(NV097_SET_SURFACE_COLOR_OFFSET,address);return hw_color(0);}
static void verify_owner_mark_work(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();hw_enter();
    HwColor *a=owner_color(0x20000),*b=owner_color(0x20020),*c=owner_color(0xb0000),*marked=owner_color(0x20040);
    a->dirty=b->dirty=c->dirty=marked->dirty=0;
    a->upload_needed=b->upload_needed=c->upload_needed=0;marked->upload_needed=1;
    owner_mark_checks=0;hw_begin_color_write(a);
    if(owner_mark_checks!=2){fprintf(stderr,"[OWNER MARK] actual rangechecks=%u expected2\n",owner_mark_checks);exit(5);}
    assert(!a->upload_needed && b->upload_needed && !c->upload_needed && marked->upload_needed);
    owner_mark_checks=0;hw_begin_color_write(a);assert(owner_mark_checks==1); /* only unmarkeddisjoint stilltested */
    /* Originalupload clearsaliasflag; next actualwrite invalidatesit again. */
    hw_upload_color(b,0);assert(!b->upload_needed);owner_mark_checks=0;hw_begin_color_write(a);assert(owner_mark_checks==2 && b->upload_needed);
    /* Depthpublication still runsbeforecolor-onlymark optimization. */
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x20000);method(NV097_SET_SURFACE_ZETA_OFFSET,0x20000);
    HwDepth *depth=hw_depth();assert(depth);hw_upload_depth(depth);depth->dirty=1;
    uint64_t reads=depth_transfer_test_reads;hw_begin_color_write(a);assert(!depth->dirty && depth->upload_needed && depth_transfer_test_reads==reads+1);
    hw_leave();
    /* Existing real alias/sampler consumer validates pixels and protections afterwrite/rebind. */
    verify_clean_texture_overlap();
    fprintf(stderr,"[OWNER MARK] marked/unmarked/self/disjoint,clearthenremark,depthpublication and originalaliaspixels passed\n");
}
