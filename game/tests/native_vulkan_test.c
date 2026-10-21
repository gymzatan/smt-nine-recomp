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
static void check_images(void) {
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
        vertices[i][0]=xy[i][0];vertices[i][1]=xy[i][1];vertices[i][2]=.5f;vertices[i][3]=1;
        /* Avoid an exact half-UNORM rounding boundary in pixel equality. */
        vertices[i][4]=.25f;vertices[i][5]=.6f;vertices[i][6]=.75f;vertices[i][7]=1;
    }
    float screen[4]={32,24,0,1};NV2APSConstants constants={0};
    struct {uint32_t cx[8],cy[8];float depth[4];uint32_t flags[4],bounds[4];} pixel={0};
    pixel.cx[0]=31u<<16;pixel.cy[0]=23u<<16;pixel.depth[1]=pixel.depth[2]=1;
    pixel.flags[3]=1;pixel.bounds[2]=32;pixel.bounds[3]=24;
    NvkDraw draw={0};draw.color=color;draw.depth=depth;draw.vertex=vs;draw.pixel=ps;
    draw.vertices=vertices;draw.vertex_bytes=sizeof vertices;draw.stride=sizeof vertices[0];draw.count=6;
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
    ok(nvk_depth_transfer(depth,checkpoint,0));
    for(unsigned y=0;y<24;y++)for(unsigned x=0;x<32;x++) {
        float z;memcpy(&z,checkpoint[y][x],4);
        assert(z==(x>=4 && x<28 && y>=4 && y<20?.5f:1.f));
        assert(checkpoint[y][x][4]==(x>=8 && x<24 && y>=8 && y<16?7:0));
    }
    ok(nvk_depth_clear(depth,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,.2f,9,NULL));
    ok(nvk_depth_transfer(depth,checkpoint,1));ok(nvk_depth_transfer(depth,restored,0));
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
