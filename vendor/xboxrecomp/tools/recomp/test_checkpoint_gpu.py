"""Restore original NV2A/VSH/pixel consumer and both logical/hardware zeta."""
from pathlib import Path
import shutil
import subprocess
import os
import sys

import pytest


@pytest.fixture(scope='module')
def native_gpu(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('checkpoint_gpu')
    compiler=shutil.which('cl');assert compiler, 'requires existing MSVC environment'
    source=r'''
#include "kernel/kernel_bridge.c"
#include <assert.h>
#include "d3d8_internal.h"
#include "nv2a_vulkan.h"
static uint64_t native_vertex_generation;
static uint32_t native_vertex_offset;
static unsigned native_vertex_bytes,native_stream_bytes,native_vertex_stride;
/* Observe the production caller's exact binding and payload, before the native
 * backend consumes it. Native upload immutability has a separate pixel test. */
static unsigned char native_uniform_bytes[32][4096];
static unsigned native_uniform_sizes[32];
static unsigned polygon_native_draw_calls;
static int native_draw_for_test(NvkDraw *draw) {
    polygon_native_draw_calls++;
    uint64_t generation=0;uint32_t offset=0;
    uint64_t *original_generation=draw->upload_generation;uint32_t *original_offset=draw->upload_offset;
    if(!original_generation){draw->upload_generation=&generation;draw->upload_offset=&offset;}
    memset(native_uniform_sizes,0,sizeof native_uniform_sizes);
    for(unsigned i=0;i<draw->uniform_count;i++) {
        const NvkUniform *u=&draw->uniforms[i];
        assert(u->binding<32 && u->bytes<=4096);
        memcpy(native_uniform_bytes[u->binding],u->data,u->bytes);native_uniform_sizes[u->binding]=u->bytes;
    }
    int result=nvk_draw(draw);
    native_vertex_generation=*draw->upload_generation;native_vertex_offset=*draw->upload_offset;
    native_vertex_bytes=(unsigned)draw->vertex_bytes;native_stream_bytes=native_vertex_bytes+(draw->indices?draw->count*4:0);native_vertex_stride=draw->stride;
    draw->upload_generation=original_generation;draw->upload_offset=original_offset;
    return result;
}
#define nvk_draw native_draw_for_test
typedef struct { char operation; ID3D11Resource *resource; } ReadbackTrace;
extern ReadbackTrace readback_trace[128];
extern unsigned readback_trace_count;
static unsigned char readback_present_pixels[640*480*4*16];
static unsigned readback_present_bytes;
extern void xbox_FramebufferWindowPresentHost(const void *,unsigned,unsigned,unsigned);
static void readback_present_for_test(const void *pixels,unsigned pitch,unsigned width,unsigned height) {
    readback_present_bytes=width*height*4;assert(readback_present_bytes<=sizeof readback_present_pixels);
    for(unsigned y=0;y<height;++y)memcpy(readback_present_pixels+(size_t)y*width*4,(const unsigned char *)pixels+(size_t)y*pitch,(size_t)width*4);
    xbox_FramebufferWindowPresentHost(pixels,pitch,width,height);
}
#define xbox_FramebufferWindowPresentHost readback_present_for_test
static uintptr_t depth_guard_watch;
static unsigned depth_guard_test_calls,depth_transfer_test_reads;
static int depth_transfer_for_test(NvkImage *image,void *pixels,int upload) {if(!upload)++depth_transfer_test_reads;return nvk_depth_transfer(image,pixels,upload);}
#define nvk_depth_transfer depth_transfer_for_test
static unsigned protection_test_calls,protection_test_identical,protection_test_inspect=1;
static BOOL protection_for_test(void *address,SIZE_T bytes,DWORD protection,DWORD *previous) {
    MEMORY_BASIC_INFORMATION region;
    ++protection_test_calls;
    if(depth_guard_watch && (uintptr_t)address<=depth_guard_watch && depth_guard_watch-(uintptr_t)address<bytes)++depth_guard_test_calls;
    if(protection_test_inspect && VirtualQuery(address,&region,sizeof region) && region.Protect==protection &&
       bytes<=region.RegionSize-((uintptr_t)address-(uintptr_t)region.BaseAddress)) ++protection_test_identical;
    return VirtualProtect(address,bytes,protection,previous);
}
#define VirtualProtect protection_for_test
static unsigned attribute_offset_calls;
static const void *texture_compare_left;
static unsigned texture_compare_calls;
static int texture_compare_for_test(const void *a,const void *b,size_t bytes) {
    if(a==texture_compare_left) ++texture_compare_calls;
    return memcmp(a,b,bytes);
}
static __declspec(noinline) ptrdiff_t attribute_offset_for_test(void) {
    ++attribute_offset_calls;
    return xbox_GetMemoryOffset();
}
#define xbox_GetMemoryOffset attribute_offset_for_test
static unsigned query_status_calls,query_event_payload_calls;
static unsigned query_context_getter_calls;
static UINT vertex_stream_test_bytes;
static UINT vertex_upload_for_test(const void *data,UINT vertex_bytes,const void *indices,UINT index_bytes,ID3D11Buffer **buffer) {
    vertex_stream_test_bytes=vertex_bytes+index_bytes;
    return d3d8_upload_vertices(data,vertex_bytes,indices,index_bytes,buffer);
}

static BOOL vertex_reuse_for_test(uint64_t generation,UINT offset,UINT bytes,ID3D11Buffer **buffer) {
    BOOL result=d3d8_reuse_vertex_upload(generation,offset,bytes,buffer);
    if(result) vertex_stream_test_bytes=bytes;
    return result;
}
#define d3d8_reuse_vertex_upload vertex_reuse_for_test

#define d3d8_upload_vertices vertex_upload_for_test
static ID3D11DeviceContext *query_context_for_test(void) {
    ++query_context_getter_calls;return d3d8_GetD3D11Context();
}
static ID3D11DeviceContext *query_macro_context;
static void query_context_observe(ID3D11DeviceContext *context) {query_macro_context=context;}
static HRESULT query_data_for_test(ID3D11DeviceContext *context,ID3D11Asynchronous *query,void *data,UINT bytes,UINT flags) {
    assert(context==query_macro_context);
    if(!data && !bytes)++query_status_calls;
    if(data && bytes==sizeof(BOOL))++query_event_payload_calls;
    return ID3D11DeviceContext_GetData(context,query,data,bytes,flags);
}
#undef ID3D11DeviceContext_GetData
#define ID3D11DeviceContext_GetData(This,query,data,bytes,flags) (query_context_observe(This),query_data_for_test(This,query,data,bytes,flags))
static unsigned pixel_state_maps;
static unsigned event_query_creates;
static HRESULT query_create_for_test(ID3D11Device *device,const D3D11_QUERY_DESC *desc,ID3D11Query **query) {
    if(desc->Query==D3D11_QUERY_EVENT)++event_query_creates;
    return ID3D11Device_CreateQuery(device,desc,query);
}
#pragma push_macro("ID3D11Device_CreateQuery")
#undef ID3D11Device_CreateQuery
#define ID3D11Device_CreateQuery query_create_for_test
static HRESULT pixel_state_map_for_test(ID3D11DeviceContext *,ID3D11Resource *,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE *);
#pragma push_macro("ID3D11DeviceContext_Map")
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map pixel_state_map_for_test
#define d3d8_GetD3D11Context query_context_for_test
#define memcmp texture_compare_for_test
#include "nv2a/nv2a_state.h"
static unsigned progress_kick_calls;
static void progress_kick_for_test(NV2AState *gpu) {
    ++progress_kick_calls;nv2a_fifo_kick(gpu);
}
#define nv2a_fifo_kick progress_kick_for_test
#include "kernel/nv2a_pb_exec.c"
#undef nv2a_fifo_kick
#undef memcmp
#undef d3d8_upload_vertices
#undef d3d8_reuse_vertex_upload
#undef d3d8_GetD3D11Context
#pragma pop_macro("ID3D11DeviceContext_Map")
#pragma pop_macro("ID3D11Device_CreateQuery")
#undef ID3D11DeviceContext_GetData
static HRESULT pixel_state_map_for_test(ID3D11DeviceContext *ctx,ID3D11Resource *resource,UINT subresource,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE *mapped) {
    if(resource==(ID3D11Resource *)s_hw_ps_cb)++pixel_state_maps;
    if(type==D3D11_MAP_READ){D3D11_RESOURCE_DIMENSION dimension;ID3D11Resource_GetType(resource,&dimension);if(dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D){D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource,&desc);if(desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS)++depth_transfer_test_reads;}}
    return ID3D11DeviceContext_Map(ctx,resource,subresource,type,flags,mapped);
}
#undef xbox_GetMemoryOffset
#undef VirtualProtect
#undef xbox_FramebufferWindowPresentHost
static unsigned vsh_decode_calls;
#include "vsh_counted.c"
static unsigned geometry_test_maps,geometry_test_fail;
static HRESULT geometry_map_for_test(ID3D11DeviceContext *ctx,ID3D11Resource *resource,UINT subresource,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE *mapped) {
    ++geometry_test_maps;
    if(geometry_test_fail) {geometry_test_fail=0;return E_OUTOFMEMORY;}
    return ID3D11DeviceContext_Map(ctx,resource,subresource,type,flags,mapped);
}
#include "vsh_geometry_test.c"
extern unsigned combiner_test_maps,combiner_test_fail;

#include <assert.h>
recomp_func_t recomp_lookup(uint32_t va) { (void)va;return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va;return NULL; }
static void method(uint32_t name,uint32_t value) {pgraph_method(nv2a_get_state(),0,name,value);}
static void vertices(float z) {
    z*=16777215.0f;
    struct {float x,y,z,w;uint32_t color;} data[3]={{0,0,z,1,0xffff0000},{8,0,z,1,0xffff0000},{0,8,z,1,0xffff0000}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
}
static void seed(void) {
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;
    ramin[0x100/4]=2;ramin[0x104/4]=0x03ffffff;ramin[0x108/4]=0;
    ramin[0x200/4]=NV_KELVIN_PRIMITIVE;method(0,0x200);
    method(NV097_SET_CONTEXT_DMA_COLOR,0x100);method(NV097_SET_CONTEXT_DMA_ZETA,0x100);
    method(NV097_SET_SURFACE_FORMAT,8|(2<<4));method(NV097_SET_SURFACE_PITCH,32u|(32u<<16));
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x20000);method(NV097_SET_SURFACE_ZETA_OFFSET,0x30000);
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,8<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,8<<16);
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff334455);method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x7f0000a5);
    method(NV097_CLEAR_SURFACE,0xf3);
    method(NV097_SET_CONTEXT_DMA_VERTEX_A,0x100);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET,0x40000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1442);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+12,0x40010);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+12,0x1440);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,7u<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,7u<<16);
    method(NV097_SET_COLOR_MASK,0x01010101);method(NV097_SET_DEPTH_TEST_ENABLE,1);
    method(NV097_SET_DEPTH_FUNC,0x201);method(NV097_SET_DEPTH_MASK,1);
    method(NV097_SET_CLIP_MIN,0);method(NV097_SET_CLIP_MAX,0x4b7fffff);
    method(NV097_SET_COMBINER_CONTROL,1);method(NV097_SET_SHADER_STAGE_PROGRAM,0);
    method(NV097_SET_COMBINER_COLOR_ICW,0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    method(NV097_SET_COMBINER_COLOR_OCW,0xc00);method(NV097_SET_COMBINER_ALPHA_OCW,0xc00);
    method(NV097_SET_COMBINER_SPECULAR_FOG_CW0,0xc);method(NV097_SET_COMBINER_SPECULAR_FOG_CW1,0x1c80);
    const uint32_t program[8]={0,(1u<<21)|0x1b,2u<<26,(15u<<12)|(1u<<11),
                              0,(1u<<21)|(3u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)|1};
    nv2a_vsh_set_instruction(0,program);nv2a_vsh_set_instruction(1,program+4);nv2a_vsh_set_start_slot(0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);vertices(0.75f);
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);
    float constant[4]={0.25f,0.5f,0.75f,1};nv2a_vsh_set_constant(17,constant);
    nv2a_vsh_set_load_slot(4);nv2a_vsh_program_word(0x11223344);
    nv2a_vsh_set_constant_load(19);nv2a_vsh_constant_word(0x3f800000);
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]=0x1234;gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]=0x5678;
    gpu->pgraph.waiting_for_nop=true;gpu->pgraph.regs[NV_PGRAPH_CTX_CACHE1+4]=0x12345678;
    nv2a_mmio_write(gpu,0x6013d4,0x13,1);nv2a_mmio_write(gpu,0x6013d5,0x2a,1);
}
static void verify(void) {
    NV2AState *gpu=nv2a_get_state();
    assert(s_gpu.color_offset==0x20000 && s_gpu.zeta_offset==0x30000 && s_gpu.clear_color==0xff334455);
    assert(gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]==0x1234 && gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]==0x5678);
    assert(gpu->pgraph.waiting_for_nop && gpu->pgraph.regs[NV_PGRAPH_CTX_CACHE1+4]==0x12345678);
    assert(nv2a_mmio_read(gpu,0x6013d4,1)==0x13 && nv2a_mmio_read(gpu,0x6013d5,1)==0x2a);
    const float *constant=nv2a_vsh_constant(17);assert(constant[0]==0.25f && constant[2]==0.75f);
    nv2a_pb_checkpoint_publish();assert(BRIDGE_MEM32(s_gpu.color_base)==0xff334455);
    if(hw_enabled()) {
        FILE *dump=tmpfile();assert(dump && hw_save_depths(dump));rewind(dump);
        uint32_t count;HwDepthCheckpoint header;float depth;uint32_t stencil;
        assert(fread(&count,4,1,dump)==1 && count==1 && fread(&header,sizeof header,1,dump)==1);
        assert(header.address==0x80030000 && header.width==8 && header.height==8);
        assert(fread(&depth,4,1,dump)==1 && fread(&stencil,4,1,dump)==1 && (stencil&255)==0xa5);
        assert(fabsf(depth-(float)0x7f0000/(float)header.normalization)<1e-6f);fclose(dump);
    } else {
        ZetaSurface zeta=zeta_surface();assert(zeta.address==0x80030000);
        assert(*(const uint32_t *)zeta.memory==0x7f0000a5);
    }
}
static void verify_consumers(void) {
    assert(s_gpu.idx_count==3 && s_gpu.prim==5);
    method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    assert(BRIDGE_MEM32(s_gpu.color_base)==0xff334455);
    vertices(0.25f);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);
    method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000);
    nv2a_vsh_constant_word(0x40000000);assert(nv2a_vsh_constant(19)[0]==1 && nv2a_vsh_constant(19)[1]==2);
    nv2a_vsh_program_word(0x55667788);nv2a_vsh_program_word(0x99aabbcc);nv2a_vsh_program_word(0xddeeff00);
    FILE *vsh=tmpfile();assert(vsh && nv2a_vsh_save_checkpoint(vsh));rewind(vsh);
    VshCheckpoint *loaded=nv2a_vsh_prepare_checkpoint(vsh);assert(loaded && loaded->program[4][0]==0x11223344 && loaded->program[4][1]==0x55667788);
    assert(loaded->load_slot==5 && loaded->load_word==0);free(loaded);fclose(vsh);
    if(hw_enabled() && d3d8_RenderScale()>1) {
        HwColor *color=s_hw_colors;assert(color && (color->host_surface || color->native_host));
        unsigned width=color->width*d3d8_RenderScale(),height=color->height*d3d8_RenderScale();
        size_t row=(size_t)width*4,bytes=row*height;unsigned char *expected=malloc(bytes),*actual=malloc(bytes);assert(expected && actual);
        D3DLOCKED_RECT pixels;
        if(color->native_host)assert(nvk_image_read(color->native_host,expected,row));
        else {
            D3D8Surface *surface=(D3D8Surface *)color->host_surface;
            assert(surface->width==width && surface->height==height);
            assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
            for(unsigned y=0;y<height;y++)memcpy(expected+row*y,(unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,row);
            assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
        }
        FILE *snapshot=tmpfile();assert(snapshot && nv2a_gpu_save_checkpoint(snapshot));
        const float changed[4]={0,1,0,1};
        if(color->native_host)assert(nvk_image_clear(color->native_host,changed));
        else ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)color->host_surface)->rtv,changed);
        rewind(snapshot);assert(nv2a_gpu_load_checkpoint(snapshot));fclose(snapshot);
        color=s_hw_colors;
        if(color->native_host)assert(nvk_image_read(color->native_host,actual,row));
        else {
            assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
            for(unsigned y=0;y<height;y++)memcpy(actual+row*y,(unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,row);
            assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
        }
        assert(!memcmp(expected,actual,bytes));free(expected);free(actual);
    }
}
static void verify_swizzled_regions(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x60000);
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,128u<<16);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,256u<<16);
    method(NV097_SET_SURFACE_FORMAT,0x08070224);
    HwColor *color=hw_color(0);assert(color->width==128 && color->height==256 && color->bpp==4);
    size_t bytes=128u*256u*4u;uint32_t *linear=malloc(bytes),*swizzled=malloc(bytes);assert(linear && swizzled);
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++)linear[y*128+x]=0xFF000000u|(x<<16)|(y<<8)|(x^y);
    xbox_swizzle_rect(swizzled,linear,128,256,4);
    uint8_t *memory=color->memory->host_view+color->memory_offset;
    memcpy(memory,swizzled,bytes);hw_upload_native_color(color);
    D3DLOCKED_RECT locked;
    if(color->native_surface){locked.pBits=malloc(bytes);locked.Pitch=128*4;assert(locked.pBits && nvk_image_read(color->native_surface,locked.pBits,locked.Pitch));}
    else hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,D3DLOCK_READONLY),"test native upload pixels");
    for(unsigned y=0;y<256;y++)assert(!memcmp((uint8_t *)locked.pBits+(size_t)y*locked.Pitch,linear+y*128,128u*4));
    if(color->native_surface)free(locked.pBits);
    else hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"test native upload unlock");
    const float rgba[4]={102.0f/255,85.0f/255,68.0f/255,1};
    if(color->native_surface)assert(nvk_image_clear(color->native_host?color->native_host:color->native_surface,rgba));
    else {
        D3D8Surface *surface=(D3D8Surface *)hw_render_surface(color);
        ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),surface->rtv,rgba);
    }
    color->dirty=1;color->dirty_bounds=(RECT){17,129,119,233};hw_copy_color_to_memory(color);
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++) {
        uint32_t at=swizzle_offset(x,y,128,256),expected=x>=17&&x<119&&y>=129&&y<233?0xFF665544u:linear[y*128+x];
        assert(((const uint32_t *)memory)[at]==expected);
    }
    memcpy(memory,swizzled,bytes);hw_upload_native_color(color);
    if(color->native_host)assert(nvk_image_copy(color->native_surface,color->native_host,NULL,0));
    if(color->host_surface)assert(SUCCEEDED(d3d8_surface_resample(color->surface,color->host_surface)));
    const RECT regions[]={{0,0,128,256},{17,129,119,233},{1,3,2,4},{0,3,1,4},{1,3,3,5},{2,3,4,5},{0,255,127,256},{127,0,128,256}};
    for(unsigned region=0;region<sizeof regions/sizeof regions[0];++region) {
        memset(memory,0xa5,bytes);color->dirty=1;color->dirty_bounds=regions[region];hw_copy_color_to_memory(color);
        const RECT *r=&regions[region];
        for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++) {
            uint32_t expected=x>=(unsigned)r->left&&x<(unsigned)r->right&&y>=(unsigned)r->top&&y<(unsigned)r->bottom?linear[y*128+x]:0xa5a5a5a5u;
            assert(((const uint32_t *)memory)[swizzle_offset(x,y,128,256)]==expected);
        }
    }
    method(NV097_SET_SURFACE_CLIP_VERTICAL,128u<<16);
    method(NV097_SET_SURFACE_FORMAT,0x07070224);
    color=hw_color(0);assert(color->height==256);
    if(color->native_surface){locked.pBits=malloc(bytes);locked.Pitch=128*4;assert(locked.pBits && nvk_image_read(color->native_surface,locked.pBits,locked.Pitch));}
    else hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,D3DLOCK_READONLY),"test retained native extent");
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++)
        assert(((const uint32_t *)((const uint8_t *)locked.pBits+(size_t)y*locked.Pitch))[x]==((const uint32_t *)memory)[swizzle_offset(x,y,128,128)]);
    if(color->native_surface)free(locked.pBits);
    else hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"test retained native extent unlock");
    free(linear);free(swizzled);
    fprintf(stderr,"[SWIZZLE REGION] actual native128x256 upload, pitched readback, highY dirty origin and all outside bytes passed\n");
}
static HANDLE fault_observed;
static void verify_clean_texture_overlap(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x60000);
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,128u<<16);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,256u<<16);
    method(NV097_SET_SURFACE_FORMAT,0x08070224);
    HwColor *source_color=hw_color(0);
    uint32_t *memory=(uint32_t *)(source_color->memory->host_view+source_color->memory_offset);
    for(unsigned i=0;i<128u*256u;i++)memory[i]=0xff335577;
    hw_upload_color(source_color,0);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x100000);
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,8u<<16);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);
    method(NV097_SET_SURFACE_FORMAT,8|(2<<4));
    HwColor *unrelated=hw_color(0);
    const float rgba[4]={1,0,0,1};
    if(unrelated->native_surface)assert(nvk_image_clear(unrelated->native_host?unrelated->native_host:unrelated->native_surface,rgba));
    else ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(unrelated))->rtv,rgba);
    unrelated->dirty=1;unrelated->dirty_bounds=(RECT){0,0,8,8};
    s_gpu.texs[0]=(Texture){.offset=source_color->address,.width=128,.height=256,.pitch=512,.color=6,.levels=1,.valid=1};
    s_gpu.rc.stage_program=1;
    uint64_t readbacks=s_gpu.hardware_color_readbacks;
    hw_texture_bind(0,hw_color(0));
    assert(unrelated->dirty && s_gpu.hardware_color_readbacks==readbacks);
    assert(s_hw_textures && !memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    /* An output elsewhere must not publish unrelated rendering just because
     * this clean texture overlaps a retained colour identity. */
    hw_defer_output((uint8_t *)(xbox_GetMemoryOffset()+0x80180000u),0x11223344);
    hw_texture_bind(0,hw_color(0));
    assert(s_hw_query_count==1 && unrelated->dirty && s_gpu.hardware_color_readbacks==readbacks);
    /* The existing physical texture-range query still collects relevant output,
     * including earlier queued outputs, before reading the texture bytes. */
    hw_defer_output((uint8_t *)(xbox_GetMemoryOffset()+source_color->address),0xffabcdef);
    hw_texture_bind(0,hw_color(0));
    assert(!s_hw_query_count && memory[0]==0xffabcdef && unrelated->dirty);
    assert(BRIDGE_MEM32(0x80180000u)==0x11223344);
    assert(s_gpu.hardware_color_readbacks==readbacks);
    assert(!memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    readbacks=s_gpu.hardware_color_readbacks;
    unrelated->dirty=1;
    if(source_color->native_surface)assert(nvk_image_clear(source_color->native_host?source_color->native_host:source_color->native_surface,rgba));
    else ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(source_color))->rtv,rgba);
    source_color->dirty=1;source_color->dirty_bounds=(RECT){0,0,128,256};
    hw_texture_bind(0,hw_color(0));
    assert(!source_color->dirty && !unrelated->dirty && s_gpu.hardware_color_readbacks==readbacks+2);
    assert(memory[0]==0xffff0000 && !memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    fprintf(stderr,"[CLEAN OVERLAP] clean texture avoids unrelated colour readback; dirty texture still publishes original pixels\n");
}
static void verify_host_write_scope(const char *path) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    pg->regs[NV_PGRAPH_CLEARRECTX]=pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff334455);method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x2468ac7b);
    method(NV097_CLEAR_SURFACE,0xf3);
    HwColor *color=hw_color(0);HwDepth *depth=hw_depth();assert(color->dirty && depth->dirty);
    uint64_t reads=s_gpu.hardware_color_readbacks;
    uint32_t *unrelated=(uint32_t *)(xbox_GetMemoryOffset()+0x80180000u);
    hw_defer_output((uint8_t *)unrelated,0x11223344);assert(s_hw_query_count==1);
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);assert(file!=INVALID_HANDLE_VALUE);
    uint32_t word=0xff123456;DWORD written=0;assert(WriteFile(file,&word,4,&written,NULL) && written==4);
    LARGE_INTEGER offset={0};XBOX_IO_STATUS_BLOCK ios={0};
    uint32_t *disjoint=(uint32_t *)(xbox_GetMemoryOffset()+0x80200000u);disjoint[1]=0xaabbccdd;
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,disjoint,8,&offset)==STATUS_SUCCESS);
    assert(ios.Information==4 && disjoint[0]==word && disjoint[1]==0xaabbccdd);
    assert(color->dirty && depth->dirty && s_gpu.hardware_color_readbacks==reads && s_hw_query_count==1);
    assert(xbox_NtReadFile(INVALID_HANDLE_VALUE,NULL,NULL,NULL,&ios,disjoint,8,&offset)<0);
    assert(color->dirty && depth->dirty && s_gpu.hardware_color_readbacks==reads && s_hw_query_count==1);
    /* Query on the destination page but outside the bytes must finish first,
     * including preceding unrelated FIFO entries. */
    uint32_t *c=(uint32_t *)((uint8_t *)color->memory->views.cpu_views[1]+color->memory_offset);
    hw_defer_output((uint8_t *)c+1024,0x55667788);
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,c+1,8,&offset)==STATUS_SUCCESS);
    const uint32_t *cm=(const uint32_t *)(color->memory->host_view+color->memory_offset);
    assert(ios.Information==4 && cm[0]==0xff334455 && cm[1]==word && cm[2]==0xff334455);
    assert(!color->dirty && color->upload_needed && depth->dirty && s_gpu.hardware_color_readbacks==reads+1);
    assert(!s_hw_query_count && *unrelated==0x11223344 && cm[256]==0x55667788);
    uint32_t *d=(uint32_t *)((uint8_t *)depth->memory->views.cpu_views[1]+depth->memory_offset);
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,d+1,8,&offset)==STATUS_SUCCESS);
    const uint32_t *dm=(const uint32_t *)(depth->memory->host_view+depth->memory_offset);
    assert(ios.Information==4 && dm[0]==0x2468ac7b && dm[1]==word && dm[2]==0x2468ac7b);
    assert(!depth->dirty && depth->upload_needed);
    method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x12345659);method(NV097_CLEAR_SURFACE,3);assert(depth->dirty);
    assert(xbox_NtReadFile(INVALID_HANDLE_VALUE,NULL,NULL,NULL,&ios,d+1,8,&offset)<0);
    assert(!depth->dirty && depth->upload_needed && dm[0]==0x12345659 && dm[1]==0x12345659 && dm[2]==0x12345659);
    CloseHandle(file);
    fprintf(stderr,"[HOST WRITE SCOPE] disjoint no publication, relevant FIFO, alias short/failed read preserves color/depth and untouched bytes passed\n");
}

static LONG CALLBACK observe_output_fault(PEXCEPTION_POINTERS e) {
    if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->ExceptionInformation[0]<=1) {
        SetEvent(fault_observed);
        if(nv2a_pb_handle_memory_fault(e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionInformation[0])) return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI write_output(LPVOID address) {*(volatile uint32_t *)address=0x33445566;return *(volatile uint32_t *)address;}
static uint32_t texture_first_pixel(HwTexture *entry) {
    if(entry->native) {
        size_t bytes=(size_t)entry->host_width*entry->host_height*4*(entry->identity.cube?6:1);
        uint32_t *pixels=malloc(bytes);assert(pixels && nvk_image_read(entry->native,pixels,entry->host_width*4));
        uint32_t pixel=pixels[0];free(pixels);return pixel;
    }
    IDirect3DTexture8 *texture=(IDirect3DTexture8 *)entry->texture;
    D3DLOCKED_RECT pixels;hw_check(texture->lpVtbl->LockRect(texture,0,&pixels,NULL,D3DLOCK_READONLY),"texture test read");
    uint32_t pixel=*(const uint32_t *)pixels.pBits;
    hw_check(texture->lpVtbl->UnlockRect(texture,0),"texture test unlock");return pixel;
}
static void verify_texture_content_reuse(const char *snapshot,const char *memory_path) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    fault_observed=CreateEventA(NULL,TRUE,FALSE,NULL);assert(fault_observed);
    void *handler=AddVectoredExceptionHandler(1,observe_output_fault);assert(handler);
    uint32_t *raw=(uint32_t *)(xbox_GetMemoryOffset()+0x80080000u);
    for(unsigned i=0;i<4096;i++) raw[i]=0xff335577;
    Texture source={.offset=0x80080000u,.width=64,.height=64,.pitch=256,.color=6,.levels=1,.valid=1};
    s_gpu.texs[0]=source;s_gpu.rc.stage_program=1;hw_texture_bind(0,hw_color(0));
    HwTexture *entry=s_hw_textures;assert(entry && entry->raw_size==16384 && entry->content_valid);
    texture_compare_left=entry->raw;texture_compare_calls=0;
    uint64_t uploads=s_gpu.hardware_texture_uploads;
    for(unsigned i=0;i<100;i++) hw_texture_bind(0,hw_color(0));
    assert(!texture_compare_calls && s_gpu.hardware_texture_uploads==uploads);
    assert(texture_first_pixel(entry)==0xff335577);
    for(unsigned view=0;view<entry->memory->views.view_count;view++) {
        volatile uint32_t *alias=(volatile uint32_t *)((uint8_t *)entry->memory->views.cpu_views[view]+entry->memory_offset);
        uint32_t expected=0xff446600+view;
        uint32_t *unrelated=(uint32_t *)(xbox_GetMemoryOffset()+0x80180000u);
        hw_defer_output((uint8_t *)unrelated,0x11223300+view);
        assert(s_hw_query_count==1 && s_hw_queries[0].pending);
        alias[0]=expected;alias[1024]=expected+1;
        /* A guarded texture write has no dependency on another physical page.
         * Preserve the existing queued output until an actual consumer needs it. */
        assert(s_hw_query_count==1 && s_hw_queries[0].pending);
        assert(!entry->content_valid);hw_texture_bind(0,hw_color(0));
        assert(entry->content_valid && !memcmp(entry->raw,raw,entry->raw_size));
        assert(texture_first_pixel(entry)==expected);
        assert(s_hw_query_count==1 && s_hw_queries[0].pending);
        /* A write sharing an output page must still collect the original queue
         * before making that page writable, including every earlier output. */
        hw_defer_output((uint8_t *)(alias+2),0xffabcdef);
        hw_defer_output((uint8_t *)alias,0xff654321);
        alias[0]=expected;
        assert(!s_hw_query_count && raw[0]==expected && raw[2]==0xffabcdef);
        assert(*unrelated==0x11223300+view && !entry->content_valid);
        hw_texture_bind(0,hw_color(0));assert(texture_first_pixel(entry)==expected);
    }
    assert(texture_compare_calls>=entry->memory->views.view_count);
    source.width=32;source.pitch=128;s_gpu.texs[0]=source;hw_texture_bind(0,hw_color(0));
    HwTexture *overlap=s_hw_textures;assert(overlap!=entry && overlap->content_valid);
    raw[0]=0xffaabbcc;assert(!entry->content_valid && !overlap->content_valid);
    hw_texture_bind(0,hw_color(0));assert(texture_first_pixel(overlap)==0xffaabbcc);
    source.width=64;source.pitch=256;s_gpu.texs[0]=source;hw_texture_bind(0,hw_color(0));
    assert(texture_first_pixel(entry)==0xffaabbcc);

    HANDLE file=CreateFileA(memory_path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(file!=INVALID_HANDLE_VALUE);
    uint32_t file_pixel=0xff123456;DWORD written=0;
    assert(WriteFile(file,&file_pixel,4,&written,NULL) && written==4);
    LARGE_INTEGER offset={0};XBOX_IO_STATUS_BLOCK ios={0};
    uint32_t *alias=(uint32_t *)((uint8_t *)entry->memory->views.cpu_views[1]+entry->memory_offset);
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,alias,4,&offset)==STATUS_SUCCESS);
    assert(ios.Information==4 && !entry->content_valid && !overlap->content_valid);hw_texture_bind(0,hw_color(0));
    assert(texture_first_pixel(entry)==file_pixel);
    xbox_MmSetAddressProtect(alias,4096,PAGE_READONLY);hw_texture_bind(0,hw_color(0));
    assert(!nv2a_pb_handle_memory_fault((uintptr_t)alias,1));
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,alias,4,&offset)!=STATUS_SUCCESS);
    MEMORY_BASIC_INFORMATION page;assert(VirtualQuery(alias,&page,sizeof page) && page.Protect==PAGE_READONLY);
    xbox_MmSetAddressProtect(alias,4096,PAGE_READWRITE);CloseHandle(file);
    hw_texture_bind(0,hw_color(0));
    hw_defer_output((uint8_t *)raw,0xffabcdef);hw_texture_bind(0,hw_color(0));
    assert(!s_hw_query_count && texture_first_pixel(entry)==0xffabcdef);

    uint32_t *palette=(uint32_t *)(xbox_GetMemoryOffset()+0x80090000u);
    uint32_t *palette2=(uint32_t *)(xbox_GetMemoryOffset()+0x80091000u);
    for(unsigned i=0;i<256;i++){palette[i]=0xff56789a;palette2[i]=0xff987654;}
    memset(raw,0,4096);
    source.color=0x0b;source.pitch=64;source.palette=0x80090000u;s_gpu.texs[0]=source;
    hw_texture_bind(0,hw_color(0));HwTexture *paletted=s_hw_textures;
    assert(texture_first_pixel(paletted)==palette[0]);
    source.palette=0x80091000u;s_gpu.texs[0]=source;hw_texture_bind(0,hw_color(0));
    assert(texture_first_pixel(paletted)==palette2[0]);
    palette2[0]=0xff112233;assert(!paletted->content_valid);hw_texture_bind(0,hw_color(0));
    assert(texture_first_pixel(paletted)==palette2[0]);
    assert(VirtualQuery(palette,&page,sizeof page) && page.Protect==PAGE_READWRITE);
    FILE *saved=fopen(snapshot,"wb");assert(saved && nv2a_gpu_save_checkpoint(saved));fclose(saved);
    saved=fopen(memory_path,"wb");assert(saved && xbox_SavePhysicalMemory(saved));fclose(saved);
    palette2[0]=0xff778899;hw_texture_bind(0,hw_color(0));
    saved=fopen(memory_path,"rb");assert(saved && xbox_LoadPhysicalMemory(saved));fclose(saved);
    saved=fopen(snapshot,"rb");assert(saved && nv2a_gpu_load_checkpoint(saved));fclose(saved);
    assert(!s_hw_textures);hw_texture_bind(0,hw_color(0));
    assert(texture_first_pixel(s_hw_textures)==0xff112233);
    nv2a_pb_release_memory_views();
    assert(VirtualQuery(raw,&page,sizeof page) && page.Protect==PAGE_READWRITE);
    assert(VirtualQuery(palette2,&page,sizeof page) && page.Protect==PAGE_READWRITE);
    RemoveVectoredExceptionHandler(handler);CloseHandle(fault_observed);
    fprintf(stderr,"[TEXTURE REUSE] unchanged binds skip comparison; CPU aliases, shared pages, native reads, guest read-only views, GPU output, palette moves and checkpoint restoration preserve pixels\n");
}
static void verify_vertex_bytes(const unsigned char *expected,UINT bytes,UINT vertex_bytes,UINT expected_stride) {
    if(d3d8_IsNativeVulkan()) {
        assert(bytes==native_stream_bytes && vertex_bytes==native_vertex_bytes && expected_stride==native_vertex_stride);
        unsigned char *actual=malloc(bytes?bytes:1);assert(actual);
        assert(nvk_read_upload(native_vertex_generation,native_vertex_offset,actual,bytes));
        assert(!memcmp(actual,expected,bytes));free(actual);return;
    }
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    ID3D11Buffer *vb=NULL,*ib=NULL;UINT stride,offset,index_offset;DXGI_FORMAT format;
    ID3D11DeviceContext_IAGetVertexBuffers(ctx,0,1,&vb,&stride,&offset);
    ID3D11DeviceContext_IAGetIndexBuffer(ctx,&ib,&format,&index_offset);
    fprintf(stderr,"[VERTEX PAYLOAD] stride=%u bytes=%u index_offset=%u\n",stride,vertex_stream_test_bytes,index_offset-offset);
    assert(stride==expected_stride && vertex_stream_test_bytes==bytes);
    assert(vb==ib && format==DXGI_FORMAT_R32_UINT && index_offset==offset+vertex_bytes);
    D3D11_BUFFER_DESC desc={0};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *staging=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&staging)));
    D3D11_BOX range={offset,0,0,offset+bytes,1,1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx,(ID3D11Resource *)staging,0,0,0,0,(ID3D11Resource *)vb,0,&range);
    D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
    assert(!memcmp(mapped.pData,expected,bytes));
    ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);
    ID3D11Buffer_Release(staging);ID3D11Buffer_Release(vb);ID3D11Buffer_Release(ib);
}
static void verify_vertex_payloads(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+15*4,0x40010);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+15*4,0x1440);
    const uint32_t masks[]={9,0x8001,0xffff,0,1,9,0x1fff,0x8001,9};
    for(unsigned test=0;test<sizeof masks/sizeof masks[0];++test) {
        uint32_t mask=masks[test];int fixed=test==6;
        method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,fixed?0x1432:0x1442);
        if(!fixed) {
            unsigned slot=0;
            for(unsigned attr=0;attr<16;++attr) if(mask&(1u<<attr)) {
                uint32_t word[4]={0,(1u<<21)|(attr<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(attr?3u<<3:0)};
                if(!(mask&(~0u<<(attr+1)))) word[3]|=1;
                nv2a_vsh_set_instruction(slot++,word);
            }
            if(!mask) {
                const uint32_t word[4]={0,(1u<<21)|0x1b,3u<<26,(15u<<12)|(1u<<11)|1};
                nv2a_vsh_set_instruction(0,word);
            }
            nv2a_vsh_set_start_slot(0);assert(nv2a_vsh_inputs_used()==mask);
        }
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,fixed?0:2);
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        unsigned char expected[3*256+24];unsigned bytes=0,count=0;
        for(unsigned attr=0;attr<16;++attr) if(mask&(1u<<attr)) ++count;
        for(unsigned row=0;row<3;++row) for(unsigned attr=0;attr<16;++attr) if(mask&(1u<<attr)) {
            float value[4];fetch_attr(&s_gpu.attr[attr],row,value);memcpy(expected+bytes,value,16);bytes+=16;
        }
        memcpy(expected+bytes,s_hw_program_upload.indices,24);
        fprintf(stderr,"[VERTEX MASK] %04x fixed=%d\n",mask,fixed);
        verify_vertex_bytes(expected,bytes+24,bytes,count*16);
    }
    nv2a_pb_checkpoint_publish();
}

static void draw_reused_vertex_payload(void) {
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,5u<<24);method(NV097_SET_BEGIN_END,0);
    unsigned char expected[240];unsigned bytes=0;
    for(unsigned row=0;row<6;row++) for(unsigned attr=0;attr<16;attr++) if(nv2a_vsh_inputs_used()&(1u<<attr)) {
        float value[4];fetch_attr(&s_gpu.attr[attr],row,value);memcpy(expected+bytes,value,16);bytes+=16;
    }
    assert(bytes==192);memcpy(expected+bytes,s_hw_program_upload.indices,48);
    verify_vertex_bytes(expected,240,192,32);
}
static void verify_vertex_conversion_reuse(const char *snapshot,const char *memory_path) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    uint32_t *raw=(uint32_t *)(xbox_GetMemoryOffset()+0x80040000u);
    memcpy(raw+15,raw,60);
    uint64_t small_hits=s_hw_vertex_cache_hits,small_misses=s_hw_vertex_cache_misses;
    for(unsigned i=0;i<4;i++) {
        method(NV097_SET_BEGIN_END,6);method(NV097_DRAW_ARRAYS,3u<<24);method(NV097_SET_BEGIN_END,0);
        unsigned char expected[176];unsigned bytes=0;
        for(unsigned row=0;row<4;row++) for(unsigned attr=0;attr<16;attr++) if(nv2a_vsh_inputs_used()&(1u<<attr)) {
            float value[4];fetch_attr(&s_gpu.attr[attr],row,value);memcpy(expected+bytes,value,16);bytes+=16;
        }
        assert(bytes==128);memcpy(expected+bytes,s_hw_program_upload.indices,48);
        verify_vertex_bytes(expected,176,128,32);
        MEMORY_BASIC_INFORMATION info;assert(VirtualQuery(raw,&info,sizeof info) && info.Protect==PAGE_READWRITE);
        raw[4]^=0x00332211;
    }
    assert(s_hw_vertex_cache_hits==small_hits && s_hw_vertex_cache_misses==small_misses);
    draw_reused_vertex_payload();
    uint64_t hits=s_hw_vertex_cache_hits;
    for(unsigned i=0;i<4;i++)draw_reused_vertex_payload();
    assert(s_hw_vertex_cache_hits==hits+4);
    uint64_t uploads=s_gpu.hardware_vertex_stream_uploads;
    draw_reused_vertex_payload();assert(s_gpu.hardware_vertex_stream_uploads==uploads);
    if(d3d8_IsNativeVulkan()) {
        for(unsigned i=0;i<5;i++) {
            const float black[4]={0,0,0,1};assert(nvk_image_clear(s_hw_colors->native_surface,black));assert(nvk_submit());
        }
        draw_reused_vertex_payload();assert(s_gpu.hardware_vertex_stream_uploads==uploads);
    } else {
    uint64_t generation=d3d8_vertex_upload_generation();
    void *overwrite=malloc(4*1024*1024);assert(overwrite);memset(overwrite,0xC7,4*1024*1024);
    ID3D11Buffer *ring=NULL;
    assert(d3d8_upload_vertices(overwrite,4*1024*1024,NULL,0,&ring)!=(UINT)-1);free(overwrite);
    assert(d3d8_vertex_upload_generation()!=generation);
    draw_reused_vertex_payload();assert(s_gpu.hardware_vertex_stream_uploads==uploads+1);
    }
    uploads=s_gpu.hardware_vertex_stream_uploads;
    /* Force a fresh upload from a cache hit while the unused staging rows
     * contain different bytes. verify_vertex_bytes reads the actual GPU data. */
    unsigned char untouched[128];memset(untouched,0xA5,sizeof untouched);
    memcpy(s_hw_program_upload.vertex,untouched,sizeof untouched);
    method(NV097_SET_PROVOKING_VERTEX,1);draw_reused_vertex_payload();
    assert(s_gpu.hardware_vertex_stream_uploads==uploads+1);
    assert(!memcmp(s_hw_program_upload.vertex,untouched,sizeof untouched));
    method(NV097_SET_PROVOKING_VERTEX,0);

    XboxSharedMemoryViews views;assert(xbox_GetSharedMemoryViews(raw,60,&views));
    for(unsigned view=0;view<views.view_count;view++) {
        uint32_t *alias=(uint32_t *)((uint8_t *)views.cpu_views[view]+views.offset);
        hits=s_hw_vertex_cache_hits;alias[4]=0xff335500+view;
        draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits);
        draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits+1);
    }
    HANDLE file=CreateFileA(memory_path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(file!=INVALID_HANDLE_VALUE);uint32_t colour=0xff123456;DWORD written;
    assert(WriteFile(file,&colour,4,&written,NULL) && written==4);
    LARGE_INTEGER offset={0};XBOX_IO_STATUS_BLOCK ios={0};
    hits=s_hw_vertex_cache_hits;
    assert(xbox_NtReadFile(file,NULL,NULL,NULL,&ios,raw+4,4,&offset)==STATUS_SUCCESS);CloseHandle(file);
    draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits);
    draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits+1);
    hits=s_hw_vertex_cache_hits;hw_defer_output((uint8_t *)(raw+4),0xffabcdef);
    hw_collect_queries(1);draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits);
    FILE *saved=fopen(snapshot,"wb");assert(saved && nv2a_gpu_save_checkpoint(saved));fclose(saved);
    saved=fopen(memory_path,"wb");assert(saved && xbox_SavePhysicalMemory(saved));fclose(saved);
    raw[4]=0xff556677;draw_reused_vertex_payload();
    saved=fopen(memory_path,"rb");assert(saved && xbox_LoadPhysicalMemory(saved));fclose(saved);
    saved=fopen(snapshot,"rb");assert(saved && nv2a_gpu_load_checkpoint(saved));fclose(saved);
    assert(raw[4]==0xffabcdef);hits=s_hw_vertex_cache_hits;
    draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits);
    draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits+1);
    /* NINE's transition mesh disables diffuse arrays and supplies white
     * through SET_VERTEX_DATA4F; later changes must invalidate packed rows. */
    method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,0x1402);
    for(unsigned pass=0;pass<2;pass++) {
        uint32_t expected=pass?0xff00ff00u:0xffffffffu;
        const float rgba[4]={pass?0:1,1,pass?0:1,1};
        for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&rgba[c],4);method(NV097_SET_VERTEX_DATA4F_M+3*16+c*4,bits);}
        hits=s_hw_vertex_cache_hits;draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits);
        draw_reused_vertex_payload();assert(s_hw_vertex_cache_hits==hits+1);
        nv2a_pb_checkpoint_publish();assert(BRIDGE_MEM32(0x80020000u+(2*8+2)*4)==expected);
    }
    nv2a_pb_release_memory_views();MEMORY_BASIC_INFORMATION page;
    assert(VirtualQuery(raw,&page,sizeof page) && page.Protect==PAGE_READWRITE);
    fprintf(stderr,"[VERTEX REUSE] actual GPU bytes after unchanged draws, both CPU views, native IO, GPU writes and checkpoint restoration\n");
}

static void verify_semaphore_outputs(void) {
    seed();NV2AState *gpu=nv2a_get_state();
    if(hw_enabled()) {
        hw_flush_batch();method(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE,1);vertices(0.25f);
        BRIDGE_MEM32(0x80040010)=0xffff0000;
        BRIDGE_MEM32(0x80040024)=0xff00ff00;
        BRIDGE_MEM32(0x80040038)=0xff0000ff;
        method(NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_FLAT);
        method(NV097_SET_PROVOKING_VERTEX,0);
        uint64_t reused=s_gpu.vertices_reused;
        assert(nv2a_vsh_inputs_used()==((1u<<0)|(1u<<3)));
        method(NV097_SET_BEGIN_END,5);
        method(NV097_ARRAY_ELEMENT16,(1u<<16)|0u);
        method(NV097_ARRAY_ELEMENT16,(0u<<16)|2u);
        method(NV097_ARRAY_ELEMENT16,(2u<<16)|1u);
        method(NV097_SET_BEGIN_END,0);assert(s_gpu.vertices_reused==reused+3);
        assert(s_hw_program_vertex_count==3);
        for(unsigned i=0;i<12;i++) assert(s_hw_program_upload.indices[i]<3);
        unsigned char expected[144];
        for(unsigned row=0;row<3;row++) {
            float attribute[4];fetch_attr(&s_gpu.attr[0],row,attribute);
            memcpy(expected+row*32,attribute,16);
            fetch_attr(&s_gpu.attr[3],row,attribute);memcpy(expected+row*32+16,attribute,16);
        }
        memcpy(expected+96,s_hw_program_upload.indices,48);
        hw_flush_batch();
        verify_vertex_bytes(expected,sizeof expected,96,32);
    }
    uint8_t *output=nv_dma_span(gpu,0x100,0x60ffe,4);
    XboxSharedMemoryViews views;assert(xbox_GetSharedMemoryViews(output,4,&views));
    method(NV097_SET_CONTEXT_DMA_SEMAPHORE,0x100);
    method(NV097_SET_SEMAPHORE_OFFSET,0x60ffe);
    memset(output,0xa5,4);
    uint64_t completions=s_gpu.hardware_gpu_completions;
    uint64_t readbacks=s_gpu.hardware_color_readbacks;
    method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x12345678);
    if(hw_enabled()) {
        MEMORY_BASIC_INFORMATION page;
        assert(VirtualQuery(output,&page,sizeof page) && page.Protect==PAGE_READONLY);
        assert(VirtualQuery(output+3,&page,sizeof page) && page.Protect==PAGE_READONLY);
        assert(ldl_le_p(output)==0xa5a5a5a5);
        HwMemory *memory=hw_memory(&views);
        assert(ldl_le_p(memory->host_view+views.offset)==0xa5a5a5a5);
        assert(s_gpu.hardware_gpu_completions==completions);
        method(NV097_SET_SEMAPHORE_OFFSET,0x62004);
        method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0xabcdef01);
        method(NV097_SET_SEMAPHORE_OFFSET,0x60ffe);
        method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x76543210);
        hw_enter();fault_observed=CreateEventW(NULL,TRUE,FALSE,NULL);assert(fault_observed);
        PVOID handler=AddVectoredExceptionHandler(1,observe_output_fault);assert(handler);
        uint8_t *alias=(uint8_t *)views.cpu_views[1]+views.offset;
        HANDLE reader=CreateThread(NULL,0,write_output,alias,0,NULL);assert(reader);
        assert(WaitForSingleObject(fault_observed,5000)==WAIT_OBJECT_0);
        hw_collect_queries(1);
        assert(ldl_le_p(output)==0x76543210 && ldl_le_p(output+0x1006)==0xabcdef01);
        assert(s_gpu.zpass_count>0);
        assert(s_gpu.hardware_color_readbacks==readbacks);hw_leave();
        assert(WaitForSingleObject(reader,5000)==WAIT_OBJECT_0);
        DWORD value;assert(GetExitCodeThread(reader,&value) && value==0x33445566);
        CloseHandle(reader);RemoveVectoredExceptionHandler(handler);CloseHandle(fault_observed);
        assert(s_gpu.hardware_gpu_completions==completions+3);
        assert(ldl_le_p(output)==0x33445566 && ldl_le_p(output+0x1006)==0xabcdef01);
        *(volatile uint32_t *)alias=0x33445566;assert(ldl_le_p(output)==0x33445566);
        method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x87654321);
        nv2a_pb_checkpoint_publish();assert(!s_hw_query_count && ldl_le_p(output)==0x87654321);
        assert(BRIDGE_MEM32(s_gpu.color_base)==0xff0000ff);
        method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x13572468);
        method(NV097_CLEAR_REPORT_VALUE,0);assert(ldl_le_p(output)==0x13572468 && s_gpu.zpass_count==0);
        method(NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x24681357);
        DWORD deadline=GetTickCount()+5000;
        while(ldl_le_p(output)!=0x24681357 && (LONG)(GetTickCount()-deadline)<0) {nv2a_pb_progress();SwitchToThread();}
        assert(ldl_le_p(output)==0x24681357 && !s_hw_query_count);
    } else assert(ldl_le_p(output)==0x12345678);
    method(NV097_SET_ZPASS_PIXEL_COUNT_ENABLE,0);seed();
    verify();verify_consumers();
}
static void verify_vsh_upload_updates(void) {
    const uint32_t words[4]={0x11223344u,(1u<<21)|(3u<<9)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|1u};
    nv2a_vsh_set_start_slot(5);nv2a_vsh_set_load_slot(5);vsh_decode_calls=0;
    for(unsigned i=0;i<4;i++)nv2a_vsh_program_word(words[i]);
    assert(!vsh_decode_calls);
    assert(nv2a_vsh_inputs_used()==(1u<<3) && vsh_decode_calls==1);
    int length=0;uint32_t used=0;const uint32_t *program=nv2a_vsh_program(&length,&used);
    assert(used==(1u<<3));
    assert(program && length==1 && !memcmp(program,words,sizeof words) && vsh_decode_calls==1);
    float inputs[16][4]={{0}};const float value[4]={1.25f,-2.5f,3.75f,4};memcpy(inputs[3],value,sizeof value);
    Nv2aVshOutput output;assert(nv2a_vsh_run(inputs,&output) && !memcmp(output.pos,value,sizeof value));
    unsigned before=vsh_decode_calls;nv2a_vsh_set_load_slot(5);
    for(unsigned i=0;i<4;i++)nv2a_vsh_program_word(words[i]);
    assert(nv2a_vsh_inputs_used()==(1u<<3) && vsh_decode_calls==before);
    nv2a_vsh_set_load_slot(5);nv2a_vsh_program_word(0xaabbccddu);
    nv2a_vsh_program_word((1u<<21)|(7u<<9)|(1u<<8)|0x1bu);
    assert(nv2a_vsh_inputs_used()==(1u<<7) && vsh_decode_calls==before+1);
    memcpy(inputs[7],value,sizeof value);assert(nv2a_vsh_run(inputs,&output));
    for(unsigned i=0;i<4;i++)assert(output.pos[i]==-value[i]);
    nv2a_vsh_program_word(words[2]);nv2a_vsh_program_word(words[3]);
    nv2a_vsh_set_load_slot(5);nv2a_vsh_program_word(words[0]);
    nv2a_vsh_program_word((1u<<21)|(9u<<9)|0x1bu);
    FILE *file=tmpfile();assert(file && nv2a_vsh_save_checkpoint(file));rewind(file);
    void *saved=nv2a_vsh_prepare_checkpoint(file);assert(saved);fclose(file);
    nv2a_vsh_set_instruction(5,words);nv2a_vsh_apply_checkpoint(saved);free(saved);
    before=vsh_decode_calls;assert(s_load_slot==5 && s_load_word==2);
    assert(nv2a_vsh_inputs_used()==(1u<<9) && vsh_decode_calls==before);
    nv2a_vsh_program_word(words[2]);nv2a_vsh_program_word(words[3]);
    memcpy(inputs[9],value,sizeof value);assert(nv2a_vsh_run(inputs,&output) && !memcmp(output.pos,value,sizeof value));
    nv2a_vsh_set_load_slot(5);nv2a_vsh_program_word(0x55667788u);
    before=vsh_decode_calls;dump_program(5);assert(vsh_decode_calls==before+1);
    nv2a_vsh_set_load_slot(5);nv2a_vsh_program_word(words[0]);
    nv2a_vsh_set_instruction(5,words);before=vsh_decode_calls;
    assert(nv2a_vsh_inputs_used()==(1u<<3) && vsh_decode_calls==before);
    nv2a_vsh_set_load_slot(NV2A_VSH_SLOTS);
    for(unsigned i=0;i<4;i++)nv2a_vsh_program_word(words[i]);
    assert(nv2a_vsh_inputs_used()==(1u<<3) && vsh_decode_calls==before);
    before=vsh_decode_calls;nv2a_vsh_set_load_slot(0);
    for(unsigned i=0;i<4;i++)nv2a_vsh_program_word(words[i]);
    nv2a_vsh_set_load_slot(NV2A_VSH_SLOTS-1);
    for(unsigned i=0;i<4;i++)nv2a_vsh_program_word(words[i]);
    assert(nv2a_vsh_inputs_used()==(1u<<3) && vsh_decode_calls==before+2);
    for(unsigned slot=0;slot<NV2A_VSH_SLOTS;slot++) {
        Nv2aVshInstruction expected;nv2a_vsh_decode_instruction(s_program[slot],&expected);
        assert(!memcmp(&expected,&s_decoded[slot],sizeof expected));
    }
    uint32_t first[4];memcpy(first,words,sizeof first);first[3]&=~1u;
    nv2a_vsh_set_instruction(5,first);nv2a_vsh_set_instruction(6,words);
    nv2a_vsh_set_load_slot(6);nv2a_vsh_program_word(words[0]);
    nv2a_vsh_program_word((1u<<21)|(9u<<9)|0x1bu);
    assert(nv2a_vsh_program(&length,&used) && length==2 && used==((1u<<3)|(1u<<9)));
    assert(used==nv2a_vsh_inputs_used() && nv2a_vsh_program(&length,NULL));
    first[3]=(15u<<12);nv2a_vsh_set_instruction(5,first);nv2a_vsh_set_cxt_write(1);
    length=-1;used=0x12345678;
    assert(!nv2a_vsh_program(&length,&used) && length==-1 && used==0x12345678);
    nv2a_vsh_set_cxt_write(0);nv2a_vsh_set_start_slot(NV2A_VSH_SLOTS-1);
    nv2a_vsh_set_instruction(NV2A_VSH_SLOTS-1,first);
    assert(!nv2a_vsh_program(&length,&used) && length==-1 && used==0x12345678);
    nv2a_vsh_set_start_slot(NV2A_VSH_SLOTS);
    assert(!nv2a_vsh_program(&length,&used) && length==-1 && used==0x12345678);
    fprintf(stderr,"[VSH] four changed upload words use one original decode; partial consumer, raw identity, output, direct setter, dump and pending checkpoint passed\n");
}
static void geometry_driver_bytes(const NV2ANativeGeometry *expected) {
    ID3D11Device *device=d3d8_GetD3D11Device();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    D3D11_BUFFER_DESC desc;ID3D11Buffer_GetDesc(g_vsh_geometry_cb,&desc);
    assert(desc.ByteWidth==sizeof *expected);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;desc.StructureByteStride=0;
    ID3D11Buffer *readback=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&readback)));
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)readback,(ID3D11Resource *)g_vsh_geometry_cb);
    D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)readback,0,D3D11_MAP_READ,0,&mapped)));
    assert(!memcmp(mapped.pData,expected,sizeof *expected));ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)readback,0);ID3D11Buffer_Release(readback);
}
static void verify_geometry_upload_updates(void) {
    hw_initialize();assert(g_vsh_cb && !g_vsh_geometry_cb);
    float constants[192][4]={{0}};NV2ANativeGeometry geometry={0};geometry_test_maps=0;
    assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,0)) && geometry_test_maps==1);geometry_driver_bytes(&geometry);
    for(unsigned i=0;i<64;i++)assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,i%3)));
    assert(geometry_test_maps==1);geometry_driver_bytes(&geometry);
    geometry.screen[0]=8;assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,1)) && geometry_test_maps==2);geometry_driver_bytes(&geometry);
    const uint32_t bits[]={0x80000000u,0x7fc12345u,0x7fc12346u};
    for(unsigned i=0;i<3;i++) {
        memcpy(&geometry.fog[0],&bits[i],4);assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,2)) && geometry_test_maps==3+i);geometry_driver_bytes(&geometry);
    }
    NV2ANativeGeometry previous=geometry;geometry.bounds[1]=32;geometry_test_fail=1;
    assert(bind_native_geometry(constants[0],&geometry,0)==E_OUTOFMEMORY && geometry_test_maps==6);geometry_driver_bytes(&previous);
    assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,0)) && geometry_test_maps==7);geometry_driver_bytes(&geometry);
    assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,1)) && geometry_test_maps==7);
    d3d8_vsh_shutdown();assert(!g_vsh_geometry_cb);assert(SUCCEEDED(d3d8_vsh_init()));
    assert(SUCCEEDED(bind_native_geometry(constants[0],&geometry,2)) && geometry_test_maps==8);geometry_driver_bytes(&geometry);
    fprintf(stderr,"[GEOMETRY] actual buffer bytes, first zero upload,64 equal binds across3kinds, raw -zero/NaN payload changes, failed Map retry and resource recreation passed\n");
}
static void protection_pages_for_test(HwMemory *memory,size_t offset,unsigned pages,DWORD protection) {
    for(unsigned view=0;view<memory->views.view_count;view++)for(unsigned page=0;page<pages;page++) {
        MEMORY_BASIC_INFORMATION region;
        assert(VirtualQuery((uint8_t *)memory->views.cpu_views[view]+offset+page*4096,&region,sizeof region));
        assert(region.Protect==protection);
    }
}
static void verify_colour_protection_work(void) {
    seed();hw_flush_batch();(void)hw_color(0);hw_publish_all();assert(s_hw_colors && !s_hw_colors->dirty);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x200000);
    method(NV097_SET_SURFACE_PITCH,(640u*4)|((640u*4)<<16));
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,640u<<16);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,480u<<16);
    (void)hw_color(0);hw_publish_all();
    protection_test_calls=protection_test_identical=0;
    for(unsigned i=0;i<20;i++)hw_publish_all();
    fprintf(stderr,"[PROTECTION] actual640x480 clean publication20 calls=%u identical=%u\n",protection_test_calls,protection_test_identical);
    protection_test_calls=protection_test_identical=0;
    protection_test_inspect=0;
    LARGE_INTEGER start,finish,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(unsigned i=0;i<500;i++)hw_publish_all();
    QueryPerformanceCounter(&finish);
    fprintf(stderr,"[PROTECTION] clean publication500 calls=%u identical=%u seconds=%.9f\n",protection_test_calls,protection_test_identical,(double)(finish.QuadPart-start.QuadPart)/frequency.QuadPart);
    assert(!protection_test_calls && !protection_test_identical);
    protection_test_inspect=1;
    HwColor *color=s_hw_colors;assert(color->width==640 && color->height==480 && !color->upload_needed);
    hw_mark_cpu_write(color->memory,color->memory_offset+4096,4,NULL);
    assert(!color->dirty && color->upload_needed);
    unsigned previous_calls=protection_test_calls;hw_publish_all();assert(protection_test_calls>previous_calls);
    protection_pages_for_test(color->memory,color->memory_offset,(unsigned)((hw_color_bytes(color)+4095)/4096),PAGE_READWRITE);
    hw_upload_native_color(color);assert(!color->upload_needed);
    const float rgba[4]={0.25f,0.5f,0.75f,1};
    if(d3d8_IsNativeVulkan())assert(nvk_image_clear(hw_vk_render(color),rgba));
    else ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(color))->rtv,rgba);
    color->dirty=1;color->dirty_bounds=(RECT){0,0,640,480};
    uint64_t readbacks=s_gpu.hardware_color_readbacks;previous_calls=protection_test_calls;
    hw_publish_all();assert(!color->dirty && s_gpu.hardware_color_readbacks==readbacks+1 && protection_test_calls>previous_calls);
    protection_pages_for_test(color->memory,color->memory_offset,(unsigned)((hw_color_bytes(color)+4095)/4096),PAGE_READONLY);
    HwMemory *memory=s_hw_colors->memory;size_t offset=0x50000;
    assert(offset+3*4096<memory->views.view_size);
    hw_protect_views(memory,offset,3*4096,PAGE_READWRITE);
    DWORD previous;
    assert(VirtualProtect((uint8_t *)memory->views.cpu_views[memory->views.view_count-1]+offset+4096,4096,PAGE_READONLY,&previous));
    hw_protect_views(memory,offset,3*4096,PAGE_READWRITE);
    protection_pages_for_test(memory,offset,3,PAGE_READWRITE);
    hw_protect_views(memory,offset+17,4096,PAGE_READONLY);
    protection_pages_for_test(memory,offset,2,PAGE_READONLY);
    protection_pages_for_test(memory,offset+2*4096,1,PAGE_READWRITE);
    hw_protect_views(memory,offset+31,4096,PAGE_NOACCESS);
    protection_pages_for_test(memory,offset,2,PAGE_NOACCESS);
    hw_protect_views(memory,offset,3*4096,PAGE_READWRITE);
    protection_pages_for_test(memory,offset,3,PAGE_READWRITE);
    size_t last=memory->views.view_size-4096;
    hw_protect_views(memory,last+1,4095,PAGE_READONLY);
    protection_pages_for_test(memory,last,1,PAGE_READONLY);
    hw_protect_views(memory,last,4096,PAGE_READWRITE);
    protection_pages_for_test(memory,last,1,PAGE_READWRITE);
    fprintf(stderr,"[PROTECTION] clean publication, CPU-dirty full range, GPU-dirty readback, all aliases, mixed-region and partial-page transitions, noaccess and final view boundary passed\n");
}
static void verify_readback_publication(void) {
    seed();method(NV097_SET_BEGIN_END,0);hw_publish_all();
    HwColor *color=s_hw_colors;assert(color && color->host_surface);
    D3D8Surface *host=(D3D8Surface *)color->host_surface,*guest=(D3D8Surface *)color->surface;
    const float rgba[4]={0.25f,0.5f,0.75f,1};
    unsigned char expected_host[4096],expected_guest[256];D3DLOCKED_RECT pixels;
    assert(host->width*host->height*4<=sizeof expected_host && color->pitch*color->height==sizeof expected_guest);
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),host->rtv,rgba);
    color->dirty=1;color->dirty_bounds=(RECT){0,0,8,8};readback_trace_count=0;
    hw_publish_all();
    assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
    for(unsigned y=0;y<host->height;++y)memcpy(expected_host+(size_t)y*host->width*4,(unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,(size_t)host->width*4);
    assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
    memcpy(expected_guest,color->memory->host_view+color->memory_offset,sizeof expected_guest);
    assert(readback_trace_count==4 && readback_trace[0].operation=='C' && readback_trace[1].operation=='M' && readback_trace[2].operation=='C' && readback_trace[3].operation=='M');
    assert(readback_trace[0].resource==(ID3D11Resource *)guest->staging && readback_trace[2].resource==(ID3D11Resource *)host->staging);
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),host->rtv,rgba);
    color->dirty=1;color->dirty_bounds=(RECT){0,0,8,8};readback_trace_count=0;readback_present_bytes=0;
    _putenv_s("RECOMP_FB_WINDOW","1");method(NV097_FLIP_STALL,0);
    assert(readback_trace_count==4 && readback_trace[0].operation=='C' && readback_trace[1].operation=='C' && readback_trace[2].operation=='M' && readback_trace[3].operation=='M');
    assert(readback_trace[0].resource==(ID3D11Resource *)host->staging && readback_trace[1].resource==(ID3D11Resource *)guest->staging);
    assert(readback_trace[2].resource==(ID3D11Resource *)guest->staging && readback_trace[3].resource==(ID3D11Resource *)host->staging);
    assert(readback_present_bytes==host->width*host->height*4 && !memcmp(readback_present_pixels,expected_host,readback_present_bytes));
    assert(!memcmp(color->memory->host_view+color->memory_offset,expected_guest,sizeof expected_guest));
    /* A queued snapshot must map that copy rather than recopy a later draw. */
    assert(SUCCEEDED(d3d8_surface_queue_readback(color->host_surface)) && !host->locked);
    const float green[4]={0,1,0,1};ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),host->rtv,green);
    assert(SUCCEEDED(d3d8_surface_lock_readback(color->host_surface,&pixels)) && host->locked);
    for(unsigned y=0;y<host->height;++y)assert(!memcmp((unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,expected_host+(size_t)y*host->width*4,(size_t)host->width*4));
    assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
    RECT region={2,1,5,4};assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,&region,0)));
    for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x)((uint32_t *)((unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch))[x]=0xff123456;
    assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
    assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
    assert(((uint32_t *)pixels.pBits)[0]==0xff00ff00);
    assert(*(uint32_t *)((unsigned char *)pixels.pBits+pixels.Pitch+8)==0xff123456);
    assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
    fprintf(stderr,"[READBACK] original C-M-C-M becomes C-C-M-M; both pixel outputs identical,queued snapshot and ordinary writable region preserved\n");
}
static void pixel_state_bytes(uint32_t bytes[32]) {
    if(d3d8_IsNativeVulkan()) {assert(native_uniform_sizes[17]==128);memcpy(bytes,native_uniform_bytes[17],128);return;}
    D3D11_BUFFER_DESC desc;ID3D11Buffer_GetDesc(s_hw_ps_cb,&desc);assert(desc.ByteWidth==128);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ID3D11Buffer *staging=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&staging)));
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)s_hw_ps_cb);
    D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
    memcpy(bytes,mapped.pData,128);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);ID3D11Buffer_Release(staging);
}
static void pixel_state_draw(void) {
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
}
static void combiner_bytes(NV2APSConstants *value) {
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11Buffer *buffer=NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(ctx,0,1,&buffer);assert(buffer);
    D3D11_BUFFER_DESC desc;ID3D11Buffer_GetDesc(buffer,&desc);assert(desc.ByteWidth==sizeof *value);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *staging=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&staging)));
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)buffer);
    D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
    memcpy(value,mapped.pData,sizeof *value);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);
    ID3D11Buffer_Release(staging);ID3D11Buffer_Release(buffer);
}
static void verify_combiner_uploads(void) {
    hw_initialize();combiner_test_maps=0;
    const unsigned expected_repeat_maps=0;
    NV2ACombinerState state={0};NV2APSConstants value={0},actual,prepared;
    assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(combiner_test_maps==1);
    for(unsigned i=0;i<64;i++)assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));
    unsigned repeated=combiner_test_maps-1;combiner_bytes(&actual);assert(!memcmp(&value,&actual,sizeof value));assert(repeated==expected_repeat_maps);
    const uint32_t bits[]={0x80000000u,0x7fc00001u,0x7fc00002u};
    for(unsigned i=0;i<3;i++) {memcpy(&value.c0[0][0],&bits[i],4);assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));combiner_bytes(&actual);assert(!memcmp(&value,&actual,sizeof value));}
    unsigned before=combiner_test_maps;value.alpha_ref=.25f;combiner_test_fail=1;
    assert(d3d8_combiners_bind(&state,&value)==E_OUTOFMEMORY);assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(combiner_test_maps==before+2);
    combiner_bytes(&actual);assert(!memcmp(&value,&actual,sizeof value));
    before=combiner_test_maps;value.alpha_only[NV2A_MAX_TEXTURES-1]=1;
    assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(combiner_test_maps==before+1);
    combiner_bytes(&actual);assert(!memcmp(&value,&actual,sizeof value));
    ID3D11Buffer *original=NULL,*other=NULL;ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11DeviceContext_PSGetConstantBuffers(ctx,0,1,&original);
    D3D11_BUFFER_DESC desc={0};desc.ByteWidth=sizeof value;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&other)));
    ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&other);assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));
    ID3D11Buffer *bound=NULL;ID3D11DeviceContext_PSGetConstantBuffers(ctx,0,1,&bound);assert(bound==original);
    ID3D11Buffer_Release(bound);ID3D11Buffer_Release(original);ID3D11Buffer_Release(other);
    d3d8_combiners_set_pixel_shader(1);assert(d3d8_combiners_prepare_draw());combiner_bytes(&prepared);before=combiner_test_maps;
    for(unsigned i=0;i<64;i++)assert(d3d8_combiners_prepare_draw());assert(combiner_test_maps-before==expected_repeat_maps);
    assert(SUCCEEDED(d3d8_GetDevice()->lpVtbl->SetRenderState(d3d8_GetDevice(),D3DRS_FOGCOLOR,0x12345678)));
    assert(d3d8_combiners_prepare_draw());combiner_bytes(&prepared);assert(prepared.fog_color[0]==0x34/255.0f && prepared.fog_color[3]==0x12/255.0f);
    assert(SUCCEEDED(d3d8_GetDevice()->lpVtbl->SetRenderState(d3d8_GetDevice(),D3DRS_FOGENABLE,!prepared.fog_enable)));
    before=combiner_test_maps;combiner_test_fail=1;assert(d3d8_combiners_prepare_draw());
    combiner_bytes(&actual);assert(!memcmp(&actual,&prepared,sizeof actual));
    assert(d3d8_combiners_prepare_draw());assert(combiner_test_maps==before+2);
    combiner_bytes(&actual);assert(actual.fog_enable==!prepared.fog_enable);prepared=actual;
    value=prepared;before=combiner_test_maps;assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(d3d8_combiners_prepare_draw());
    if(!expected_repeat_maps)assert(combiner_test_maps==before);
    value.fog_enable^=1;assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(d3d8_combiners_prepare_draw());combiner_bytes(&actual);assert(!memcmp(&actual,&prepared,sizeof actual));
    d3d8_combiners_shutdown();assert(SUCCEEDED(d3d8_combiners_init()));before=combiner_test_maps;
    assert(SUCCEEDED(d3d8_combiners_bind(&state,&value)));assert(combiner_test_maps==before+1);combiner_bytes(&actual);assert(!memcmp(&actual,&value,sizeof actual));
    fprintf(stderr,"[COMBINER] bytes=%zu equal64_bind_maps=%u;both writers,raw bits,failed retry,rebind and original resource recreation passed\n",sizeof value,repeated);
}
static void verify_pixel_state_uploads(void) {
    pixel_state_maps=0;seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    uint32_t first[32],current[32],initial_value[32];pixel_state_bytes(first);memcpy(initial_value,first,sizeof first);unsigned initial=pixel_state_maps;assert(initial==(d3d8_IsNativeVulkan()?0:1));
    for(unsigned i=0;i<64;++i)pixel_state_draw();
    pixel_state_bytes(current);assert(!memcmp(first,current,sizeof first));unsigned repeated=pixel_state_maps-initial;assert(repeated==0);
    FILE *checkpoint=tmpfile();assert(checkpoint && nv2a_gpu_save_checkpoint(checkpoint));
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,6u<<16);pixel_state_draw();pixel_state_bytes(current);
    for(unsigned i=0;i<8;++i)assert(current[i]==nv2a_get_state()->pgraph.regs[NV_PGRAPH_WINDOWCLIPX0+i*4]);
    assert(!memcmp(first+8,current+8,sizeof first-32));
    const uint32_t depth_bits[3]={0x80000000,0x7fc00001,0x7fc00002};
    for(unsigned i=0;i<3;++i) {
        method(NV097_SET_CLIP_MIN,depth_bits[i]);pixel_state_draw();pixel_state_bytes(current);
        assert(current[16]==nv2a_get_state()->pgraph.regs[NV_PGRAPH_ZCLIPMIN] && current[16]==depth_bits[i]);
        unsigned committed=pixel_state_maps;pixel_state_draw();pixel_state_bytes(first);assert(!memcmp(first,current,sizeof first));assert(pixel_state_maps==committed);
    }
    if(!d3d8_IsNativeVulkan()) {
    D3D11_BUFFER_DESC desc={0};desc.ByteWidth=128;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *other=NULL,*bound=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&other)));
    unsigned before_rebind=pixel_state_maps;
    ID3D11DeviceContext_PSSetConstantBuffers(d3d8_GetD3D11Context(),1,1,&other);pixel_state_draw();assert(pixel_state_maps==before_rebind);
    ID3D11DeviceContext_PSGetConstantBuffers(d3d8_GetD3D11Context(),1,1,&bound);assert(bound==s_hw_ps_cb);ID3D11Buffer_Release(bound);ID3D11Buffer_Release(other);
    }
    pixel_state_bytes(first);assert(!memcmp(first,current,sizeof first));
    rewind(checkpoint);assert(nv2a_gpu_load_checkpoint(checkpoint));fclose(checkpoint);
    unsigned before_restore_draw=pixel_state_maps;
    pixel_state_draw();pixel_state_bytes(current);assert(!memcmp(current,initial_value,sizeof current));assert(pixel_state_maps==before_restore_draw+(d3d8_IsNativeVulkan()?0:1));
    fprintf(stderr,"[PIXEL STATE] initial_maps=%u equal64_maps=%u bytes128_equal;changed_clip,raw_depth_bits,rebind,hot_restore passed\n",initial,repeated);
}
static void verify_method_dispatch_work(void) {
    const uint32_t words[4]={0x3fa00000u,0xc0200000u,0x40700000u,0x40800000u};
    const float expected[4]={1.25f,-2.5f,3.75f,4};
    int (*volatile dispatch)(uint32_t,uint32_t,uint32_t,uint8_t *)=nv2a_pb_exec_method;
    LARGE_INTEGER frequency,start,end;assert(QueryPerformanceFrequency(&frequency));
    for(unsigned round=0;round<5;++round) {
        QueryPerformanceCounter(&start);
        for(unsigned iteration=0;iteration<1000000;++iteration) {
            nv2a_vsh_set_constant_load(17);
            for(unsigned component=0;component<4;++component)
                assert(!dispatch(0,0xb80+component*4,words[component],NULL));
        }
        QueryPerformanceCounter(&end);
        assert(!memcmp(nv2a_vsh_constant(17),expected,sizeof expected));
        fprintf(stderr,"[METHOD WORK] round=%u constant_words=4000000 ns_per_word=%.6f\n",round,
            (end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/4000000);
    }
    NV2AState *gpu=nv2a_get_state();struct PGRAPHState *pg=&gpu->pgraph;
    uint32_t surface_object[4]={NV_CONTEXT_SURFACES_2D,0x11223344,0x55667788,0x99aabbcc};
    uint32_t kelvin_object[4]={NV_KELVIN_PRIMITIVE,0x12345678,0x87654321,0xabcdef01};
    memcpy(gpu->ramin_ptr+0x400,surface_object,sizeof surface_object);
    memcpy(gpu->ramin_ptr+0x500,kelvin_object,sizeof kelvin_object);
    pgraph_method(gpu,3,NV_SET_OBJECT,0x400);
    assert(pg->context_surfaces_2d.object_instance==0x400);
    pgraph_method(gpu,0,NV_SET_OBJECT,0x500);
    assert(pg->context_surfaces_2d.object_instance==0x400);
    assert(pg->regs[NV_PGRAPH_CTX_SWITCH1]==NV_KELVIN_PRIMITIVE && pg->regs[NV_PGRAPH_CTX_SWITCH2]==kelvin_object[1] &&
           pg->regs[NV_PGRAPH_CTX_SWITCH3]==kelvin_object[2] && pg->regs[NV_PGRAPH_CTX_SWITCH4]==kelvin_object[3] && pg->regs[NV_PGRAPH_CTX_SWITCH5]==0x500);
    pgraph_method(gpu,3,NV062_SET_COLOR_FORMAT,0x0a);
    assert(pg->context_surfaces_2d.color_format==0x0a && pg->regs[NV_PGRAPH_CTX_SWITCH1]==NV_CONTEXT_SURFACES_2D &&
           pg->regs[NV_PGRAPH_CTX_SWITCH2]==surface_object[1] && pg->regs[NV_PGRAPH_CTX_SWITCH3]==surface_object[2] &&
           pg->regs[NV_PGRAPH_CTX_SWITCH4]==surface_object[3] && pg->regs[NV_PGRAPH_CTX_SWITCH5]==0x400);
    nv2a_vsh_set_constant_load(17);
    pgraph_method(gpu,0,NV097_SET_TRANSFORM_CONSTANT,words[0]);
    assert(!memcmp(nv2a_vsh_constant(17),expected,4) && pg->regs[NV_PGRAPH_CTX_SWITCH1]==NV_KELVIN_PRIMITIVE &&
           pg->regs[NV_PGRAPH_CTX_SWITCH2]==kelvin_object[1] && pg->regs[NV_PGRAPH_CTX_SWITCH3]==kelvin_object[2] &&
           pg->regs[NV_PGRAPH_CTX_SWITCH4]==kelvin_object[3] && pg->regs[NV_PGRAPH_CTX_SWITCH5]==0x500);
    seed();
    for(unsigned round=0;round<5;++round) {
        QueryPerformanceCounter(&start);
        for(unsigned iteration=0;iteration<1000000;++iteration) {
            nv2a_vsh_set_constant_load(17);
            for(unsigned component=0;component<4;++component)
                pgraph_method(gpu,0,0xb80+component*4,words[component]);
        }
        QueryPerformanceCounter(&end);
        assert(!memcmp(nv2a_vsh_constant(17),expected,sizeof expected));
        fprintf(stderr,"[PGRAPH WORK] round=%u constant_words=4000000 ns_per_word=%.6f\n",round,
            (end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/4000000);
    }
}

static void clear_transfer_pixels(HwColor *color,uint32_t expected) {
    const uint8_t *memory=color->memory->host_view+color->memory_offset;
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        size_t offset=color_pixel_offset(color->format,color->pitch,4,x,y);
        /* Read the actual CPU view first: its original guard publishes GPU pixels. */
        assert(*(const uint32_t *)(xbox_GetMemoryOffset()+color->address+offset)==expected);
        assert(*(const uint32_t *)(memory+offset)==expected);
    }
    if(d3d8_IsNativeVulkan()) {
        NvkImage *surfaces[2]={color->native_surface,color->native_host};
        for(unsigned i=0;i<2;i++)if(surfaces[i]) {
            unsigned width=color->width*(i?d3d8_RenderScale():1),height=color->height*(i?d3d8_RenderScale():1);
            uint32_t *pixels=malloc((size_t)width*height*4);assert(pixels);
            assert(nvk_image_read(surfaces[i],pixels,width*4));
            for(unsigned j=0;j<width*height;j++)assert(pixels[j]==expected);
            free(pixels);
        }
    }
    IDirect3DSurface8 *surfaces[2]={color->surface,color->host_surface};
    for(unsigned i=0;i<2;i++)if(surfaces[i]) {
        D3D8Surface *surface=(D3D8Surface *)surfaces[i];D3DLOCKED_RECT pixels;
        assert(SUCCEEDED(surfaces[i]->lpVtbl->LockRect(surfaces[i],&pixels,NULL,D3DLOCK_READONLY)));
        for(unsigned y=0;y<surface->height;y++)for(unsigned x=0;x<surface->width;x++)
            assert(((const uint32_t *)((const uint8_t *)pixels.pBits+(size_t)y*pixels.Pitch))[x]==expected);
        assert(SUCCEEDED(surfaces[i]->lpVtbl->UnlockRect(surfaces[i])));
    }
    assert(!color->upload_needed && !color->dirty);
    protection_pages_for_test(color->memory,color->memory_offset,1,PAGE_READONLY);
}
static void verify_clear_transfer(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
    unsigned expected=getenv("NINE_CLEAR_TRANSFER_BASELINE")?256:0;
    for(unsigned layout=0;layout<2;layout++)for(unsigned opaque=0;opaque<2;opaque++) {
        unsigned format=opaque?NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
        method(NV097_SET_SURFACE_COLOR_OFFSET,0x200000+0x10000*(layout*2+opaque));
        method(NV097_SET_SURFACE_FORMAT,format|0x20|(layout?0x03030200:0x100));
        uint64_t uploads=s_gpu.hardware_color_uploads;
        HwColor *previous=NULL;uint32_t old_cpu_pixel=0;
        for(unsigned i=0;i<256;i++) {
            if(previous)old_cpu_pixel=*(const uint32_t *)(previous->memory->host_view+previous->memory_offset);
            uint32_t value=(i<<24)|((255-i)<<16)|(((i*73)&255)<<8)|((i*151)&255);
            method(NV097_SET_COLOR_CLEAR_VALUE,value);method(NV097_CLEAR_SURFACE,0xf0);
            HwColor *color=hw_color(0);
            assert(color->dirty && !color->upload_needed);
            if(previous)assert(*(const uint32_t *)(color->memory->host_view+color->memory_offset)==old_cpu_pixel);
            clear_transfer_pixels(color,opaque?value|0xff000000:value);previous=color;
        }
        unsigned actual=(unsigned)(s_gpu.hardware_color_uploads-uploads);assert(actual==expected);
        uint32_t before=s_gpu.clear_color;if(opaque)before|=0xff000000;
        method(NV097_SET_COLOR_CLEAR_VALUE,0x87654321);uploads=s_gpu.hardware_color_uploads;
        method(NV097_CLEAR_SURFACE,0x10);clear_transfer_pixels(hw_color(0),(before&~0x00ff0000)|0x00650000);
        assert(s_gpu.hardware_color_uploads==uploads+1);
        pg->regs[NV_PGRAPH_CLEARRECTX]=(5u<<16)|2;pg->regs[NV_PGRAPH_CLEARRECTY]=(5u<<16)|2;
        uploads=s_gpu.hardware_color_uploads;method(NV097_CLEAR_SURFACE,0xf0);assert(s_gpu.hardware_color_uploads==uploads+1);
        HwColor *color=hw_color(0);const uint8_t *memory=color->memory->host_view+color->memory_offset;
        D3DLOCKED_RECT pixels;uint32_t native_pixels[64];
        if(d3d8_IsNativeVulkan()){assert(nvk_image_read(color->native_surface,native_pixels,8*4));pixels.pBits=native_pixels;pixels.Pitch=8*4;}
        else assert(SUCCEEDED(color->surface->lpVtbl->LockRect(color->surface,&pixels,NULL,D3DLOCK_READONLY)));
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            uint32_t value=x>=2&&x<=5&&y>=2&&y<=5?(opaque?0xff654321:0x87654321):((before&~0x00ff0000)|0x00650000);
            assert(*(const uint32_t *)(memory+color_pixel_offset(color->format,color->pitch,4,x,y))==value);
            assert(((const uint32_t *)((const uint8_t *)pixels.pBits+(size_t)y*pixels.Pitch))[x]==value);
        }
        if(!d3d8_IsNativeVulkan())assert(SUCCEEDED(color->surface->lpVtbl->UnlockRect(color->surface)));
        pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
        if(!layout) {
            method(NV097_SET_COLOR_CLEAR_VALUE,0xff123456);method(NV097_CLEAR_SURFACE,0xf0);
            method(NV097_SET_SURFACE_CLIP_VERTICAL,4u<<16);
            pg->regs[NV_PGRAPH_CLEARRECTY]=3u<<16;
            method(NV097_SET_COLOR_CLEAR_VALUE,0xff654321);method(NV097_CLEAR_SURFACE,0xf0);
            color=hw_color(0);assert(color->height==8);
            uint32_t retained_pixels[64];D3DLOCKED_RECT retained;
            if(color->native_surface){assert(nvk_image_read(color->native_surface,retained_pixels,32));retained.pBits=retained_pixels;retained.Pitch=32;}
            else assert(SUCCEEDED(color->surface->lpVtbl->LockRect(color->surface,&retained,NULL,D3DLOCK_READONLY)));
            for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
                uint32_t expected=y<4?0xff654321:0xff123456;
                assert(BRIDGE_MEM32(color->address+y*color->pitch+x*4)==expected);
                assert(((const uint32_t *)((const uint8_t *)retained.pBits+y*retained.Pitch))[x]==expected);
            }
            if(!color->native_surface)assert(SUCCEEDED(color->surface->lpVtbl->UnlockRect(color->surface)));
            method(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
        }
        fprintf(stderr,"[CLEAR TRANSFER] layout=%u opaque=%u full256_uploads=%u; deferred CPU pixels, aliases, native/scaled GPU bytes, protection, retained rows, partial channels and rectangle passed\n",layout,opaque,actual);
    }
}

static void verify_clear_address_work(void) {
    const uint32_t params[8]={0x10,0x20,0x40,0x80,0x70,0xf0,0,1};
    const uint32_t expected32[8]={0x12a15678,0x1234b278,0x123456c3,0xff345678,0x12a1b2c3,0xffa1b2c3,0x12345678,0x12345678};
    const uint16_t expected16[8]={0xa25a,0x5d9a,0x5a58,0x5a5a,0xa598,0xa598,0x5a5a,0x5a5a};
    const uint32_t formats[3]={8,4,3};
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x100000);
    uint8_t *memory=(uint8_t *)(xbox_GetMemoryOffset()+0x80100000u);
    for(unsigned layout=0;layout<2;++layout)for(unsigned kind=0;kind<3;++kind) {
        unsigned bpp=kind==2?2:4,pitch=32*bpp+(layout?0:16);
        method(NV097_SET_SURFACE_FORMAT,(layout?0x04050220:0x120)|formats[kind]);
        method(NV097_SET_SURFACE_PITCH,pitch|(pitch<<16));
        method(NV097_SET_SURFACE_CLIP_HORIZONTAL,(27u<<16)|2);
        method(NV097_SET_SURFACE_CLIP_VERTICAL,(13u<<16)|1);
        struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
        pg->regs[NV_PGRAPH_CLEARRECTX]=(29u<<16)|5;
        pg->regs[NV_PGRAPH_CLEARRECTY]=(14u<<16)|3;
        method(NV097_SET_COLOR_CLEAR_VALUE,0xffa1b2c3);
        for(unsigned which=0;which<8;++which) {
            memset(memory-16,0xcd,pitch*16+32);
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x) {
                uint8_t *pixel=memory+color_pixel_offset(s_gpu.format,pitch,bpp,x,y);
                if(bpp==4)*(uint32_t *)pixel=0x12345678;else *(uint16_t *)pixel=0x5a5a;
            }
            clear_surface(params[which]);
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x) {
                const uint8_t *pixel=memory+color_pixel_offset(s_gpu.format,pitch,bpp,x,y);
                int inside=x>=5 && x<29 && y>=3 && y<14;
                if(bpp==4)assert(*(const uint32_t *)pixel==(inside?expected32[which]:0x12345678));
                else assert(*(const uint16_t *)pixel==(inside?expected16[which]:0x5a5a));
            }
            for(unsigned i=0;i<16;++i)assert(memory[-16+(int)i]==0xcd);
            if(!layout)for(unsigned y=0;y<16;++y)for(unsigned x=32*bpp;x<pitch;++x)assert(memory[y*pitch+x]==0xcd);
        }
    }
    method(NV097_SET_SURFACE_FORMAT,0x128);method(NV097_SET_SURFACE_PITCH,(640u*4)|((640u*4)<<16));
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,640u<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,480u<<16);
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    pg->regs[NV_PGRAPH_CLEARRECTX]=639u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=479u<<16;
    LARGE_INTEGER frequency,start,end;assert(QueryPerformanceFrequency(&frequency));
    for(unsigned round=0;round<5;++round) {
        QueryPerformanceCounter(&start);
        for(unsigned iteration=0;iteration<100;++iteration)clear_surface(iteration&1?0x10:0xf0);
        QueryPerformanceCounter(&end);
        assert(*(uint32_t *)memory==0xffa1b2c3 && *(uint32_t *)(memory+640*480*4-4)==0xffa1b2c3);
        fprintf(stderr,"[CLEAR WORK] round=%u linear640x480=100 ms=%.6f\n",round,(end.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart);
    }
}
static void verify_completed_output_work(void) {
    seed();hw_flush_batch();hw_collect_queries(1);
    uint8_t *output=(uint8_t *)(xbox_GetMemoryOffset()+0x80068004u);
    hw_initialize();XboxSharedMemoryViews views;
    assert(xbox_GetSharedMemoryViews(output,4,&views));
    HwMemory *memory=hw_memory(&views);
    unsigned calls=0;LARGE_INTEGER frequency,start,end;
    assert(QueryPerformanceFrequency(&frequency));
    hw_complete_commands();assert(d3d8_IsNativeVulkan()?nvk_completed(nvk_fence())==1:s_hw_completion!=NULL);
    event_query_creates=0;
    query_context_getter_calls=0;
    query_status_calls=query_event_payload_calls=0;
    protection_test_inspect=0;protection_test_calls=0;QueryPerformanceCounter(&start);
    for(unsigned i=0;i<100;++i) {
        hw_defer_output(output,i+1);
        hw_collect_queries(1);
        assert(!s_hw_query_count && ldl_le_p(output)==i+1);
        for(unsigned v=0;v<views.view_count;++v) {
            MEMORY_BASIC_INFORMATION region;
            uint8_t *alias=(uint8_t *)views.cpu_views[v]+views.offset;
            assert(VirtualQuery(alias,&region,sizeof region) && region.Protect==PAGE_READONLY);
            assert(ldl_le_p(alias)==i+1);
        }
    }
    QueryPerformanceCounter(&end);calls=protection_test_calls;
    fprintf(stderr,"[OUTPUT WORK] completed100 protection_calls=%u views=%u seconds=%.7f\n",calls,views.view_count,
        (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart);
    fprintf(stderr,"[QUERY PAYLOAD] status_calls=%u event_payload_calls=%u\n",query_status_calls,query_event_payload_calls);
    fprintf(stderr,"[QUERY CONTEXT] getter_calls=%u poll_calls=%u\n",query_context_getter_calls,query_status_calls);
    if(d3d8_IsNativeVulkan()) {
        assert(!query_context_getter_calls && !query_status_calls && !query_event_payload_calls);
        assert(nvk_completed(nvk_fence())==1);
    } else {
        assert(query_context_getter_calls==query_status_calls+200);
        assert(query_status_calls>=100 && query_event_payload_calls==0);
    }
    /* Sequential completions retain the original owned write guard. */
    assert(calls==views.view_count);
    fprintf(stderr,"[EVENT RESOURCE] completed100 creates=%u\n",event_query_creates);
    assert(event_query_creates==0);
    /* Neither a second pending output nor a synchronous barrier may reuse
     * an event whose original queue entry has not retired. */
    protection_test_calls=0;
    hw_defer_output(output,101);hw_defer_output(output+4,102);
    assert(protection_test_calls==0);
    protection_pages_for_test(memory,views.offset&~(size_t)4095,1,PAGE_READONLY);
    assert(s_hw_query_count==2 && s_hw_queries[0].pending && s_hw_queries[1].pending);
    if(d3d8_IsNativeVulkan()) assert(s_hw_queries[0].native_serial==s_hw_queries[1].native_serial);
    else assert(s_hw_queries[0].query!=s_hw_queries[1].query);
    ID3D11Query *first=s_hw_queries[0].query,*second=s_hw_queries[1].query;
    hw_complete_commands();
    if(d3d8_IsNativeVulkan()) assert(nvk_completed(s_hw_queries[1].native_serial)==1);
    else assert(s_hw_completion!=first && s_hw_completion!=second);
    protection_test_calls=0;
    hw_collect_queries(1);assert(!s_hw_query_count && ldl_le_p(output)==101 && ldl_le_p(output+4)==102);
    assert(protection_test_calls==0);
    /* Different and straddling output pages retain their own protection. */
    uint8_t *edge=output-4+4094;
    hw_defer_output(output,103);hw_defer_output(edge,104);
    protection_pages_for_test(memory,views.offset&~(size_t)4095,2,PAGE_READONLY);
    protection_test_calls=0;
    protection_pages_for_test(memory,(views.offset&~(size_t)4095)+8192,1,PAGE_READWRITE);
    hw_protect(memory,(views.offset&~(size_t)4095)+8192,4096,PAGE_READWRITE,NULL);
    assert(protection_test_calls==views.view_count);
    protection_pages_for_test(memory,views.offset&~(size_t)4095,2,PAGE_READONLY);
    hw_collect_queries(1);
    assert(ldl_le_p(output)==103 && ldl_le_p(edge)==104);
    protection_pages_for_test(memory,views.offset&~(size_t)4095,2,PAGE_READONLY);
    protection_pages_for_test(memory,(views.offset&~(size_t)4095)+8192,1,PAGE_READWRITE);
    int host_write_held=nv2a_pb_host_write_begin(output-4,3*4096);
    nv2a_pb_host_write_end(host_write_held);
    protection_pages_for_test(memory,views.offset&~(size_t)4095,3,PAGE_READWRITE);
    assert(d3d8_IsNativeVulkan()?nvk_completed(nvk_fence())==1:s_hw_completion!=NULL);
    protection_test_inspect=1;
}
/* Completion pages retain write ownership until a real writer needs them. */
static void verify_sequential_output_guard_work(void) {
    seed();hw_flush_batch();hw_collect_queries(1);hw_complete_commands();
    NV2AState *gpu=nv2a_get_state();
    uint8_t *output=nv_dma_span(gpu,0x100,0x68004,8192);
    assert(output);XboxSharedMemoryViews views;
    assert(xbox_GetSharedMemoryViews(output,8192,&views));
    HwMemory *memory=hw_memory(&views);
    const unsigned iterations[2]={100,1024};
    for(unsigned round=0;round<2;round++) {
        uint8_t *word=output+round*4096;
        size_t at=views.offset+round*4096;
        uint64_t completions=s_gpu.hardware_gpu_completions;
        protection_test_calls=0;
        for(unsigned i=0;i<iterations[round];i++) {
            hw_defer_output(word,0x12340000u+i);
            assert(s_hw_query_count==1 && s_hw_queries[0].pending);
            hw_collect_queries(1);
            assert(!s_hw_query_count);
            for(unsigned v=0;v<views.view_count;v++) {
                uint8_t *alias=(uint8_t *)views.cpu_views[v]+at;
                MEMORY_BASIC_INFORMATION region;
                assert(VirtualQuery(alias,&region,sizeof region));
                assert(region.Protect==PAGE_READONLY);
                assert(ldl_le_p(alias)==0x12340000u+i);
            }
        }
        assert(s_gpu.hardware_gpu_completions==completions+iterations[round]);
        fprintf(stderr,"[OUTPUT GUARD SEQUENTIAL] n=%u protection_calls=%u views=%u\n",
            iterations[round],protection_test_calls,views.view_count);
        assert(protection_test_calls==views.view_count);
    }
    /* Real guest writes use the already registered original VEH. Each write
     * releases this physical page's owned guard across every CPU alias. */
    for(unsigned writer=0;writer<views.view_count;writer++) {
        volatile uint32_t *alias=(volatile uint32_t *)((uint8_t *)views.cpu_views[writer]+views.offset);
        *alias=0x33445500u+writer;
        for(unsigned v=0;v<views.view_count;v++) {
            uint8_t *a=(uint8_t *)views.cpu_views[v]+views.offset;
            MEMORY_BASIC_INFORMATION region;
            assert(VirtualQuery(a,&region,sizeof region) && region.Protect==PAGE_READWRITE);
            assert(ldl_le_p(a)==0x33445500u+writer);
        }
        protection_test_calls=0;
        hw_defer_output(output,0x55667700u+writer);hw_collect_queries(1);
        assert(!s_hw_query_count && ldl_le_p(output)==0x55667700u+writer);
        assert(protection_test_calls==views.view_count);
    }
    /* A CPU write sharing pending output must collect both entries before
     * becoming writable; the later neighbouring result must not be lost. */
    hw_defer_output(output,0x11223344);hw_defer_output(output+4,0x55667788);
    assert(s_hw_query_count==2 && s_hw_queries[0].pending && s_hw_queries[1].pending);
    *(volatile uint32_t *)output=0xabcdef01;
    assert(!s_hw_query_count && ldl_le_p(output)==0xabcdef01 && ldl_le_p(output+4)==0x55667788);
    /* Host-write lifecycle releases the same retained guard; no raw permission
     * injection and no synthetic success from a substitute handler. */
    hw_defer_output(output,0x22334455);hw_collect_queries(1);
    int held=nv2a_pb_host_write_begin(output,4);
    *(uint32_t *)output=0x66778899;
    nv2a_pb_host_write_end(held);
    assert(ldl_le_p(output)==0x66778899);
    hw_defer_output(output,0x33445566);hw_collect_queries(1);
    /* Public original lifetime teardown must restore all retained pages. */
    nv2a_pb_release_memory_views();
    for(unsigned v=0;v<views.view_count;v++) {
        uint8_t *alias=(uint8_t *)views.cpu_views[v]+views.offset;
        MEMORY_BASIC_INFORMATION region;
        assert(VirtualQuery(alias,&region,sizeof region) && region.Protect==PAGE_READWRITE);
        assert(ldl_le_p(alias)==0x33445566);
    }
    (void)memory;
    fprintf(stderr,"[OUTPUT GUARD LIFETIME] alias writes, rearm, pending order, host-write and release passed\n");
}

static void verify_attribute_offset_work(void) {
    VertexAttr *attr=&s_gpu.attr[0];memset(attr,0,sizeof *attr);float out[4],reference[4];
    const float defaults[4]={0,0,0,1},payload[4]={-1.25f,0.5f,17,2};
    memcpy(s_gpu.imm_attr[0],defaults,sizeof defaults);
    const uint32_t address=0x80070000u;
    unsigned invalid_calls=0,inline_calls=0,array_calls=0;
    int (*volatile read_attribute)(const VertexAttr *,uint32_t,float *)=fetch_attr;
    memcpy((void *)(xbox_GetMemoryOffset()+address),payload,sizeof payload);
    memcpy(s_gpu.inline_buf,payload,sizeof payload);
    attribute_offset_calls=0;
    assert(read_attribute(attr,0,out) && !memcmp(out,defaults,sizeof out));
    attr->size=4;
    assert(!read_attribute(attr,0,out) && !memcmp(out,defaults,sizeof out));
    attr->type=2;attr->stride=16;s_gpu.inline_active=0;
    assert(!read_attribute(attr,0,out) && !memcmp(out,defaults,sizeof out));
    s_gpu.inline_active=1;s_gpu.inline_count=4;
    assert(!read_attribute(attr,1,out) && !memcmp(out,defaults,sizeof out));
    invalid_calls=attribute_offset_calls;
    attribute_offset_calls=0;
    assert(read_attribute(attr,0,out) && !memcmp(out,payload,sizeof out));
    inline_calls=attribute_offset_calls;
    s_gpu.inline_active=0;attr->offset=address;attribute_offset_calls=0;
    assert(read_attribute(attr,0,out) && !memcmp(out,payload,sizeof out));
    array_calls=attribute_offset_calls;
    /* Both storage paths use the production decoder for every supported format
     * and component count, including signed and unknown-format payloads. */
    const uint32_t types[]={0,2,4,1,5,6,7};
    for(unsigned t=0;t<sizeof types/sizeof types[0];++t) {
        for(unsigned size=1;size<=4;++size) {
            attr->type=types[t];attr->size=size;attr->offset=address;s_gpu.inline_active=0;
            int expected=read_attribute(attr,0,reference);
            attr->offset=0;s_gpu.inline_active=1;
            assert(read_attribute(attr,0,out)==expected && !memcmp(out,reference,sizeof out));
        }
    }
    attr->type=2;attr->size=4;attr->offset=0;attr->stride=16;
    LARGE_INTEGER frequency,start,end;assert(QueryPerformanceFrequency(&frequency));
    attribute_offset_calls=0;double checksum=0;
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<1000000;++i) {assert(read_attribute(attr,0,out));checksum+=out[0];}
    QueryPerformanceCounter(&end);
    fprintf(stderr,"[ATTRIBUTE] invalid=%u inline=%u array=%u million_inline_queries=%u seconds=%.7f checksum=%.0f\n",
        invalid_calls,inline_calls,array_calls,attribute_offset_calls,
        (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
    assert(checksum==-1250000 && invalid_calls==0 && inline_calls==0 && array_calls==0 && attribute_offset_calls==0);
    s_gpu.inline_active=0;attr->offset=address;attribute_offset_calls=0;checksum=0;
    assert(g_xbox_mem_offset==xbox_GetMemoryOffset());
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<1000000;++i) {assert(read_attribute(attr,0,out));checksum+=out[0];}
    QueryPerformanceCounter(&end);
    fprintf(stderr,"[ATTRIBUTE BASE] million_array_queries=%u seconds=%.7f checksum=%.0f\n",
        attribute_offset_calls,(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
    assert(checksum==-1250000 && attribute_offset_calls==0);
    const unsigned measured_types[2]={2,0};
    for(unsigned t=0;t<2;++t) {
        attr->type=measured_types[t];
        for(unsigned round=0;round<5;++round) {
            checksum=0;QueryPerformanceCounter(&start);
            for(unsigned i=0;i<4000000;++i) {assert(read_attribute(attr,0,out));checksum+=out[0];}
            QueryPerformanceCounter(&end);
            fprintf(stderr,"[ATTRIBUTE ALIAS] type=%u round=%u seconds=%.7f checksum=%.9f\n",
                attr->type,round,(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
        }
    }
}
static void verify_fixed_texgen_route(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    for(unsigned test=0;test<3;test++) {
        for(unsigned stage=0;stage<4;stage++) {
            for(unsigned j=0;j<4;j++)method(NV097_SET_TEXGEN_S+stage*16+j*4,test==2?0:test?NV097_SET_TEXGEN_S_OBJECT_LINEAR:NV097_SET_TEXGEN_S_EYE_LINEAR);
            method(NV097_SET_TEXTURE_MATRIX_ENABLE+stage*4,test==2);
            for(unsigned row=0;row<4;row++){float value[4]={0};value[row]=1;nv2a_vsh_set_constant(64+stage*8+row,value);nv2a_vsh_set_constant(68+stage*8+row,value);}
        }
        vertices(0.25f);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
        assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000);
        NV2ANativeFixed actual;
        if(d3d8_IsNativeVulkan()) {assert(native_uniform_sizes[3]==sizeof actual);memcpy(&actual,native_uniform_bytes[3],sizeof actual);}
        else {
        assert(g_vsh_fixed_cb);ID3D11Buffer *copy=NULL;D3D11_BUFFER_DESC desc={sizeof actual,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0,0};
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&copy)));ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
        ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)copy,(ID3D11Resource *)g_vsh_fixed_cb);D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)copy,0,D3D11_MAP_READ,0,&mapped)));memcpy(&actual,mapped.pData,sizeof actual);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)copy,0);ID3D11Buffer_Release(copy);
        }
        for(unsigned stage=0;stage<4;stage++){assert(actual.texture_matrix[stage]==(test==2));for(unsigned j=0;j<4;j++)assert(actual.texgen[stage][j]==(test==2?0:test?2:1));}
    }
    fprintf(stderr,"[FIXED TEXGEN] original methods route XYZRHW through fixed vertex consumer; all stage/component and matrix bindings exact; pixels preserved\n");
}


static void verify_normal_reflection_texgen(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t colors[4]={0xff112233,0xff446688,0xff7799aa,0xffbbccdd};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),colors,16);
    for(unsigned row=0;row<24;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    vertices(1.0f/16777215.0f);
    unsigned checks=0;
    for(unsigned stage=0;stage<4;stage++) {
        for(unsigned st=0;st<4;st++){method(NV097_SET_TEXTURE_CONTROL0+st*64,0);for(unsigned j=0;j<4;j++)method(NV097_SET_TEXGEN_S+st*16+j*4,0);method(NV097_SET_TEXTURE_MATRIX_ENABLE+st*4,0);}
        method(NV097_SET_TEXTURE_OFFSET+stage*64,0x80000);method(NV097_SET_TEXTURE_FORMAT+stage*64,(0x12<<8)|(1<<16));
        method(NV097_SET_TEXTURE_CONTROL1+stage*64,8<<16);method(NV097_SET_TEXTURE_IMAGE_RECT+stage*64,(2<<16)|2);
        method(NV097_SET_TEXTURE_CONTROL0+stage*64,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_FILTER+stage*64,0x01010000);method(NV097_SET_TEXTURE_ADDRESS+stage*64,3|(3<<8));
        method(NV097_SET_SHADER_STAGE_PROGRAM,1u<<(stage*5));method(NV097_SET_COMBINER_COLOR_ICW,0x08200000|(stage<<24));method(NV097_SET_COMBINER_ALPHA_ICW,0x18200000|(stage<<24));
        method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+(9+stage)*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+(9+stage)*4,0x1042);
        for(unsigned mapping=0;mapping<3;mapping++)for(unsigned viewer=0;viewer<2;viewer++)for(unsigned norm=0;norm<2;norm++)for(unsigned matrix=0;matrix<2;matrix++)for(unsigned skin=0;skin<2;skin++) {
            float n[4]={.3f,.1f,.1f,0};
            method(NV097_SET_SKIN_MODE,skin);
            float weight=.25f;uint32_t weight_bits;memcpy(&weight_bits,&weight,4);method(NV097_SET_VERTEX_DATA4F_M+16,weight_bits);
            for(unsigned bone=0;bone<2;bone++)for(unsigned row=0;row<4;row++){float value[4]={0};value[row]=row<3?(float)(row+2+bone):1;nv2a_vsh_set_constant(12+bone*8+row,value);}
            for(unsigned j=0;j<4;j++){uint32_t bits;memcpy(&bits,n+j,4);method(NV097_SET_VERTEX_DATA4F_M+2*16+j*4,bits);}
            for(unsigned j=0;j<3;j++)n[j]*=(float)(j+2)+(skin?.75f:0);
            method(NV097_SET_NORMALIZATION_ENABLE,norm);if(norm){float len=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);for(unsigned j=0;j<3;j++)n[j]/=len;}
            float coords[3][4];const float pos[3][3]={{0,0,1},{8,0,1},{0,8,1}};
            for(unsigned v=0;v<3;v++) {
                float u[3]={0,0,1};if(!viewer){float len=sqrtf(pos[v][0]*pos[v][0]+pos[v][1]*pos[v][1]+1);for(unsigned j=0;j<3;j++)u[j]=pos[v][j]/len;}
                float dot=u[0]*n[0]+u[1]*n[1]+u[2]*n[2];for(unsigned j=0;j<3;j++)coords[v][j]=mapping?u[j]-2*dot*n[j]:n[j];coords[v][3]=1;
                if(mapping==2){float z=coords[v][2]+1;float m=2*sqrtf(coords[v][0]*coords[v][0]+coords[v][1]*coords[v][1]+z*z);coords[v][0]=coords[v][0]/m+.5f;coords[v][1]=coords[v][1]/m+.5f;}
                if(matrix){float a=coords[v][0],b=coords[v][1],c=coords[v][2];coords[v][0]=1.25f*a+.5f*c+.375f;coords[v][1]=.75f*b-.5f*c+.875f;}
            }
            float rows[4][4]={{1.25f,0,.5f,.375f},{0,.75f,-.5f,.875f},{0,0,1,0},{0,0,0,1}};
            for(unsigned row=0;row<4;row++)nv2a_vsh_set_constant(68+stage*8+row,rows[row]);
            uint32_t pixels[3][64];
            const uint32_t program[12]={0,(1u<<21)|0x1b,2u<<26,(15u<<12)|(1u<<11),0,(1u<<21)|(3u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3),0,(1u<<21)|((9+stage)<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|((9+stage)<<3)|1};
            for(unsigned ins=0;ins<3;ins++)nv2a_vsh_set_instruction(ins,program+ins*4);
            for(unsigned pass=0;pass<3;pass++) {
                method(NV097_SET_TRANSFORM_EXECUTION_MODE,pass==2?2:0);
                for(unsigned j=0;j<3;j++)method(NV097_SET_TEXGEN_S+stage*16+j*4,pass?((mapping==2 && j==2)?0:mapping==2?0x2402:mapping?0x8512:0x8511):0);
                method(NV097_SET_TEXGEN_VIEW_MODEL,viewer);method(NV097_SET_TEXTURE_MATRIX_ENABLE+stage*4,pass&&matrix);
                memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coords,sizeof coords);
                method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
                memcpy(pixels[pass],(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof pixels[pass]);
            }
            if(memcmp(pixels[0],pixels[1],sizeof pixels[0])){fprintf(stderr,"texgen mismatch stage=%u map=%u viewer=%u norm=%u matrix=%u reference=%08x actual=%08x\n",stage,mapping,viewer,norm,matrix,pixels[0][0],pixels[1][0]);assert(0);}
            assert(!memcmp(pixels[0],pixels[2],sizeof pixels[0]));assert(pixels[0][0]!=0);checks++;
        }
    }
    fprintf(stderr,"[NORMAL REFLECTION TEXGEN] %u original-method cases match independently computed textured outputs\n",checks);
}

static uint32_t *index_fifo_for_test(void) {
    NV2AState *gpu=nv2a_get_state();uint32_t *r=gpu->pfifo.regs;
    gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *object=(uint32_t *)(gpu->ramin_ptr+0x400);
    object[0]=2;object[1]=65535;object[2]=0x80000;
    r[NV_PFIFO_MODE]=1;r[NV_PFIFO_CACHE1_PUSH1]=0x100;r[NV_PFIFO_CACHE1_PUSH0]=1;
    r[NV_PFIFO_CACHE1_DMA_PUSH]=1;r[NV_PFIFO_CACHE1_PULL0]=1;r[NV_PFIFO_CACHE1_DMA_INSTANCE]=0x40;
    r[NV_PFIFO_CACHE1_DMA_GET]=r[NV_PFIFO_CACHE1_DMA_STATE]=0;
    gpu->pgraph.waiting_for_nop=false;gpu->pgraph.regs[NV_PGRAPH_FIFO]=1;
    return (uint32_t *)(gpu->vram_ptr+0x80000);
}


static void verify_m2mf_old_gpu_layout(void) {
    typedef struct Nv2aDiskForTest {uint32_t state_bytes;uint64_t clock;NV2AState state;} Nv2aDiskForTest;
    const size_t old_state_bytes=103000; /* measured from the frozen upstream header */
    assert(sizeof(NV2AState)==old_state_bytes+48);
    FILE *current=tmpfile(),*old=tmpfile();assert(current && old && nv2a_gpu_save_checkpoint(current));
    assert(!_fseeki64(current,0,SEEK_END));size_t bytes=(size_t)_ftelli64(current);rewind(current);
    uint8_t *data=malloc(bytes);assert(data && fread(data,bytes,1,current)==1);fclose(current);
    size_t registers=6+sizeof(PbCheckpoint);assert(!memcmp(data,"XBGPUR",6) && !memcmp(data+registers,"XBNV2A",6));
    size_t disk=registers+6;stl_le_p(data+disk,(uint32_t)old_state_bytes);
    size_t removed=disk+offsetof(Nv2aDiskForTest,state)+offsetof(NV2AState,pgraph.memory_to_memory.offset_in);
    assert(removed+48<bytes);assert(fwrite(data,removed,1,old)==1 && fwrite(data+removed+48,bytes-removed-48,1,old)==1);free(data);rewind(old);
    NV2AState *gpu=nv2a_get_state();gpu->pgraph.pending_interrupts=0x13572468;gpu->pgraph.memory_to_memory.offset_in=0x87654321;
    s_gpu.clear_color=0x89674523;const float current_constant[4]={.125f,.375f,.625f,.875f};nv2a_vsh_set_constant(17,current_constant);
    uint8_t *ram=(uint8_t *)(xbox_GetMemoryOffset()+0x800d0000u);memset(ram,0x5a,64);
    struct PGRAPHState *before=malloc(sizeof *before);assert(before);memcpy(before,&gpu->pgraph,sizeof *before);PbState *pb=malloc(sizeof *pb);assert(pb);memcpy(pb,&s_gpu,sizeof *pb);
    assert(!nv2a_gpu_load_checkpoint(old));fclose(old);
    assert(!memcmp(before,&gpu->pgraph,sizeof *before) && !memcmp(pb,&s_gpu,sizeof *pb));
    assert(!memcmp(nv2a_vsh_constant(17),current_constant,16));for(unsigned i=0;i<64;i++)assert(ram[i]==0x5a);free(before);free(pb);
    fprintf(stderr,"[M2MF OLD GPU LAYOUT] original103000-byte register state inside XBGPUR rejected before register/VSH/pixel/RAM apply\n");
}

typedef struct M2mfObserver {volatile LONG *completion;const uint8_t *destination,*expected,*record;size_t bytes;int observed;} M2mfObserver;
static DWORD WINAPI m2mf_observe(void *opaque) {
    M2mfObserver *o=opaque;DWORD deadline=GetTickCount()+5000;
    while(ReadAcquire(o->completion)!=0){assert((LONG)(GetTickCount()-deadline)<0);SwitchToThread();}
    assert(!memcmp(o->destination,o->expected,o->bytes));assert(ldl_le_p(o->record+8)==0);
    o->observed=1;return 0;
}


static void verify_m2mf_increment_state(const char *mode) {
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr,*words=index_fifo_for_test(),*regs=gpu->pfifo.regs;
    regs[NV_PFIFO_RAMHT]=0x10;ramin[0x600/4]=NV_MEMORY_TO_MEMORY_FORMAT;ramin[0x1070/4]=0xe;ramin[0x1074/4]=NV_RAMHT_STATUS|NV_RAMHT_ENGINE_GRAPHICS|0x60;
    words[0]=(1u<<18)|(1u<<13);words[1]=0xe;
    const unsigned increments[]={1,2,4};unsigned at=2;uint8_t *guard=gpu->vram_ptr+0xd0000;memset(guard,0x6a,64);
    for(unsigned input=0;input<3;input++)for(unsigned output=0;output<3;output++) {
        unsigned format=increments[input]|(increments[output]<<8);
        words[at++]=(1u<<18)|(1u<<13)|0x324;words[at++]=format;regs[NV_PFIFO_CACHE1_DMA_PUT]=at*4;nv2a_fifo_run(gpu);
        assert(gpu->pgraph.memory_to_memory.format==format && (gpu->pgraph.memory_to_memory.valid&512));
        for(unsigned j=0;j<64;j++)assert(guard[j]==0x6a);
        assert(!gpu->pgraph.pending_interrupts && !gpu->pgraph.memory_to_memory.notify_pending);
    }
    if(strcmp(mode,"m2mf-increment-legal")) {
        unsigned bad=!strcmp(mode,"m2mf-increment-zero")?0:!strcmp(mode,"m2mf-increment-three")?0x303:!strcmp(mode,"m2mf-increment-reserved-low")?0x109:!strcmp(mode,"m2mf-increment-reserved-high")?0x10001:0x801;
        words[at++]=(1u<<18)|(1u<<13)|0x324;words[at++]=bad;regs[NV_PFIFO_CACHE1_DMA_PUT]=at*4;nv2a_fifo_run(gpu);assert(0);
    }
    fprintf(stderr,"[M2MF INCREMENT STATE] nine original1/2/4 setup combinations retained; no copy/notification synthesized\n");
}

typedef struct M2mfOrdinaryIrq {NV2AState *gpu;uint8_t *record;unsigned calls;} M2mfOrdinaryIrq;
static void m2mf_ordinary_irq(void *opaque,int level) {
    M2mfOrdinaryIrq *o=opaque;
    if(level){assert(ldl_le_p(o->record+8)==0 && ldl_le_p(o->record+12)==0);assert(o->gpu->pgraph.pending_interrupts&NV_PGRAPH_INTR_NOTIFY);assert(!(o->gpu->pgraph.pending_interrupts&NV_PGRAPH_INTR_BUFFER_NOTIFY));o->calls++;}
}
static void verify_m2mf_ordinary(const char *mode) {
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;
    void (*prior_irq)(void *,int)=gpu->parent_obj.irq_notify;void *prior_opaque=gpu->parent_obj.irq_opaque;
    unsigned trials=!strcmp(mode,"m2mf-ordinary-copy")?2:1;
    for(unsigned awaken=0;awaken<trials;awaken++) {
        uint32_t *words=index_fifo_for_test(),*regs=gpu->pfifo.regs;regs[NV_PFIFO_RAMHT]=0x10;
        ramin[0x600/4]=NV_MEMORY_TO_MEMORY_FORMAT;ramin[0xb00/4]=2;ramin[0xb04/4]=0xff;ramin[0xb08/4]=0xc0000;
        ramin[0x1070/4]=0xe;ramin[0x1074/4]=NV_RAMHT_STATUS|NV_RAMHT_ENGINE_GRAPHICS|0x60;
        ramin[(0x1000+0x92*8)/4]=0x92;ramin[(0x1004+0x92*8)/4]=NV_RAMHT_STATUS|NV_RAMHT_ENGINE_GRAPHICS|0xb0;
        uint8_t *record=gpu->vram_ptr+0xc0000;memset(record,0xcc,16);memset(record+16,0xbb,16);
        M2mfOrdinaryIrq irq={gpu,record,0};gpu->parent_obj.irq_notify=m2mf_ordinary_irq;gpu->parent_obj.irq_opaque=&irq;gpu->parent_obj.irq_level=0;
        gpu->pgraph.pending_interrupts=0;gpu->pgraph.enabled_interrupts=NV_PGRAPH_INTR_NOTIFY;gpu->pmc.enabled_interrupts=1;
        words[0]=(1u<<18)|(1u<<13);words[1]=0xe;
        words[2]=(1u<<18)|(1u<<13)|0x180;words[3]=0x92;
        words[4]=(1u<<18)|(1u<<13)|0x104;words[5]=!strcmp(mode,"m2mf-ordinary-parameter")?2:awaken;
        regs[NV_PFIFO_CACHE1_DMA_PUT]=24;nv2a_fifo_run(gpu);
        assert(gpu->pgraph.memory_to_memory.notify_pending && gpu->pgraph.memory_to_memory.notify_type==awaken);
        for(unsigned i=0;i<16;i++)assert(record[i]==0xcc);assert(irq.calls==0);
        FILE *snapshot=tmpfile();assert(snapshot && nv2a_save_checkpoint(snapshot));rewind(snapshot);void *loaded=nv2a_prepare_checkpoint(snapshot);assert(loaded);gpu->pgraph.memory_to_memory.notify_pending=0;nv2a_apply_checkpoint(loaded);free(loaded);fclose(snapshot);
        assert(gpu->pgraph.memory_to_memory.notify_pending && gpu->pgraph.memory_to_memory.notify_instance==0x600);
        uint32_t method=0x100,param=0,subchannel=1;
        if(!strcmp(mode,"m2mf-ordinary-double"))method=0x104;
        if(!strcmp(mode,"m2mf-ordinary-dma")){method=0x180;param=0x92;}
        if(!strcmp(mode,"m2mf-ordinary-bind")){method=0;param=0xe;}
        if(!strcmp(mode,"m2mf-ordinary-subchannel"))subchannel=2;
        if(!strcmp(mode,"m2mf-ordinary-negative-pitch")){method=0x314;param=0xfffffff0;}
        words[6]=(1u<<18)|(subchannel<<13)|method;words[7]=param;
        uint64_t before=ptimer_get_clock(gpu)<<5;regs[NV_PFIFO_CACHE1_DMA_PUT]=32;nv2a_fifo_run(gpu);uint64_t after=ptimer_get_clock(gpu)<<5;
        uint64_t timestamp=(uint64_t)ldl_le_p(record)|((uint64_t)ldl_le_p(record+4)<<32);
        assert(timestamp>=before && timestamp<=after && !ldl_le_p(record+8) && !ldl_le_p(record+12));
        for(unsigned i=16;i<32;i++)assert(record[i]==0xbb);
        assert(!gpu->pgraph.memory_to_memory.notify_pending && irq.calls==awaken);
        assert(gpu->pgraph.pending_interrupts==(awaken?NV_PGRAPH_INTR_NOTIFY:0));
    }
    gpu->parent_obj.irq_notify=prior_irq;gpu->parent_obj.irq_opaque=prior_opaque;
    fprintf(stderr,"[M2MF ORDINARY] original104 waits next-method, checkpoint retains request, slot0 and IRQ after record match; buffer slot unchanged\n");
}

static void verify_m2mf_copy(const char *mode) {
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;
    unsigned trials=!strcmp(mode,"m2mf-copy")?4:1;
    for(unsigned test=0;test<trials;test++) {
        uint32_t *words=index_fifo_for_test(),*regs=gpu->pfifo.regs;
        ramin[0x600/4]=NV_MEMORY_TO_MEMORY_FORMAT;
        const unsigned handles[]={0xe,0x90,0x91,0x92},instances[]={0x600,0x900,0xa00,0xb00};
        regs[NV_PFIFO_RAMHT]=0x10;
        for(unsigned i=0;i<4;i++){ramin[(0x1000+handles[i]*8)/4]=handles[i];ramin[(0x1004+handles[i]*8)/4]=NV_RAMHT_STATUS|NV_RAMHT_ENGINE_GRAPHICS|(instances[i]>>4);}
        const unsigned frames[]={0xa0000,0xb0000,test==1?0xa0000:0xc0000};
        for(unsigned i=0;i<3;i++){ramin[instances[i+1]/4]=2;ramin[(instances[i+1]+4)/4]=0xffff;ramin[(instances[i+1]+8)/4]=frames[i];}
        unsigned source_offset=test==1?0x10:test==3?0xfff0:0,destination_offset=test==3?0x8000:0;
        unsigned length=test==0?17:test==1?37:16,rows=test==0?3:test==1?2:test==2?2047:1;
        unsigned source_pitch=test==0?32:test==1?64:16,destination_pitch=test==0?40:test==1?64:16;
        uint8_t *source=gpu->vram_ptr+0xa0000+source_offset,*destination=gpu->vram_ptr+0xb0000+destination_offset,*record=gpu->vram_ptr+frames[2]+0x10;
        size_t bytes=(rows-1)*destination_pitch+length;
        memset(gpu->vram_ptr+0xa0000,0x4b,0x10000);memset(gpu->vram_ptr+0xb0000,0xa5,0x10000);memset(gpu->vram_ptr+0xc0000,0x6d,0x100);
        for(unsigned row=0;row<rows;row++)for(unsigned x=0;x<length;x++)source[row*source_pitch+x]=(uint8_t)(row*19+x*7+test);
        memset(record,0xcc,16);((uint32_t *)record)[3]=0x80000000;
        uint8_t guard_before=destination[-1],guard_after=destination[bytes];
        uint8_t *expected=malloc(bytes);assert(expected);memcpy(expected,destination,bytes);
        for(unsigned row=0;row<rows;row++)memcpy(expected+row*destination_pitch,source+row*source_pitch,length);
        words[0]=(1u<<18)|(1u<<13);words[1]=0xe;
        words[2]=(3u<<18)|(1u<<13)|0x180;words[3]=0x92;words[4]=0x90;words[5]=0x91;
        words[6]=(7u<<18)|(1u<<13)|0x30c;
        const uint32_t fields[]={source_offset,destination_offset,source_pitch,destination_pitch,length,rows,!strcmp(mode,"m2mf-format")?0x202:0x101};
        memcpy(words+7,fields,sizeof fields);regs[NV_PFIFO_CACHE1_DMA_PUT]=14*4;
        nv2a_fifo_run(gpu);assert(regs[NV_PFIFO_CACHE1_DMA_GET]==14*4 && ((uint32_t *)record)[3]==0x80000000);
        assert(gpu->pgraph.memory_to_memory.valid==0x3ff && gpu->pgraph.memory_to_memory.dma_in==0x900 && gpu->pgraph.memory_to_memory.dma_out==0xa00);
        FILE *snapshot=tmpfile();assert(snapshot && nv2a_save_checkpoint(snapshot));rewind(snapshot);void *loaded=nv2a_prepare_checkpoint(snapshot);assert(loaded);gpu->pgraph.memory_to_memory.valid=0;gpu->pgraph.memory_to_memory.line_length=0;nv2a_apply_checkpoint(loaded);free(loaded);fclose(snapshot);
        assert(gpu->pgraph.memory_to_memory.valid==0x3ff && gpu->pgraph.memory_to_memory.line_length==length);
        if(!strcmp(mode,"m2mf-overlap")){pgraph_method(gpu,1,0x188,0x900);pgraph_method(gpu,1,0x310,source_offset);}
        words[14]=(1u<<18)|(1u<<13)|0x328;words[15]=!strcmp(mode,"m2mf-awaken")?1:0;
        unsigned interrupts=gpu->pgraph.pending_interrupts;
        uint64_t before=ptimer_get_clock(gpu)<<5;
        M2mfObserver observed={(volatile LONG *)(record+12),destination,expected,record,bytes,0};
        HANDLE observer=CreateThread(NULL,0,m2mf_observe,&observed,0,NULL);assert(observer);
        regs[NV_PFIFO_CACHE1_DMA_PUT]=16*4;nv2a_fifo_run(gpu);
        assert(WaitForSingleObject(observer,5000)==WAIT_OBJECT_0);CloseHandle(observer);assert(observed.observed);
        uint64_t after=ptimer_get_clock(gpu)<<5,timestamp=(uint64_t)ldl_le_p(record)|((uint64_t)ldl_le_p(record+4)<<32);
        assert(timestamp>=before && timestamp<=after && ldl_le_p(record+8)==0 && ldl_le_p(record+12)==0);
        assert(gpu->pgraph.pending_interrupts==interrupts && regs[NV_PFIFO_CACHE1_DMA_GET]==64);
        assert(!memcmp(destination,expected,bytes));assert(destination[-1]==guard_before && destination[bytes]==guard_after);free(expected);
    }
    fprintf(stderr,"[M2MF COPY] RAMHT/class39 DMA contexts, bounded format101 rows, checkpoint state, ordered record publication and PTIMER bytes passed\n");
}

static void index_sequence_for_test(unsigned first,unsigned count) {
    NV2AState *gpu=nv2a_get_state();uint32_t *words=index_fifo_for_test();
    assert(!(count&1) && count<=4094);
    words[0]=NV2A_NATIVE_INDEX_RANGE|((count/2)<<18);
    for(unsigned i=0;i<count;i+=2)words[1+i/2]=((first+i)&0xffff)|(((first+i+1)&0xffff)<<16);
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]=(1+count/2)*4;
    nv2a_fifo_run(gpu);
    assert(gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]==(1+count/2)*4);
}
typedef struct IndexStateForTest {
    uint32_t regs[sizeof(((NV2AState *)0)->pfifo.regs)/4];
    struct PGRAPHState pgraph;
    PbState renderer;
} IndexStateForTest;
static void capture_indices_for_test(IndexStateForTest *out) {
    NV2AState *gpu=nv2a_get_state();
    memcpy(out->regs,gpu->pfifo.regs,sizeof out->regs);
    memcpy(&out->pgraph,&gpu->pgraph,sizeof out->pgraph);
    memcpy(&out->renderer,&s_gpu,sizeof s_gpu);
}
static void restore_indices_for_test(const IndexStateForTest *in) {
    NV2AState *gpu=nv2a_get_state();
    memcpy(gpu->pfifo.regs,in->regs,sizeof in->regs);
    memcpy(&gpu->pgraph,&in->pgraph,sizeof in->pgraph);
    memcpy(&s_gpu,&in->renderer,sizeof s_gpu);
}
static void verify_native_indices(const char *mode) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    NV2AState *gpu=nv2a_get_state();uint32_t *r=gpu->pfifo.regs,*words=index_fifo_for_test();
    if(strcmp(mode,"native-indices")) {
        words[0]=NV2A_NATIVE_INDEX_RANGE|(1u<<18);words[1]=0x10000;
        r[NV_PFIFO_CACHE1_DMA_PUT]=8;
        if(!strcmp(mode,"reject-native-index-extent"))((uint32_t *)(gpu->ramin_ptr+0x400))[1]=3;
        else if(!strcmp(mode,"reject-native-index-put"))r[NV_PFIFO_CACHE1_DMA_PUT]=7;
        else if(!strcmp(mode,"reject-native-index-consumer"))gpu->pfifo.method_consumer=NULL;
        else if(!strcmp(mode,"reject-native-index-class"))gpu->pgraph.regs[NV_PGRAPH_CTX_CACHE1]=NV_IMAGE_BLIT;
        else assert(0);
        nv2a_fifo_run(gpu);assert(!"invalid native index operation accepted");return;
    }
    IndexStateForTest *initial=calloc(1,sizeof *initial),*expected=calloc(1,sizeof *expected);
    IndexStateForTest *actual=calloc(1,sizeof *actual),*pending=calloc(1,sizeof *pending);
    assert(initial&&expected&&actual&&pending);capture_indices_for_test(initial);
    const unsigned counts[]={0,1,2,31,511,2047};unsigned trials=0,cuts=0;
    for(unsigned n=0;n<6;n++)for(unsigned active=0;active<2;active++)for(unsigned wrap=0;wrap<2;wrap++)
    for(unsigned pull=counts[n]?1:0;pull<2;pull++) {
        unsigned count=counts[n],get=wrap?512:0,end=get+(count+1)*4,put=wrap?8:end;
        uint32_t *packet=words+get/4;
        for(unsigned pass=0;pass<2;pass++) {
            restore_indices_for_test(initial);s_gpu.prim=active?5:0;
            s_gpu.idx_count=1;s_gpu.idx[0]=0xabcdef01;
            r[NV_PFIFO_CACHE1_DMA_STATE]=0;r[NV_PFIFO_CACHE1_DMA_GET]=get;r[NV_PFIFO_CACHE1_DMA_PUT]=put;
            r[NV_PFIFO_CACHE1_PULL0]=pull;
            packet[0]=(pass?NV2A_NATIVE_INDEX_RANGE:0x40000000u|NV097_ARRAY_ELEMENT16)|(count<<18);
            for(unsigned i=0;i<count;i++)packet[1+i]=(0x9e3779b9u*i)^0xface1234u;
            if(wrap) {words[end/4]=1;words[0]=0x40000000u|NV097_ARRAY_ELEMENT16;words[1]=0x40000000u|NV097_ARRAY_ELEMENT16;}
            if(pass && active && !wrap) {
                capture_indices_for_test(pending);
                for(unsigned cut=1;cut<=count;cut++) {
                    r[NV_PFIFO_CACHE1_DMA_PUT]=get+cut*4;nv2a_fifo_run(gpu);
                    pending->regs[NV_PFIFO_CACHE1_DMA_PUT]=get+cut*4;capture_indices_for_test(actual);
                    assert(!memcmp(pending,actual,sizeof *actual));++cuts;
                }
                r[NV_PFIFO_CACHE1_DMA_PUT]=put;
                for(unsigned stop=count?0:1;stop<3;stop++) {
                    if(stop==0)r[NV_PFIFO_CACHE1_PULL0]=0;
                    if(stop==1)gpu->pgraph.waiting_for_nop=true;
                    if(stop==2)gpu->irq_in_service=true;
                    nv2a_fifo_run(gpu);assert(r[NV_PFIFO_CACHE1_DMA_GET]==get);
                    r[NV_PFIFO_CACHE1_PULL0]=pull;gpu->pgraph.waiting_for_nop=false;gpu->irq_in_service=false;
                }
            }
            nv2a_fifo_run(gpu);assert(r[NV_PFIFO_CACHE1_DMA_GET]==put);
            assert(s_gpu.idx_count==(active?1+count*2:1));
            method(NV097_ARRAY_ELEMENT32,0x12345678);
            capture_indices_for_test(actual);
            if(!pass)memcpy(expected,actual,sizeof *actual);
            else assert(!memcmp(expected,actual,sizeof *actual));
        }
        ++trials;
    }
    restore_indices_for_test(initial);free(initial);free(expected);free(actual);free(pending);
    fprintf(stderr,"[NATIVE INDICES] %u complete FIFO/PGRAPH/renderer states, %u partial publications, zero count, inactive primitive, wrap, scalar tail and suspended work passed\n",trials,cuts);
}


typedef struct ProgramStateForTest {
    IndexStateForTest base;
    VshCheckpoint vsh;
} ProgramStateForTest;
static void capture_program_for_test(ProgramStateForTest *out) {
    memset(out,0,sizeof *out);capture_indices_for_test(&out->base);
    memcpy(out->vsh.program,s_program,sizeof s_program);memcpy(out->vsh.constants,s_const,sizeof s_const);
    out->vsh.load_slot=s_load_slot;out->vsh.load_word=s_load_word;out->vsh.start_slot=s_start_slot;
    out->vsh.const_load=s_const_load;out->vsh.const_word=s_const_word;out->vsh.cxt_write=s_cxt_write;
}
static void restore_program_for_test(const ProgramStateForTest *in) {
    restore_indices_for_test(&in->base);nv2a_vsh_apply_checkpoint(&in->vsh);
}
static void verify_native_program(const char *mode) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    NV2AState *gpu=nv2a_get_state();uint32_t *r=gpu->pfifo.regs,*words=index_fifo_for_test();
    if(strcmp(mode,"native-program")) {
        words[0]=NV2A_NATIVE_PROGRAM_RESOURCE|(5u<<18);words[1]=0;
        words[2]=(4u<<18)|NV097_SET_TRANSFORM_PROGRAM;
        for(unsigned i=0;i<4;i++)words[3+i]=0;
        r[NV_PFIFO_CACHE1_DMA_PUT]=28;
        if(!strcmp(mode,"reject-native-program-extent"))((uint32_t *)(gpu->ramin_ptr+0x400))[1]=15;
        else if(!strcmp(mode,"reject-native-program-put"))r[NV_PFIFO_CACHE1_DMA_PUT]=27;
        else if(!strcmp(mode,"reject-native-program-consumer"))gpu->pfifo.method_consumer=NULL;
        else if(!strcmp(mode,"reject-native-program-class"))gpu->pgraph.regs[NV_PGRAPH_CTX_CACHE1]=NV_IMAGE_BLIT;
        else if(!strcmp(mode,"reject-native-program-block-count"))words[2]=(33u<<18)|NV097_SET_TRANSFORM_PROGRAM;
        else if(!strcmp(mode,"reject-native-program-block-method"))words[2]=(4u<<18)|NV097_SET_TRANSFORM_CONSTANT;
        else if(!strcmp(mode,"reject-native-program-block-extent"))words[2]=(5u<<18)|NV097_SET_TRANSFORM_PROGRAM;
        else assert(0);
        nv2a_fifo_run(gpu);assert(!"invalid native program resource accepted");return;
    }
    ProgramStateForTest *initial=calloc(1,sizeof *initial),*expected=calloc(1,sizeof *expected);
    ProgramStateForTest *actual=calloc(1,sizeof *actual),*pending=calloc(1,sizeof *pending);
    assert(initial&&expected&&actual&&pending);capture_program_for_test(initial);
    const unsigned counts[]={0,1,3,4,31,32,33,65,768};
    const uint32_t loads[]={0,1,NV2A_VSH_SLOTS-1,NV2A_VSH_SLOTS,UINT32_MAX};
    unsigned cases=0,cuts=0;
    for(unsigned n=0;n<9;n++)for(unsigned load=0;load<5;load++)for(unsigned wrap=0;wrap<2;wrap++) {
        unsigned count=counts[n],get=wrap?4096:0,encoded=count+(count+31)/32;
        unsigned end=get+(encoded+2)*4,put=wrap?8:end;uint32_t *packet=words+get/4;
        for(unsigned pass=0;pass<2;pass++) {
            restore_program_for_test(initial);
            r[NV_PFIFO_CACHE1_DMA_GET]=get;r[NV_PFIFO_CACHE1_DMA_PUT]=put;
            packet[0]=pass?NV2A_NATIVE_PROGRAM_RESOURCE|(encoded<<18):(1u<<18)|NV097_SET_TRANSFORM_PROGRAM_LOAD;
            packet[1]=loads[load];unsigned at=2;
            for(unsigned i=0;i<count;) {
                unsigned block=count-i>32?32:count-i;packet[at++]=(block<<18)|NV097_SET_TRANSFORM_PROGRAM;
                for(unsigned j=0;j<block;j++,i++)packet[at++]=0x9e3779b9u*i^0x12345678u;
            }
            assert(at==encoded+2);
            if(wrap){words[end/4]=1;words[0]=0x40100;words[1]=0;}
            if(pass && !wrap) {
                capture_program_for_test(pending);
                for(unsigned cut=1;cut<encoded+2;cut++) {
                    r[NV_PFIFO_CACHE1_DMA_PUT]=cut*4;nv2a_fifo_run(gpu);
                    pending->base.regs[NV_PFIFO_CACHE1_DMA_PUT]=cut*4;capture_program_for_test(actual);
                    assert(!memcmp(pending,actual,sizeof *actual));++cuts;
                }
                r[NV_PFIFO_CACHE1_DMA_PUT]=put;
                for(unsigned stop=0;stop<3;stop++) {
                    if(stop==0)r[NV_PFIFO_CACHE1_PULL0]=0;
                    if(stop==1)gpu->pgraph.waiting_for_nop=true;
                    if(stop==2)gpu->irq_in_service=true;
                    nv2a_fifo_run(gpu);assert(r[NV_PFIFO_CACHE1_DMA_GET]==get);
                    r[NV_PFIFO_CACHE1_PULL0]=1;gpu->pgraph.waiting_for_nop=false;gpu->irq_in_service=false;
                }
            }
            nv2a_fifo_run(gpu);assert(r[NV_PFIFO_CACHE1_DMA_GET]==put);
            for(unsigned tail=0;tail<5;tail++)nv2a_vsh_program_word(0x11223344u+tail);
            capture_program_for_test(actual);
            if(!pass)memcpy(expected,actual,sizeof *actual);
            else assert(!memcmp(expected,actual,sizeof *actual));
        }
        ++cases;
    }
    restore_program_for_test(initial);free(initial);free(expected);free(actual);free(pending);
    fprintf(stderr,"[NATIVE PROGRAM] %u complete FIFO/PGRAPH/renderer/program states and %u partial publications, wrap, boundary cursors, scalar tails and suspension passed\n",cases,cuts);
}

static void verify_batch_boundary(const char *producer) {
    seed();method(NV097_SET_BEGIN_END,5);
    if(!strcmp(producer,"inline")) {
        for(unsigned i=0;i<NV2A_MAX_BATCH_LENGTH;i++)method(NV097_INLINE_ARRAY,i);
        assert(s_gpu.inline_count==NV2A_MAX_BATCH_LENGTH);
        assert(s_gpu.inline_buf[NV2A_MAX_BATCH_LENGTH-1]==NV2A_MAX_BATCH_LENGTH-1);
        fprintf(stderr,"[BATCH BOUNDARY] full inline payload retained\n");
        method(NV097_INLINE_ARRAY,0);
    } else if(!strcmp(producer,"immediate")) {
        for(unsigned i=0;i<NV2A_MAX_BATCH_LENGTH/64;i++)for(unsigned j=0;j<4;j++)method(NV097_SET_VERTEX4F+j*4,0);
        assert(s_gpu.imm_count==NV2A_MAX_BATCH_LENGTH/64);
        fprintf(stderr,"[BATCH BOUNDARY] full immediate payload retained\n");
        method(NV097_SET_VERTEX4F+12,0);
    } else {
        unsigned i=0;
        if(!strcmp(producer,"native16")) {
            for(;i+1023<NV2A_MAX_BATCH_LENGTH;i+=1024)index_sequence_for_test(i,1024);
        } else if(!strcmp(producer,"array16")) {
            for(;i+1<NV2A_MAX_BATCH_LENGTH;i+=2)method(NV097_ARRAY_ELEMENT16,(i&0xffff)|(((i+1)&0xffff)<<16));
        } else if(!strcmp(producer,"draw")) {
            for(;i+255<NV2A_MAX_BATCH_LENGTH;i+=256)method(NV097_DRAW_ARRAYS,i|(255u<<24));
        }
        for(;i<NV2A_MAX_BATCH_LENGTH;i++)method(NV097_ARRAY_ELEMENT32,i);
        assert(s_gpu.idx_count==NV2A_MAX_BATCH_LENGTH && s_gpu.idx[s_gpu.idx_count-1]==NV2A_MAX_BATCH_LENGTH-1);
        fprintf(stderr,"[BATCH BOUNDARY] full index payload retained\n");
        if(!strcmp(producer,"native16"))index_sequence_for_test(0,2);
        method(!strcmp(producer,"array16")?NV097_ARRAY_ELEMENT16:!strcmp(producer,"draw")?NV097_DRAW_ARRAYS:NV097_ARRAY_ELEMENT32,0);
    }
    assert(!"over-capacity producer must report its bound");
}
static void verify_large_batch(const char *producer) {
    enum { count=6000 };
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    struct Vertex {float position[4];uint32_t color;} *data=(void *)(xbox_GetMemoryOffset()+0x80040000u);
    for(unsigned i=0;i<count;i++) {
        data[i]=(struct Vertex){{i%3==1?8.0f:0.0f,i%3==2?8.0f:0.0f,1,1},0xffff0000};
    }
    for(unsigned primitive=5;primitive<=10;primitive++) {
        unsigned before=s_gpu.verts;
        method(NV097_SET_PROVOKING_VERTEX,1);
        method(NV097_SET_BEGIN_END,primitive);
        if(!strcmp(producer,"array32")) {
            for(unsigned i=0;i<count;i++)method(NV097_ARRAY_ELEMENT32,i);
        } else if(!strcmp(producer,"native16")) {
            for(unsigned i=0;i<count;i+=1000)index_sequence_for_test(i,1000);
        } else if(!strcmp(producer,"array16")) {
            for(unsigned i=0;i<count;i+=2)method(NV097_ARRAY_ELEMENT16,i|((i+1)<<16));
        } else if(!strcmp(producer,"draw")) {
            for(unsigned i=0;i<count;i+=200)method(NV097_DRAW_ARRAYS,i|(199u<<24));
        } else if(!strcmp(producer,"inline")) {
            for(unsigned i=0;i<count;i++)for(unsigned word=0;word<5;word++)method(NV097_INLINE_ARRAY,((uint32_t *)&data[i])[word]);
            assert(s_gpu.inline_count==count*5);
        } else {
            method(NV097_SET_VERTEX_DATA4UB+3*4,0xff0000ff);
            for(unsigned i=0;i<count;i++)for(unsigned word=0;word<4;word++)method(NV097_SET_VERTEX4F+word*4,((uint32_t *)&data[i])[word]);
            assert(s_gpu.imm_count==count);
        }
        if(strcmp(producer,"inline") && strcmp(producer,"immediate")) {
            assert(s_gpu.idx_count==count);
            for(unsigned i=0;i<count;i++)assert(s_gpu.idx[i]==i);
        }
        method(NV097_SET_BEGIN_END,0);
        assert(s_gpu.verts==before+count && s_hw_program_vertex_count==count);
        unsigned char *expected=malloc(count*(32+24));assert(expected);
        for(unsigned i=0;i<count;i++) {
            memcpy(expected+i*32,data[i].position,16);
            const float red[4]={1,0,0,1};memcpy(expected+i*32+16,red,16);
        }
        uint32_t *indices=(uint32_t *)(expected+count*32);unsigned used=0;
        for(unsigned i=0;i<count;i++) {
            unsigned a,b,c,p;
            if(primitive==5) {if(i%3 || i+2>=count)continue;a=i;b=i+1;c=i+2;p=i;}
            else if(primitive==6) {if(i+2>=count)continue;a=i+(i&1);b=i+1-(i&1);c=i+2;p=i;}
            else if(primitive==7 || primitive==10) {if(!i || i+1>=count)continue;a=0;b=i;c=i+1;p=primitive==10?i+1:0;}
            else {
                unsigned step=primitive==8?4:2;if(i%step || i+3>=count)continue;
                a=i;b=i+1;c=primitive==8?i+2:i+3;p=i+3;
                indices[used++]=a;indices[used++]=p;indices[used++]=b;indices[used++]=p;indices[used++]=c;indices[used++]=p;
                b=c;c=primitive==8?i+3:i+2;
            }
            indices[used++]=a;indices[used++]=p;indices[used++]=b;indices[used++]=p;indices[used++]=c;indices[used++]=p;
        }
        verify_vertex_bytes(expected,count*32+used*4,count*32,32);free(expected);
        nv2a_pb_checkpoint_publish();
        assert(BRIDGE_MEM32(0x80020000u)==0xffff0000);
    }
    fprintf(stderr,"[LARGE BATCH] %s: 6000 vertices, six topologies, complete native payload and pixels passed\n",producer);
}
static void verify_fifo_worker_checkpoint(int partial,int indices) {
    NV2AState *gpu=nv2a_get_state();
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    /* The original checkpoint barrier holds the real worker before it can
     * consume a posted request. Use an actual deferred output, not a fake
     * query count, to exercise repeated main-thread progress requests. */
    assert(!ReadAcquire(&gpu->pfifo.fifo_kick) && !ReadAcquire64(&s_hw_query_count));
    hw_defer_output((uint8_t *)XBOX_TO_NATIVE(0x80070000u),0);
    assert(ReadAcquire64(&s_hw_query_count)==1);
    unsigned kicks=progress_kick_calls;
    nv2a_pb_request_progress();
    assert(progress_kick_calls==kicks+1 && ReadAcquire(&gpu->pfifo.fifo_kick)==1);
    for(unsigned repeat=0;repeat<4096;repeat++)nv2a_pb_request_progress();
    fprintf(stderr,"[PROGRESS REQUEST] repeated4096 extra_kicks=%u\n",progress_kick_calls-kicks-1);
    assert(progress_kick_calls==kicks+1 && ReadAcquire64(&s_hw_query_count)==1);
    hw_collect_queries(1);
    assert(!ReadAcquire64(&s_hw_query_count) && BRIDGE_MEM32(0x80070000u)==0);
    nv2a_pb_request_progress();assert(progress_kick_calls==kicks+1);
    gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *object=(uint32_t *)(gpu->ramin_ptr+0x400);
    object[0]=2;object[1]=4095;object[2]=0x80000;
    uint32_t *commands=(uint32_t *)(gpu->vram_ptr+0x80000);
    commands[0]=NV2A_NATIVE_CONSTANT_RANGE;commands[1]=17;commands[2]=16;
    for(unsigned i=0;i<16;i++)commands[3+i]=0x3f000000u+i;
    const float zero[4]={0};for(unsigned i=17;i<21;i++)nv2a_vsh_set_constant(i,zero);
    commands[19]=(1u<<18)|NV097_SET_COLOR_CLEAR_VALUE;commands[20]=0xff445566;
    commands[21]=(1u<<18)|NV097_CLEAR_SURFACE;commands[22]=0xf0;
    commands[23]=(1u<<18)|NV097_SET_SEMAPHORE_OFFSET;commands[24]=0x70000;
    commands[25]=(1u<<18)|NV097_BACK_END_WRITE_SEMAPHORE_RELEASE;commands[26]=0x12345678;
    unsigned end=108,pending_get=0,partial_put=8;
    if(indices) {
        method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);vertices(0.25f);
        commands[23]=(1u<<18)|NV097_SET_BEGIN_END;commands[24]=5;
        commands[25]=NV2A_NATIVE_INDEX_RANGE|(1u<<18);commands[26]=0x10000;
        commands[27]=(1u<<18)|NV097_ARRAY_ELEMENT32;commands[28]=2;
        commands[29]=(1u<<18)|NV097_SET_BEGIN_END;commands[30]=0;
        commands[31]=(1u<<18)|NV097_SET_SEMAPHORE_OFFSET;commands[32]=0x70000;
        commands[33]=(1u<<18)|NV097_BACK_END_WRITE_SEMAPHORE_RELEASE;commands[34]=0x12345678;
        end=140;pending_get=100;partial_put=104;
    }
    if(indices==2) {
        uint32_t original_program[8];memcpy(original_program,s_program,sizeof original_program);
        uint32_t invalid[4]={0};nv2a_vsh_set_instruction(0,invalid);
        commands[23]=NV2A_NATIVE_PROGRAM_RESOURCE|(9u<<18);commands[24]=0;
        commands[25]=(8u<<18)|NV097_SET_TRANSFORM_PROGRAM;
        memcpy(commands+26,original_program,sizeof original_program);
        commands[34]=(1u<<18)|NV097_SET_BEGIN_END;commands[35]=5;
        commands[36]=NV2A_NATIVE_INDEX_RANGE|(1u<<18);commands[37]=0x10000;
        commands[38]=(1u<<18)|NV097_ARRAY_ELEMENT32;commands[39]=2;
        commands[40]=(1u<<18)|NV097_SET_BEGIN_END;commands[41]=0;
        commands[42]=(1u<<18)|NV097_SET_SEMAPHORE_OFFSET;commands[43]=0x70000;
        commands[44]=(1u<<18)|NV097_BACK_END_WRITE_SEMAPHORE_RELEASE;commands[45]=0x12345678;
        end=184;pending_get=92;partial_put=100;
    }
    method(NV097_SET_CONTEXT_DMA_SEMAPHORE,0x100);
    BRIDGE_MEM32(0x80070000u)=0;
    uint32_t *r=gpu->pfifo.regs;
    r[NV_PFIFO_MODE]=1;r[NV_PFIFO_CACHE1_PUSH1]=0x100;r[NV_PFIFO_CACHE1_PUSH0]=1;
    r[NV_PFIFO_CACHE1_DMA_PUSH]=1;r[NV_PFIFO_CACHE1_PULL0]=1;r[NV_PFIFO_CACHE1_DMA_INSTANCE]=0x40;
    r[NV_PFIFO_CACHE1_DMA_GET]=r[NV_PFIFO_CACHE1_DMA_STATE]=0;
    gpu->pgraph.waiting_for_nop=false;gpu->pgraph.regs[NV_PGRAPH_FIFO]=1;
    /* Frozen submission remains pending in the canonical saved FIFO. */
    nv2a_mmio_write(gpu,0x800040,partial?partial_put:end,4);
    assert(r[NV_PFIFO_CACHE1_DMA_GET]==0 && BRIDGE_MEM32(0x80070000u)==0);
    FILE *snapshot=tmpfile(),*devices=tmpfile();assert(snapshot && devices);
    assert(nv2a_gpu_save_checkpoint(snapshot) && xbox_CheckpointWriteDevices(devices));
    rewind(devices);assert(xbox_CheckpointLoadDevices(devices));fclose(devices);
    for(unsigned pass=0;pass<2;pass++) {
        xbox_CheckpointEnd();nv2a_fifo_wait_idle(gpu);
        if(partial) {
            assert(r[NV_PFIFO_CACHE1_DMA_GET]==pending_get && BRIDGE_MEM32(0x80070000u)==0);
            if(indices) {
                if(indices==2)assert(s_gpu.prim==0 && memcmp(s_program,commands+26,8*4));
                else assert(s_gpu.prim==5 && s_gpu.idx_count==0);
                assert(!memcmp(nv2a_vsh_constant(17),commands+3,16*4));
                if(!pass) {
                    assert(xbox_CheckpointBootstrapBegin());rewind(snapshot);
                    assert(nv2a_gpu_save_checkpoint(snapshot));xbox_CheckpointEnd();
                }
            } else assert(!memcmp(nv2a_vsh_constant(17),zero,sizeof zero));
            nv2a_mmio_write(gpu,0x800040,end,4);nv2a_fifo_wait_idle(gpu);
        }
        ULONGLONG deadline=GetTickCount64()+5000;
        while(BRIDGE_MEM32(0x80070000u)!=0x12345678 && GetTickCount64()<deadline) {
            nv2a_pb_request_progress();Sleep(1);
        }
        assert(BRIDGE_MEM32(0x80070000u)==0x12345678);
        assert(xbox_CheckpointBootstrapBegin());nv2a_pb_checkpoint_publish();
        assert(r[NV_PFIFO_CACHE1_DMA_GET]==end && BRIDGE_MEM32(0x80020000u)==(indices?0xffff0000u:0xff445566u));
        if(indices)assert(s_gpu.prim==0 && s_gpu.idx_count==3 && s_gpu.idx[0]==0 && s_gpu.idx[1]==1 && s_gpu.idx[2]==2);
        if(indices==2)assert(!memcmp(s_program,commands+26,8*4));
        assert(!memcmp(nv2a_vsh_constant(17),commands+3,16*4));
        if(!pass) {
            BRIDGE_MEM32(0x80070000u)=0;rewind(snapshot);
            assert(nv2a_gpu_load_checkpoint(snapshot));
            assert(r[NV_PFIFO_CACHE1_DMA_GET]==(indices&&partial?pending_get:0) && BRIDGE_MEM32(0x80070000u)==0);
        }
    }
    fclose(snapshot);
    fprintf(stderr,"[FIFO CHECKPOINT] frozen queued commands, device barrier, restored pending FIFO, native pixels and real semaphore completion passed\n");
}
static LONG CALLBACK vertex_memory_fault_for_test(PEXCEPTION_POINTERS e) {
    if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION &&
       nv2a_pb_handle_memory_fault(e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionInformation[0]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}


static void test_dirty_colour(HwColor *color,const float rgba[4]) {
    if(d3d8_IsNativeVulkan()) assert(nvk_image_clear(hw_vk_render(color),rgba));
    else ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(color))->rtv,rgba);
    color->dirty=1;color->dirty_bounds=(RECT){0,0,8,8};
    hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_NOACCESS,NULL);
}
static void verify_clear_page_ownership(void) {
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
    HwColor *a=hw_color(0);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x20400);HwColor *neighbor=hw_color(0);
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x300000);HwColor *unrelated=hw_color(0);
    assert(a->memory==neighbor->memory && a->memory_offset+hw_color_bytes(a)<neighbor->memory_offset);
    assert((a->memory_offset/4096)==(neighbor->memory_offset/4096));
    const float red[4]={1,0,0,1},green[4]={0,1,0,1},blue[4]={0,0,1,1};
    test_dirty_colour(a,red);test_dirty_colour(neighbor,green);test_dirty_colour(unrelated,blue);
    uint64_t readbacks=s_gpu.hardware_color_readbacks;
    method(NV097_SET_SURFACE_COLOR_OFFSET,0x20000);
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff123456);method(NV097_CLEAR_SURFACE,0xf0);
    assert(s_gpu.hardware_color_readbacks==readbacks);
    assert(a->dirty && neighbor->dirty && unrelated->dirty);
    assert(BRIDGE_MEM32(a->address)==0xff123456);
    assert(s_gpu.hardware_color_readbacks==readbacks+3);
    uint32_t *am=(uint32_t *)(a->memory->host_view+a->memory_offset);
    uint32_t *nm=(uint32_t *)(neighbor->memory->host_view+neighbor->memory_offset);
    for(unsigned i=0;i<64;i++)assert(am[i]==0xff123456 && nm[i]==0xff00ff00);
    test_dirty_colour(unrelated,blue);
    /* Partial channel writes need the GPU's existing channels. */
    test_dirty_colour(a,blue);readbacks=s_gpu.hardware_color_readbacks;
    method(NV097_SET_COLOR_CLEAR_VALUE,0xffaabbcc);method(NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_R);
    assert(s_gpu.hardware_color_readbacks==readbacks+1 && unrelated->dirty);
    for(unsigned i=0;i<64;i++)assert(am[i]==0xffaa00ff && nm[i]==0xff00ff00);
    protection_pages_for_test(unrelated->memory,unrelated->memory_offset,1,PAGE_NOACCESS);
    /* An output outside the target bytes can still share its protection page. */
    uint8_t *output=(uint8_t *)(xbox_GetMemoryOffset()+a->address+0x800);
    hw_defer_output(output,0x1234abcd);assert(s_hw_query_count==1);
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff765432);method(NV097_CLEAR_SURFACE,0xf0);
    assert(a->dirty && !s_hw_query_count);
    protection_pages_for_test(a->memory,a->memory_offset,1,PAGE_NOACCESS);
    assert(BRIDGE_MEM32(a->address)==0xff765432 && *(uint32_t *)output==0x1234abcd);
    fprintf(stderr,"[CLEAR OWNERSHIP] overwrite discard, same-page nonoverlap preservation, unrelated GPU ownership and partial channels passed\n");
}

static void verify_native_finished_frame(void) {
    seed();method(NV097_SET_BEGIN_END,0);
    method(NV097_SET_SURFACE_PITCH,2560u|(2560u<<16));
    method(NV097_SET_SURFACE_CLIP_HORIZONTAL,640<<16);
    method(NV097_SET_SURFACE_CLIP_VERTICAL,480<<16);
    nv2a_get_state()->pgraph.regs[NV_PGRAPH_CLEARRECTX]=639u<<16;
    nv2a_get_state()->pgraph.regs[NV_PGRAPH_CLEARRECTY]=479u<<16;
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff6a8cad);
    method(NV097_CLEAR_SURFACE,0xf0);
    _putenv_s("RECOMP_FB_WINDOW","1");
    const FramebufferNativeRenderer *renderer=nv2a_pb_native_renderer();assert(renderer);
    s_gpu.drawn_offset=0;
    uint64_t before=s_gpu.hardware_color_readbacks;
    method(NV097_FLIP_STALL,0);
    assert(s_gpu.hardware_color_readbacks==before);
    unsigned scale=d3d8_RenderScale(),width=640*scale,height=480*scale;
    if(d3d8_IsNativeVulkan())assert(s_hw_vk_front && s_hw_vk_front_width==width && s_hw_vk_front_height==height);
    else assert(s_hw_front && ((D3D8Surface *)s_hw_front)->width==width);
    assert(renderer->capture() && readback_present_bytes==width*height*4);
    for(unsigned i=0;i<width*height;i++)assert(((uint32_t *)readback_present_pixels)[i]==0xff6a8cad);
    /* A later render cannot change the already finished display image. */
    method(NV097_SET_COLOR_CLEAR_VALUE,0xffaabbcc);method(NV097_CLEAR_SURFACE,0xf0);
    assert(s_gpu.hardware_color_readbacks==before);
    assert(renderer->capture());
    for(unsigned i=0;i<width*height;i++)assert(((uint32_t *)readback_present_pixels)[i]==0xff6a8cad);
    /* Resolve another real CPU alias to the same physical colour owner. */
    HwColor *color=hw_color(0);
    XboxSharedMemoryViews *views=&color->memory->views;assert(views->view_count>1);
    uint32_t alias=(uint32_t)((uintptr_t)views->cpu_views[1]+color->memory_offset-xbox_GetMemoryOffset());
    hw_finish_native_frame(alias,color->pitch);assert(renderer->capture());
    for(unsigned i=0;i<width*height;i++)assert(((uint32_t *)readback_present_pixels)[i]==0xffaabbcc);
    /* A larger swizzled menu target must not displace the scene at a flip. */
    note_drawn();uint32_t scene_offset=s_gpu.drawn_offset;
    uint32_t scene_color=s_gpu.color_offset,scene_format=s_gpu.format,scene_pitch=s_gpu.pitch;
    uint32_t scene_w=s_gpu.clip_w,scene_h=s_gpu.clip_h;
    s_gpu.format=0x090a0228;s_gpu.color_offset=0x300000;s_gpu.pitch=4096;s_gpu.clip_w=1024;s_gpu.clip_h=512;
    note_drawn();assert(s_gpu.drawn_offset==scene_offset);
    s_gpu.color_offset=scene_color;s_gpu.format=scene_format;s_gpu.pitch=scene_pitch;s_gpu.clip_w=scene_w;s_gpu.clip_h=scene_h;
    method(NV097_FLIP_STALL,0);assert(renderer->capture());
    for(unsigned i=0;i<width*height;i++)assert(((uint32_t *)readback_present_pixels)[i]==0xffaabbcc);
    /* A cold restore must present a saved surface before any new draw recreates it. */
    FILE *snapshot=tmpfile();assert(snapshot && nv2a_gpu_save_checkpoint(snapshot));
    hw_free_colors(s_hw_colors);s_hw_colors=NULL;
    rewind(snapshot);assert(nv2a_gpu_load_checkpoint(snapshot));fclose(snapshot);
    assert(s_hw_colors);
    hw_finish_native_frame(alias,2560);assert(renderer->capture());
    for(unsigned i=0;i<width*height;i++)assert(((uint32_t *)readback_present_pixels)[i]==0xffaabbcc);
    uint32_t *restored=malloc((size_t)width*height*4);assert(restored);
    for(unsigned i=0;i<width*height;i++)restored[i]=0xff000000|(i&0xffffff);
    assert(renderer->restore(restored,width,height) && renderer->capture());
    assert(!memcmp(restored,readback_present_pixels,(size_t)width*height*4));
    free(restored);renderer->close();assert(!s_hw_front && !s_hw_vk_front);
    fprintf(stderr,"[NATIVE DISPLAY] no flip readback, finished image isolation, physical alias, capture and restoration bytes passed\n");
}


static uint32_t stencil_sample(float *z) {
    nv2a_pb_checkpoint_publish();
    if(!hw_enabled()) {ZetaSurface depth=zeta_surface();uint32_t raw;memcpy(&raw,depth.memory,4);*z=(float)(raw>>8);return raw&255;}
    FILE *dump=tmpfile();assert(dump && hw_save_depths(dump));rewind(dump);
    uint32_t count,value;HwDepthCheckpoint header;float depth;
    assert(fread(&count,4,1,dump)==1 && count==1);
    assert(fread(&header,sizeof header,1,dump)==1 && header.address==0x80030000u);
    assert(fread(&depth,4,1,dump)==1 && fread(&value,4,1,dump)==1);fclose(dump);
    *z=depth*(float)header.normalization;return value&255;
}
static void verify_stencil_operations(void) {
    seed();method(NV097_SET_BEGIN_END,0);vertices(0.25f);
    method(NV097_SET_STENCIL_TEST_ENABLE,1);
    method(NV097_SET_STENCIL_FUNC_REF,0xa7);method(NV097_SET_STENCIL_FUNC_MASK,255);
    const unsigned ops[]={0x1e00,0,0x1e01,0x1e02,0x1e03,0x150a,0x8507,0x8508};
    const unsigned initial[]={0,0x3c,255};
    const unsigned results[8][3]={{0,60,255},{0,0,0},{167,167,167},{1,61,255},
        {0,59,254},{255,195,0},{1,61,0},{255,59,254}};
    for(unsigned op=0;op<8;op++)for(unsigned outcome=0;outcome<3;outcome++)
    for(unsigned start=0;start<3;start++)for(unsigned mode=0;mode<5;mode++) {
        unsigned mask=mode==1?15:mode==2?0:255;
        method(NV097_SET_CONTROL0,mode==3?0:NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
        method(NV097_SET_STENCIL_MASK,mask);
        method(NV097_SET_STENCIL_FUNC,outcome==0?0x200:0x207);
        method(NV097_SET_DEPTH_FUNC,outcome==1?0x200:0x207);
        method(NV097_SET_STENCIL_OP_FAIL,outcome==0?ops[op]:0x1e00);
        method(NV097_SET_STENCIL_OP_ZFAIL,outcome==1?ops[op]:0x1e00);
        method(NV097_SET_STENCIL_OP_ZPASS,outcome==2?ops[op]:0x1e00);
        method(NV097_SET_ALPHA_TEST_ENABLE,mode==4);method(NV097_SET_ALPHA_FUNC,0x200);
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x7f000000|initial[start]);method(NV097_CLEAR_SURFACE,0xf3);
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        unsigned write_mask=mode>=3?0:mask;
        unsigned expected=(initial[start]&~write_mask)|(results[op][start]&write_mask);
        float depth;unsigned actual=stencil_sample(&depth);
        if(actual!=expected)fprintf(stderr,"stencil mismatch op=%x outcome=%u initial=%u mode=%u actual=%u expected=%u\n",ops[op],outcome,initial[start],mode,actual,expected);
        assert(actual==expected);
        int passed=outcome==2 && mode!=4;
        assert(BRIDGE_MEM32(s_gpu.color_base)==(passed?0xffff0000u:0xff334455u));
        assert(fabsf(depth-(passed?16777215.0f*0.25f:0x7f0000))<2);
    }
    /* Stencil remains active with Z testing disabled; read masks also apply
     * to the reference, and accepted stencil pixels must retain Z. */
    method(NV097_SET_ALPHA_TEST_ENABLE,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
    method(NV097_SET_STENCIL_MASK,255);method(NV097_SET_STENCIL_FUNC,0x202);
    method(NV097_SET_STENCIL_FUNC_MASK,15);method(NV097_SET_STENCIL_FUNC_REF,0xa7);
    method(NV097_SET_STENCIL_OP_ZPASS,0x1e01);method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x7f000037);
    method(NV097_CLEAR_SURFACE,0xf3);
    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
    float depth;assert(stencil_sample(&depth)==0xa7 && fabsf(depth-0x7f0000)<2);
    assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000u);
    fprintf(stderr,"[STENCIL] eight operations, three outcomes, saturation/wrap endpoints, write/read masks, write enable, alpha rejection, disabled Z and colour/depth preservation passed\n");
}


static void verify_colour_logic(void) {
    seed();method(NV097_SET_BEGIN_END,0);vertices(0.25f);
    method(NV097_SET_DEPTH_FUNC,0x207);
    for(unsigned i=0;i<3;i++)BRIDGE_MEM32(0x80040010u+i*20)=0x9669cc33u;
    method(NV097_SET_COLOR_CLEAR_VALUE,0x3cc3f00fu);
    const uint32_t results[]={0x0u,0x1441c003u,0x82280c30u,0x9669cc33u,0x2882300cu,0x3cc3f00fu,0xaaaa3c3cu,0xbeebfc3fu,0x411403c0u,0x5555c3c3u,0xc33c0ff0u,0xd77dcff3u,0x699633ccu,0x7dd7f3cfu,0xebbe3ffcu,0xffffffffu};
    const uint32_t masks[]={0x01010101,0x00010001,0x01000000,0};
    const uint32_t bits[]={0xffffffffu,0x00ff00ff,0xff000000,0};
    method(NV097_SET_BLEND_FUNC_SFACTOR,0);method(NV097_SET_BLEND_FUNC_DFACTOR,0);
    for(unsigned route=0;route<2;route++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route*2);
        for(unsigned op=0;op<16;op++)for(unsigned mask=0;mask<4;mask++)for(unsigned blend=0;blend<2;blend++) {
            method(NV097_SET_COLOR_MASK,masks[mask]);
            method(NV097_SET_BLEND_ENABLE,blend);method(NV097_SET_LOGIC_OP_ENABLE,1);
            method(NV097_SET_LOGIC_OP,0x1500+op);method(NV097_CLEAR_SURFACE,0xf3);
            method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
            nv2a_pb_checkpoint_publish();
            uint32_t expected=(results[op]&bits[mask])|(0x3cc3f00fu&~bits[mask]);
            uint32_t actual=BRIDGE_MEM32(s_gpu.color_base);
            if(actual!=expected)fprintf(stderr,"logic mismatch op=%u mask=%u blend=%u route=%u actual=%08x expected=%08x\n",op,mask,blend,route,actual,expected);
            assert(actual==expected);
        }
        /* Consecutive XORs must consume the immediately preceding pixels,
         * including two overlapping primitives in the same command batch. */
        method(NV097_SET_COLOR_MASK,0x01010101);method(NV097_SET_LOGIC_OP,0x1506);
        method(NV097_CLEAR_SURFACE,0xf3);method(NV097_SET_BEGIN_END,5);
        method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        nv2a_pb_checkpoint_publish();assert(BRIDGE_MEM32(s_gpu.color_base)==0x3cc3f00fu);
        method(NV097_SET_LOGIC_OP_ENABLE,0);
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        nv2a_pb_checkpoint_publish();assert(BRIDGE_MEM32(s_gpu.color_base)==0);
        method(NV097_SET_BLEND_ENABLE,0);
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        nv2a_pb_checkpoint_publish();assert(BRIDGE_MEM32(s_gpu.color_base)==0x9669cc33u);
    }
    fprintf(stderr,"[COLOUR LOGIC] all operations, masks, blend precedence, overlapping draws and state transitions passed\n");
}

static float polygon_offset_depth(void) {
    nv2a_pb_checkpoint_publish();
    if(!hw_enabled()) {ZetaSurface depth=zeta_surface();uint32_t raw=0;if(depth.format==1){uint16_t value;memcpy(&value,depth.memory,2);raw=value;}else {memcpy(&raw,depth.memory,4);raw>>=8;}return (float)raw;}
    FILE *dump=tmpfile();assert(dump && hw_save_depths(dump));rewind(dump);
    uint32_t count;HwDepthCheckpoint header;float depth;
    assert(fread(&count,4,1,dump)==1 && count>=1);
    assert(fread(&header,sizeof header,1,dump)==1 && header.address==s_gpu.zeta_offset+0x80000000u);
    assert(fread(&depth,4,1,dump)==1);fclose(dump);
    return depth*(float)header.normalization;
}
static void polygon_offset_float(uint32_t method_id,float value) {
    uint32_t bits;memcpy(&bits,&value,4);method(method_id,bits);
}
static void skin_matrix_for_test(uint32_t command,unsigned matrix,float x,float normal_z) {
    for(unsigned row=0;row<4;row++)for(unsigned component=0;component<4;component++) {
        float value=row==component?1.0f:0.0f;
        if(row==0 && component==3)value=x;
        if(row==2 && component==2)value=normal_z;
        uint32_t bits;memcpy(&bits,&value,4);method(command+matrix*64+(row*4+component)*4,bits);
    }
}
static void image_blit_for_test(unsigned format,unsigned source,unsigned destination,unsigned source_pitch,unsigned destination_pitch,unsigned in_x,unsigned in_y,unsigned out_x,unsigned out_y,unsigned width,unsigned height) {
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;
    ramin[0x600/4]=NV_CONTEXT_SURFACES_2D;ramin[0x700/4]=NV_IMAGE_BLIT;ramin[0x900/4]=0x30;
    pgraph_method(gpu,2,NV_SET_OBJECT,0x600);
    pgraph_method(gpu,2,NV062_SET_CONTEXT_DMA_IMAGE_SOURCE,0x100);pgraph_method(gpu,2,NV062_SET_CONTEXT_DMA_IMAGE_DESTIN,0x100);
    pgraph_method(gpu,2,NV062_SET_COLOR_FORMAT,format);pgraph_method(gpu,2,NV062_SET_PITCH,source_pitch|(destination_pitch<<16));
    pgraph_method(gpu,2,NV062_SET_OFFSET_SOURCE,source);pgraph_method(gpu,2,NV062_SET_OFFSET_DESTIN,destination);
    pgraph_method(gpu,3,NV_SET_OBJECT,0x700);pgraph_method(gpu,3,NV09F_SET_CONTEXT_SURFACES,0x600);
    /* Original sub_002F7880 binds the real class-30 object to these slots. */
    for(unsigned slot=0x184;slot<=0x198;slot+=4)pgraph_method(gpu,3,slot,0x900);
    pgraph_method(gpu,3,NV09F_SET_OPERATION,NV09F_SET_OPERATION_SRCCOPY);
    pgraph_method(gpu,3,NV09F_CONTROL_POINT_IN,in_x|(in_y<<16));pgraph_method(gpu,3,NV09F_CONTROL_POINT_OUT,out_x|(out_y<<16));
    pgraph_method(gpu,3,NV09F_SIZE,width|(height<<16));
}

static void verify_blit_overlap_direction(void) {
    NV2AState *gpu=nv2a_get_state();gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;ramin[0x100/4]=ramin[0x200/4]=2;
    ramin[0x104/4]=ramin[0x204/4]=0x03ffffff;ramin[0x108/4]=ramin[0x208/4]=0;
    ramin[0xe00/4]=NV_CONTEXT_ROP;
    for(unsigned tile=0;tile<NV_NUM_GPU_TILES;tile++)gpu->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(tile)]=0;
    const unsigned formats[]={NV062_SET_COLOR_FORMAT_LE_Y8,NV062_SET_COLOR_FORMAT_LE_R5G6B5,
        NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,NV062_SET_COLOR_FORMAT_LE_X8R8G8B8};
    /* Down/up, right/left, both diagonals and equal origin, padded rows. */
    const unsigned moves[][4]={{1,1,1,2},{1,2,1,1},{1,1,2,1},{2,1,1,1},{1,1,2,2},{2,2,1,1},{1,1,1,1}};
    unsigned trials=0;
    for(unsigned f=0;f<4;f++)for(unsigned alias=0;alias<2;alias++)for(unsigned n=0;n<7;n++) {
        unsigned bpp=f==0?1:f==1?2:4,pitch=32;
        uint8_t *memory=gpu->vram_ptr+0xa0000,original[256],expected[256];
        image_blit_for_test(formats[f],0xa0000,0xa0000,pitch,pitch,0,0,0,0,0,0);
        if(alias)pgraph_method(gpu,2,NV062_SET_CONTEXT_DMA_IMAGE_DESTIN,0x200);
        for(unsigned i=0;i<sizeof original;i++)memory[i]=(uint8_t)(i*13+7);
        memcpy(original,memory,sizeof original);memcpy(expected,original,sizeof expected);
        unsigned sx=moves[n][0],sy=moves[n][1],dx=moves[n][2],dy=moves[n][3];
        for(unsigned y=0;y<3;y++) {
            memcpy(expected+(dy+y)*pitch+dx*bpp,original+(sy+y)*pitch+sx*bpp,3*bpp);
            if(f==3)for(unsigned x=0;x<3;x++)expected[(dy+y)*pitch+(dx+x)*bpp+3]=255;
        }
        pgraph_method(gpu,3,NV09F_CONTROL_POINT_IN,sx|(sy<<16));
        pgraph_method(gpu,3,NV09F_CONTROL_POINT_OUT,dx|(dy<<16));
        pgraph_method(gpu,3,NV09F_SIZE,3|(3u<<16));
        assert(!memcmp(memory,expected,sizeof expected));++trials;
        if(f==3) {
            memcpy(memory,original,sizeof original);
            pgraph_method(gpu,5,NV_SET_OBJECT,0xe00);pgraph_method(gpu,5,NV043_SET_ROP5,0xcc);
            pgraph_method(gpu,3,0x190,0xe00);pgraph_method(gpu,3,NV09F_SET_OPERATION,NV09F_SET_OPERATION_ROP_AND);
            pgraph_method(gpu,3,NV09F_SIZE,3|(3u<<16));
            assert(!memcmp(memory,expected,sizeof expected));++trials;
        }
    }
    fprintf(stderr,"[BLIT DIRECTION] %u same-pitch linear source-copy trials, all RAM/guards and canonical DMA aliases passed\n",trials);
}

static void verify_blit_exact_self_alias(void) {
    NV2AState *gpu=nv2a_get_state();gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;ramin[0x100/4]=ramin[0x200/4]=2;ramin[0x104/4]=ramin[0x204/4]=0x03ffffff;ramin[0x108/4]=ramin[0x208/4]=0;ramin[0xe00/4]=NV_CONTEXT_ROP;
    const unsigned alphas[]={0,1,127,128,255};
    for(unsigned op=0;op<17;op++)for(unsigned alias=0;alias<2;alias++)for(unsigned alpha=0;alpha<5;alpha++) {
        image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_X8R8G8B8,0xa0000,0xa0000,32,32,1,1,1,1,0,0);
        if(alias)pgraph_method(gpu,2,NV062_SET_CONTEXT_DMA_IMAGE_DESTIN,0x200);
        if(op<16){pgraph_method(gpu,5,NV_SET_OBJECT,0xe00);pgraph_method(gpu,5,NV043_SET_ROP5,op*0x11);pgraph_method(gpu,3,0x190,0xe00);}
        pgraph_method(gpu,3,NV09F_SET_OPERATION,op<16?NV09F_SET_OPERATION_ROP_AND:NV09F_SET_OPERATION_SRCCOPY_AND);
        uint8_t *memory=gpu->vram_ptr+0xa0000,expected[256];memset(memory,0xa5,256);
        for(unsigned row=0;row<3;row++)for(unsigned x=0;x<4;x++)stl_le_p(memory+(row+1)*32+(x+1)*4,(alphas[alpha]<<24)|((0x13579b+(row*4+x)*0x112233)&0xffffff));
        memcpy(expected,memory,sizeof expected);
        for(unsigned row=0;row<3;row++)for(unsigned x=0;x<4;x++) {
            uint32_t d=ldl_le_p(memory+(row+1)*32+(x+1)*4);
            /* With S=D, only ROP truth-table inputs00 and11 can occur. */
            uint32_t value=op==16?d:((op&1)?~d:0)|((op&8)?d:0);
            stl_le_p(expected+(row+1)*32+(x+1)*4,0xff000000|(value&0xffffff));
        }
        pgraph_method(gpu,3,NV09F_SIZE,4|(3u<<16));assert(!memcmp(memory,expected,sizeof expected));
    }
    fprintf(stderr,"[BLIT SELF ALIAS] sixteenROP functions plusCOPY_AND,physicalsamepixel/samepitch withsameanddistinctDMAhandles,fiverawalphas,threepaddedrows/allguards passed\n");
}

static void verify_blit_rop(const char *mode) {
    NV2AState *gpu=nv2a_get_state();gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;ramin[0x100/4]=2;ramin[0x104/4]=0x03ffffff;ramin[0x108/4]=0;ramin[0xe00/4]=NV_CONTEXT_ROP;
    const uint32_t results[]={0xff000000,0xff1403c0,0xff82300c,0xff9633cc,0xff280c30,0xff3c0ff0,0xffaa3c3c,0xffbe3ffc,0xff41c003,0xff55c3c3,0xffc3f00f,0xffd7f3cf,0xff69cc33,0xff7dcff3,0xffebfc3f,0xffffffff};
    const unsigned alphas[]={0,1,127,128,255};unsigned trials=!strcmp(mode,"blit-rop-copy")?16:1;
    for(unsigned operation=0;operation<trials;operation++)for(unsigned alpha=0;alpha<5;alpha++) {
        unsigned format=!strcmp(mode,"blit-rop-alpha")?NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:NV062_SET_COLOR_FORMAT_LE_X8R8G8B8;
        image_blit_for_test(format,0xa0000,!strcmp(mode,"blit-rop-overlap")?0xa0000:0xb0000,32,40,1,1,2,2,0,0);
        pgraph_method(gpu,5,NV_SET_OBJECT,0xe00);pgraph_method(gpu,5,NV043_SET_ROP5,!strcmp(mode,"blit-rop-patterned")?0xf0:0xabcdef00u|operation*0x11u);
        assert(gpu->pgraph.regs[NV_PGRAPH_ROP3]==(!strcmp(mode,"blit-rop-patterned")?0xf0:operation*0x11u));
        if(operation==6 && alpha==2) {
            FILE *snapshot=tmpfile();assert(snapshot && nv2a_save_checkpoint(snapshot));rewind(snapshot);void *loaded=nv2a_prepare_checkpoint(snapshot);assert(loaded);
            gpu->pgraph.regs[NV_PGRAPH_ROP3]=0xff;nv2a_apply_checkpoint(loaded);free(loaded);fclose(snapshot);
            assert(pgraph_read(gpu,NV_PGRAPH_ROP3,4)==0x66);
        }
        if(!strcmp(mode,"blit-rop-identity")) {
            pgraph_method(gpu,5,NV043_SET_ROP5,0x66);
            ramin[0xd00/4]=NV_CONTEXT_ROP;pgraph_method(gpu,5,NV_SET_OBJECT,0xd00);pgraph_method(gpu,5,NV043_SET_ROP5,0xcc);
            pgraph_method(gpu,5,NV_SET_OBJECT,0xe00);assert(pgraph_read(gpu,NV_PGRAPH_ROP3,4)==0xcc);
        }
        pgraph_method(gpu,3,0x190,0xe00);pgraph_method(gpu,3,NV09F_SET_OPERATION,NV09F_SET_OPERATION_ROP_AND);
        if(!strcmp(mode,"blit-rop-pattern")){ramin[0xf00/4]=NV_CONTEXT_PATTERN;pgraph_method(gpu,3,0x18c,0xf00);}
        if(!strcmp(mode,"blit-rop-key")){ramin[0xf00/4]=0x57;pgraph_method(gpu,3,0x184,0xf00);}
        if(!strcmp(mode,"blit-rop-overlap")){pgraph_method(gpu,3,NV09F_CONTROL_POINT_IN,0);pgraph_method(gpu,3,NV09F_CONTROL_POINT_OUT,0);}
        uint8_t *source=gpu->vram_ptr+0xa0000,*destination=gpu->vram_ptr+0xb0000;
        memset(source,0x7b,256);memset(destination,0xa5,256);uint8_t expected[256];memset(expected,0xa5,sizeof expected);
        for(unsigned row=0;row<2;row++)for(unsigned x=0;x<4;x++) {
            stl_le_p(source+(row+1)*32+(x+1)*4,(alphas[alpha]<<24)|0x0069cc33);
            stl_le_p(destination+(row+2)*40+(x+2)*4,0x3cc3f00f);
            stl_le_p(expected+(row+2)*40+(x+2)*4,results[!strcmp(mode,"blit-rop-identity")?12:operation]);
        }
        if(!strcmp(mode,"blit-rop-partial-tile")) {
            gpu->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(0)]=NV_PFB_TILE_FLAGS_VALID;
            gpu->pfb.regs[NV_PFB_TILE_LIMIT(0)]=0xb0000+2*40+2*4+4;
        }
        pgraph_method(gpu,3,NV09F_SIZE,4|(2u<<16));
        assert(!memcmp(destination,expected,256));
        if(!strcmp(mode,"blit-rop-identity")) {
            pgraph_method(gpu,5,NV043_SET_ROP5,0x66);
            for(unsigned row=0;row<2;row++)for(unsigned x=0;x<4;x++) {
                stl_le_p(destination+(row+2)*40+(x+2)*4,0x3cc3f00f);
                stl_le_p(expected+(row+2)*40+(x+2)*4,0xffaa3c3c);
            }
            pgraph_method(gpu,3,NV09F_SIZE,4|(2u<<16));assert(!memcmp(destination,expected,256));
        }
    }
    fprintf(stderr,"[BLIT ROP] sixteen pattern-independentROP3 functions, original logic consumer, X8 alpha and pitched RAM guards passed\n");
}


static void verify_blit_copy_and(const char *mode) {
    NV2AState *gpu=nv2a_get_state();gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;ramin[0x100/4]=2;ramin[0x104/4]=0x03ffffff;ramin[0x108/4]=0;
    const unsigned alphas[]={0,1,127,128,255};
    for(unsigned alpha=0;alpha<5;alpha++) {
        unsigned format=!strcmp(mode,"blit-copy-and-alpha")?NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:NV062_SET_COLOR_FORMAT_LE_X8R8G8B8;
        image_blit_for_test(format,0xa0000,!strcmp(mode,"blit-copy-and-overlap")?0xa0000:0xb0000,32,40,1,1,2,2,0,0);
        pgraph_method(gpu,3,NV09F_SET_OPERATION,NV09F_SET_OPERATION_SRCCOPY_AND);
        if(!strcmp(mode,"blit-copy-and-pattern")){ramin[0xf00/4]=NV_CONTEXT_PATTERN;pgraph_method(gpu,3,0x18c,0xf00);}
        if(!strcmp(mode,"blit-copy-and-key")){ramin[0xf00/4]=0x57;pgraph_method(gpu,3,0x184,0xf00);}
        if(!strcmp(mode,"blit-copy-and-overlap")){pgraph_method(gpu,3,NV09F_CONTROL_POINT_IN,0);pgraph_method(gpu,3,NV09F_CONTROL_POINT_OUT,0);}
        uint8_t *source=gpu->vram_ptr+0xa0000,*destination=gpu->vram_ptr+0xb0000;
        memset(source,0x7b,256);memset(destination,0xa5,256);uint8_t expected[256];memset(expected,0xa5,sizeof expected);
        for(unsigned row=0;row<2;row++)for(unsigned x=0;x<4;x++) {
            uint32_t rgb=0x13579b+(row*4+x)*0x112233;
            stl_le_p(source+(row+1)*32+(x+1)*4,(alphas[alpha]<<24)|rgb);
            stl_le_p(expected+(row+2)*40+(x+2)*4,0xff000000|rgb);
        }
        if(!strcmp(mode,"blit-copy-and-partial-tile")) {
            gpu->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(0)]=NV_PFB_TILE_FLAGS_VALID;
            gpu->pfb.regs[NV_PFB_TILE_LIMIT(0)]=0xb0000+2*40+2*4+4;
        }
        pgraph_method(gpu,3,NV09F_SIZE,4|(2u<<16));assert(!memcmp(destination,expected,256));
    }
    fprintf(stderr,"[BLIT COPY AND] original operation0 opaqueX8 avoids zero-alpha discard; raw high bytes0/1/127/128/255, all RAM and guards exact\n");
}

static void verify_original_image_blit_copy(void) {
    /* The runtime supplies g_contig_memory to standalone NV2A, just as the
     * existing DMA/FIFO fixtures do. The general shader fixture starts with
     * the guest-linear view because that path supplies its own DMA mapping. */
    nv2a_get_state()->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    seed();method(NV097_SET_BEGIN_END,0);
    const unsigned formats[]={NV062_SET_COLOR_FORMAT_LE_Y8,NV062_SET_COLOR_FORMAT_LE_R5G6B5,
        NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8,NV062_SET_COLOR_FORMAT_LE_X8R8G8B8,
        NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,NV062_SET_COLOR_FORMAT_LE_Y32};
    for(unsigned test=0;test<sizeof formats/sizeof formats[0];test++) {
        unsigned bpp=test==0?1:test==1?2:4;
        uint8_t *source=(uint8_t *)(xbox_GetMemoryOffset()+0x800a0000u),*destination=(uint8_t *)(xbox_GetMemoryOffset()+0x800b0000u);
        for(unsigned i=0;i<256;i++)source[i]=(uint8_t)(i*17+test);
        memset(destination,0xa5,256);uint8_t expected[256];memset(expected,0xa5,sizeof expected);
        for(unsigned row=0;row<2;row++) {
            memcpy(expected+(row+2)*40+2*bpp,source+(row+1)*32+bpp,3*bpp);
            if(test==2 || test==3)for(unsigned x=0;x<3;x++)expected[(row+2)*40+(x+2)*4+3]=(uint8_t)(test==2?0:255);
        }
        image_blit_for_test(formats[test],0xa0000,0xb0000,32,40,1,1,2,2,3,2);
        assert(!memcmp(destination,expected,sizeof expected));
    }
    /* Existing upstream copy order preserves horizontal overlap and limits
     * transferred row pixels to the smaller source/destination pitch. */
    uint8_t *overlap=(uint8_t *)(xbox_GetMemoryOffset()+0x800a0000u);
    for(unsigned i=0;i<128;i++)overlap[i]=(uint8_t)i;
    uint8_t overlap_expected[128];memcpy(overlap_expected,overlap,128);
    for(unsigned row=0;row<2;row++)memmove(overlap_expected+row*32+4,overlap_expected+row*32,16);
    image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_Y32,0xa0000,0xa0000,32,32,0,0,1,0,4,2);
    assert(!memcmp(overlap,overlap_expected,128));
    uint32_t *clip_source=(uint32_t *)(xbox_GetMemoryOffset()+0x800a0000u);
    uint32_t *clip_dest=(uint32_t *)(xbox_GetMemoryOffset()+0x800b0000u);
    for(unsigned i=0;i<64;i++)clip_source[i]=0x80201000u+i;
    memset(clip_dest,0xa5,256);
    image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_Y32,0xa0000,0xb0000,32,32,0,0,0,0,10,2);
    for(unsigned i=0;i<64;i++)assert(clip_dest[i]==(i<16?clip_source[i]:0xa5a5a5a5u));
    NV2AState *gpu=nv2a_get_state();uint32_t *ramin=(uint32_t *)gpu->ramin_ptr;
    ramin[0xa00/4]=NV_CONTEXT_CLIP_RECTANGLE;ramin[0xb00/4]=NV_BETA;
    image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0xa0000,0xb0000,32,32,0,0,1,1,0,0);
    pgraph_method(gpu,4,NV_SET_OBJECT,0xa00);
    pgraph_method(gpu,4,NV019_SET_POINT,3|(2u<<16));pgraph_method(gpu,4,NV019_SET_SIZE,2|(2u<<16));
    pgraph_method(gpu,3,0x188,0xa00);memset(clip_dest,0xa5,256);
    pgraph_method(gpu,3,NV09F_SIZE,5|(4u<<16));
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)
        assert(clip_dest[y*8+x]==(x>=3 && x<5 && y>=2 && y<4?clip_source[(y-1)*8+x-1]:0xa5a5a5a5u));
    pgraph_method(gpu,4,NV019_SET_SIZE,0);memset(clip_dest,0xa5,256);
    pgraph_method(gpu,3,NV09F_SIZE,5|(4u<<16));
    for(unsigned i=0;i<64;i++)assert(clip_dest[i]==0xa5a5a5a5u);
    const uint32_t betas[]={0,0x007fffff,0x00800000,0x444fffff,0x7f800000,0x7fffffff,0x80000000,0xffffffff};
    for(unsigned test=0;test<sizeof betas/sizeof betas[0];test++) {
        image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0xa0000,0xb0000,32,32,0,0,0,0,0,0);
        pgraph_method(gpu,5,NV_SET_OBJECT,0xb00);pgraph_method(gpu,5,NV012_SET_BETA,betas[test]);
        pgraph_method(gpu,3,0x194,0xb00);pgraph_method(gpu,3,NV09F_SET_OPERATION,NV09F_SET_OPERATION_BLEND_AND);
        clip_source[0]=0x11224466;clip_dest[0]=0x77442200;
        pgraph_method(gpu,3,NV09F_SIZE,1|(1u<<16));
        unsigned weight=(betas[test]&0x80000000u)?0:(betas[test]>>23)&255;
        uint32_t expected=0x77000000u;
        for(unsigned channel=0;channel<3;channel++) {
            unsigned src=(0x11224466u>>(channel*8))&255,dst=(0x77442200u>>(channel*8))&255;
            expected|=((src*weight+dst*(255-weight))/255)<<(channel*8);
        }
        assert(clip_dest[0]==expected);
    }
    /* Source publication and destination invalidation reuse the original
     * physical-memory fault/ownership consumer; no separate blit cache. */
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,7u<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7u<<16);
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff2a4c6e);method(NV097_CLEAR_SURFACE,0xf0);
    image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0x20000,0x90000,32,32,0,0,0,0,8,8);
    const uint32_t *copy=(const uint32_t *)(xbox_GetMemoryOffset()+0x80090000u);
    for(unsigned i=0;i<64;i++)assert(copy[i]==0xff2a4c6e);
    uint32_t *cpu_source=(uint32_t *)(xbox_GetMemoryOffset()+0x800a0000u);
    for(unsigned i=0;i<64;i++)cpu_source[i]=0xff7b3c95;
    image_blit_for_test(NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,0xa0000,0x20000,32,32,0,0,0,0,8,8);
    if(hw_enabled()) {
        HwColor *color=hw_color(0);uint32_t actual[64];
        if(d3d8_IsNativeVulkan())assert(nvk_image_read(color->native_surface,actual,32));
        else {D3DLOCKED_RECT locked;assert(SUCCEEDED(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,D3DLOCK_READONLY)));
            for(unsigned row=0;row<8;row++)memcpy(actual+row*8,(const uint8_t *)locked.pBits+row*locked.Pitch,32);
            assert(SUCCEEDED(color->surface->lpVtbl->UnlockRect(color->surface)));}
        for(unsigned i=0;i<64;i++)assert(actual[i]==0xff7b3c95);
    }
    fprintf(stderr,"[IMAGE BLIT] real null/clip/beta contexts, DMA pitches/offsets, six formats with alpha, overlap, clipping, blend and shared GPU publication/invalidation passed\n");
}
static uint32_t stencil_sample(float *z);
static void verify_dependent_texture_stages(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t texels[4]={0xff112233,0xff445566,0xff778899,0xffaabbcc};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),texels,sizeof texels);
    for(unsigned stage=1;stage<4;stage++)for(unsigned source=0;source<stage;source++)for(unsigned mode=15;mode<=16;mode++)for(unsigned quadrant=0;quadrant<4;quadrant++) {
        float u=(quadrant&1)?.75f:.25f,v=(quadrant&2)?.75f:.25f;
        for(unsigned st=0;st<4;st++) {
            float coordinates[3][4];
            for(unsigned vertex=0;vertex<3;vertex++)for(unsigned c=0;c<4;c++)coordinates[vertex][c]=.5f;
            if(st==source)for(unsigned vertex=0;vertex<3;vertex++) {
                coordinates[vertex][mode==15?3:1]=u;coordinates[vertex][mode==15?0:2]=v;
            }
            memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u+st*256),coordinates,sizeof coordinates);
            method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+(9+st)*4,0x50000+st*256);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+(9+st)*4,0x1042);
        }
        unsigned offset=stage*64;
        method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_FORMAT+offset,(2<<4)|(0x06<<8)|(1<<16)|(1<<20)|(1<<24));
        method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        uint32_t program=mode<<(stage*5);for(unsigned st=0;st<stage;st++)program|=4u<<(st*5);
        method(NV097_SET_SHADER_OTHER_STAGE_INPUT,stage==1?0:source<<(stage==2?16:20));method(NV097_SET_SHADER_STAGE_PROGRAM,program);
        method(NV097_SET_COMBINER_COLOR_ICW,((8+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((0x18+stage)<<24)|0x200000);
        for(unsigned keyed=0;keyed<2;keyed++) {
            method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE|(keyed?3:0));method(NV097_SET_COLOR_KEY_COLOR+stage*4,texels[quadrant]);
            method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
            assert(BRIDGE_MEM32(s_gpu.color_base)==(keyed?0xff334455:texels[quadrant]));
        }
    }
    /* The dependency reads the sampled/post-key color, not the original UV. */
    float chain_coordinates[3][4];for(unsigned v=0;v<3;v++)for(unsigned c=0;c<4;c++)chain_coordinates[v][c]=.75f;
    memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),chain_coordinates,sizeof chain_coordinates);
    method(NV097_SET_SHADER_OTHER_STAGE_INPUT,1u<<20);
    method(NV097_SET_TEXTURE_CONTROL0+3*64,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_COMBINER_COLOR_ICW,(11u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(27u<<24)|0x200000);
    for(unsigned mode=15;mode<=16;mode++)for(unsigned key_mode=0;key_mode<3;key_mode++) {
        method(NV097_SET_SHADER_STAGE_PROGRAM,4|(15<<5)|(4<<10)|(mode<<15));
        method(NV097_SET_TEXTURE_CONTROL0+64,NV_PGRAPH_TEXCTL0_0_ENABLE|key_mode);method(NV097_SET_COLOR_KEY_COLOR+4,texels[3]);
        method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
        unsigned expected=mode==15?(key_mode==2?0:key_mode==1?2:3):(key_mode==2?0:3);
        if(BRIDGE_MEM32(s_gpu.color_base)!=texels[expected])fprintf(stderr,"[CHAIN] mode=%u key=%u actual=%08x expected=%08x\n",mode,key_mode,BRIDGE_MEM32(s_gpu.color_base),texels[expected]);
        assert(BRIDGE_MEM32(s_gpu.color_base)==texels[expected]);
    }
    fprintf(stderr,"[DEPENDENT TEXTURES] AR/GB, stages1..3, every earlier source, four quadrants and color-key discard passed\n");
}

static void verify_texture_color_keys(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t texels[4]={0x7f112233,0x7f112233,0x7f112233,0x7f112233};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),texels,sizeof texels);
    float coordinates[3][4]={{.5f,.5f,0,1},{.5f,.5f,0,1},{.5f,.5f,0,1}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coordinates,sizeof coordinates);
    for(unsigned stage=0;stage<4;stage++) {
        unsigned offset=stage*64;
        method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+(9+stage)*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+(9+stage)*4,0x1042);
        method(NV097_SET_TEXTURE_OFFSET+offset,0x80000);method(NV097_SET_TEXTURE_CONTROL1+offset,8<<16);
        method(NV097_SET_TEXTURE_IMAGE_RECT+offset,(2<<16)|2);method(NV097_SET_TEXTURE_ADDRESS+offset,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+offset,0x01010000);
        method(NV097_SET_SHADER_STAGE_PROGRAM,1u<<(stage*5));
        method(NV097_SET_COMBINER_COLOR_ICW,((8+stage)<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,((0x18+stage)<<24)|0x200000);
        for(unsigned xrgb=0;xrgb<2;xrgb++) {
            method(NV097_SET_TEXTURE_FORMAT+offset,((xrgb?0x1e:0x12)<<8)|(1<<16));
            uint32_t sampled=xrgb?0xff112233:texels[0];
            for(unsigned mode=0;mode<4;mode++)for(unsigned key_case=0;key_case<3;key_case++) {
                uint32_t key=sampled^(key_case==1?0x10000:key_case==2?0xff000000:0);
                method(NV097_SET_TEXTURE_CONTROL0+offset,NV_PGRAPH_TEXCTL0_0_ENABLE|mode);method(NV097_SET_COLOR_KEY_COLOR+stage*4,key);
                method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
                int matched=key_case==0 || (xrgb && key_case==2);
                uint32_t expected=!mode || !matched?sampled:mode==1?(sampled&0xffffff):mode==2?0:0xff334455;
                assert(BRIDGE_MEM32(s_gpu.color_base)==expected);
            }
        }
    }
    /* Discard must suppress both depth and every stencil outcome even when
     * color writes are disabled and ordinary alpha testing is disabled. */
    method(NV097_SET_SHADER_STAGE_PROGRAM,1);method(NV097_SET_TEXTURE_FORMAT,(0x12<<8)|(1<<16));
    method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE|3);method(NV097_SET_COLOR_KEY_COLOR,texels[0]);
    method(NV097_SET_COLOR_MASK,0);method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);
    method(NV097_SET_STENCIL_TEST_ENABLE,1);method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
    method(NV097_SET_STENCIL_MASK,255);method(NV097_SET_STENCIL_FUNC_MASK,255);method(NV097_SET_STENCIL_FUNC_REF,0x33);
    method(NV097_SET_STENCIL_OP_FAIL,0x1e01);method(NV097_SET_STENCIL_OP_ZFAIL,0x1e01);method(NV097_SET_STENCIL_OP_ZPASS,0x1e01);
    for(unsigned outcome=0;outcome<3;outcome++) {
        method(NV097_SET_STENCIL_FUNC,outcome==0?0x200:0x207);method(NV097_SET_DEPTH_FUNC,outcome==1?0x200:0x207);
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x7f0000a5);method(NV097_CLEAR_SURFACE,0xf3);
        float before,after;assert(stencil_sample(&before)==0xa5);
        method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
        assert(stencil_sample(&after)==0xa5 && before==after);
    }
    fprintf(stderr,"[COLOR KEYS] four stages, three actions, no-match, ARGB/XRGB alpha masks and depth/stencil discard passed\n");
}
static void verify_existing_resource_format_connections(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_TEXTURE_OFFSET,0x80000);
    method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);method(NV097_SET_TEXTURE_IMAGE_RECT,(2<<16)|2);
    method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);
    method(NV097_SET_SHADER_STAGE_PROGRAM,1);method(NV097_SET_COMBINER_COLOR_ICW,0x08200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x18200000);
    const struct {unsigned format;uint32_t bits,color;} cases[]={
        {D3DFMT_AL8,0xa3,0xa3a3a3a3},{D3DFMT_LIN_AL8,0xa3,0xa3a3a3a3},
        {D3DFMT_A8L8,0x7f4c,0x7f4c4c4c},{D3DFMT_LIN_A8L8,0x7f4c,0x7f4c4c4c},
        {D3DFMT_R5G5B5A1,0xf801,0xffff0000},{D3DFMT_LIN_R5G5B5A1,0xf801,0xffff0000},
        {D3DFMT_R4G4B4A4,0xa53c,0xccaa5533},{D3DFMT_LIN_R4G4B4A4,0xa53c,0xccaa5533},
        {D3DFMT_A8B8G8R8,0x7f112233,0x7f332211},{D3DFMT_B8G8R8A8,0x1122337f,0x7f332211},{D3DFMT_R8G8B8A8,0x3322117f,0x7f332211}};
    float coordinates[3][4]={{.5f,.5f,0,1},{.5f,.5f,0,1},{.5f,.5f,0,1}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coordinates,sizeof coordinates);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+9*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+9*4,0x1042);
    for(unsigned test=0;test<sizeof cases/sizeof cases[0];test++) {
        unsigned bytes=d3d8_format_bpp((D3DFORMAT)cases[test].format)/8;
        uint8_t *pixels=(uint8_t *)(xbox_GetMemoryOffset()+0x80080000u);
        for(unsigned i=0;i<4;i++)memcpy(pixels+i*bytes,&cases[test].bits,bytes);
        method(NV097_SET_TEXTURE_CONTROL1,(bytes*2)<<16);
        method(NV097_SET_TEXTURE_FORMAT,(cases[test].format<<8)|(1<<16)|(1<<20)|(1<<24));
        for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++) {uint32_t pixel;assert(sample_tex(&s_gpu.texs[0],0,x,y,&pixel)); if(!x && !y && (cases[test].format==D3DFMT_AL8 || cases[test].format==D3DFMT_LIN_AL8))fprintf(stderr,"[AY8] format=%02X raw=%02X actual=%08X expected=%08X\n",cases[test].format,cases[test].bits,pixel,cases[test].color); assert(pixel==cases[test].color);}
        method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
        assert(BRIDGE_MEM32(s_gpu.color_base)==cases[test].color);
    }
    fprintf(stderr,"[RESOURCE FORMATS] eleven missing format connections reuse original conversion/linear consumers and reach actual pixels\n");
}
static void verify_rectangle_point_sprite(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_LIGHTING_ENABLE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_POINT_SIZE,32);method(NV097_SET_POINT_SMOOTH_ENABLE,1);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    struct {float x,y,z,w;uint32_t color;} point={4,4,1,1,0xffff0000};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),&point,sizeof point);
    const uint32_t colors[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),colors,sizeof colors);
    unsigned stage=3*64;
    method(NV097_SET_TEXTURE_OFFSET+stage,0x80000);method(NV097_SET_TEXTURE_CONTROL1+stage,8<<16);
    method(NV097_SET_TEXTURE_IMAGE_RECT+stage,(2<<16)|2);method(NV097_SET_TEXTURE_CONTROL0+stage,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_TEXTURE_ADDRESS+stage,3|(3<<8));method(NV097_SET_TEXTURE_FILTER+stage,0x01010000);
    method(NV097_SET_SHADER_STAGE_PROGRAM,1u<<15);
    method(NV097_SET_COMBINER_COLOR_ICW,0x0b200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x1b200000);
    for(unsigned program=0;program<2;program++) {
        uint32_t reference[64],actual[64];method(NV097_SET_TRANSFORM_EXECUTION_MODE,program?2:0);
        for(unsigned linear=0;linear<2;linear++) {
            method(NV097_SET_TEXTURE_FORMAT+stage,linear?((0x12<<8)|(1<<16)):((0x06<<8)|(1<<16)|(1<<20)|(1<<24)));
            method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,1);method(NV097_DRAW_ARRAYS,0);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
            memcpy(linear?actual:reference,(const void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof actual);
        }
        assert(reference[2*8+2]==colors[0] && reference[2*8+4]==colors[1]);
        assert(reference[4*8+2]==colors[2] && reference[4*8+4]==colors[3]);
        assert(!memcmp(actual,reference,sizeof actual));
    }
    fprintf(stderr,"[RECTANGLE SPRITE] stage3 UV replacement produces matching four-color sprite for rectangle/swizzled textures on fixed/program paths\n");
}
static void verify_two_sided_vertex_colors(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_LIGHTING_ENABLE,0);
    method(NV097_SET_SPECULAR_ENABLE,1);method(NV097_SET_LIGHT_CONTROL,NV097_SET_LIGHT_CONTROL_SEPARATE_SPECULAR);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    const float colors[3][4]={{0,0,1,1},{0,1,0,1},{1,0,1,1}};
    const unsigned attributes[3]={4,7,8};
    for(unsigned a=0;a<3;a++)for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&colors[a][c],4);method(NV097_SET_VERTEX_DATA4F_M+attributes[a]*16+c*4,bits);}
    const unsigned inputs[5]={0,3,4,7,8},outputs[5]={0,3,4,7,8};
    for(unsigned i=0;i<5;i++) {uint32_t instruction[4]={0,(1u<<21)|(inputs[i]<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(outputs[i]<<3)|(i==4)};nv2a_vsh_set_instruction(i,instruction);}
    nv2a_vsh_set_start_slot(0);vertices(1.0f/16777215.0f);
    for(unsigned program=0;program<2;program++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,program?2:0);
        for(unsigned specular=0;specular<2;specular++) {
            method(NV097_SET_COMBINER_COLOR_ICW,specular?0x05200000:0x04200000);
            method(NV097_SET_COMBINER_ALPHA_ICW,specular?0x15200000:0x14200000);
            for(unsigned flat=0;flat<2;flat++) {
                method(NV097_SET_SHADE_MODE,flat?NV097_SET_SHADE_MODE_V_FLAT:NV097_SET_SHADE_MODE_V_SMOOTH);
                for(unsigned back=0;back<2;back++)for(unsigned enabled=0;enabled<2;enabled++) {
                    method(NV097_SET_FRONT_FACE,back?NV097_SET_FRONT_FACE_V_CCW:NV097_SET_FRONT_FACE_V_CW);
                    method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,enabled);method(NV097_CLEAR_SURFACE,0xf0);
                    method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
                    uint32_t expected=specular?(back && enabled?0xffff00ff:0xff0000ff):(back && enabled?0xff00ff00:0xffff0000);
                    if(!program && back && enabled)expected=0xff000000;
                    assert(BRIDGE_MEM32(s_gpu.color_base)==expected);
                }
            }
        }
    }
    const float back_colors[6][4]={{0,1,0,1},{0,0,1,1},{1,1,1,1},{1,0,1,1},{1,1,0,1},{0,1,1,1}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80060000u),back_colors,sizeof back_colors);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+7*4,0x60000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+7*4,0x1042);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+8*4,0x60030);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+8*4,0x1042);
    method(NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_FLAT);method(NV097_SET_FRONT_FACE,NV097_SET_FRONT_FACE_V_CCW);method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,1);
    for(unsigned program=0;program<2;program++)for(unsigned specular=0;specular<2;specular++)for(unsigned first=0;first<2;first++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,program?2:0);method(NV097_SET_PROVOKING_VERTEX,first);
        method(NV097_SET_COMBINER_COLOR_ICW,specular?0x05200000:0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,specular?0x15200000:0x14200000);
        method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
        uint32_t expected=specular?(first?0xffff00ff:0xff00ffff):(first?0xff00ff00:0xffffffff);
        if(!program)expected=0xff000000;
        assert(BRIDGE_MEM32(s_gpu.color_base)==expected);
    }
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);method(NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_SMOOTH);
    method(NV097_SET_COLOR_CLEAR_VALUE,0xff101010);method(NV097_SET_COMBINER_COLOR_ICW,0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    const float clip_scale[4]={4,-4,1,0},clip_offset[4]={4,4,0,0};nv2a_vsh_set_constant(58,clip_scale);nv2a_vsh_set_constant(59,clip_offset);
    struct {float x,y,z,w;uint32_t color;} clipped[3]={{0,0,0,-1,0xffff0000},{7,0,0,1,0xffff0000},{0,7,0,1,0xffff0000}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),clipped,sizeof clipped);
    /* The clipped positive-W polygon has reversed screen winding relative
       to the original mixed-W triangle. Select its back face after clipping. */
    uint32_t reference[64],actual[64];
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned i=0;i<5;i++) {unsigned attribute=i==0?0:(!pass && i<=2?6+i:inputs[i]);uint32_t instruction[4]={0,(1u<<21)|(attribute<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(outputs[i]<<3)|(i==4)};nv2a_vsh_set_instruction(i,instruction);}
        method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,pass);method(NV097_SET_FRONT_FACE,NV097_SET_FRONT_FACE_V_CW);
        method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
        memcpy(pass?actual:reference,(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof reference);
    }
    unsigned covered=0;for(unsigned i=0;i<64;i++)covered+=reference[i]!=0xff101010u;
    if(!covered || memcmp(reference,actual,sizeof reference)) {fprintf(stderr,"[BACK CLIP] covered=%u\n",covered);for(unsigned i=0;i<64;i++)if(reference[i]!=actual[i])fprintf(stderr,"%u %08x %08x\n",i,reference[i],actual[i]);}
    assert(covered && !memcmp(reference,actual,sizeof reference));
    fprintf(stderr,"[TWO SIDED COLORS] program/unlit fixed front/back diffuse/specular, face convention, flat/smooth, first/last provoking and enable state passed\n");
}
static void lighting_scalar_for_test(uint32_t command,float value) {
    uint32_t bits;memcpy(&bits,&value,4);method(command,bits);
}
static void lighting_draw_for_test(uint32_t pixels[64]) {
    method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    memcpy(pixels,(const void *)(xbox_GetMemoryOffset()+s_gpu.color_base),64*4);
}
static void verify_fixed_lighting_coefficients(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    method(NV097_SET_LIGHTING_ENABLE,1);method(NV097_SET_MATERIAL_ALPHA,0x3f800000);
    method(NV097_SET_VERTEX_DATA4F_M+2*16,0);method(NV097_SET_VERTEX_DATA4F_M+2*16+4,0);method(NV097_SET_VERTEX_DATA4F_M+2*16+8,0x3f800000);
    vertices(1.0f/16777215.0f);method(NV097_SET_LIGHT_ENABLE_MASK,1);method(NV097_SET_LIGHT_CONTROL,0);
    method(NV097_SET_LIGHT_INFINITE_DIRECTION,0);method(NV097_SET_LIGHT_INFINITE_DIRECTION+4,0);method(NV097_SET_LIGHT_INFINITE_DIRECTION+8,0x3f800000);
    const float cases[][5]={{0,0,1,.5f,.5f},{-.3f,.5f,.4f,.5f,.30769231f},{.1f,-1,.5f,.5f,1},{.1f,-1,.25f,.5f,0},{-.7f,0,1,.5f,0},{.2f,.4f,.6f,0,.33333333f},{-.2f,0,0,.5f,1}};
    for(unsigned test=0;test<sizeof cases/sizeof cases[0];test++) {
        uint32_t expected[64],actual[64];
        for(unsigned pass=0;pass<2;pass++) {
            method(NV097_SET_SPECULAR_ENABLE,pass);
            for(unsigned c=0;c<3;c++) {
                lighting_scalar_for_test(NV097_SET_LIGHT_AMBIENT_COLOR+c*4,pass?0:cases[test][4]);
                lighting_scalar_for_test(NV097_SET_LIGHT_DIFFUSE_COLOR+c*4,0);
                lighting_scalar_for_test(NV097_SET_LIGHT_SPECULAR_COLOR+c*4,pass?1:0);
                lighting_scalar_for_test(NV097_SET_SPECULAR_PARAMS+c*4,cases[test][c]);
            }
            method(NV097_SET_LIGHT_INFINITE_HALF_VECTOR,0);method(NV097_SET_LIGHT_INFINITE_HALF_VECTOR+4,0);
            lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_HALF_VECTOR+8,cases[test][3]);
            lighting_draw_for_test(pass?actual:expected);
        }
        assert((expected[0]&0xffffff)!=0 || cases[test][4]==0);
        assert((expected[0]&0xffffff)==0 || cases[test][4]!=0);
        assert(!memcmp(actual,expected,sizeof actual));
    }
    method(NV097_SET_SPECULAR_ENABLE,0);
    const float spot_cases[][5]={{0,0,1,-.1f,0},{0,0,1,1.1f,1},{-.1f,.5f,.2f,.25f,.46153846f},{-.7f,0,1,.5f,0},{0,0,0,.5f,1},{0,0,-1,.5f,0}};
    for(unsigned light=0;light<8;light+=7) {
        unsigned offset=light*128;
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_RANGE+offset,1000);
        method(NV097_SET_LIGHT_LOCAL_POSITION+offset,0);method(NV097_SET_LIGHT_LOCAL_POSITION+offset+4,0);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_POSITION+offset+8,10);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset,1);
        method(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset+4,0);method(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset+8,0);
        for(unsigned c=0;c<3;c++) {method(NV097_SET_LIGHT_SPOT_DIRECTION+offset+c*4,0);method(NV097_SET_LIGHT_DIFFUSE_COLOR+offset+c*4,0);}
        for(unsigned test=0;test<sizeof spot_cases/sizeof spot_cases[0];test++) {
            uint32_t expected[64],actual[64];
            for(unsigned pass=0;pass<2;pass++) {
                method(NV097_SET_LIGHT_ENABLE_MASK,(pass?3u:2u)<<(light*2));
                for(unsigned c=0;c<3;c++) {
                    lighting_scalar_for_test(NV097_SET_LIGHT_AMBIENT_COLOR+offset+c*4,pass?1:spot_cases[test][4]);
                    lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_FALLOFF+offset+c*4,spot_cases[test][c]);
                }
                lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_DIRECTION+offset+12,spot_cases[test][3]);
                lighting_draw_for_test(pass?actual:expected);
            }
            assert((expected[0]&0xffffff)!=0 || spot_cases[test][4]==0);
            assert((expected[0]&0xffffff)==0 || spot_cases[test][4]!=0);
            assert(!memcmp(actual,expected,sizeof actual));
        }
    }
    fprintf(stderr,"[LIGHTING COEFFICIENTS] rational specular thresholds/denominators and spotlight cones/falloff on first/last lights passed\n");
}
static void verify_fixed_two_sided_lighting(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    method(NV097_SET_LIGHTING_ENABLE,1);method(NV097_SET_NORMALIZATION_ENABLE,1);
    vertices(1.0f/16777215.0f);
    for(unsigned attribute=3;attribute<=8;attribute++)for(unsigned c=0;c<4;c++)
        lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+attribute*16+c*4,c==3?.625f:attribute==3?.25f:attribute==4?.125f:.875f);
    const float light_color[3][3]={{.03125f,.0625f,.09375f},{.125f,.25f,.375f},{.25f,.125f,.0625f}};
    for(unsigned light=0;light<8;light+=7)for(unsigned mode=1;mode<=3;mode++)
    for(unsigned source=0;source<3;source++)for(unsigned separate=0;separate<2;separate++) {
        uint32_t expected[64],actual[64];unsigned offset=light*128;
        method(NV097_SET_LIGHT_ENABLE_MASK,mode<<(light*2));
        method(NV097_SET_SPECULAR_ENABLE,1);method(NV097_SET_LIGHT_CONTROL,separate?NV097_SET_LIGHT_CONTROL_SEPARATE_SPECULAR:0);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_RANGE+offset,1000);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_POSITION+offset,0);lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_POSITION+offset+4,0);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_POSITION+offset+8,10);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset,1);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset+4,0);lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_ATTENUATION+offset+8,0);
        for(unsigned c=0;c<3;c++) {
            lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_DIRECTION+offset+c*4,c==2?1:0);
            lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_HALF_VECTOR+offset+c*4,c==2?.5f:0);
            lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_DIRECTION+offset+c*4,0);
            lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_FALLOFF+offset+c*4,c==2?1:0);
        }
        lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_DIRECTION+offset+12,.75f);
        for(unsigned pass=0;pass<2;pass++) {
            method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,pass);
            method(NV097_SET_FRONT_FACE,pass?NV097_SET_FRONT_FACE_V_CCW:NV097_SET_FRONT_FACE_V_CW);
            method(NV097_SET_COLOR_MATERIAL,(source*0x55u)<<(pass?8:0));
            for(unsigned c=0;c<3;c++) {
                lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+2*16+c*4,c==2?(pass?-1:1):0);
                lighting_scalar_for_test(NV097_SET_SCENE_AMBIENT_COLOR+c*4,pass?0:.0625f);
                lighting_scalar_for_test(NV097_SET_MATERIAL_EMISSION+c*4,.25f);
                lighting_scalar_for_test(NV097_SET_BACK_SCENE_AMBIENT_COLOR+c*4,.0625f);
                lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_EMISSION+c*4,.25f);
                lighting_scalar_for_test(NV097_SET_SPECULAR_PARAMS+c*4,c==2?1:0);
                lighting_scalar_for_test(NV097_SET_SPECULAR_PARAMS_BACK+c*4,c==2?1:0);
                for(unsigned product=0;product<3;product++) {
                    lighting_scalar_for_test(NV097_SET_LIGHT_AMBIENT_COLOR+offset+product*12+c*4,pass?0:light_color[product][c]);
                    lighting_scalar_for_test(NV097_SET_BACK_LIGHT_AMBIENT_COLOR+light*64+product*12+c*4,light_color[product][c]);
                }
            }
            /* Local/spot consume the original second coefficient triplet. */
            for(unsigned c=3;c<6;c++) {
                lighting_scalar_for_test(NV097_SET_SPECULAR_PARAMS+c*4,c==5?1:0);
                lighting_scalar_for_test(NV097_SET_SPECULAR_PARAMS_BACK+c*4,c==5?1:0);
            }
            lighting_scalar_for_test(NV097_SET_MATERIAL_ALPHA,pass?.875f:.375f);
            lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_ALPHA,.375f);
            if(pass && light==7 && mode==3 && source==2 && separate==1) {
                FILE *saved=tmpfile();assert(saved && nv2a_save_checkpoint(saved));rewind(saved);
                struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
                pg->back_material_alpha=0;pg->back_material_sources=0;memset(pg->back_specular_params,0,sizeof pg->back_specular_params);
                memset(pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BR_AMB],0,16);memset(pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BCM_COL],0,16);
                for(unsigned product=0;product<3;product++)memset(pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BAMB+7*6+product],0,16);
                void *loaded=nv2a_prepare_checkpoint(saved);assert(loaded);nv2a_apply_checkpoint(loaded);free(loaded);fclose(saved);
            }
            lighting_draw_for_test(pass?actual:expected);
        }
        assert(expected[0]!=0 && !memcmp(actual,expected,sizeof actual));
    }
    fprintf(stderr,"[FIXED TWO SIDED LIGHTING] independent back LT/material/source/alpha with reversed normals, local/directional/spot first/last lights and specular separation passed\n");
}
static void verify_fixed_specular_six(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,0);method(NV097_SET_NORMALIZATION_ENABLE,0);
    vertices(1.0f/16777215.0f);
    for(unsigned row=0;row<16;row++){float v[4]={0};if(row<4 || row>=12)v[row%4]=1;if(row==11)v[3]=1;nv2a_vsh_set_constant(row,v);}
    const float axis[4]={.8660254037844386f,0,.5f,1};
    for(unsigned c=0;c<4;c++)lighting_scalar_for_test(NV097_SET_EYE_POSITION+c*4,axis[c]);
    for(unsigned c=0;c<3;c++) {
        lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_DIRECTION+c*4,axis[c]);
        lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_HALF_VECTOR+c*4,axis[c]);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_POSITION+c*4,axis[c]);
        lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_ATTENUATION+c*4,c==0?1:0);
        lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_DIRECTION+c*4,0);
        lighting_scalar_for_test(NV097_SET_SCENE_AMBIENT_COLOR+c*4,0);lighting_scalar_for_test(NV097_SET_BACK_SCENE_AMBIENT_COLOR+c*4,0);
        lighting_scalar_for_test(NV097_SET_MATERIAL_EMISSION+c*4,0);lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_EMISSION+c*4,0);
        for(unsigned product=0;product<3;product++) {
            lighting_scalar_for_test(NV097_SET_LIGHT_AMBIENT_COLOR+product*12+c*4,product==2?1:0);
            lighting_scalar_for_test(NV097_SET_BACK_LIGHT_AMBIENT_COLOR+product*12+c*4,product==2?1:0);
        }
        lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+4*16+c*4,0);
    }
    lighting_scalar_for_test(NV097_SET_LIGHT_LOCAL_RANGE,1000);lighting_scalar_for_test(NV097_SET_LIGHT_SPOT_DIRECTION+12,1);
    lighting_scalar_for_test(NV097_SET_MATERIAL_ALPHA,1);lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_ALPHA,1);
    lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+3*16+12,1);
    const float second[][4]={{0,0,1,.25f},{0,0,2,.125f},{.25f,0,1,.5f},{0,4,1,.125f}};
    for(unsigned mode=1;mode<=3;mode++)for(unsigned local=0;local<2;local++)
    for(unsigned face=0;face<2;face++)for(unsigned test=0;test<4;test++) {
        uint32_t expected[64],actual[64];float result=mode==1 && !local?.5f:second[test][3];
        method(NV097_SET_LIGHT_ENABLE_MASK,mode);method(NV097_SET_LIGHT_CONTROL,local?NV097_SET_LIGHT_CONTROL_LOCALEYE:0);
        method(NV097_SET_COLOR_MATERIAL,0);
        for(unsigned pass=0;pass<2;pass++) {
            method(NV097_SET_LIGHTING_ENABLE,pass);method(NV097_SET_SPECULAR_ENABLE,pass);
            method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,pass && face);
            method(NV097_SET_FRONT_FACE,pass && face?NV097_SET_FRONT_FACE_V_CCW:NV097_SET_FRONT_FACE_V_CW);
            for(unsigned c=0;c<3;c++) {
                lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+3*16+c*4,result);
                lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+2*16+c*4,c==2?(face?-1:1):0);
            }
            for(unsigned side=0;side<2;side++)for(unsigned c=0;c<6;c++) {
                float value=c==2?(side==face?1:2):c<3?0:side==face?second[test][c-3]:c==3?-1:c==5?1:0;
                lighting_scalar_for_test((side?NV097_SET_SPECULAR_PARAMS_BACK:NV097_SET_SPECULAR_PARAMS)+c*4,value);
            }
            if(pass && mode==3 && local && face && test==2) {
                FILE *saved=tmpfile();assert(saved && nv2a_save_checkpoint(saved));rewind(saved);
                memset(nv2a_get_state()->pgraph.back_specular_params,0,24);
                void *loaded=nv2a_prepare_checkpoint(saved);assert(loaded);nv2a_apply_checkpoint(loaded);free(loaded);fclose(saved);
            }
            lighting_draw_for_test(pass?actual:expected);
        }
        if(memcmp(actual,expected,sizeof actual))fprintf(stderr,"[SIX SPECULAR] mode=%u local=%u face=%u case=%u expected=%08x actual=%08x\n",mode,local,face,test,expected[0],actual[0]);
        assert(!memcmp(actual,expected,sizeof actual));
    }
    fprintf(stderr,"[SIX SPECULAR] absolute A/B/C versus D/E/F mode and local-eye branches, both faces and restored back coefficients passed\n");
}
static void verify_fixed_material_products(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);
    for(unsigned row=0;row<16;row++){float value[4]={0};value[row%4]=1;nv2a_vsh_set_constant(row,value);}
    vertices(1.0f/16777215.0f);method(NV097_SET_LIGHT_ENABLE_MASK,1);method(NV097_SET_SPECULAR_ENABLE,0);
    for(unsigned c=0;c<3;c++) {
        lighting_scalar_for_test(NV097_SET_SCENE_AMBIENT_COLOR+c*4,.125f);lighting_scalar_for_test(NV097_SET_BACK_SCENE_AMBIENT_COLOR+c*4,.125f);
        lighting_scalar_for_test(NV097_SET_MATERIAL_EMISSION+c*4,.25f);lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_EMISSION+c*4,.25f);
        lighting_scalar_for_test(NV097_SET_LIGHT_AMBIENT_COLOR+c*4,.125f);lighting_scalar_for_test(NV097_SET_BACK_LIGHT_AMBIENT_COLOR+c*4,.125f);
        lighting_scalar_for_test(NV097_SET_LIGHT_DIFFUSE_COLOR+c*4,0);lighting_scalar_for_test(NV097_SET_BACK_LIGHT_DIFFUSE_COLOR+c*4,0);
        lighting_scalar_for_test(NV097_SET_LIGHT_SPECULAR_COLOR+c*4,0);lighting_scalar_for_test(NV097_SET_BACK_LIGHT_SPECULAR_COLOR+c*4,0);
    }
    lighting_scalar_for_test(NV097_SET_MATERIAL_ALPHA,1);lighting_scalar_for_test(NV097_SET_BACK_MATERIAL_ALPHA,1);
    lighting_scalar_for_test(NV097_SET_LIGHT_INFINITE_DIRECTION+8,1);lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+2*16+8,1);
    /* Original scene-product and light-product results for emissive/ambient
       material, vertex diffuse=.25, vertex specular=.5; scene=.125,
       material factor=.25, light ambient=.125. */
    const float result[3][3]={{.25f,.21875f,.3125f},{.3125f,.34375f,.4375f},{.375f,.59375f,.6875f}};
    for(unsigned e=0;e<3;e++)for(unsigned a=0;a<3;a++)for(unsigned back=0;back<2;back++) {
        uint32_t expected[64],actual[64];method(NV097_SET_LIGHTING_ENABLE,0);method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,0);method(NV097_SET_FRONT_FACE,NV097_SET_FRONT_FACE_V_CW);
        for(unsigned c=0;c<4;c++)lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+3*16+c*4,c==3?1:result[e][a]);
        /* Use a disabled diffuse array so both reference and lighting inputs
           come from the declared current vertex register. */
        method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,0);
        lighting_draw_for_test(expected);
        method(NV097_SET_LIGHTING_ENABLE,1);method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,back);method(NV097_SET_FRONT_FACE,back?NV097_SET_FRONT_FACE_V_CCW:NV097_SET_FRONT_FACE_V_CW);
        method(NV097_SET_COLOR_MATERIAL,(e|(a<<2))<<(back?8:0));
        for(unsigned c=0;c<4;c++){lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+3*16+c*4,c==3?1:.25f);lighting_scalar_for_test(NV097_SET_VERTEX_DATA4F_M+4*16+c*4,c==3?1:.5f);}
        lighting_draw_for_test(actual);assert(!memcmp(expected,actual,sizeof actual));
    }
    fprintf(stderr,"[FIXED MATERIAL PRODUCTS] all nine scene/emission/ambient bindings have independent absolute scalar outputs on both faces\n");
}
static void verify_fixed_skinning(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1432);
    for(unsigned row=0;row<4;row++) {float value[4]={0};value[row]=1;nv2a_vsh_set_constant(row,value);}
    skin_matrix_for_test(NV097_SET_PROJECTION_MATRIX,0,100,1);
    const float input_weights[4]={0.25f,0.5f,0.75f,1.0f};
    for(unsigned c=0;c<4;c++) {uint32_t bits;memcpy(&bits,input_weights+c,4);method(NV097_SET_VERTEX_DATA4F_M+16+c*4,bits);}
    method(NV097_SET_VERTEX_DATA4F_M+2*16,0);method(NV097_SET_VERTEX_DATA4F_M+2*16+4,0);
    method(NV097_SET_VERTEX_DATA4F_M+2*16+8,0x3f800000);method(NV097_SET_VERTEX_DATA4F_M+2*16+12,0);
    method(NV097_SET_MATERIAL_ALPHA,0x3f800000);method(NV097_SET_LIGHT_ENABLE_MASK,1);
    for(unsigned c=0;c<3;c++)method(NV097_SET_LIGHT_DIFFUSE_COLOR+c*4,0x3f800000);
    method(NV097_SET_LIGHT_INFINITE_DIRECTION,0);method(NV097_SET_LIGHT_INFINITE_DIRECTION+4,0);method(NV097_SET_LIGHT_INFINITE_DIRECTION+8,0x3f800000);
    for(unsigned mode=1;mode<=6;mode++) {
        float total=0,weighted_x=0,weighted_normal=0,remaining=1;unsigned count=(mode+3)/2;
        for(unsigned bone=0;bone<count;bone++) {
            float weight=(mode&1) && bone==count-1?remaining:input_weights[bone];remaining-=weight;
            total+=weight;weighted_x+=weight*bone*0.5f;weighted_normal+=weight*(bone+1)*0.125f;
        }
        for(unsigned lighting=0;lighting<2;lighting++) {
            uint32_t reference[64];method(NV097_SET_LIGHTING_ENABLE,lighting);
            for(unsigned active=0;active<2;active++) {
                for(unsigned bone=0;bone<4;bone++) {
                    skin_matrix_for_test(NV097_SET_MODEL_VIEW_MATRIX,bone,active?bone*0.5f:0,1);
                    skin_matrix_for_test(NV097_SET_INVERSE_MODEL_VIEW_MATRIX,bone,0,active?(bone+1)*0.125f:weighted_normal);
                }
                struct {float x,y,z,w;uint32_t color;} data[3]={{0,0,1,1,0xffff0000},{6,0,1,1,0xffff0000},{0,6,1,1,0xffff0000}};
                if(!active)for(unsigned v=0;v<3;v++)data[v].x+=weighted_x/total;
                memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
                method(NV097_SET_SKIN_MODE,active?mode:0);method(NV097_CLEAR_SURFACE,0xf0);
                method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
                const void *pixels=(const void *)(xbox_GetMemoryOffset()+s_gpu.color_base);
                if(!active)memcpy(reference,pixels,sizeof reference);else assert(!memcmp(reference,pixels,sizeof reference));
            }
        }
    }
    fprintf(stderr,"[SKINNING] six modes, generated and explicit weights, four original matrix pairs, position and lit normal pixels passed\n");
}
static void verify_texture_addressing(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    method(NV097_SET_CONTEXT_DMA_A,0x100);
    const uint32_t texels[4]={0xff112233,0xff446688,0xff7799aa,0xffbbccdd};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),texels,sizeof texels);
    method(NV097_SET_TEXTURE_OFFSET,0x80000);
    method(NV097_SET_TEXTURE_FORMAT,(0x12<<8)|(1<<16));
    method(NV097_SET_TEXTURE_CONTROL1,8<<16);
    method(NV097_SET_TEXTURE_IMAGE_RECT,(2<<16)|2);
    method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_TEXTURE_FILTER,0x01010000);
    method(NV097_SET_SHADER_STAGE_PROGRAM,1);
    method(NV097_SET_COMBINER_COLOR_ICW,0x08200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x18200000);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+9*4,0x50000);
    method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+9*4,0x1042);
    const int coords[]={-5,-2,-1,0,1,2,5};
    const unsigned wrap[]={1,0,1,0,1,0,1},mirror[]={0,1,0,0,1,1,1},clamp[]={0,0,0,0,1,1,1},once[]={1,1,0,0,1,1,1};
    const unsigned *indices[]={wrap,mirror,clamp,NULL,once};
    for(unsigned mode=1;mode<=5;mode++) {
        for(unsigned border=0;border<5;border++) {
            const uint32_t colors[]={0,0xff000000,0xffffffff,0xff3399cc,0x7fbb6611};
            method(NV097_SET_TEXTURE_ADDRESS,mode|(3<<8));
            method(NV097_SET_TEXTURE_BORDER_COLOR,colors[border]);
            for(unsigned i=0;i<7;i++) {
                uint32_t expected=mode==4 && (coords[i]<0 || coords[i]>=2)?colors[border]:texels[mode==4?(unsigned)coords[i]:indices[mode-1][i]];
                uint32_t sampled=0;assert(sample_tex(&s_gpu.texs[0],0,(uint32_t)coords[i],0,&sampled) && sampled==expected);
                float coordinates[3][4];for(unsigned v=0;v<3;v++) {coordinates[v][0]=coords[i]+0.5f;coordinates[v][1]=0.5f;coordinates[v][2]=0;coordinates[v][3]=1;}
                memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coordinates,sizeof coordinates);
                method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
                nv2a_pb_checkpoint_publish();
                assert(BRIDGE_MEM32(s_gpu.color_base)==expected);
            }
        }
    }
    fprintf(stderr,"[TEXTURE ADDRESS] signed taps, wrap/mirror/clamp/border/mirror-once and standing border updates passed\n");
}
struct RasterVertexForTest { float x,y,z,w;uint32_t color; };
static void raster_vertices_for_test(const struct RasterVertexForTest *data,unsigned count) {
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,count*sizeof *data);
}
static void raster_mode_for_test(unsigned front,unsigned back) {
    const unsigned mode[]={0x1b02,0x1b00,0x1b01};
    method(NV097_SET_FRONT_POLYGON_MODE,mode[front]);method(NV097_SET_BACK_POLYGON_MODE,mode[back]);
}
static void raster_draw_for_test(unsigned primitive,unsigned start,unsigned count) {
    method(NV097_SET_BEGIN_END,primitive);method(NV097_DRAW_ARRAYS,start|((count-1)<<24));method(NV097_SET_BEGIN_END,0);
}
static FILE *polygon_pixels_file;
static void raster_pixels_for_test(uint32_t *pixels) {
    nv2a_pb_checkpoint_publish();memcpy(pixels,(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),64*64*4);
    if(polygon_pixels_file)assert(fwrite(pixels,64*64*4,1,polygon_pixels_file)==1);
}
static float raster_depth_for_test(unsigned mode) {
    if(!hw_enabled()){nv2a_pb_checkpoint_publish();return (float)(BRIDGE_MEM32(0x80060000u+(mode==0?16u:0u)*4)>>8);}
    nv2a_pb_checkpoint_publish();FILE *dump=tmpfile();assert(dump && hw_save_depths(dump));rewind(dump);
    uint32_t count;assert(fread(&count,4,1,dump)==1);
    for(unsigned i=0;i<count;i++) {
        HwDepthCheckpoint header;assert(fread(&header,sizeof header,1,dump)==1);
        if(header.address==0x80060000u){float depth;if(mode==0)assert(!fseek(dump,(long)header.width*header.scale*header.scale*8,SEEK_CUR));assert(fread(&depth,4,1,dump)==1);fclose(dump);return depth*(float)header.normalization;}
        assert(!fseek(dump,(long)header.bytes,SEEK_CUR));
    }
    fclose(dump);assert(!"actual depth surface missing");return -1;
}
static void verify_polygon_raster_consumers(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_POINT_SIZE,8);method(NV097_SET_LINE_WIDTH,8);method(NV097_SET_EDGE_FLAG,1);method(NV097_SET_FRONT_FACE,0x900);
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    uint32_t reference[64*64],actual[64*64];
    for(unsigned route=0;route<2;route++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route?0:2);
        struct RasterVertexForTest triangle[3]={{16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{16,48,1,1,0xffff0000}};
        raster_vertices_for_test(triangle,3);
        raster_mode_for_test(2,2);method(NV097_SET_LINE_WIDTH,8);method(NV097_CLEAR_SURFACE,0xf0);
        unsigned line_mark=polygon_native_draw_calls;
        raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);
        fprintf(stderr,"[POLYGON LINE SUBMISSIONS] route=%u physical=%u calls=%u\n",route,(unsigned)d3d8_RenderScale(),polygon_native_draw_calls-line_mark);
        raster_mode_for_test(0,0);
        /* Polygon points use the same point-size and generated coverage as points. */
        for(unsigned size=1;size<=4;size++) {
            method(NV097_SET_POINT_SIZE,size*8);raster_mode_for_test(0,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,3);raster_pixels_for_test(reference);
            unsigned red=0;for(unsigned i=0;i<64*64;i++)red+=reference[i]==0xffff0000;if(red!=3*size*size)fprintf(stderr,"[POINT REFERENCE] route=%u size=%u red=%u\n",route,size,red);assert(red==3*size*size);
            raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        method(NV097_SET_POINT_SIZE,8);
        const uint32_t vertex_colors[]={0xffff0000,0xff00ff00,0xff0000ff};
        for(unsigned first=0;first<2;first++)for(unsigned mode=1;mode<=2;mode++) {
            method(NV097_SET_PROVOKING_VERTEX,first);method(NV097_SET_SHADE_MODE,0x1d00);
            struct RasterVertexForTest uniform[6];unsigned n=mode==1?3:6;
            for(unsigned i=0;i<n;i++){uniform[i]=triangle[mode==1?i:(i/2+i%2)%3];uniform[i].color=vertex_colors[first?0:2];}
            raster_mode_for_test(0,0);raster_vertices_for_test(uniform,n);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(mode==1?1:2,0,n);raster_pixels_for_test(reference);
            for(unsigned i=0;i<3;i++)triangle[i].color=vertex_colors[i];
            raster_vertices_for_test(triangle,3);raster_mode_for_test(mode,mode);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
            for(unsigned i=0;i<3;i++)triangle[i].color=0xffff0000;
        }
        method(NV097_SET_PROVOKING_VERTEX,0);
        /* Per-vertex boundary flags are captured by every original producer. */
        for(unsigned producer=0;producer<4;producer++)for(unsigned point=0;point<2;point++)for(unsigned mask=0;mask<8;mask++) {
            struct RasterVertexForTest segments[6];unsigned used=0;
            for(unsigned i=0;i<3;i++)if(mask&(1u<<i)){segments[used++]=triangle[i];if(!point)segments[used++]=triangle[(i+1)%3];}
            raster_mode_for_test(0,0);method(NV097_SET_EDGE_FLAG,1);method(NV097_CLEAR_SURFACE,0xf0);
            if(used){raster_vertices_for_test(segments,used);raster_draw_for_test(point?1:2,0,used);}raster_pixels_for_test(reference);
            raster_vertices_for_test(triangle,3);raster_mode_for_test(point?1:2,point?1:2);method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);
            for(unsigned i=0;i<3;i++) {
                method(NV097_SET_EDGE_FLAG,(mask>>i)&1);
                if(producer==0)method(NV097_DRAW_ARRAYS,i);
                else if(producer==1)method(NV097_ARRAY_ELEMENT32,i);
                else if(producer==2)for(unsigned j=0;j<5;j++)method(NV097_INLINE_ARRAY,((uint32_t *)&triangle[i])[j]);
                else {method(NV097_SET_VERTEX_DATA4UB+3*4,0xff0000ff);for(unsigned j=0;j<4;j++)method(NV097_SET_VERTEX4F+j*4,((uint32_t *)&triangle[i])[j]);}
            }
            method(NV097_SET_BEGIN_END,0);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        method(NV097_SET_EDGE_FLAG,1);
        /* Whole quads and polygon fans retain only their real boundary. */
        struct RasterVertexForTest polygon[5]={{16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{48,40,1,1,0xffff0000},{32,48,1,1,0xffff0000},{16,40,1,1,0xffff0000}};
        for(unsigned count=4;count<=5;count++)for(unsigned point=0;point<2;point++) {
            struct RasterVertexForTest segments[10];for(unsigned i=0;i<count;i++){segments[point?i:i*2]=polygon[i];if(!point)segments[i*2+1]=polygon[(i+1)%count];}
            raster_mode_for_test(0,0);raster_vertices_for_test(segments,count*(point?1:2));method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(point?1:2,0,count*(point?1:2));raster_pixels_for_test(reference);
            raster_vertices_for_test(polygon,count);raster_mode_for_test(point?1:2,point?1:2);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(count==4?8:10,0,count);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        /* Polygon clipping precedes mode selection: clipping creates real
         * boundary vertices/edges rather than merely trimming original lines. */
        const unsigned clip_widths[]={8,24,128};
        for(unsigned cw=0;cw<sizeof clip_widths/sizeof *clip_widths;cw++) {
        method(NV097_SET_LINE_WIDTH,clip_widths[cw]);
        for(unsigned quad=0;quad<2;quad++)for(unsigned point=0;point<2;point++) {
            const struct RasterVertexForTest clipped[5]={{0,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{24,64,1,1,0xffff0000},{8,64,1,1,0xffff0000},{0,48,1,1,0xffff0000}};
            const struct RasterVertexForTest clipped_quad[4]={{0,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{48,64,1,1,0xffff0000},{0,64,1,1,0xffff0000}};
            const struct RasterVertexForTest *boundary=quad?clipped_quad:clipped;unsigned count=quad?4:5;
            struct RasterVertexForTest segments[10];for(unsigned i=0;i<count;i++){segments[point?i:i*2]=boundary[i];if(!point)segments[i*2+1]=boundary[(i+1)%count];}
            raster_mode_for_test(0,0);raster_vertices_for_test(segments,count*(point?1:2));method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(point?1:2,0,count*(point?1:2));raster_pixels_for_test(reference);
            struct RasterVertexForTest source[4]={{-16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{16,80,1,1,0xffff0000},{-16,80,1,1,0xffff0000}};
            if(quad)source[2].x=48;
            raster_vertices_for_test(source,quad?4:3);raster_mode_for_test(point?1:2,point?1:2);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(quad?8:5,0,quad?4:3);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        }
        method(NV097_SET_LINE_WIDTH,8);
        /* Reversing one overlapping primitive makes both faces visible.
         * Sequential uniform draws establish the required original blend order. */
        struct RasterVertexForTest overlap[6];for(unsigned i=0;i<3;i++){overlap[i]=triangle[i];overlap[i].color=0x80ff0000;overlap[3+i]=triangle[2-i];overlap[3+i].color=0x8000ff00;}
        raster_vertices_for_test(overlap,6);method(NV097_SET_BLEND_ENABLE,1);method(NV097_SET_BLEND_FUNC_SFACTOR,0x302);method(NV097_SET_BLEND_FUNC_DFACTOR,0x303);
        const unsigned polygon_widths[]={8,16,24,128,136};
        for(unsigned pw=0;pw<sizeof polygon_widths/sizeof *polygon_widths;pw++) {
        method(NV097_SET_LINE_WIDTH,polygon_widths[pw]);
        for(unsigned front=0;front<3;front++)for(unsigned back=0;back<3;back++) {
            method(NV097_CLEAR_SURFACE,0xf0);raster_mode_for_test(front,front);raster_draw_for_test(5,0,3);raster_mode_for_test(back,back);raster_draw_for_test(5,3,3);raster_pixels_for_test(reference);
            method(NV097_CLEAR_SURFACE,0xf0);raster_mode_for_test(front,back);raster_draw_for_test(5,0,6);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        }
        method(NV097_SET_LINE_WIDTH,8);
        method(NV097_SET_BLEND_ENABLE,0);raster_mode_for_test(0,0);
        /* Aliased width is nearest integer; the major axis selects contiguous
         * native unit lines. Compare actual wide output with those original lines. */
        const unsigned widths[]={0,1,4,7,8,9,11,12,16,24,56,248,256,264,504};
        const float lines[][4]={{8,32,56,32},{32,8,32,56},{8,16,56,36},{16,8,36,56}};
        for(unsigned line=0;line<4;line++)for(unsigned w=0;w<sizeof widths/sizeof *widths;w++) {
            unsigned width=(widths[w]+4)/8;if(!width)width=1;
            struct RasterVertexForTest copies[126];
            for(unsigned j=0;j<width;j++)for(unsigned endpoint=0;endpoint<2;endpoint++) {
                copies[j*2+endpoint]=(struct RasterVertexForTest){lines[line][endpoint*2],lines[line][endpoint*2+1],1,1,0xffff0000};
                float offset=j-(width-1)*.5f;if(line==0 || line==2)copies[j*2+endpoint].y+=offset;else copies[j*2+endpoint].x+=offset;
            }
            raster_vertices_for_test(copies,width*2);method(NV097_SET_LINE_WIDTH,8);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(2,0,width*2);raster_pixels_for_test(reference);
            struct RasterVertexForTest endpoints[2]={{lines[line][0],lines[line][1],1,1,0xffff0000},{lines[line][2],lines[line][3],1,1,0xffff0000}};
            raster_vertices_for_test(endpoints,2);method(NV097_SET_LINE_WIDTH,widths[w]);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(2,0,2);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        method(NV097_SET_LINE_WIDTH,8);
    }
    /* Point/line/fill offset enables follow the actual polygon mode;
     * standalone point and line primitives inherit none of those offsets. */
    method(NV097_SET_SURFACE_PITCH,32|(32u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,8<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,8<<16);
    method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000);method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);
    method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x207);
    method(NV097_SET_POLYGON_OFFSET_BIAS,0x40400000);method(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,0);
    struct RasterVertexForTest depth_triangle[3]={{0,.5f,1,1,0xffff0000},{8,.5f,1,1,0xffff0000},{0,8,1,1,0xffff0000}};
    raster_vertices_for_test(depth_triangle,3);
    const unsigned offset_method[]={NV097_SET_POLY_OFFSET_FILL_ENABLE,NV097_SET_POLY_OFFSET_POINT_ENABLE,NV097_SET_POLY_OFFSET_LINE_ENABLE};
    for(unsigned route=0;route<2;route++)for(unsigned mode=0;mode<3;mode++)for(unsigned enabled=0;enabled<3;enabled++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route?0:2);raster_mode_for_test(mode,mode);
        for(unsigned i=0;i<3;i++)method(offset_method[i],i==enabled);
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,(100<<8)|0xa5);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(5,0,3);
        float depth=raster_depth_for_test(mode);if(lroundf(depth)!=(mode==enabled?4:1))fprintf(stderr,"[OFFSET] route=%u mode=%u enabled=%u actual=%g\n",route,mode,enabled,depth);assert(lroundf(depth)==(mode==enabled?4:1));
    }
    for(unsigned primitive=1;primitive<=2;primitive++) {
        for(unsigned i=0;i<3;i++)method(offset_method[i],1);
        method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(primitive,0,primitive);float depth=raster_depth_for_test(primitive);assert(lroundf(depth)==1);
    }
    fprintf(stderr,"[POLYGON RASTER] point sizes, per-vertex boundaries, original quad/polygon edges, all mixed modes and ordered blending; aliased widths through63 on two vertex routes passed\n");
}
static void verify_polygon_stipple_consumers(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_PITCH,256);method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_POINT_SIZE,8);method(NV097_SET_LINE_WIDTH,8);
    uint32_t reference[4096],actual[4096],pattern[32];
    struct RasterVertexForTest quad[6]={{0,0,1,1,0xffff0000},{64,0,1,1,0xffff0000},{0,64,1,1,0xffff0000},{64,0,1,1,0xffff0000},{64,64,1,1,0xffff0000},{0,64,1,1,0xffff0000}};
    raster_vertices_for_test(quad,6);
    for(unsigned route=0;route<2;route++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route?0:2);
        method(NV097_SET_POLYGON_STIPPLE_ENABLE,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,6);raster_pixels_for_test(reference);
        for(unsigned kind=0;kind<5;kind++) {
            for(unsigned row=0;row<32;row++) {
                pattern[row]=kind==0?0:kind==1?~0u:kind==2?0xaaaaaaaa:kind==3?(row&4?0xf0f0f0f0:0x0f0f0f0f):(row<8?0xc0c0ffff:row<16?0x3030ffff:row<24?0x00008cef:0x0f000f0f);
                method(NV097_SET_POLYGON_STIPPLE_PATTERN+row*4,pattern[row]);
            }
            method(NV097_SET_POLYGON_STIPPLE_ENABLE,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,6);raster_pixels_for_test(actual);
            for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){uint32_t expected=(pattern[31-(y&31)]&(1u<<((x&31)^7)))?reference[y*64+x]:s_gpu.clear_color;if(actual[y*64+x]!=expected)fprintf(stderr,"stipple mismatch route=%u kind=%u x=%u y=%u actual=%08x ref=%08x expected=%08x\n",route,kind,x,y,actual[y*64+x],reference[y*64+x],expected);assert(actual[y*64+x]==expected);}
        }
        /* Zero pattern affects filled polygons only; standing enable must not mask standalone points/lines. */
        for(unsigned row=0;row<32;row++)method(NV097_SET_POLYGON_STIPPLE_PATTERN+row*4,0);
        struct RasterVertexForTest line[2]={{8,8,1,1,0xffff0000},{56,48,1,1,0xffff0000}};raster_vertices_for_test(line,2);
        for(unsigned prim=1;hw_enabled() && prim<=2;prim++) {
            method(NV097_SET_POLYGON_STIPPLE_ENABLE,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(prim,0,2);raster_pixels_for_test(reference);
            method(NV097_SET_POLYGON_STIPPLE_ENABLE,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(prim,0,2);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
        }
        raster_vertices_for_test(quad,6);
    }
    method(NV097_SET_POLYGON_STIPPLE_ENABLE,0);
    fprintf(stderr,"[POLYGON STIPPLE] 32-row pattern/enable, asymmetric hardware bit/row order, scale and standalone primitive isolation passed\n");
}
static void verify_polygon_point_params_consumers(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_POINT_PARAMS_ENABLE,1);
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    struct RasterVertexForTest triangle[3]={{12,12,1,1,0xffff0000},{48,12,1,1,0xffff0000},{12,48,1,1,0xffff0000}};raster_vertices_for_test(triangle,3);
    uint32_t reference[4096],actual[4096];
    for(unsigned kind=0;kind<3;kind++) {
        float params[8]={1,kind==2?.01f:0,0,kind==0?2:6,0,0,0,1};
        for(unsigned i=0;i<8;i++)polygon_offset_float(NV097_SET_POINT_PARAMS+i*4,params[i]);
        raster_mode_for_test(0,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,3);raster_pixels_for_test(reference);
        unsigned red=0;for(unsigned i=0;i<4096;i++)red+=reference[i]==0xffff0000;assert(red>3);
        raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
    }
    fprintf(stderr,"[POLYGON POINT PARAMS] original fixed eye-distance/parameter size output matches standalone points\n");
}
static void verify_screen_output_packing_consumers(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_POINT_SIZE,24);raster_mode_for_test(1,1);
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    const float params[8]={1,0,0,2,0,0,0,1};for(unsigned i=0;i<8;i++)polygon_offset_float(NV097_SET_POINT_PARAMS+i*4,params[i]);
    struct RasterVertexForTest triangle[3]={{12,12,1,1,0xffff0000},{48,12,1,1,0xffff0000},{12,48,1,1,0xffff0000}};raster_vertices_for_test(triangle,3);
    const uint32_t texels[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};memcpy((void *)(xbox_GetMemoryOffset()+0x80080000u),texels,sizeof texels);
    method(NV097_SET_CONTEXT_DMA_A,0x100);method(NV097_SET_TEXTURE_OFFSET,0x80000);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_TEXTURE_FORMAT,(6<<8)|(1<<16)|(1<<20)|(1<<24));method(NV097_SET_TEXTURE_ADDRESS,3|(3<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);
    method(NV097_SET_SHADER_STAGE_PROGRAM,1);method(NV097_SET_COMBINER_COLOR_ICW,0x08200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x18200000);
    float coordinates[3][4]={{.75f,.25f,0,1},{.75f,.25f,0,1},{.75f,.25f,0,1}};memcpy((void *)(xbox_GetMemoryOffset()+0x80050000u),coordinates,sizeof coordinates);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+9*4,0x50000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+9*4,0x1042);
    float fogs[3]={.25f,.25f,.25f};memcpy((void *)(xbox_GetMemoryOffset()+0x80051000u),fogs,sizeof fogs);
    method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+5*4,0x51000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+5*4,0x0412);
    method(NV097_SET_FOG_GEN_MODE,6);method(NV097_SET_FOG_MODE,0x804);method(NV097_SET_FOG_COLOR,0xffff0000);
    polygon_offset_float(NV097_SET_FOG_PARAMS,1);polygon_offset_float(NV097_SET_FOG_PARAMS+4,1);
    uint32_t reference[4096],actual[4096];
    for(unsigned fog=0;fog<2;fog++) {
        method(NV097_SET_FOG_ENABLE,fog);method(NV097_SET_POINT_PARAMS_ENABLE,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(reference);
        unsigned colored=0;for(unsigned i=0;i<4096;i++)colored+=reference[i]!=s_gpu.clear_color;assert(colored>0);
        method(NV097_SET_POINT_PARAMS_ENABLE,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(reference,actual,sizeof actual));
    }
    fprintf(stderr,"[SCREEN OUTPUT PACKING] original8-row rawvertexprotocol retains texture and fog across expanded interpreter output structs\n");
}
static void verify_software_point_consumers(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);method(NV097_SET_POINT_PARAMS_ENABLE,0);method(NV097_SET_POINT_SMOOTH_ENABLE,0);
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    method(NV097_SET_CULL_FACE_ENABLE,1);method(NV097_SET_CULL_FACE,0x408);method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,1);
    method(NV097_SET_POLYGON_STIPPLE_ENABLE,1);for(unsigned row=0;row<32;row++)method(NV097_SET_POLYGON_STIPPLE_PATTERN+row*4,0);
    method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x207);
    method(NV097_SET_STENCIL_TEST_ENABLE,1);method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
    method(NV097_SET_STENCIL_MASK,255);method(NV097_SET_STENCIL_FUNC,0x207);method(NV097_SET_STENCIL_FUNC_MASK,255);method(NV097_SET_STENCIL_OP_ZPASS,0x1e02);
    method(NV097_SET_POLY_OFFSET_FILL_ENABLE,1);method(NV097_SET_POLY_OFFSET_POINT_ENABLE,1);polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,1000);
    struct RasterVertexForTest point={4,4,1,1,0xffff0000};raster_vertices_for_test(&point,1);
    const uint32_t color_instruction[4]={0,(1u<<21)|(3u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)};
    const uint32_t point_instruction[4]={0,(1u<<21)|(6u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(6u<<3)|1};
    nv2a_vsh_set_instruction(1,color_instruction);nv2a_vsh_set_instruction(2,point_instruction);
    for(unsigned parameters=0;parameters<2;parameters++)for(unsigned route=0;route<2;route++)for(unsigned size=1;size<=6;size++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route?0:2);method(NV097_SET_POINT_SIZE,parameters?8:size*8);
        method(NV097_SET_POINT_PARAMS_ENABLE,parameters);
        const float params[8]={1,0,0,(float)size-1,0,0,0,1};for(unsigned i=0;i<8;i++)polygon_offset_float(NV097_SET_POINT_PARAMS+i*4,params[i]);
        polygon_offset_float(NV097_SET_VERTEX_DATA4F_M+6*16,(float)size);
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,(100<<8)|0xa5);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(1,0,1);nv2a_pb_checkpoint_publish();
        const uint32_t *colors=(const uint32_t *)(xbox_GetMemoryOffset()+s_gpu.color_base);
        const uint32_t *depth=(const uint32_t *)(xbox_GetMemoryOffset()+0x80030000u);
        unsigned begin=4-size/2,end=begin+size;
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            int inside=x>=begin && x<end && y>=begin && y<end;
            if(colors[y*8+x]!=(inside?0xffff0000:s_gpu.clear_color))fprintf(stderr,"point mismatch route=%u size=%u xy=%u,%u actual=%08x expected=%08x\n",route,size,x,y,colors[y*8+x],inside?0xffff0000:s_gpu.clear_color);
            assert(colors[y*8+x]==(inside?0xffff0000:s_gpu.clear_color));
            assert(depth[y*8+x]==(inside?(1<<8)|0xa6:(100<<8)|0xa5));
        }
    }
    method(NV097_SET_CULL_FACE_ENABLE,0);method(NV097_SET_LIGHT_TWO_SIDE_ENABLE,0);method(NV097_SET_POLYGON_STIPPLE_ENABLE,0);method(NV097_SET_POINT_PARAMS_ENABLE,0);
    method(NV097_SET_STENCIL_TEST_ENABLE,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_POLY_OFFSET_FILL_ENABLE,0);method(NV097_SET_POLY_OFFSET_POINT_ENABLE,0);
    /* Existing sprite consumer verifies four actual texture quadrants on fixed/program routes. */
    verify_rectangle_point_sprite();
    fprintf(stderr,"[SOFTWARE POINT] original square coverage, fixed/program routes, depth/stencil, cull/offset isolation and sprite sampling passed\n");
}
static void verify_point_setup_boundary(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);method(NV097_SET_POINT_SIZE,24);
    struct RasterVertexForTest point={-.01f,4,1,1,0xffff0000};raster_vertices_for_test(&point,1);
    method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,1);nv2a_pb_checkpoint_publish();
    const uint32_t *pixels=(const uint32_t *)(xbox_GetMemoryOffset()+s_gpu.color_base);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)assert(pixels[y*8+x]==(x<2 && y>=3 && y<6?0xffff0000:s_gpu.clear_color));
    fprintf(stderr,"[POINT SETUP BOUNDARY] original setup truncation precedes point center clip\n");
}
static void verify_software_polygon_points(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_DEPTH_MASK,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_POINT_PARAMS_ENABLE,0);method(NV097_SET_EDGE_FLAG,1);method(NV097_SET_FRONT_FACE,0x900);
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    uint32_t reference[4096],actual[4096];
    for(unsigned route=0;route<2;route++)for(unsigned size=1;size<=3;size+=2) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route?0:2);method(NV097_SET_POINT_SIZE,size*8);
        struct RasterVertexForTest triangle[3]={{16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{16,48,1,1,0xffff0000}};
        for(unsigned mask=0;mask<8;mask++) {
            struct RasterVertexForTest points[3];unsigned n=0;for(unsigned i=0;i<3;i++)if(mask&(1u<<i))points[n++]=triangle[i];
            method(NV097_CLEAR_SURFACE,0xf0);raster_mode_for_test(0,0);if(n){raster_vertices_for_test(points,n);raster_draw_for_test(1,0,n);}raster_pixels_for_test(reference);
            raster_vertices_for_test(triangle,3);raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,5);
            for(unsigned i=0;i<3;i++){method(NV097_SET_EDGE_FLAG,(mask>>i)&1);method(NV097_DRAW_ARRAYS,i);}
            method(NV097_SET_BEGIN_END,0);raster_pixels_for_test(actual);assert(!memcmp(actual,reference,sizeof actual));
        }
        method(NV097_SET_EDGE_FLAG,1);
        const struct RasterVertexForTest quad[4]={{16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{48,48,1,1,0xffff0000},{16,48,1,1,0xffff0000}};
        for(unsigned primitive=8;primitive<=10;primitive+=2) {
            raster_vertices_for_test(quad,4);raster_mode_for_test(0,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,4);raster_pixels_for_test(reference);
            raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(primitive,0,4);raster_pixels_for_test(actual);assert(!memcmp(actual,reference,sizeof actual));
        }
        const struct RasterVertexForTest clip_source[3]={{-8,16,1,1,0xffff0000},{32,16,1,1,0xffff0000},{32,48,1,1,0xffff0000}};
        const struct RasterVertexForTest clip_points[4]={{0,16,1,1,0xffff0000},{32,16,1,1,0xffff0000},{32,48,1,1,0xffff0000},{0,22.4f,1,1,0xffff0000}};
        raster_vertices_for_test(clip_points,4);raster_mode_for_test(0,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,4);raster_pixels_for_test(reference);
        raster_vertices_for_test(clip_source,3);raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(actual,reference,sizeof actual));
        const float clip_scale[4]={1,1,1,0},clip_offset[4]={0,0,0,0};nv2a_vsh_set_constant(58,clip_scale);nv2a_vsh_set_constant(59,clip_offset);
        const struct RasterVertexForTest w_source[3]={{0,0,1,-1,0xffff0000},{32,0,1,1,0xffff0000},{0,32,1,1,0xffff0000}};
        const struct RasterVertexForTest w_points[5]={{64,0,1,1,0xffff0000},{32,0,1,1,0xffff0000},{0,32,1,1,0xffff0000},{0,64,1,1,0xffff0000},{64,64,1,1,0xffff0000}};
        raster_vertices_for_test(w_points,5);raster_mode_for_test(0,0);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(1,0,5);raster_pixels_for_test(reference);
        raster_vertices_for_test(w_source,3);raster_mode_for_test(1,1);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(5,0,3);raster_pixels_for_test(actual);assert(!memcmp(actual,reference,sizeof actual));
        struct RasterVertexForTest mixed[6]={{16,16,1,1,0x80ff0000},{48,16,1,1,0x80ff0000},{16,48,1,1,0x80ff0000},{16,16,1,1,0x8000ff00},{16,48,1,1,0x8000ff00},{48,16,1,1,0x8000ff00}};
        raster_vertices_for_test(mixed,6);method(NV097_SET_BLEND_ENABLE,1);method(NV097_SET_BLEND_FUNC_SFACTOR,0x302);method(NV097_SET_BLEND_FUNC_DFACTOR,0x303);
        for(unsigned front=0;front<2;front++)for(unsigned back=0;back<2;back++) {
            method(NV097_CLEAR_SURFACE,0xf0);raster_mode_for_test(front,front);raster_draw_for_test(5,0,3);raster_mode_for_test(back,back);raster_draw_for_test(5,3,3);raster_pixels_for_test(reference);
            method(NV097_CLEAR_SURFACE,0xf0);raster_mode_for_test(front,back);raster_draw_for_test(5,0,6);raster_pixels_for_test(actual);assert(!memcmp(actual,reference,sizeof actual));
        }
        method(NV097_SET_BLEND_ENABLE,0);
    }
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);raster_mode_for_test(1,1);method(NV097_SET_POINT_SIZE,8);
    method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000);method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x207);
    polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,3);polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,0);
    struct RasterVertexForTest triangle[3]={{16,16,1,1,0xffff0000},{48,16,1,1,0xffff0000},{16,48,1,1,0xffff0000}};raster_vertices_for_test(triangle,3);
    method(NV097_SET_POLYGON_STIPPLE_ENABLE,1);for(unsigned row=0;row<32;row++)method(NV097_SET_POLYGON_STIPPLE_PATTERN+row*4,0);
    for(unsigned point=0;point<2;point++) {
        method(NV097_SET_POLY_OFFSET_POINT_ENABLE,point);method(NV097_SET_POLY_OFFSET_FILL_ENABLE,!point);
        method(NV097_SET_ZSTENCIL_CLEAR_VALUE,(100<<8)|0xa5);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
        uint32_t raw=BRIDGE_MEM32(0x80060000u+(16*64+16)*4);assert((raw>>8)==(point?4:1));assert(BRIDGE_MEM32(s_gpu.color_base+(16*64+16)*4)==0xffff0000);
    }
    fprintf(stderr,"[SOFTWARE POLYGON POINTS] boundary masks, whole quads/polygon fans, generated clipped points, mixed face order/blending, POINT offset and stipple isolation passed\n");
}

static void verify_legacy_texfog(unsigned entry) {
    static const uint32_t methods[4][4]={{NV097_SET_TEXCOORD0_2F,NV097_SET_TEXCOORD0_4F,NV097_SET_TEXCOORD0_2S,NV097_SET_TEXCOORD0_4S},{NV097_SET_TEXCOORD1_2F,NV097_SET_TEXCOORD1_4F,NV097_SET_TEXCOORD1_2S,NV097_SET_TEXCOORD1_4S},{NV097_SET_TEXCOORD2_2F,NV097_SET_TEXCOORD2_4F,NV097_SET_TEXCOORD2_2S,NV097_SET_TEXCOORD2_4S},{NV097_SET_TEXCOORD3_2F,NV097_SET_TEXCOORD3_4F,NV097_SET_TEXCOORD3_2S,NV097_SET_TEXCOORD3_4S}};
    assert(entry<17);unsigned kind=entry%4,attr=entry==16?5:9+entry/4,base=entry==16?NV097_SET_FOG_COORD:methods[entry/4][kind],words=entry==16?1:kind==0?2:kind==1?4:kind==2?1:2;
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_POINT_SIZE,32);
    uint32_t ins[4]={0,(1u<<21)|(attr<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)|1};nv2a_vsh_set_instruction(1,ins);
    method(NV097_SET_COMBINER_COLOR_ICW,0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    const float values[4]={.25f,.5f,.75f,.125f},initial[4]={.9f,.8f,.7f,.2f};const uint32_t packed[2][2]={{0xffff0000u,0x00010002u},{0x7fff8000u,0x80017ffeu}};
    for(unsigned sample=0;sample<2;sample++)for(unsigned partial=1;partial<=words;partial++) {
        uint32_t reference[64],actual[64];float final[4];
        for(unsigned legacy=0;legacy<2;legacy++) {
            float expected[4];memcpy(expected,initial,sizeof expected);
            for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&initial[c],4);method(NV097_SET_VERTEX_DATA4F_M+attr*16+c*4,bits);}
            for(unsigned word=0;word<partial;word++) {
                uint32_t param;float value=sample?1-values[word]:values[word];memcpy(&param,&value,4);
                if(entry==16)for(unsigned c=0;c<4;c++)expected[c]=value;
                else if(kind>=2){param=packed[sample][word];for(unsigned c=0;c<2;c++)expected[word*2+c]=(float)(int16_t)(param>>(16*c));if(kind==2){expected[2]=0;expected[3]=1;}}
                else {expected[word]=value;if(kind==0){expected[2]=0;expected[3]=1;}}
                if(legacy)method(base+word*4,param);
                else for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&expected[c],4);method(NV097_SET_VERTEX_DATA4F_M+attr*16+c*4,bits);}
                assert(s_gpu.imm_used&(1u<<attr));assert(!memcmp(s_gpu.imm_attr[attr],expected,sizeof expected));
            }
            memcpy(final,expected,sizeof final);method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,1);
            const float position[3]={4,4,1};for(unsigned c=0;c<3;c++){uint32_t bits;memcpy(&bits,&position[c],4);method(NV097_SET_VERTEX3F+c*4,bits);}
            method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();memcpy(legacy?actual:reference,(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof actual);
        }
        assert(!memcmp(actual,reference,sizeof actual));unsigned covered=0;
        for(unsigned i=0;i<64;i++)if(actual[i]!=s_gpu.clear_color){covered++;for(unsigned c=0;c<4;c++){unsigned expected=(unsigned)(fminf(1,fmaxf(0,final[c]))*255+.5f);int observed=(actual[i]>>(c==3?24:(2-c)*8))&255;assert(abs((int)expected-observed)<=1);if(expected==0||expected==255)assert(observed==expected);}}assert(covered);
    }
    fprintf(stderr,"[LEGACY TEXFOG] entry=%u method=%04X attr=%u passed\n",entry,base,attr);
}

static void verify_legacy_attribute_group(unsigned entry) {
    static const uint32_t methods[]={NV097_SET_DIFFUSE_COLOR3F,NV097_SET_SPECULAR_COLOR4F,NV097_SET_SPECULAR_COLOR3F,NV097_SET_SPECULAR_COLOR4UB,NV097_SET_NORMAL3F,NV097_SET_WEIGHT1F,NV097_SET_WEIGHT2F,NV097_SET_WEIGHT3F,NV097_SET_WEIGHT4F,NV097_SET_NORMAL3S};
    static const unsigned attrs[]={3,4,4,4,2,1,1,1,1,2},counts[]={3,4,3,1,3,1,2,3,4,2};
    assert(entry<10);unsigned attr=attrs[entry];
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_POINT_SIZE,32);method(NV097_SET_SPECULAR_ENABLE,1);
    uint32_t ins[4]={0,(1u<<21)|(attr<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|((attr==4?4u:3u)<<3)|1};nv2a_vsh_set_instruction(1,ins);
    method(NV097_SET_COMBINER_COLOR_ICW,attr==4?0x05200000:0x04200000);method(NV097_SET_COMBINER_ALPHA_ICW,attr==4?0x15200000:0x14200000);
    const float values[4]={.25f,.5f,.75f,.125f},initial[4]={.9f,.8f,.7f,.2f};
    const uint32_t packed[4][2]={{0x80008001u,0x7fff0000u},{0x4000c000u,0x20007fffu},{0x7fff8000u,0x80004000u},{0x4000c000u,0x00004000u}};
    for(unsigned sample=0;sample<(entry==9?4u:1u);sample++) {
        uint32_t reference[64],actual[64];float final[4];
        for(unsigned legacy=0;legacy<2;legacy++) {
            float expected[4];memcpy(expected,initial,sizeof expected);
            for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&initial[c],4);method(NV097_SET_VERTEX_DATA4F_M+attr*16+c*4,bits);}
            for(unsigned word=0;word<counts[entry];word++) {
                uint32_t param;memcpy(&param,&values[word],4);
                if(entry==3){param=0x80402010;for(unsigned c=0;c<4;c++)expected[c]=(float)((param>>(c*8))&255)/255;}
                else if(entry==9){param=packed[sample][word];for(unsigned c=0;c<2;c++)expected[word*2+c]=fmaxf(-1,(float)(int16_t)(param>>(16*c))/32767.0f);}
                else {expected[word]=values[word];if(entry==0||entry==2||entry==7)expected[3]=1;if(entry==5){expected[1]=0;expected[2]=0;expected[3]=1;}if(entry==6){expected[2]=0;expected[3]=1;}}
                if(legacy)method(methods[entry]+word*4,param);
                else for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&expected[c],4);method(NV097_SET_VERTEX_DATA4F_M+attr*16+c*4,bits);}
                assert(s_gpu.imm_used&(1u<<attr));assert(!memcmp(s_gpu.imm_attr[attr],expected,sizeof expected));
            }
            memcpy(final,expected,sizeof final);
            method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,1);
            const float position[3]={4,4,1};for(unsigned c=0;c<3;c++){uint32_t bits;memcpy(&bits,&position[c],4);method(NV097_SET_VERTEX3F+c*4,bits);}
            method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();memcpy(legacy?actual:reference,(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof actual);
        }
        assert(!memcmp(actual,reference,sizeof actual));unsigned channels[4];for(unsigned c=0;c<4;c++)channels[c]=(unsigned)(fminf(1,fmaxf(0,final[c]))*255+.5f);
        uint32_t argb=(channels[3]<<24)|(channels[0]<<16)|(channels[1]<<8)|channels[2];unsigned covered=0;
        for(unsigned i=0;i<64;i++)if(actual[i]!=s_gpu.clear_color){covered++;for(unsigned c=0;c<4;c++){int expected=(argb>>(8*c))&255,observed=(actual[i]>>(8*c))&255;if(abs(expected-observed)>1 || ((expected==0||expected==255)&&observed!=expected))fprintf(stderr,"[LEGACY BYTE] entry=%u pixel=%u actual=%08x expected=%08x\n",entry,i,actual[i],argb);assert(abs(expected-observed)<=1);if(expected==0||expected==255)assert(observed==expected);}}assert(covered>0);
        fprintf(stderr,"[LEGACY GROUP] entry=%u method=%04x attr=%u sample=%u ideal_argb=%08x actual=%08x covered=%u\n",entry,methods[entry],attr,sample,argb,actual[36],covered);
    }
}

static void verify_legacy_diffuse_4ub_consumer(void) {
    seed();method(NV097_SET_CLEAR_RECT_HORIZONTAL,7<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,7<<16);method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_POINT_SIZE,8);method(NV097_SET_LINE_WIDTH,8);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);method(NV097_SET_SHADE_MODE,0x1d01);
    const float positions[3][4]={{2,2,1,1},{6,2,1,1},{2,6,1,1}};const uint32_t colours[2]={0xff204080,0x80402010};uint32_t reference[64],actual[64];
    for(unsigned prim=0;prim<3;prim++)for(unsigned colour=0;colour<2;colour++) {
        unsigned primitive=prim==0?1:prim==1?2:5,count=prim+1;
        for(unsigned legacy=0;legacy<2;legacy++) {
            method(NV097_CLEAR_SURFACE,0xf0);method(NV097_SET_BEGIN_END,primitive);
            for(unsigned i=0;i<count;i++) {
                method(legacy?NV097_SET_DIFFUSE_COLOR4UB:NV097_SET_VERTEX_DATA4UB+3*4,colours[colour]);
                for(unsigned c=0;c<4;c++){uint32_t bits;memcpy(&bits,&positions[i][c],4);method(NV097_SET_VERTEX4F+c*4,bits);}
            }
            method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();memcpy(legacy?actual:reference,(void *)(xbox_GetMemoryOffset()+s_gpu.color_base),sizeof reference);
        }
        assert(!memcmp(actual,reference,sizeof actual));unsigned covered=0;uint32_t c=colours[colour],argb=(c&0xff00ff00u)|((c&255u)<<16)|((c>>16)&255u);
        for(unsigned i=0;i<64;i++)if(actual[i]!=s_gpu.clear_color){covered++;if(actual[i]!=argb)fprintf(stderr,"[LEGACY PIXEL] prim=%u colour=%u pixel=%u actual=%08x expected=%08x clear=%08x\n",primitive,colour,i,actual[i],argb,s_gpu.clear_color);assert(actual[i]==argb);}assert(covered>0);
    }
    fprintf(stderr,"[LEGACY DIFFUSE] original156C producer reaches existinggeneric4UB state and immediate point/line/triangle colours\n");
}
static void verify_constant_w_slope_consumers(unsigned reject,unsigned screen) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000);method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x207);method(NV097_SET_TRANSFORM_EXECUTION_MODE,screen?0:2);
    method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE);method(NV097_SET_LINE_WIDTH,16);method(NV097_SET_POINT_SIZE,24);
    method(NV097_SET_POLY_OFFSET_POINT_ENABLE,1);method(NV097_SET_POLY_OFFSET_LINE_ENABLE,1);method(NV097_SET_POLY_OFFSET_FILL_ENABLE,1);polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,0);
    const float w_values[]={1,16,325};uint32_t before[4096],after[4096];
    for(unsigned k=0;k<3;k++)for(unsigned mode=0;mode<3;mode++) {
        struct RasterVertexForTest triangle[3]={{8,8,40,w_values[k],0xffff0000},{48,8,120,w_values[k],0xffff0000},{8,48,160,w_values[k],0xffff0000}};
        if(reject)triangle[2].w*=2;
        raster_vertices_for_test(triangle,3);raster_mode_for_test(mode,mode);
        for(unsigned factor=0;factor<3;factor++) {
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,factor==0?0:factor==1?65536:-65536);
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,1000<<8);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
            memcpy(factor?after:before,(void *)(xbox_GetMemoryOffset()+0x80060000u),sizeof before);
            if(factor)assert(!memcmp(before,after,sizeof before));
        }
        unsigned covered=0;for(unsigned i=0;i<4096;i++)covered+=(before[i]>>8)!=1000;assert(covered>0);
    }
    if(!reject) {
        /* An inactive FILL enable must not force W slope on POINT draws. */
        method(NV097_SET_POLY_OFFSET_POINT_ENABLE,0);method(NV097_SET_POLY_OFFSET_LINE_ENABLE,0);raster_mode_for_test(1,1);
        struct RasterVertexForTest varying[3]={{8,8,40,16,0xffff0000},{48,8,120,32,0xffff0000},{8,48,160,64,0xffff0000}};raster_vertices_for_test(varying,3);
        for(unsigned factor=0;factor<2;factor++) {
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,factor?65536:0);
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,1000<<8);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
            memcpy(factor?after:before,(void *)(xbox_GetMemoryOffset()+0x80060000u),sizeof before);
        }
        assert(!memcmp(before,after,sizeof before));
    }
    assert(!reject);fprintf(stderr,"[CONSTANT W] finite original W plane has exactzero slope for FILL/POINT/LINE at factors±65536; nonconstant W remains explicit\n");
}
static void verify_smoothing_control_consumers(unsigned rejected) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    if(rejected==2){method(NV097_SET_ANTI_ALIASING_CONTROL,0x00010001);assert(0);}
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);method(NV097_SET_LINE_WIDTH,16);method(NV097_SET_POINT_SIZE,24);
    struct RasterVertexForTest vertices[3]={{8,8,1,1,0xffff0000},{48,16,1,1,0xffff0000},{16,48,1,1,0xffff0000}};raster_vertices_for_test(vertices,3);
    uint32_t expected[4096],actual[4096];struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    for(unsigned primitive=0;primitive<4;primitive++) {
        unsigned prim=primitive==0?1:primitive==1?2:5,count=primitive==0?1:primitive==1?2:3;
        raster_mode_for_test(primitive==3?2:0,primitive==3?2:0);
        method(NV097_SET_LINE_SMOOTH_ENABLE,0);method(NV097_SET_POLY_SMOOTH_ENABLE,0);method(NV097_SET_ANTI_ALIASING_CONTROL,0xffff0000);
        method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(prim,0,count);raster_pixels_for_test(expected);
        method(NV097_SET_LINE_SMOOTH_ENABLE,1);method(NV097_SET_POLY_SMOOTH_ENABLE,1);method(NV097_SET_ANTI_ALIASING_CONTROL,rejected?0xffff0000:0xffff0001);
        assert((pg->regs[NV_PGRAPH_SETUPRASTER]&(NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE|NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE))==(NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE|NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE));
        if(!rejected && primitive==0) {
            FILE *saved=tmpfile();assert(saved && nv2a_save_checkpoint(saved));rewind(saved);
            method(NV097_SET_ANTI_ALIASING_CONTROL,0);method(NV097_SET_LINE_SMOOTH_ENABLE,0);method(NV097_SET_POLY_SMOOTH_ENABLE,0);
            void *loaded=nv2a_prepare_checkpoint(saved);assert(loaded);nv2a_apply_checkpoint(loaded);free(loaded);fclose(saved);
            assert(pg->regs[NV_PGRAPH_ANTIALIASING]&NV_PGRAPH_ANTIALIASING_ENABLE);
            assert(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE);assert(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE);
        }
        method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(prim,0,count);raster_pixels_for_test(actual);assert(!memcmp(expected,actual,sizeof actual));
    }
    assert(!rejected);fprintf(stderr,"[SMOOTH CONTROL] original control1 masks LINE/POLY smooth in1x mode; state/checkpoint and aliased point/line/fill/wire output retained\n");
}
static void verify_software_polygon_line_slope(void) {
    seed();method(NV097_SET_BEGIN_END,0);
    method(NV097_SET_SURFACE_PITCH,256|(256u<<16));method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000);method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_MASK,1);method(NV097_SET_DEPTH_FUNC,0x207);
    method(NV097_SET_LINE_WIDTH,16);method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);
    method(NV097_SET_POLY_OFFSET_POINT_ENABLE,1);method(NV097_SET_POINT_SIZE,24);method(NV097_SET_POLY_OFFSET_LINE_ENABLE,1);method(NV097_SET_POLY_OFFSET_FILL_ENABLE,1);polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,0);
    uint32_t before[4096],after[4096];
    skin_matrix_for_test(NV097_SET_COMPOSITE_MATRIX,0,0,1);
    for(unsigned route=0;route<3;route++)for(unsigned clipped=0;clipped<2;clipped++)for(unsigned sign=0;sign<2;sign++)for(unsigned reverse=0;reverse<2;reverse++)for(unsigned mode=0;mode<3;mode++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,route==0?2:0);
        method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,route==2?0x1432:0x1442);
        struct RasterVertexForTest triangle[3]={{8,8,40,1,0xffff0000},{48,8,120,1,0xffff0000},{8,48,160,1,0xffff0000}};
        if(clipped){triangle[0]=(struct RasterVertexForTest){-8,8,8,1,0xffff0000};triangle[2]=(struct RasterVertexForTest){-8,64,176,1,0xffff0000};}
        if(reverse){struct RasterVertexForTest t=triangle[1];triangle[1]=triangle[2];triangle[2]=t;}
        raster_vertices_for_test(triangle,3);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,route==2?0x1432:0x1442);raster_mode_for_test(mode==2?1:mode?2:0,mode==2?1:mode?2:0);
        for(unsigned factor=0;factor<2;factor++) {
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,factor?(sign?-2:2):0);
            method(NV097_SET_ZSTENCIL_CLEAR_VALUE,1000<<8);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(5,0,3);nv2a_pb_checkpoint_publish();
            memcpy(factor?after:before,(void *)(xbox_GetMemoryOffset()+0x80060000u),sizeof before);
        }
        unsigned covered=0;
        for(unsigned i=0;i<4096;i++) {
            if(before[i]>>8==1000){assert(after[i]==before[i]);continue;}
            covered++;if((int)(after[i]>>8)-(int)(before[i]>>8)!=(sign?-6:6))fprintf(stderr,"[LINE SLOPE] clip=%u mode=%u reverse=%u pixel=%u raw=%u ->%u expected delta%u\n",clipped,mode,reverse,i,before[i]>>8,after[i]>>8,sign?-6:6);
            assert((int)(after[i]>>8)-(int)(before[i]>>8)==(sign?-6:6));
        }
        assert(covered>0);
    }
    fprintf(stderr,"[LINE SLOPE] original FILL plane factor reaches all polygon boundary lines, both orientations and wide copies\n");
}
static void dump_software_line_consumers(const char *path) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_SURFACE_PITCH,(256u<<16)|256u);method(NV097_SET_SURFACE_CLIP_HORIZONTAL,64u<<16);method(NV097_SET_SURFACE_CLIP_VERTICAL,64u<<16);
    method(NV097_SET_CLEAR_RECT_HORIZONTAL,63<<16);method(NV097_SET_CLEAR_RECT_VERTICAL,63<<16);
    method(NV097_SET_WINDOW_CLIP_HORIZONTAL,63<<16);method(NV097_SET_WINDOW_CLIP_VERTICAL,63<<16);
    method(NV097_SET_TRANSFORM_EXECUTION_MODE,2);raster_mode_for_test(0,0);
    FILE *out=fopen(path,"wb");assert(out);uint32_t pixels[4096];
    const float ends[][4]={{8,8,48,8},{8,8,8,48},{8,8,48,48},{8,48,48,8},{8.5f,8.5f,48.5f,24.5f},{8.5f,8.5f,24.5f,48.5f},{8,8,48,24},{8,8,24,48},{8.25f,8.75f,48.125f,25.625f}};
    for(unsigned width=1;width<=5;width++)for(unsigned i=0;i<sizeof(ends)/sizeof(ends[0]);i++)for(unsigned reverse=0;reverse<2;reverse++) {
        struct RasterVertexForTest line[2]={{ends[i][reverse?2:0],ends[i][reverse?3:1],1,1,0xffff0000},{ends[i][reverse?0:2],ends[i][reverse?1:3],1,1,0xffff0000}};
        method(NV097_SET_LINE_WIDTH,width*8);raster_vertices_for_test(line,2);method(NV097_CLEAR_SURFACE,0xf0);raster_draw_for_test(2,0,2);raster_pixels_for_test(pixels);assert(fwrite(pixels,sizeof pixels,1,out)==1);
    }
    method(NV097_SET_LINE_WIDTH,8);method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000);
    method(NV097_SET_DEPTH_TEST_ENABLE,1);method(NV097_SET_DEPTH_FUNC,0x207);method(NV097_SET_DEPTH_MASK,1);
    method(NV097_SET_STENCIL_TEST_ENABLE,1);method(NV097_SET_CONTROL0,NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
    method(NV097_SET_STENCIL_MASK,255);method(NV097_SET_STENCIL_FUNC,0x207);method(NV097_SET_STENCIL_FUNC_MASK,255);method(NV097_SET_STENCIL_OP_ZPASS,0x1e02);
    const uint32_t position[4]={0,(1u<<21)|0x1b,2u<<26,(15u<<12)|(1u<<11)};
    const uint32_t color[4]={0,(1u<<21)|(3u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)|1};
    nv2a_vsh_set_instruction(0,position);nv2a_vsh_set_instruction(1,color);
    for(unsigned program=0;program<2;program++)for(unsigned smooth=0;smooth<2;smooth++)for(unsigned reverse=0;reverse<2;reverse++) {
        method(NV097_SET_TRANSFORM_EXECUTION_MODE,program?2:0);method(NV097_SET_SHADE_MODE,smooth?0x1d01:0x1d00);
        struct RasterVertexForTest line[2]={{8.5f,8.5f,16,1,0xffff0000},{48.5f,28.5f,96,2,0xff0000ff}};
        if(reverse){struct RasterVertexForTest t=line[0];line[0]=line[1];line[1]=t;}
        raster_vertices_for_test(line,2);method(NV097_SET_ZSTENCIL_CLEAR_VALUE,(1000<<8)|0xa5);method(NV097_CLEAR_SURFACE,0xf3);raster_draw_for_test(2,0,2);raster_pixels_for_test(pixels);assert(fwrite(pixels,sizeof pixels,1,out)==1);
        nv2a_pb_checkpoint_publish();assert(fwrite((void *)(xbox_GetMemoryOffset()+0x80060000u),sizeof pixels,1,out)==1);
    }
    fclose(out);
}
static void verify_short_primitive_submission(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);
    method(NV097_SET_POINT_SIZE,8);
    uint32_t reference[64];
    struct {float x,y,z,w;uint32_t color;} data[2]={{2,2,1,1,0xffff0000},{6,2,1,1,0xffff0000}};
    memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
    for(unsigned primitive=1;primitive<=2;primitive++) {
        for(unsigned submit=0;submit<3;submit++) {
            method(NV097_CLEAR_SURFACE,0xf0);
            method(NV097_SET_BEGIN_END,primitive);
            if(!submit) method(NV097_DRAW_ARRAYS,(primitive-1)<<24);
            else if(submit==1) {
                const uint32_t *words=(const uint32_t *)data;
                for(unsigned i=0;i<primitive*5;i++) method(NV097_INLINE_ARRAY,words[i]);
            } else {
                for(unsigned i=0;i<primitive;i++) {
                    float color[4]={1,0,0,1};
                    for(unsigned c=0;c<4;c++) {uint32_t bits;memcpy(&bits,&color[c],4);method(NV097_SET_DIFFUSE_COLOR4F+c*4,bits);}
                    for(unsigned c=0;c<4;c++) {uint32_t bits;memcpy(&bits,((float *)&data[i])+c,4);method(NV097_SET_VERTEX4F+c*4,bits);}
                }
            }
            method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
            const uint32_t *pixels=(const uint32_t *)(xbox_GetMemoryOffset()+s_gpu.color_base);
            if(!submit) {
                memcpy(reference,pixels,sizeof reference);
                unsigned red=0;for(unsigned i=0;i<64;i++)red+=pixels[i]==0xffff0000;
                assert(red>0);
            } else assert(!memcmp(reference,pixels,sizeof reference));
        }
    }
    fprintf(stderr,"[SHORT PRIMITIVES] one point and two line vertices match array, inline and immediate output\n");
}
static void verify_subchannel_kelvin(void) {
    seed();
    for(unsigned channel=1;channel<8;channel++) {
        pgraph_method(nv2a_get_state(),channel,NV_SET_OBJECT,0x200);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_COLOR_CLEAR_VALUE,0xff334455);
        pgraph_method(nv2a_get_state(),channel,NV097_CLEAR_SURFACE,0xf3);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_DEPTH_TEST_ENABLE,0);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_TRANSFORM_CONSTANT_LOAD,21);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_TRANSFORM_CONSTANT,0x3f000000);
        assert(nv2a_vsh_constant(21)[0]==0.5f);
        vertices(0.25f);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_BEGIN_END,5);
        pgraph_method(nv2a_get_state(),channel,NV097_DRAW_ARRAYS,2u<<24);
        pgraph_method(nv2a_get_state(),channel,NV097_SET_BEGIN_END,0);
        nv2a_pb_checkpoint_publish();
        assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000);
    }
    fprintf(stderr,"[SUBCHANNEL] all seven bound Kelvin contexts rendered and uploaded constants\n");
}
static void verify_depth_encodings(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_STENCIL_TEST_ENABLE,0);
    method(NV097_SET_ZMIN_MAX_CONTROL,0x10);
    for(unsigned format=1;format<=2;format++) {
        method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000+format*0x10000);
        method(NV097_SET_SURFACE_FORMAT,8|(format<<4));
        for(unsigned floating=0;floating<2;floating++)for(unsigned route=0;route<2;route++) {
            unsigned control=floating?NV097_SET_CONTROL0_Z_FORMAT:0;
            method(NV097_SET_CONTROL0,control);method(NV097_SET_TRANSFORM_EXECUTION_MODE,route*2);
            polygon_offset_float(NV097_SET_CLIP_MAX,floating?(format==1?511.9375f:1e30f):(format==1?65535.0f:16777215.0f));
            const float values[]={0,0.5f,1.0f,1.125f,1.375f,4.0f,511.9375f};
            const unsigned f16[]={0,0x6000,0x7000,0x7200,0x7600,0x9000,0xffff};
            const unsigned f24[]={0,0x7e0000,0x7f0000,0x7f2000,0x7f6000,0x810000,0x87fff0};
            for(unsigned i=0;i<sizeof(values)/sizeof(*values);i++) {
                unsigned encoded=floating?(format==1?f16[i]:f24[i]):(unsigned)values[i];
                method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?encoded:encoded<<8);
                method(NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_Z|0xf0);
                struct {float x,y,z,w;uint32_t color;} data[3]={
                    {0,0,values[i],1,0xffff0000},{8,0,values[i],1,0xffff0000},{0,8,values[i],1,0xffff0000}};
                memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
                /* Equality against a raw clear must work without relying on
                 * round trips through the same shader conversion. */
                method(NV097_SET_DEPTH_FUNC,0x202);
                method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
                float actual=polygon_offset_depth();
                if(BRIDGE_MEM32(s_gpu.color_base)!=0xffff0000u || fabsf(actual-encoded)>1)
                    fprintf(stderr,"depth encoding mismatch format=%u float=%u route=%u index=%u actual=%g encoded=%u colour=%08x\n",format,floating,route,i,actual,encoded,BRIDGE_MEM32(s_gpu.color_base));
                assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000u && fabsf(actual-encoded)<=1);
            }
            /* A mode switch reinterprets the existing bits without clearing;
             * the encoded 1.125 value is the integer comparison reference. */
            if(floating) {
                unsigned code=format==1?0x7200:0x7f2000;
                method(NV097_SET_ZSTENCIL_CLEAR_VALUE,format==1?code:code<<8);
                method(NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_Z|0xf0);
                method(NV097_SET_CONTROL0,0);
                polygon_offset_float(NV097_SET_CLIP_MAX,16777215.0f);
                struct {float x,y,z,w;uint32_t color;} data[3]={
                    {0,0,(float)code,1,0xffff0000},{8,0,(float)code,1,0xffff0000},{0,8,(float)code,1,0xffff0000}};
                memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
                method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
                polygon_offset_depth();assert(BRIDGE_MEM32(s_gpu.color_base)==0xffff0000u);
            }
            method(NV097_SET_CONTROL0,control|NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE);
            method(NV097_SET_DEPTH_FUNC,0x207);method(NV097_SET_POLY_OFFSET_FILL_ENABLE,1);
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,0.0f);
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,-0.5f);
            struct {float x,y,z,w;uint32_t color;} data[3]={
                {0,0,17,4,0xffff0000},{8,0,17,4,0xffff0000},{0,8,17,4,0xffff0000}};
            memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
            method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
            unsigned expected=floating?(format==1?0x8c00:0x80c000):3;
            float actual=polygon_offset_depth();
            if(fabsf(actual-expected)>1)fprintf(stderr,"W depth mismatch format=%u float=%u route=%u actual=%g expected=%u\n",format,floating,route,actual,expected);
            assert(fabsf(actual-expected)<=1);
            method(NV097_SET_POLY_OFFSET_FILL_ENABLE,0);
        }
    }
    fprintf(stderr,"[DEPTH ENCODING] integer and floating depth, raw clears, equal comparisons, format reinterpretation, W depth, bias and both vertex routes passed\n");
}
static void verify_polygon_offset(void) {
    seed();method(NV097_SET_BEGIN_END,0);
    method(NV097_SET_DEPTH_FUNC,0x207);
    method(NV097_SET_POLY_OFFSET_POINT_ENABLE,1);
    method(NV097_SET_POLY_OFFSET_LINE_ENABLE,1);
    for(unsigned format=1;format<=2;format++) {
        method(NV097_SET_SURFACE_ZETA_OFFSET,0x60000+format*0x10000);
        method(NV097_SET_SURFACE_FORMAT,8|(format<<4));
        float base=format==1?4096.0f:4194304.0f,step=format==1?32.0f:65536.0f;
        polygon_offset_float(NV097_SET_CLIP_MAX,format==1?65535.0f:16777215.0f);
        for(unsigned route=0;route<2;route++) {
            method(NV097_SET_TRANSFORM_EXECUTION_MODE,route*2);
            struct {float x,y,z,w;uint32_t color;} data[3]={
                {0,0,base,1,0xffff0000},{8,0,base+8*step,1,0xffff0000},{0,8,base+16*step,1,0xffff0000}};
            memcpy((void *)(xbox_GetMemoryOffset()+0x80040000u),data,sizeof data);
            const float factors[]={-4,2,0,-4,0};
            const float biases[]={-16,32,16,-16,-2*base};
            const unsigned enables[]={1,1,1,0,1};
            for(unsigned i=0;i<5;i++) {
                method(NV097_SET_ZMIN_MAX_CONTROL,0x10);
                polygon_offset_float(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,factors[i]);
                polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,biases[i]);
                method(NV097_SET_POLY_OFFSET_FILL_ENABLE,enables[i]);
                method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
                float sample=0.5f/(hw_enabled()?d3d8_RenderScale():1);
                float expected=base+3*step*sample;
                if(enables[i])expected+=biases[i]+factors[i]*2*step;
                expected=fmaxf(0,expected);
                float actual=polygon_offset_depth();
                if(fabsf(actual-expected)>2)fprintf(stderr,"offset mismatch format=%u route=%u case=%u actual=%g expected=%g\n",format,route,i,actual,expected);
                assert(fabsf(actual-expected)<=2);
            }
            method(NV097_SET_ZMIN_MAX_CONTROL,0);
            polygon_offset_float(NV097_SET_POLYGON_OFFSET_BIAS,-2*base);
            method(NV097_SET_BEGIN_END,5);method(NV097_DRAW_ARRAYS,2u<<24);method(NV097_SET_BEGIN_END,0);
            assert(polygon_offset_depth()==0);
        }
    }
    fprintf(stderr,"[POLYGON OFFSET] original -4 factor, positive/negative bias, disable, independent mode flags, clipping range, both depth formats and vertex routes passed\n");
}

int main(int argc,char **argv) {
    assert(argc==6 || (argc==7 && !strncmp(argv[1],"fifo-worker",11) &&
        (!strcmp(argv[6],"partial") || !strcmp(argv[6],"complete"))));_putenv_s("RECOMP_GUEST_LOCK","1");_putenv_s("RECOMP_GPU_D3D11",argv[5]);
    const char *render_scale=getenv("RECOMP_RENDER_SCALE");
    d3d8_DisplayConfigure(render_scale?(unsigned)atoi(render_scale):1,4.0f/3.0f);
    FILE *file=fopen(argv[2],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,file)==1);fclose(file);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);
    NV2AState *gpu=nv2a_init_standalone((void *)xbox_GetMemoryOffset(),0x04000000,
                         (void *)(xbox_GetMemoryOffset()+0xfd700000u),0x100000);assert(gpu);
    gpu->pfifo.method_consumer=nv2a_pb_exec_method;
    assert(AddVectoredExceptionHandler(0,vertex_memory_fault_for_test));
    if(!strncmp(argv[1],"fifo-worker",11))assert(nv2a_fifo_start(gpu, 1));
    assert(xbox_CheckpointBootstrapBegin());
    if(!strncmp(argv[1],"fifo-worker",11)) {
        verify_fifo_worker_checkpoint(argc>6 && !strcmp(argv[6],"partial"),
            !strcmp(argv[1],"fifo-worker-program")?2:!strcmp(argv[1],"fifo-worker-index"));
    } else if(!strcmp(argv[1],"native-program") || !strncmp(argv[1],"reject-native-program-",22)) {
        verify_native_program(argv[1]);
    } else if(!strcmp(argv[1],"native-indices") || !strncmp(argv[1],"reject-native-index-",20)) {
        verify_native_indices(argv[1]);
    } else if(!strncmp(argv[1],"boundary-",9)) {
        verify_batch_boundary(argv[1]+9);
    } else if(!strncmp(argv[1],"large-",6)) {
        verify_large_batch(argv[1]+6);
    } else if(!strcmp(argv[1],"normal-reflection-texgen")) {
        verify_normal_reflection_texgen();
    } else if(!strcmp(argv[1],"invalid-texgen-view")) {
        method(NV097_SET_TEXGEN_VIEW_MODEL,2);assert(0);
    } else if(!strcmp(argv[1],"blit-overlap-direction")) {
        verify_blit_overlap_direction();
    } else if(!strcmp(argv[1],"blit-exact-self-alias")) {
        verify_blit_exact_self_alias();
    } else if(!strncmp(argv[1],"blit-copy-and-",14)) {
        verify_blit_copy_and(argv[1]);
    } else if(!strncmp(argv[1],"blit-rop-",9)) {
        verify_blit_rop(argv[1]);
    } else if(!strncmp(argv[1],"m2mf-increment-",15)) {
        verify_m2mf_increment_state(argv[1]);
    } else if(!strncmp(argv[1],"m2mf-ordinary-",14)) {
        verify_m2mf_ordinary(argv[1]);
    } else if(!strcmp(argv[1],"m2mf-old-layout")) {
        verify_m2mf_old_gpu_layout();
    } else if(!strncmp(argv[1],"m2mf-",5)) {
        verify_m2mf_copy(argv[1]);
    } else if(!strcmp(argv[1],"fixed-texgen")) {
        verify_fixed_texgen_route();
    } else if(!strcmp(argv[1],"vertex-reuse")) {
        verify_vertex_conversion_reuse(argv[3],argv[4]);
    } else if(!strcmp(argv[1],"clear-ownership")) {
        verify_clear_page_ownership();
    } else if(!strcmp(argv[1],"colour-logic")) {
        verify_colour_logic();
    } else if(!strcmp(argv[1],"image-blit-copy")) {
        verify_original_image_blit_copy();
    } else if(!strcmp(argv[1],"software-polygon-points")) {
        verify_software_polygon_points();
    } else if(!strcmp(argv[1],"point-setup-boundary")) {
        verify_point_setup_boundary();
    } else if(!strncmp(argv[1],"legacy-attribute-",17)) {
        verify_legacy_attribute_group((unsigned)strtoul(argv[1]+17,NULL,10));
    } else if(!strncmp(argv[1],"legacy-texfog-",14)) {
        verify_legacy_texfog((unsigned)strtoul(argv[1]+14,NULL,10));
    } else if(!strcmp(argv[1],"legacy-diffuse-4ub")) {
        verify_legacy_diffuse_4ub_consumer();
    } else if(!strcmp(argv[1],"native-constant-w-slope")) {
        verify_constant_w_slope_consumers(0,1);
    } else if(!strncmp(argv[1],"constant-w-slope",16)) {
        verify_constant_w_slope_consumers(!strcmp(argv[1],"constant-w-slope-reject"),0);
    } else if(!strncmp(argv[1],"smoothing-control",17)) {
        verify_smoothing_control_consumers(!strcmp(argv[1],"smoothing-control-active")?1:!strcmp(argv[1],"smoothing-control-mask")?2:0);
    } else if(!strcmp(argv[1],"software-line-slope")) {
        verify_software_polygon_line_slope();
    } else if(!strcmp(argv[1],"software-lines")) {
        dump_software_line_consumers(argv[3]);
    } else if(!strcmp(argv[1],"software-points")) {
        verify_software_point_consumers();
    } else if(!strcmp(argv[1],"screen-output-packing")) {
        verify_screen_output_packing_consumers();
    } else if(!strcmp(argv[1],"polygon-point-params")) {
        verify_polygon_point_params_consumers();
    } else if(!strcmp(argv[1],"polygon-stipple")) {
        verify_polygon_stipple_consumers();
    } else if(!strcmp(argv[1],"polygon-raster")) {
        char pixels_path[4096];assert(snprintf(pixels_path,sizeof pixels_path,"%s.pixels",argv[3])>0);
        polygon_pixels_file=fopen(pixels_path,"wb");assert(polygon_pixels_file);
        verify_polygon_raster_consumers();
        assert(fclose(polygon_pixels_file)==0);polygon_pixels_file=NULL;
    } else if(!strcmp(argv[1],"dependent-textures")) {
        verify_dependent_texture_stages();
    } else if(!strcmp(argv[1],"texture-color-keys")) {
        verify_texture_color_keys();
    } else if(!strcmp(argv[1],"resource-format-connections")) {
        verify_existing_resource_format_connections();
    } else if(!strcmp(argv[1],"rectangle-point-sprite")) {
        verify_rectangle_point_sprite();
    } else if(!strcmp(argv[1],"two-sided-colors")) {
        verify_two_sided_vertex_colors();
    } else if(!strcmp(argv[1],"fixed-lighting-coefficients")) {
        verify_fixed_lighting_coefficients();
    } else if(!strcmp(argv[1],"fixed-two-sided-lighting")) {
        verify_fixed_two_sided_lighting();
    } else if(!strcmp(argv[1],"fixed-specular-six")) {
        verify_fixed_specular_six();
    } else if(!strcmp(argv[1],"fixed-material-products")) {
        verify_fixed_material_products();
    } else if(!strcmp(argv[1],"fixed-skinning")) {
        verify_fixed_skinning();
    } else if(!strcmp(argv[1],"texture-addressing")) {
        verify_texture_addressing();
    } else if(!strcmp(argv[1],"short-primitives")) {
        verify_short_primitive_submission();
    } else if(!strcmp(argv[1],"subchannel-kelvin")) {
        verify_subchannel_kelvin();
    } else if(!strcmp(argv[1],"polygon-offset")) {
        verify_polygon_offset();
    } else if(!strcmp(argv[1],"stencil-operations")) {
        verify_stencil_operations();
    } else if(!strcmp(argv[1],"depth-encodings")) {
        verify_depth_encodings();
    } else if(!strcmp(argv[1],"native-display")) {
        verify_native_finished_frame();
    } else if(!strcmp(argv[1],"vertex-payload")) {
        verify_vertex_payloads();
    } else if(!strcmp(argv[1],"clear-transfer")) {
        verify_clear_transfer();
    } else if(!strcmp(argv[1],"combiner-work")) {
        verify_combiner_uploads();
    } else if(!strcmp(argv[1],"pixel-state-work")) {
        verify_pixel_state_uploads();
    } else if(!strcmp(argv[1],"readback-work")) {
        verify_readback_publication();
    } else if(!strcmp(argv[1],"method-work")) {
        verify_method_dispatch_work();
    } else if(!strcmp(argv[1],"clear-work")) {
        verify_clear_address_work();
    } else if(!strcmp(argv[1],"output-work")) {
        verify_completed_output_work();
    } else if(!strcmp(argv[1],"output-guard-sequential")) {
        verify_sequential_output_guard_work();
    } else if(!strcmp(argv[1],"attribute-work")) {
        verify_attribute_offset_work();
    } else if(!strcmp(argv[1],"protection-work")) {
        verify_colour_protection_work();
    } else if(!strcmp(argv[1],"geometry-updates")) {
        verify_geometry_upload_updates();
    } else if(!strcmp(argv[1],"vsh-updates")) {
        verify_vsh_upload_updates();
    } else if(!strcmp(argv[1],"swizzled-regions")) {
        verify_swizzled_regions();
    } else if(!strcmp(argv[1],"clean-overlap")) {
        verify_clean_texture_overlap();
    } else if(!strcmp(argv[1],"host-write-scope")) {
        verify_host_write_scope(argv[4]);
    } else if(!strcmp(argv[1],"texture-reuse")) {
        verify_texture_content_reuse(argv[3],argv[4]);
    } else if(!strcmp(argv[1],"semaphore")) {
        verify_semaphore_outputs();
    } else if(!strcmp(argv[1],"save")) {
        seed();file=fopen(argv[3],"wb");assert(file && nv2a_gpu_save_checkpoint(file));fclose(file);
        file=fopen(argv[4],"wb");assert(file && xbox_SavePhysicalMemory(file));fclose(file);verify();verify_consumers();
        method(NV097_SET_COLOR_CLEAR_VALUE,0xffabcdef);method(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xff000077);method(NV097_CLEAR_SURFACE,0xf3);
        file=fopen(argv[4],"rb");assert(file && xbox_LoadPhysicalMemory(file));fclose(file);
        file=fopen(argv[3],"rb");assert(file && nv2a_gpu_load_checkpoint(file));fclose(file);verify();verify_consumers();
    } else {
        file=fopen(argv[4],"rb");assert(file && xbox_LoadPhysicalMemory(file));fclose(file);
        file=fopen(argv[3],"rb");assert(file);
        if(!strcmp(argv[1],"reject")) {
            s_gpu.clear_color=0xdeadbeef;assert(!nv2a_gpu_load_checkpoint(file));fclose(file);
            assert(s_gpu.clear_color==0xdeadbeef);return 0;
        }
        assert(nv2a_gpu_load_checkpoint(file));fclose(file);verify();verify_consumers();
    }
    xbox_CheckpointEnd();nv2a_fifo_stop(gpu);xbox_MemoryLayoutShutdown();return 0;
}
'''
    # Count calls in the actual compiled decoder without a production tracing hook.
    vsh_source=(root/'src/kernel/nv2a_vsh_interp.c').read_text(encoding='utf-8')
    entry='void nv2a_vsh_decode_instruction(const uint32_t ins[4], Nv2aVshInstruction *d)\n{'
    assert vsh_source.count(entry)==1
    (work/'vsh_counted.c').write_text(vsh_source.replace(entry,entry+'\n    ++vsh_decode_calls;'),encoding='utf-8')
    # Keep the original shader/buffer implementation; intercept only its real geometry Map for counting/failure injection.
    vsh=(root/'src/d3d/d3d8_vsh.c').read_text(encoding='utf-8')
    geometry=(root/'src/d3d/d3d8_nv2a_geometry.inc').read_text(encoding='utf-8')
    entry='hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)g_vsh_geometry_cb'
    assert geometry.count(entry)==1
    (work/'d3d8_nv2a_geometry.inc').write_text(geometry.replace(entry,'hr=geometry_map_for_test(ctx,(ID3D11Resource *)g_vsh_geometry_cb'),encoding='utf-8')
    (work/'vsh_geometry_test.c').write_text(vsh,encoding='utf-8')
    (work/'combiner_counted.c').write_text(r'''
#include "d3d8_internal.h"
unsigned combiner_test_maps,combiner_test_fail;
static HRESULT combiner_map_for_test(ID3D11DeviceContext *ctx,ID3D11Resource *resource,UINT subresource,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE *mapped) {
    ++combiner_test_maps;if(combiner_test_fail) {combiner_test_fail=0;return E_OUTOFMEMORY;}
    return ID3D11DeviceContext_Map(ctx,resource,subresource,type,flags,mapped);
}
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map combiner_map_for_test
#include "d3d8_combiners.c"
unsigned combiner_test_shader_count(void) { unsigned count=0;for(unsigned i=0;i<COMBINER_CACHE_SIZE;i++)count+=g_cache[i].in_use!=0;return count; }
''',encoding='utf-8')
    depth_checks=(root/'tools/recomp/test_depth_coherence.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_software_depth.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_integer_shadow.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_luma16.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_tile_blit.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_bump.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_pair_texture.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_bump_luma_order.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_stage.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_cube.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_reflection.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_diffuse.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_remap.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_hilo.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_constant_eye.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_dot_str.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_volume_texture.c.inc').read_text(encoding='utf-8')+'\n'+(root/'tools/recomp/test_project3d_volume.c.inc').read_text(encoding='utf-8')
    depth_checks+='\n'+(root/'tools/recomp/test_combiner_effective.c.inc').read_text(encoding='utf-8')
    source=source.replace('int main(int argc,char **argv) {',depth_checks+'\nint main(int argc,char **argv) {')
    source=source.replace('    if(!strncmp(argv[1],"fifo-worker",11)) {','    if(!strcmp(argv[1],"standing-volume-w")) { verify_standing_volume_w(); }\n    else if(!strncmp(argv[1],"volume-missing-palette-",23)) { verify_volume_missing_palette((unsigned)atoi(argv[1]+23)); }\n    else if(!strcmp(argv[1],"project3d-volume")) { verify_project3d_volume(); }\n    else if(!strcmp(argv[1],"volume-texture")) { verify_volume_texture(); }\n    else if(!strcmp(argv[1],"dot-str-texture")) { verify_dot_str_texture(); }\n    else if(!strcmp(argv[1],"constant-eye-texture")) { verify_constant_eye_texture(); }\n    else if(!strcmp(argv[1],"dot-hilo-texture")) { verify_dot_hilo_texture(); }\n    else if(!strcmp(argv[1],"dot-remap-texture")) { verify_dot_remap_texture(); }\n    else if(!strcmp(argv[1],"dot-diffuse-texture")) { verify_dot_diffuse_texture(); }\n    else if(!strcmp(argv[1],"dot-reflection-texture")) { verify_dot_reflection_texture(); }\n    else if(!strcmp(argv[1],"dot-cube-texture")) { verify_dot_cube_texture(); }\n    else if(!strcmp(argv[1],"dot-texture-stage")) { verify_dot_texture_stage(); }\n    else if(!strcmp(argv[1],"bump-luma-order")) { verify_bump_luma_order(); }\n    else if(!strcmp(argv[1],"pair-texture")) { verify_pair_texture(); }\n    else if(!strcmp(argv[1],"bump-texture")) { verify_bump_texture(); }\n    else if(!strcmp(argv[1],"tile-blit-guard")) { verify_tile_blit_guard(); }\n    else if(!strcmp(argv[1],"luma16-texture")) { verify_luma16_texture(); }\n    else if(!strcmp(argv[1],"integer-shadow-textures")) { verify_integer_shadow_textures(); }\n    else if(!strcmp(argv[1],"software-depth-ram")) { verify_software_depth_ram(); }\n    else if(!strcmp(argv[1],"depth-write-guards")) { verify_depth_write_guards(); }\n    else if(!strcmp(argv[1],"depth-ram-coherence")) { verify_depth_ram_coherence(); }\n    else if(!strncmp(argv[1],"fifo-worker",11)) {')
    source=source.replace('    if(!strcmp(argv[1],"standing-volume-w")) {','    if(!strcmp(argv[1],"combiner-effective")) { verify_combiner_effective_key(); }\n    else if(!strcmp(argv[1],"standing-volume-w")) {')
    (work/'gpu.c').write_text(source,encoding='utf-8')
    (work/'resources_counted.c').write_text(r'''
#include "d3d8_internal.h"
typedef struct { char operation; ID3D11Resource *resource; } ReadbackTrace;
ReadbackTrace readback_trace[128];unsigned readback_trace_count;
static void trace_readback(char operation,ID3D11Resource *resource) {
    if(readback_trace_count<128)readback_trace[readback_trace_count]=(ReadbackTrace){operation,resource};
    ++readback_trace_count;
}
static void readback_copy_for_test(ID3D11DeviceContext *context,ID3D11Resource *target,UINT target_subresource,UINT x,UINT y,UINT z,ID3D11Resource *source,UINT source_subresource,const D3D11_BOX *box) {
    trace_readback('C',target);ID3D11DeviceContext_CopySubresourceRegion(context,target,target_subresource,x,y,z,source,source_subresource,box);
}
static HRESULT readback_map_for_test(ID3D11DeviceContext *context,ID3D11Resource *resource,UINT subresource,D3D11_MAP kind,UINT flags,D3D11_MAPPED_SUBRESOURCE *mapped) {
    trace_readback('M',resource);return ID3D11DeviceContext_Map(context,resource,subresource,kind,flags,mapped);
}
#undef ID3D11DeviceContext_CopySubresourceRegion
#define ID3D11DeviceContext_CopySubresourceRegion readback_copy_for_test
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map readback_map_for_test
#include "d3d8_resources.c"
''',encoding='utf-8')
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    result=subprocess.run([compiler,'/nologo','/O2','/Gy','/MD','/utf-8',
        '/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/D_USE_MATH_DEFINES',
        '/I'+str(root/'src'),'/I'+str(root/'src/kernel'),'/I'+str(root/'src/d3d'),str(work/'gpu.c'),str(work/'resources_counted.c'),str(work/'combiner_counted.c'),
        str(root/'src/kernel/xbox_memory_layout.c'),str(root/'src/kernel/recomp_continuation.c'),
        str(root/'src/kernel/kernel_hal.c'),str(root/'src/kernel/kernel_sync.c'),
        '/Fe:'+str(work/'gpu.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],
        'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib',
        'd3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib',
        'xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib'],
        cwd=work,capture_output=True,text=True,errors='replace')
    assert result.returncode==0,result.stdout+result.stderr
    shutil.copytree(root.parents[1]/'backends',work/'backends')
    fixture=work/'fixture.xbe'
    subprocess.run([sys.executable,str(root/'tools/conformance/mkxbe.py'),str(fixture)],check=True,capture_output=True)
    return work/'gpu.exe',fixture


@pytest.mark.parametrize('hardware,backend,scale',[
    ('0','D3D11',1),('1','D3D11',1),('1','D3D11',2),
    ('1','Vulkan',1),('1','Vulkan',2),('1','Vulkan',3),('1','Vulkan',4),
    ('1','OpenGL',1),('1','OpenGL',2)])
def test_original_gpu_state_and_depth_survive_same_and_new_process(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu;snapshot=tmp_path/'gpu.state';memory=tmp_path/'memory.state'
    def run(mode,path=snapshot):
        result=subprocess.run([str(executable),mode,str(xbe),str(path),str(memory),hardware],
                              env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
                              capture_output=True,text=True,errors='replace',timeout=30)
        assert result.returncode==0,result.stdout+result.stderr
    run('save');run('load');bad=tmp_path/'bad.state';bad.write_bytes(snapshot.read_bytes()[:-1]);run('reject',bad)

@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_original_swizzled_colour_upload_and_dirty_region(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'swizzled-regions',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SWIZZLE REGION]' in result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('D3D12',1),('D3D12',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_clean_texture_overlap_keeps_unrelated_colour_deferred(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'clean-overlap',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[CLEAN OVERLAP]' in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_texture_content_reuse_observes_all_existing_writers(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'texture-reuse',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[TEXTURE REUSE]' in result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_original_colour_protection_avoids_identical_os_updates(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'protection-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'mixed-region and partial-page transitions' in result.stderr
    for line in result.stderr.splitlines():
        if line.startswith('[PROTECTION]'):print(line)


@pytest.mark.parametrize('backend,scale',[('D3D11',2),('D3D11',4),('D3D12',2),('D3D12',4),('OpenGL',2),('OpenGL',4)])
def test_original_flip_queues_both_readbacks_before_mapping(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'readback-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[READBACK]' in result.stderr


def test_original_method_dispatch_word_work(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'method-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},
        capture_output=True,text=True,errors='replace',timeout=30)
    for line in result.stderr.splitlines():
        if line.startswith(('[METHOD WORK]','[PGRAPH WORK]')):print(line)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_completed_output_establishes_final_protection_once(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'output-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    for line in result.stderr.splitlines():
        if line.startswith(('[OUTPUT WORK]','[QUERY PAYLOAD]','[EVENT RESOURCE]','[QUERY CONTEXT]')):print(line)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_sequential_output_guard_reuse_and_write_lifecycle(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'output-guard-sequential',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    for line in result.stderr.splitlines():
        if line.startswith(('[OUTPUT GUARD SEQUENTIAL]','[OUTPUT GUARD LIFETIME]')):print(line)
    assert result.returncode==0,result.stdout+result.stderr


def test_inline_and_disabled_attributes_avoid_guest_base_query(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'attribute-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},
        capture_output=True,text=True,errors='replace',timeout=30)
    for line in result.stderr.splitlines():
        if line.startswith(('[ATTRIBUTE]','[ATTRIBUTE BASE]','[ATTRIBUTE ALIAS]')):print(line)
    assert result.returncode==0,result.stdout+result.stderr


def test_automatic_backend_uses_opengl_when_vulkan_driver_is_unavailable(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'save',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'Auto','VK_LOADER_DRIVERS_DISABLE':'*',
             'VK_LOADER_LAYERS_DISABLE':'~implicit~','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[GPU] active backend OpenGL' in result.stderr


def test_automatic_backend_reaches_direct3d_after_two_failures(native_gpu,tmp_path):
    executable,xbe=native_gpu
    library=executable.parent/'backends/opengl/d3d11.dll'
    saved=library.with_suffix('.disabled')
    library.rename(saved)
    try:
        result=subprocess.run([str(executable),'save',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
            env={**os.environ,'RECOMP_GPU_BACKEND':'Auto','VK_LOADER_DRIVERS_DISABLE':'*',
                 'VK_LOADER_LAYERS_DISABLE':'~implicit~','DXVK_LOG_PATH':str(tmp_path)},
            capture_output=True,text=True,errors='replace',timeout=30)
    finally:
        saved.rename(library)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[GPU] Vulkan initialization failed:' in result.stderr
    assert '[GPU] OpenGL initialization failed:' in result.stderr
    assert '[GPU] trying D3D12' in result.stderr
    if '[GPU] active backend D3D12' not in result.stderr:
        assert '[GPU] D3D12 initialization failed:' in result.stderr
        assert '[GPU] active backend D3D11' in result.stderr

@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','Vulkan'),('1','OpenGL')])
def test_original_dma_completion_aliases_and_checkpoint(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'semaphore',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'2','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_native_vertex_payload_matches_original_attributes_and_indices(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'vertex-payload',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr


def test_original_vsh_upload_updates_coalesce_at_real_consumers(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'vsh-updates',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11'},capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'four changed upload words use one original decode' in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','OpenGL'])
def test_original_geometry_buffer_content_updates_and_lifetime(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'geometry-updates',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'failed Map retry and resource recreation passed' in result.stderr


def test_original_clear_preserves_channels_clip_layout_and_padding(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'clear-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    for line in result.stderr.splitlines():
        if line.startswith('[CLEAR WORK]'):print(line)


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_original_pixel_state_uploads_preserve_bytes_and_binding(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'pixel-state-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    for line in result.stderr.splitlines():
        if line.startswith('[PIXEL STATE]'):print(line)


@pytest.mark.parametrize('backend',['D3D11','D3D12','OpenGL'])
def test_original_combiner_buffer_both_writers(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'combiner-work',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    for line in result.stderr.splitlines():
        if line.startswith('[COMBINER]'):print(line)


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_original_full_clear_cpu_and_gpu_transfer(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'clear-transfer',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    for line in result.stderr.splitlines():
        if line.startswith('[CLEAR TRANSFER]'):print(line)


@pytest.mark.parametrize('backend',['D3D11','Vulkan'])
def test_original_linear_texgen_fixed_consumer(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-texgen',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'3','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FIXED TEXGEN]' in result.stderr


@pytest.mark.parametrize('producer',['array32','array16','native16','draw','inline','immediate'])
@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_large_batches_preserve_geometry(native_gpu,tmp_path,producer,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'large-'+producer,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LARGE BATCH]' in result.stderr


@pytest.mark.parametrize('producer',['array32','array16','native16','draw','inline','immediate'])
def test_batch_capacity_boundary_is_explicit(native_gpu,tmp_path,producer):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'boundary-'+producer,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==5,result.stdout+result.stderr
    assert '[BATCH BOUNDARY] full' in result.stderr
    assert 'batch exceeds NV2A capacity:' in result.stderr


def test_native_program_resource_preserves_state_and_partial_publication(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'native-program',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[NATIVE PROGRAM]' in result.stderr
    print(next(line for line in result.stderr.splitlines() if line.startswith('[NATIVE PROGRAM]')))


@pytest.mark.parametrize('invalid',['extent','put','consumer','class','block-count','block-method','block-extent'])
def test_native_program_resource_rejects_invalid_submission(native_gpu,tmp_path,invalid):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'reject-native-program-'+invalid,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==5,result.stdout+result.stderr
    assert '[STOP] PFIFO native program' in result.stderr


def test_native_index_range_preserves_complete_state_and_pending_publication(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'native-indices',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[NATIVE INDICES]' in result.stderr
    print(next(line for line in result.stderr.splitlines() if line.startswith('[NATIVE INDICES]')))


@pytest.mark.parametrize('invalid',['extent','put','consumer','class'])
def test_native_index_range_rejects_invalid_submission(native_gpu,tmp_path,invalid):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'reject-native-index-'+invalid,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==5,result.stdout+result.stderr
    expected='native index range outside DMA data' if invalid in ('extent','put') else 'native indices require the active game renderer'
    assert '[STOP] PFIFO '+expected in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
@pytest.mark.parametrize('partial',[False,True])
@pytest.mark.parametrize('operation',['constant','index','program'])
def test_fifo_worker_preserves_pending_checkpoint_and_gpu_completion(native_gpu,tmp_path,backend,partial,operation):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fifo-worker' if operation=='constant' else 'fifo-worker-'+operation,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1','partial' if partial else 'complete'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FIFO CHECKPOINT]' in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_cached_vertex_conversion_observes_original_writers_and_restore(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'vertex-reuse',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[VERTEX REUSE]' in result.stderr


@pytest.mark.parametrize('backend,scale',[(b,s) for b in ('D3D11','D3D12','Vulkan','OpenGL') for s in (1,2)])
def test_native_finished_image_avoids_ram_roundtrip(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'native-display',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=40)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[NATIVE DISPLAY]' in result.stderr


@pytest.mark.parametrize('backend,scale',[(b,s) for b in ('D3D11','D3D12','Vulkan','OpenGL') for s in (1,2)])
def test_clear_keeps_shared_page_and_unrelated_owners(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'clear-ownership',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=40)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[CLEAR OWNERSHIP]' in result.stderr


@pytest.mark.parametrize('hardware,backend,scale', [('0','D3D11',1)]+[
    ('1',backend,scale) for backend in ('D3D11','D3D12','Vulkan','OpenGL') for scale in (1,2)])
def test_polygon_offset_reaches_original_depth_consumer(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-offset',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON OFFSET]' in result.stderr


@pytest.mark.parametrize('hardware,backend,scale', [('0','D3D11',1)]+[
    ('1',backend,scale) for backend in ('D3D11','D3D12','Vulkan','OpenGL') for scale in (1,2)])
def test_stencil_operations_reach_existing_depth_consumer(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'stencil-operations',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[STENCIL]' in result.stderr


@pytest.mark.parametrize('hardware,backend,scale', [('0','D3D11',1)]+[
    ('1',backend,scale) for backend in ('D3D11','D3D12','Vulkan','OpenGL') for scale in (1,2)])
def test_depth_encodings_match_raw_clear_and_original_comparison(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'depth-encodings',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[DEPTH ENCODING]' in result.stderr


@pytest.mark.parametrize('hardware,backend,scale', [('0','D3D11',1)]+[
    ('1',backend,scale) for backend in ('D3D11','D3D12','Vulkan','OpenGL') for scale in (1,2)])
def test_colour_logic_operations_and_output_state_transitions(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'colour-logic',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[COLOUR LOGIC]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','Vulkan'),('1','OpenGL')])
def test_bound_kelvin_on_all_nonzero_subchannels(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'subchannel-kelvin',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SUBCHANNEL]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_single_point_and_two_vertex_line_submission(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'short-primitives',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SHORT PRIMITIVES]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_texture_address_modes_and_border_updates(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'texture-addressing',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[TEXTURE ADDRESS]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_fixed_skinning_uses_original_matrices_and_weights(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-skinning',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SKINNING]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_fixed_spot_and_specular_rational_coefficients(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-lighting-coefficients',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LIGHTING COEFFICIENTS]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_two_sided_program_and_unlit_fixed_vertex_colors(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'two-sided-colors',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[TWO SIDED COLORS]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_rectangle_point_sprite_reuses_native_uv_generation(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'rectangle-point-sprite',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[RECTANGLE SPRITE]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_missing_formats_reuse_existing_resource_conversion(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'resource-format-connections',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[RESOURCE FORMATS]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_texture_color_keys_reach_original_fragment_consumers(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'texture-color-keys',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[COLOR KEYS]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_bound_image_blit_copy_uses_original_dma_and_memory_ownership(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'image-blit-copy',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[IMAGE BLIT]' in result.stderr

@pytest.mark.parametrize('hardware,backend', [('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_dependent_textures_use_original_earlier_stage_outputs(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'dependent-textures',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[DEPENDENT TEXTURES]' in result.stderr


@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
def test_fixed_two_sided_material_lighting_consumes_original_back_state(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-two-sided-lighting',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FIXED TWO SIDED LIGHTING]' in result.stderr

@pytest.mark.parametrize('backend', ['D3D11','D3D12','Vulkan','OpenGL'])
@pytest.mark.parametrize('scale', [1,2,3,4])
def test_polygon_modes_edges_and_width_use_original_raster_consumers(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-raster',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    for line in result.stderr.splitlines():
        if line.startswith('[POLYGON LINE SUBMISSIONS]'):print(line)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON RASTER]' in result.stderr


@pytest.mark.parametrize('mode',['two-sided-colors','fixed-two-sided-lighting','fixed-skinning'])
def test_software_original_vertex_back_outputs_and_fixed_skinning(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr

@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_original_fixed_material_scene_and_light_products(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-material-products',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FIXED MATERIAL PRODUCTS]' in result.stderr

@pytest.mark.parametrize('hardware,backend,scale', [('0','D3D11',1),('1','D3D11',1),('1','Vulkan',2)])
def test_polygon_stipple_uses_original_fragment_consumers(native_gpu,tmp_path,hardware,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-stipple',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON STIPPLE]' in result.stderr

@pytest.mark.parametrize('backend,scale', [('D3D11',1),('Vulkan',2)])
def test_polygon_point_params_use_original_fixed_vertex_output(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-point-params',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON POINT PARAMS]' in result.stderr

@pytest.mark.parametrize('backend,scale', [('D3D11',1),('Vulkan',2)])
def test_screen_output_packing_retains_original_texture_and_fog(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'screen-output-packing',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SCREEN OUTPUT PACKING]' in result.stderr

@pytest.mark.parametrize('hardware', ['0','1'])
def test_software_points_reuse_original_fragment_and_vertex_consumers(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'software-points',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SOFTWARE POINT]' in result.stderr

@pytest.mark.parametrize('hardware', ['0','1'])
def test_point_setup_rounding_precedes_center_clip(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'point-setup-boundary',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POINT SETUP BOUNDARY]' in result.stderr


@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_original_six_specular_coefficients_follow_light_and_eye_mode(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fixed-specular-six',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SIX SPECULAR]' in result.stderr

@pytest.mark.parametrize('hardware', ['0','1'])
def test_software_polygon_points_share_original_fragment_consumers(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'software-polygon-points',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SOFTWARE POLYGON POINTS]' in result.stderr

def test_software_polygon_point_params_use_original_fixed_output(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-point-params',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON POINT PARAMS]' in result.stderr

@pytest.mark.parametrize('backend',['D3D11','Vulkan'])
@pytest.mark.parametrize('hardware',[0,1])
def test_original_normal_reflection_texgen(native_gpu,tmp_path,backend,hardware):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'normal-reflection-texgen',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),str(hardware)],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=45)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[NORMAL REFLECTION TEXGEN] 192' in result.stderr

def test_original_texgen_view_rejects_invalid(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'invalid-texgen-view',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==5,result.stdout+result.stderr
    assert 'method=0x09cc parameter=0x00000002' in result.stderr.lower()


def test_software_aliased_lines_match_native_fragment_coverage(native_gpu,tmp_path):
    executable,xbe=native_gpu
    outputs=[]
    for hardware in ['1','0']:
        path=tmp_path/('line-'+hardware+'.bin')
        result=subprocess.run([str(executable),'software-lines',str(xbe),str(path),str(tmp_path/'memory.state'),hardware],
            env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},
            capture_output=True,text=True,errors='replace',timeout=60)
        assert result.returncode==0,result.stdout+result.stderr
        outputs.append(path.read_bytes())
    if outputs[0]!=outputs[1]:
        import struct
        native=struct.unpack('<'+'I'*(len(outputs[0])//4),outputs[0]);software=struct.unpack('<'+'I'*(len(outputs[1])//4),outputs[1])
        mismatch=[(i//4096,(i%4096)%64,(i%4096)//64,hex(a),hex(b)) for i,(a,b) in enumerate(zip(native,software)) if a!=b]
        # Existing software color packing rounds floating point channels;
        # native UNORM can differ by one byte at an interpolation half tie.
        # Coverage/alpha and all raw depth/stencil bytes remain exact.
        failures=[]
        for i,(a,b) in enumerate(zip(native,software)):
            case=i//4096
            color=case>=90 and (case-90)%2==0
            if a!=b and not (color and a>>24==b>>24 and all(abs(((a>>shift)&255)-((b>>shift)&255))<=1 for shift in (0,8,16))):
                failures.append((case,(i%4096)%64,(i%4096)//64,hex(a),hex(b)))
        assert not failures, str(failures[:40])+' total='+str(len(failures))


def test_software_polygon_lines_use_original_boundary_fragment_consumer(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'polygon-raster',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[POLYGON RASTER]' in result.stderr


def test_software_polygon_line_slope_uses_original_fill_plane(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'software-line-slope',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LINE SLOPE]' in result.stderr

@pytest.mark.parametrize('mode',['copy','format','awaken','overlap'])
def test_original_m2mf_operational_subset(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'m2mf-'+mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    if mode=='copy':
        assert result.returncode==0,result.stdout+result.stderr
        assert '[M2MF COPY]' in result.stderr
    else:
        assert result.returncode==5,result.stdout+result.stderr
        assert '[STOP]' in result.stderr

def test_original_m2mf_old_gpu_layout_rejected_before_apply(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'m2mf-old-layout',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[M2MF OLD GPU LAYOUT]' in result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('D3D12',1),('Vulkan',1),('OpenGL',1)])
def test_native_polygon_line_slope_uses_original_fill_plane(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'software-line-slope',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LINE SLOPE]' in result.stderr

@pytest.mark.parametrize('mode',['copy','parameter','double','dma','bind','subchannel','negative-pitch'])
def test_original_m2mf_ordinary_notification(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'m2mf-ordinary-'+mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    if mode=='copy':
        assert result.returncode==0,result.stdout+result.stderr
        assert '[M2MF ORDINARY]' in result.stderr
    else:
        assert result.returncode==5,result.stdout+result.stderr
        assert '[STOP]' in result.stderr

@pytest.mark.parametrize('mode',['legal','zero','three','reserved-low','reserved-high','bit11'])
def test_original_m2mf_increment_setup_contract(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'m2mf-increment-'+mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    if mode=='legal':
        assert result.returncode==0,result.stdout+result.stderr
        assert '[M2MF INCREMENT STATE]' in result.stderr
    else:
        assert result.returncode==5,result.stdout+result.stderr
        assert 'method=0x0324' in result.stderr


@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','Vulkan')])
def test_original_smoothing_control1_disables_smooth_consumers(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'smoothing-control',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[SMOOTH CONTROL]' in result.stderr

@pytest.mark.parametrize('mode',['smoothing-control-active','smoothing-control-mask'])
def test_uncertified_smoothing_and_sample_masks_remain_explicit(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==5,result.stdout+result.stderr
    assert '[STOP]' in result.stderr

@pytest.mark.parametrize('mode',['copy','identity','alpha','pattern','key','overlap','patterned','partial-tile'])
def test_original_blit_rop_logic_consumer(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'blit-rop-'+mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    if mode in ('copy','identity'):
        assert result.returncode==0,result.stdout+result.stderr
        assert '[BLIT ROP]' in result.stderr
    else:
        assert result.returncode==5,result.stdout+result.stderr
        assert 'class=0x9F' in result.stderr


def test_software_constant_w_polygon_slope_is_exact_zero(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'constant-w-slope',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[CONSTANT W]' in result.stderr

def test_software_nonconstant_w_polygon_slope_remains_explicit(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'constant-w-slope-reject',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==5,result.stdout+result.stderr
    assert '[STOP] W-buffer polygon slope' in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_native_constant_w_slope_consumes_existing_screen_outputs(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'native-constant-w-slope',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[CONSTANT W]' in result.stderr

@pytest.mark.parametrize('mode',['copy','alpha','pattern','key','overlap','partial-tile'])
def test_original_blit_source_copy_and_consumer(native_gpu,tmp_path,mode):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'blit-copy-and-'+mode,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    if mode=='copy':
        assert result.returncode==0,result.stdout+result.stderr
        assert '[BLIT COPY AND]' in result.stderr
    else:
        assert result.returncode==5,result.stdout+result.stderr
        assert 'class=0x9F' in result.stderr


@pytest.mark.parametrize('hardware',['0','1'])
def test_legacy_diffuse_4ub_reuses_existing_generic_register_consumer(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'legacy-diffuse-4ub',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],
        env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LEGACY DIFFUSE]' in result.stderr


@pytest.mark.parametrize('hardware',['0','1'])
def test_legacy_attribute_group_reuses_original_generic_consumers(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    failures=[]
    for entry in range(10):
        result=subprocess.run([str(executable),'legacy-attribute-'+str(entry),str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
        if result.returncode!=0:failures.append(f'entry={entry} return={result.returncode}\n{result.stderr}')
        else:print(result.stderr[result.stderr.find('[LEGACY GROUP]'):])
    assert not failures,'\n'.join(failures)

@pytest.mark.parametrize('hardware',['0','1'])
def test_legacy_texture_and_fog_use_original_generic_consumer(native_gpu,tmp_path,hardware):
    executable,xbe=native_gpu
    failures=[]
    for entry in range(17):
        result=subprocess.run([str(executable),'legacy-texfog-'+str(entry),str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=60)
        if result.returncode!=0:failures.append(f'entry={entry} return={result.returncode}\n{result.stderr}')
        else:print(result.stderr[result.stderr.find('[LEGACY TEXFOG]'):])
    assert not failures,'\n'.join(failures)

def test_blit_exact_self_alias_consumes_original_logic(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'blit-exact-self-alias',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[BLIT SELF ALIAS]' in result.stderr

@pytest.mark.parametrize('backend',['D3D11','Vulkan'])
def test_host_file_write_publishes_only_overlapping_pages(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'host-write-scope',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1'},capture_output=True,text=True,errors='replace',timeout=45)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[HOST WRITE SCOPE]' in result.stderr


def test_blit_same_pitch_linear_copy_selects_overlap_direction(native_gpu,tmp_path):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'blit-overlap-direction',str(xbe),'-','-','0'],
                          capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[BLIT DIRECTION]' in result.stderr


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('D3D12',1),('OpenGL',1)])
def test_combiner_effective_fields_reuse_original_shader_cache(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'combiner-effective',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[COMBINER EFFECTIVE] byte-identical' in result.stderr
