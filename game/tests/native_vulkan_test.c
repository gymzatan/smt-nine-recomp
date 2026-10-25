#include "../../vendor/xboxrecomp/src/d3d/d3d8_internal.h"
#include "../../vendor/xboxrecomp/src/d3d/d3d8_combiners.h"
#include "../../vendor/xboxrecomp/src/d3d/d3d8_vsh.h"
#include "../../vendor/xboxrecomp/src/d3d/nv2a_vulkan.h"
#include <assert.h>
#include <stdio.h>
extern uint64_t g_nvk_timings[8];

static void ok(int value) {
    if(!value){fprintf(stderr,"%s\n",nvk_error());abort();}
}
static void *resource(void *p) {ok(p!=NULL);return p;}
static void check_original_shaders(void) {
    char *fixed=resource(d3d8_vsh_fixed_source());
    const char *sources[]={d3d8_vsh_geometry_source(),d3d8_vsh_geometry_source(),
        d3d8_vsh_geometry_source(),fixed};
    const char *entries[]={"main","main_line","main_point","main_fixed"};
    for(unsigned i=0;i<4;i++) {
        NvkShader *shader=resource(nvk_shader_create(sources[i],entries[i],i==3?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_GEOMETRY_BIT));
        nvk_shader_destroy(shader);
    }
    free(fixed);
    /* This is the original parsed-program representation, including sparse
     * input identities; generation still uses the existing implementation. */
    NV2AVshProgram program={0};program.inputs_read=(1u<<0)|(1u<<3)|(1u<<10);
    char hlsl[65536];assert(d3d8_vsh_generate_hlsl(&program,hlsl,sizeof hlsl)>0);
    NvkShader *shader=resource(nvk_shader_create(hlsl,"main",VK_SHADER_STAGE_VERTEX_BIT));
    nvk_shader_destroy(shader);
}
/* Both reads complete into caller-owned storage before the next API call. */
static void check_readback_storage(void) {
    const unsigned widths[]={8,40,80,8},heights[]={8,32,48,8};
    for(unsigned pass=0;pass<4;pass++) {
        unsigned w=widths[pass],h=heights[pass];size_t n=(size_t)w*h;
        NvkImage *color=resource(nvk_image_create(w,h,1,VK_FORMAT_B8G8R8A8_UNORM,0));
        NvkImage *depth=resource(nvk_image_create(w,h,1,VK_FORMAT_D32_SFLOAT_S8_UINT,0));
        unsigned char *z=resource(malloc(n*8+32));
        uint32_t *pixels=resource(malloc((w+1)*h*4+32));
        const float rgba[4]={1,0,0,1};
        float expected=(pass+1)*0.125f;unsigned stencil=17+pass;
        ok(nvk_image_clear(color,rgba));
        ok(nvk_depth_clear(depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,expected,stencil,NULL));
        memset(pixels,0xcc,(w+1)*h*4+32);
        ok(nvk_image_read(color,pixels,(w+1)*4));
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<=w;x++)
            assert(pixels[y*(w+1)+x]==(x<w?0xffff0000u:0xccccccccu));
        memset(z,0xcc,n*8+32);ok(nvk_depth_transfer(depth,z,0,1));
        for(size_t i=0;i<n;i++) {
            float actual;memcpy(&actual,z+i*8,4);assert(actual==expected);
            assert(z[i*8+4]==stencil && z[i*8+5]==0 && z[i*8+6]==0 && z[i*8+7]==0);
        }
        for(unsigned i=0;i<32;i++)assert(z[n*8+i]==0xcc);
        /* Preserve the original fresh upload path and verify subsequent reuse. */
        for(size_t i=0;i<n;i++){float value=0.75f;memcpy(z+i*8,&value,4);z[i*8+4]=93;}
        ok(nvk_depth_transfer(depth,z,1,1));memset(z,0xcc,n*8);
        ok(nvk_depth_transfer(depth,z,0,1));
        for(size_t i=0;i<n;i++){float value;memcpy(&value,z+i*8,4);assert(value==0.75f && z[i*8+4]==93);}
        ok(nvk_image_read(color,pixels,(w+1)*4));
        for(unsigned i=0;i<32;i++)assert(((unsigned char*)pixels)[(w+1)*h*4+i]==0xcc && z[n*8+i]==0xcc);
        free(z);free(pixels);nvk_image_destroy(depth);nvk_image_destroy(color);
    }
    fprintf(stderr,"[READBACK STORAGE] alternating color/depth-stencil growth, reuse, upload and caller guards passed\n");
}
/* Upload owns its CPU source immediately; GPU copies share the original ring. */
static void check_queued_image_upload(void) {
    NvkImage *images[4]={resource(nvk_image_create(3,2,1,VK_FORMAT_B8G8R8A8_UNORM,0)),
        resource(nvk_image_create(2,2,6,VK_FORMAT_B8G8R8A8_UNORM,1)),
        resource(nvk_image_create_volume(3,2,3,VK_FORMAT_B8G8R8A8_UNORM)),
        resource(nvk_image_create(3,2,1,VK_FORMAT_R32_SFLOAT,0))};
    const unsigned widths[4]={3,2,3,3},rows[4]={2,12,6,2};
    ok(nvk_wait(nvk_fence()));
    uint64_t submitted=g_nvk_timings[4],waited=g_nvk_timings[5];
    for(unsigned pass=0;pass<2;pass++)for(unsigned image=0;image<4;image++) {
        unsigned w=widths[image],h=rows[image];
        uint32_t *source=resource(malloc((w+1)*h*4));
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<=w;x++)
            source[y*(w+1)+x]=0xff000000u|(pass<<20)|(image<<16)|(y<<8)|x;
        ok(nvk_image_upload(images[image],source,(w+1)*4));
        memset(source,0xee,(w+1)*h*4);free(source);
    }
    if(g_nvk_timings[4]!=submitted || g_nvk_timings[5]!=waited) {
        fprintf(stderr,"[QUEUED IMAGE UPLOAD] unexpected submit/wait: %llu/%llu -> %llu/%llu\n",
            (unsigned long long)submitted,(unsigned long long)waited,
            (unsigned long long)g_nvk_timings[4],(unsigned long long)g_nvk_timings[5]);exit(5);
    }
    assert(g_nvk_timings[4]==submitted && g_nvk_timings[5]==waited);
    /* Exercise original four-chunk rollover while the small copies are queued. */
    NvkImage *large=resource(nvk_image_create(2048,2048,1,VK_FORMAT_B8G8R8A8_UNORM,0));
    const size_t count=2048u*2048u;uint32_t *large_source=resource(malloc(count*4));
    for(unsigned pass=0;pass<5;pass++) {
        for(size_t i=0;i<count;i++)large_source[i]=0xff000000u|(pass<<20)|(i&0xfffff);
        ok(nvk_image_upload(large,large_source,2048*4));
        memset(large_source,0xee,count*4);
    }
    free(large_source);
    for(unsigned image=0;image<4;image++) {
        unsigned w=widths[image],h=rows[image];uint32_t *output=resource(malloc((w+1)*h*4));
        memset(output,0xcc,(w+1)*h*4);ok(nvk_image_read(images[image],output,(w+1)*4));
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<=w;x++)
            assert(output[y*(w+1)+x]==(x<w?(0xff100000u|(image<<16)|(y<<8)|x):0xccccccccu));
        free(output);nvk_image_destroy(images[image]);
    }
    uint32_t *large_output=resource(malloc(count*4));ok(nvk_image_read(large,large_output,2048*4));
    for(size_t i=0;i<count;i++)assert(large_output[i]==(0xff400000u|(i&0xfffff)));
    free(large_output);nvk_image_destroy(large);
    fprintf(stderr,"[QUEUED IMAGE UPLOAD] pitched/cube/volume/R32 repeated writes and temporary sources passed without per-image submit/wait\n");
}
static void check_images(void) {
    check_queued_image_upload();
    check_readback_storage();
    NvkImage *image=resource(nvk_image_create(32,24,1,VK_FORMAT_B8G8R8A8_UNORM,0));
    uint32_t input[24][35],output[24][35];
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<35;x++)input[y][x]=0xff000000|(x<<16)|(y<<8)|0x6a;
    memset(output,0xcc,sizeof output);
    ok(nvk_image_upload(image,input,sizeof input[0]));
    ok(nvk_image_read(image,output,sizeof output[0]));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<35;x++)
        assert(output[y][x]==(x<32?input[y][x]:0xcccccccc));
    NvkImage *copy=resource(nvk_image_create(40,32,1,VK_FORMAT_B8G8R8A8_UNORM,0));
    const float black[4]={0,0,0,1};ok(nvk_image_clear(copy,black));
    const int32_t bounds[4]={4,4,36,28};ok(nvk_image_copy(image,copy,bounds,0));
    uint32_t actual[32][40];ok(nvk_image_read(copy,actual,sizeof actual[0]));
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<40;x++)
        assert(actual[y][x]==(x>=4 && x<36 && y>=4 && y<28?input[y-4][x-4]:0xff000000));
    /* Alternate dimensions and new GPU contents: retained transfer storage
     * must not retain pixels or overwrite the caller's row padding. */
    for(unsigned pass=0;pass<3;pass++) {
        for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++)input[y][x]^=0x00010305;
        ok(nvk_image_upload(image,input,sizeof input[0]));
        memset(output,0xcc,sizeof output);ok(nvk_image_read(image,output,sizeof output[0]));
        for(unsigned y=0;y<24;y++)for(unsigned x=0;x<35;x++)
            assert(output[y][x]==(x<32?input[y][x]:0xcccccccc));
        const float color[4]={pass==0?1.0f:0,pass==1?1.0f:0,pass==2?1.0f:0,1};
        ok(nvk_image_clear(copy,color));ok(nvk_image_read(copy,actual,sizeof actual[0]));
        uint32_t expected=0xff000000u|(pass==0?0x00ff0000u:pass==1?0x0000ff00u:0x000000ffu);
        for(unsigned y=0;y<32;y++)for(unsigned x=0;x<40;x++)assert(actual[y][x]==expected);
    }
    nvk_image_destroy(copy);nvk_image_destroy(image);
}
/* Exercise the original backend/key through real draws and image/depth reads.
 * No cache observer or alternate pipeline constructor is needed. */
static void check_effective_pipeline_state(const NvkDraw *original) {
    NvkDraw draw=*original;draw.query=NULL;
    draw.stencil=draw.depth_test=draw.depth_write=draw.blend=draw.logic=0;
    const float black[4]={0,0,0,1};uint32_t pixels[24][32];
    const uint32_t source=0xff4099bf;
    ok(nvk_image_clear(draw.color,black));
    /* Stable modules plus 8193 genuinely effective stencil keys cross the
     * original capacity without relying on incidental shader-cache eviction.
     * The first query remains owned by its original submission across recycle. */
    draw.stencil=1;
    for(unsigned i=0;i<8193;i++) {
        draw.stencil_state=(VkStencilOpState){VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,
            VK_STENCIL_OP_KEEP,VK_COMPARE_OP_EQUAL,(i>>8)&255,0,i&255};
        draw.query=i==0?original->query:NULL;
        if(!nvk_draw(&draw)){fprintf(stderr,"[PIPELINE CAPACITY] enabled draw %u: %s\n",i,nvk_error());exit(5);}
    }
    draw.query=NULL;
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++)
        assert(pixels[y][x]==(x>=4 && x<28 && y>=4 && y<20?source:0xff000000));
    uint64_t first_samples=0;
    ok(nvk_query_result(original->query,&first_samples,1)==1);assert(first_samples==24*16);
    draw.stencil=0;
    fprintf(stderr,"[PIPELINE CAPACITY] 8193 enabled keys, original query and attachment pixels passed\n");
    /* Each original disabled stencil reference/mask pair is unique, including
     * all values within the actual eight-bit stencil range. The old key must
     * exhaust its existing 8192 capacity before completing these 8193 draws. */
    for(unsigned i=0;i<8193;i++) {
        draw.stencil_state=(VkStencilOpState){VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,
            VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,(i>>8)&255,(i*17)&255,i&255};
        draw.depth_write=i&1;draw.depth_compare=(VkCompareOp)(i%8);
        draw.source_blend=(i&1)?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ZERO;
        draw.destination_blend=(i&2)?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ZERO;
        draw.blend_op=(VkBlendOp)(i%5);draw.logic_op=(VkLogicOp)(i%16);
        if(!nvk_draw(&draw)){fprintf(stderr,"[PIPELINE EFFECTIVE] draw %u: %s\n",i,nvk_error());exit(5);}
    }
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++)
        assert(pixels[y][x]==(x>=4 && x<28 && y>=4 && y<20?source:0xff000000));
    draw.depth_write=0;draw.depth_compare=VK_COMPARE_OP_ALWAYS;
    draw.source_blend=VK_BLEND_FACTOR_ONE;draw.destination_blend=VK_BLEND_FACTOR_ZERO;
    draw.blend_op=VK_BLEND_OP_ADD;draw.logic_op=VK_LOGIC_OP_COPY;
    /* Enabled stencil must distinguish reference, compare mask and write mask. */
    draw.stencil=1;draw.stencil_state=(VkStencilOpState){VK_STENCIL_OP_KEEP,
        VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_EQUAL,255,0,7};
    ok(nvk_depth_clear(draw.depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,1,7,NULL));
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    draw.stencil_state.reference=6;
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0xff000000);
    draw.stencil_state.compareMask=0;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    unsigned char depth[24][32][8];
    draw.stencil_state.compareOp=VK_COMPARE_OP_ALWAYS;
    draw.stencil_state.passOp=VK_STENCIL_OP_REPLACE;draw.stencil_state.reference=9;
    draw.stencil_state.writeMask=0;
    ok(nvk_draw(&draw));ok(nvk_depth_transfer(draw.depth,depth,0,1));assert(depth[10][10][4]==7);
    draw.stencil_state.writeMask=255;
    ok(nvk_draw(&draw));ok(nvk_depth_transfer(draw.depth,depth,0,1));assert(depth[10][10][4]==9);
    /* Enabled depth compare/write must remain distinct. The original shader's
     * depth is in [0,1), independent of its raw-to-window encoding. */
    draw.stencil=0;draw.depth_test=1;draw.depth_write=0;draw.depth_compare=VK_COMPARE_OP_LESS;
    ok(nvk_depth_clear(draw.depth,VK_IMAGE_ASPECT_DEPTH_BIT,1,0,NULL));
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    ok(nvk_depth_transfer(draw.depth,depth,0,1));float z;memcpy(&z,depth[10][10],4);assert(z==1);
    draw.depth_compare=VK_COMPARE_OP_GREATER;
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0xff000000);
    draw.depth_compare=VK_COMPARE_OP_LESS;draw.depth_write=1;
    ok(nvk_draw(&draw));ok(nvk_depth_transfer(draw.depth,depth,0,1));memcpy(&z,depth[10][10],4);assert(z>=0 && z<1);
    draw.depth_test=draw.depth_write=0;
    /* Enabled blend factors and operations still select different pixels. */
    draw.blend=1;draw.source_blend=VK_BLEND_FACTOR_ZERO;draw.destination_blend=VK_BLEND_FACTOR_ONE;
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0xff000000);
    draw.source_blend=VK_BLEND_FACTOR_ONE;draw.destination_blend=VK_BLEND_FACTOR_ZERO;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    draw.blend_op=VK_BLEND_OP_MIN;
    ok(nvk_image_clear(draw.color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0xff000000);
    draw.blend_op=VK_BLEND_OP_MAX;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    /* Disabled blend cannot suppress enabled logical operation identity. */
    draw.blend=0;draw.logic=1;draw.logic_op=VK_LOGIC_OP_CLEAR;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0);
    draw.logic_op=VK_LOGIC_OP_COPY;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==source);
    draw.logic_op=VK_LOGIC_OP_XOR;
    ok(nvk_draw(&draw));ok(nvk_image_read(draw.color,pixels,sizeof pixels[0]));assert(pixels[10][10]==0);
    /* Return original attachments to their initial fixture contents. */
    ok(nvk_image_clear(draw.color,black));
    ok(nvk_depth_clear(draw.depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,1,0,NULL));
    fprintf(stderr,"[PIPELINE EFFECTIVE] 8193 disabled-state combinations and enabled stencil/depth/blend/logic pixels passed\n");
}
static void check_original_draw(void) {
    NvkImage *color=resource(nvk_image_create(32,24,1,VK_FORMAT_B8G8R8A8_UNORM,0));
    NvkImage *depth=resource(nvk_image_create(32,24,1,VK_FORMAT_D32_SFLOAT_S8_UINT,0));
    const float black[4]={0,0,0,1};ok(nvk_image_clear(color,black));
    ok(nvk_depth_clear(depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,1,0,NULL));
    NvkShader *vs=resource(nvk_shader_create(d3d8_shaders_nv2a_output_source(),"main",VK_SHADER_STAGE_VERTEX_BIT));
    NV2ACombinerState state={0};state.native_coordinates=1;state.final_enabled=1;
    for(unsigned i=0;i<4;i++)state.tex_mode[i]=NV2A_TEXMODE_NONE;
    state.final_input[3].reg=state.final_input[6].reg=NV2A_REG_V0;
    state.final_input[6].alpha_rep=1;
    char hlsl[16384];assert(d3d8_combiners_generate_hlsl(&state,hlsl,sizeof hlsl)>0);
    NvkShader *ps=resource(nvk_shader_create(hlsl,"main",VK_SHADER_STAGE_FRAGMENT_BIT));
    float vertices[6][32]={0};
    const float xy[6][2]={{4,4},{28,4},{4,20},{4,20},{28,4},{28,20}};
    for(unsigned i=0;i<6;i++) {
        vertices[i][0]=xy[i][0];vertices[i][1]=xy[i][1];vertices[i][2]=32768.f;vertices[i][3]=1;
        /* Avoid an exact half-UNORM rounding boundary in pixel equality. */
        vertices[i][4]=.25f;vertices[i][5]=.6f;vertices[i][6]=.75f;vertices[i][7]=1;
    }
    float screen[4]={32,24,0,1};NV2APSConstants constants={0};
    struct {uint32_t cx[8],cy[8];float depth[4];uint32_t flags[4],bounds[4];float depth_offset[4];uint32_t stipple_flags[4],stipple_pattern[32];} pixel={0};
    pixel.cx[0]=31u<<16;pixel.cy[0]=23u<<16;pixel.depth[1]=65535;pixel.depth[2]=1.f/65536.f;
    pixel.flags[3]=1;pixel.bounds[2]=32;pixel.bounds[3]=24;
    NvkDraw draw={0};draw.color=color;draw.depth=depth;draw.vertex=vs;draw.pixel=ps;
    draw.vertices=vertices;draw.vertex_bytes=sizeof vertices;draw.stride=sizeof vertices[0];draw.count=6;draw.draw_count=draw.count;
    draw.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;draw.attribute_count=8;
    unsigned locations[]={2,0,1,7,3,4,5,6};
    for(unsigned i=0;i<8;i++)draw.attributes[i]=(VkVertexInputAttributeDescription){locations[i],0,VK_FORMAT_R32G32B32A32_SFLOAT,i*16};
    draw.uniform_count=3;
    draw.uniforms[0]=(NvkUniform){screen,sizeof screen,0};
    draw.uniforms[1]=(NvkUniform){&constants,sizeof constants,16};
    draw.uniforms[2]=(NvkUniform){&pixel,sizeof pixel,17};
    draw.depth_test=draw.depth_write=1;draw.depth_compare=VK_COMPARE_OP_LESS;
    draw.color_mask=15;draw.query=resource(nvk_query_create());
    uint64_t generation=0;uint32_t upload_offset=0;
    draw.upload_generation=&generation;draw.upload_offset=&upload_offset;
    check_effective_pipeline_state(&draw);
    ok(nvk_draw(&draw));uint64_t samples=0;ok(nvk_query_result(draw.query,&samples,1)==1);assert(samples==24*16);
    uint32_t actual[24][32];ok(nvk_image_read(color,actual,sizeof actual[0]));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++) {
        uint32_t expected=x>=4 && x<28 && y>=4 && y<20?0xff4099bf:0xff000000;
        if(actual[y][x]!=expected){fprintf(stderr,"pixel %u,%u: %08x expected %08x\n",x,y,actual[y][x],expected);abort();}
    }
    /* A second equal-depth draw reuses the live range in a previous submission. */
    ok(nvk_draw(&draw));assert(draw.uploaded==0);
    ok(nvk_query_result(draw.query,&samples,1)==1);assert(samples==0);
    /* Changing only stencil in a rectangle must retain depth. */
    const int32_t rect[4]={8,8,24,16};
    ok(nvk_depth_clear(depth,VK_IMAGE_ASPECT_STENCIL_BIT,.1f,7,rect));
    ok(nvk_draw(&draw));ok(nvk_query_result(draw.query,&samples,1)==1);assert(samples==0);
    /* Consecutive draws share one rendering instance. Depth still orders their
     * fragments, and later transfers must finish that instance before reading. */
    NvkQuery *query=draw.query;draw.query=NULL;
    ok(nvk_draw(&draw));
    uint64_t uploaded=g_nvk_timings[7];
    for(unsigned i=0;i<5;i++)ok(nvk_draw(&draw));
    assert(g_nvk_timings[7]==uploaded);
    uint32_t repeated[24][32];ok(nvk_image_read(color,repeated,sizeof repeated[0]));
    assert(!memcmp(actual,repeated,sizeof actual));draw.query=query;
    /* Checkpoint transfer retains the original interleaved float/stencil payload. */
    unsigned char checkpoint[24][32][8],restored[24][32][8];
    ok(nvk_depth_transfer(depth,checkpoint,0,1));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++) {
        float z;memcpy(&z,checkpoint[y][x],4);
        assert(z==(x>=4 && x<28 && y>=4 && y<20?.5f:1.f));
        assert(checkpoint[y][x][4]==(x>=8 && x<24 && y>=8 && y<16?7:0));
    }
    ok(nvk_depth_clear(depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.2f,9,NULL));
    ok(nvk_depth_transfer(depth,checkpoint,1,1));ok(nvk_depth_transfer(depth,restored,0,1));
    assert(!memcmp(checkpoint,restored,sizeof checkpoint));
    /* Submitting commands must not discard live uploaded geometry. */
    for(unsigned i=0;i<5;i++){ok(nvk_image_clear(color,black));assert(nvk_submit());}
    ok(nvk_draw(&draw));assert(draw.uploaded==0);
    ok(nvk_query_result(draw.query,&samples,1)==1);assert(samples==0);
    /* Physical buffer growth and reuse still invalidate the original range.
     * The unused payload tail exercises capacity without changing geometry. */
    const size_t large_bytes=17*1024*1024;
    void *large=resource(calloc(1,large_bytes));memcpy(large,vertices,sizeof vertices);
    draw.vertices=large;draw.vertex_bytes=large_bytes;draw.upload_generation=NULL;draw.upload_offset=NULL;
    query=draw.query;draw.query=NULL;
    for(unsigned i=0;i<9;i++)ok(nvk_draw(&draw));
    /* An idle ring segment can lack space without containing GPU work.
     * Only the real draw should advance the submission count. Active ring
     * rollover above and completed rollover here both preserve the pixels. */
    ok(nvk_wait(nvk_fence()));
    for(unsigned i=0;i<8;i++) {
        uint64_t submitted=g_nvk_timings[4];
        ok(nvk_draw(&draw));assert(g_nvk_timings[4]==submitted);
        ok(nvk_wait(nvk_fence()));assert(g_nvk_timings[4]==submitted+1);
    }

    draw.vertices=vertices;draw.vertex_bytes=sizeof vertices;draw.query=query;
    draw.upload_generation=&generation;draw.upload_offset=&upload_offset;
    ok(nvk_draw(&draw));assert(draw.uploaded==1);
    ok(nvk_query_result(draw.query,&samples,1)==1);assert(samples==0);free(large);
    /* Two draws with different clipping constants must keep both immutable
     * uniform ranges while sharing their descriptors in one submission. */
    query=draw.query;draw.query=NULL;draw.depth_test=draw.depth_write=0;
    ok(nvk_image_clear(color,black));
    pixel.bounds[0]=0;pixel.bounds[2]=16;ok(nvk_draw(&draw));
    pixel.bounds[0]=16;pixel.bounds[2]=32;ok(nvk_draw(&draw));
    ok(nvk_image_read(color,repeated,sizeof repeated[0]));
    assert(!memcmp(actual,repeated,sizeof actual));draw.query=query;
    /* Nonblocking query observation shares original completed serial state. */
    pixel.bounds[0]=0;pixel.bounds[2]=32;draw.depth_test=draw.depth_write=draw.stencil=0;
    for(unsigned query_case=0;query_case<3;query_case++) {
        ok(nvk_wait(nvk_fence()));
        NvkQuery *probe=resource(nvk_query_create());draw.query=probe;
        ok(nvk_image_clear(color,black));ok(nvk_draw(&draw));assert(nvk_submit());
        if(query_case==0) {
            ULONGLONG deadline=GetTickCount64()+5000;uint64_t result=0;int ready;
            do {ready=nvk_query_result(probe,&result,0);ok(ready>=0);
                if(!ready){assert(GetTickCount64()<deadline);SwitchToThread();}
            } while(!ready);
            assert(result==24*16);
        } else if(query_case==2) {
            uint64_t result=0;ok(nvk_query_result(probe,&result,1)==1);assert(result==24*16);
            ok(nvk_draw(&draw));assert(nvk_submit());
        }
        uint64_t before_destroy=g_nvk_timings[5];nvk_query_destroy(probe);draw.query=NULL;
        uint64_t expected=before_destroy+(query_case!=0);
        if(g_nvk_timings[5]!=expected) {
            fprintf(stderr,"[QUERY TIMELINE] case=%u waits=%llu expected=%llu\n",query_case,
                (unsigned long long)g_nvk_timings[5],(unsigned long long)expected);exit(5);
        }
        assert(g_nvk_timings[5]==expected);
        ok(nvk_image_read(color,repeated,sizeof repeated[0]));assert(!memcmp(actual,repeated,sizeof actual));
    }
    draw.query=query;
    fprintf(stderr,"[QUERY TIMELINE] final query384, nonblocking observation/destroy and unobserved/reused waits passed\n");
    /* Unused modules have no pipeline to retire. Their destruction must not
     * submit or wait for this unrelated pending original draw. */
    pixel.bounds[0]=0;pixel.bounds[2]=32;draw.query=NULL;
    ok(nvk_image_clear(color,black));ok(nvk_draw(&draw));
    NvkShader *vs_unused=resource(nvk_shader_create(d3d8_shaders_nv2a_output_source(),"main",VK_SHADER_STAGE_VERTEX_BIT));
    NvkShader *ps_unused=resource(nvk_shader_create(hlsl,"main",VK_SHADER_STAGE_FRAGMENT_BIT));
    uint64_t no_match_submits=g_nvk_timings[4],no_match_waits=g_nvk_timings[5];
    nvk_shader_destroy(ps_unused);nvk_shader_destroy(vs_unused);
    if(g_nvk_timings[4]!=no_match_submits || g_nvk_timings[5]!=no_match_waits) {
        fprintf(stderr,"[MODULE NO MATCH] unexpected submit/wait: %llu/%llu -> %llu/%llu\n",
            (unsigned long long)no_match_submits,(unsigned long long)no_match_waits,
            (unsigned long long)g_nvk_timings[4],(unsigned long long)g_nvk_timings[5]);exit(5);
    }
    assert(g_nvk_timings[4]==no_match_submits && g_nvk_timings[5]==no_match_waits);
    ok(nvk_image_read(color,repeated,sizeof repeated[0]));
    assert(!memcmp(actual,repeated,sizeof actual));draw.query=query;
    fprintf(stderr,"[MODULE NO MATCH] pending draw retained without submit/wait; original pixels passed\n");
    /* Retiring one module preserves another pipeline across submitted and
     * pending draws. Both pairs use the original shaders and pixel contract. */
    NvkShader *vs_other=resource(nvk_shader_create(d3d8_shaders_nv2a_output_source(),"main",VK_SHADER_STAGE_VERTEX_BIT));
    NvkShader *ps_other=resource(nvk_shader_create(hlsl,"main",VK_SHADER_STAGE_FRAGMENT_BIT));
    pixel.bounds[0]=0;pixel.bounds[2]=32;draw.vertex=vs_other;draw.pixel=ps_other;draw.query=NULL;
    ok(nvk_image_clear(color,black));ok(nvk_draw(&draw));assert(nvk_submit());
    nvk_shader_destroy(ps);ps=NULL;
    ok(nvk_image_clear(color,black));ok(nvk_draw(&draw));
    nvk_shader_destroy(vs);vs=NULL;
    ok(nvk_image_read(color,repeated,sizeof repeated[0]));
    assert(!memcmp(actual,repeated,sizeof actual));
    ok(nvk_image_clear(color,black));ok(nvk_draw(&draw));
    ok(nvk_image_read(color,repeated,sizeof repeated[0]));
    assert(!memcmp(actual,repeated,sizeof actual));
    vs=vs_other;ps=ps_other;draw.query=query;
    /* The original program producer shares one block at bindings 1 and 9.
     * Keep dynamic pixel data correct while aliases use the same upload. */
    {
        NvkDraw shared_draw=draw;shared_draw.query=NULL;
        shared_draw.depth_test=shared_draw.depth_write=0;
        shared_draw.uniforms[3]=(NvkUniform){&constants,sizeof constants,1};
        shared_draw.uniforms[4]=(NvkUniform){&constants,sizeof constants,9};
        shared_draw.uniform_count=5;
        for(unsigned pass=0;pass<2;pass++) {
            pixel.bounds[0]=0;pixel.bounds[2]=pass?32:16;
            ok(nvk_image_clear(color,black));
            uint64_t copied=g_nvk_timings[7];ok(nvk_draw(&shared_draw));
            size_t stream=shared_draw.uploaded?shared_draw.vertex_bytes+(shared_draw.indices?shared_draw.count*4:0):0;
            assert(g_nvk_timings[7]-copied<=stream+sizeof screen+sizeof constants+sizeof pixel);
            ok(nvk_image_read(color,repeated,sizeof repeated[0]));
            for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++)
                assert(repeated[y][x]==(x<(pass?32u:16u)?actual[y][x]:0xff000000u));
        }
        pixel.bounds[0]=0;pixel.bounds[2]=32;
        fprintf(stderr,"[SHARED UNIFORM] bindings 1/9, dynamic pixels and unique upload-byte bound passed\n");
    }

    HWND window=CreateWindowExW(0,L"STATIC",L"NINE direct Vulkan test",WS_OVERLAPPEDWINDOW,0,0,160,128,NULL,NULL,GetModuleHandleW(NULL),NULL);
    assert(window);
    for(unsigned i=0;i<6;i++) {
        unsigned w=i<3?128:96,h=i<3?96:72;
        SetWindowPos(window,NULL,0,0,w,h,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
        RECT client;GetClientRect(window,&client);int32_t bounds[4]={0,0,client.right,client.bottom};
        ok(nvk_window_present(window,color,client.right,client.bottom,bounds,i%2));
    }
    nvk_window_close();DestroyWindow(window);
    nvk_query_destroy(draw.query);nvk_shader_destroy(ps);nvk_shader_destroy(vs);
    nvk_image_destroy(depth);nvk_image_destroy(color);
}
int check_direct_vulkan(const char *compiler) {
    wchar_t path[2048];assert(MultiByteToWideChar(CP_ACP,0,compiler,-1,path,2048)>0);
    _putenv_s("RECOMP_GPU_TIMING","1");
    ok(nvk_initialize(path));
    assert(d3d8_GetD3D11Device()==NULL);
    check_original_shaders();check_images();check_original_draw();
    assert(d3d8_GetD3D11Device()==NULL);nvk_shutdown();
    fprintf(stderr,"[VULKAN NATIVE] original shader generation, actual pixels, pitch/copy, depth/stencil, exact occlusion completion and window presentation passed without a D3D device\n");
    return 0;
}
