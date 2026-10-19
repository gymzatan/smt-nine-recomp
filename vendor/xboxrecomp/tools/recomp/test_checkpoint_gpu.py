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
typedef struct { char operation; ID3D11Resource *resource; } ReadbackTrace;
extern ReadbackTrace readback_trace[128];
extern unsigned readback_trace_count;
static unsigned char readback_present_pixels[4096];
static unsigned readback_present_bytes;
extern void xbox_FramebufferWindowPresentHost(const void *,unsigned,unsigned,unsigned);
static void readback_present_for_test(const void *pixels,unsigned pitch,unsigned width,unsigned height) {
    readback_present_bytes=width*height*4;assert(readback_present_bytes<=sizeof readback_present_pixels);
    for(unsigned y=0;y<height;++y)memcpy(readback_present_pixels+(size_t)y*width*4,(const unsigned char *)pixels+(size_t)y*pitch,(size_t)width*4);
    xbox_FramebufferWindowPresentHost(pixels,pitch,width,height);
}
#define xbox_FramebufferWindowPresentHost readback_present_for_test
static unsigned protection_test_calls,protection_test_identical,protection_test_inspect=1;
static BOOL protection_for_test(void *address,SIZE_T bytes,DWORD protection,DWORD *previous) {
    MEMORY_BASIC_INFORMATION region;
    ++protection_test_calls;
    if(protection_test_inspect && VirtualQuery(address,&region,sizeof region) && region.Protect==protection &&
       bytes<=region.RegionSize-((uintptr_t)address-(uintptr_t)region.BaseAddress)) ++protection_test_identical;
    return VirtualProtect(address,bytes,protection,previous);
}
#define VirtualProtect protection_for_test
static unsigned attribute_offset_calls;
static __declspec(noinline) ptrdiff_t attribute_offset_for_test(void) {
    ++attribute_offset_calls;
    return xbox_GetMemoryOffset();
}
#define xbox_GetMemoryOffset attribute_offset_for_test
static unsigned query_status_calls,query_event_payload_calls;
static unsigned query_context_getter_calls;
static UINT vertex_upload_test_bytes;
static UINT vertex_upload_for_test(const void *data,UINT bytes,ID3D11Buffer **buffer) {
    vertex_upload_test_bytes=bytes;
    return d3d8_upload_vertices(data,bytes,buffer);
}
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
#include "kernel/nv2a_pb_exec.c"
#undef d3d8_upload_vertices
#undef d3d8_GetD3D11Context
#pragma pop_macro("ID3D11DeviceContext_Map")
#pragma pop_macro("ID3D11Device_CreateQuery")
#undef ID3D11DeviceContext_GetData
static HRESULT pixel_state_map_for_test(ID3D11DeviceContext *ctx,ID3D11Resource *resource,UINT subresource,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE *mapped) {
    if(resource==(ID3D11Resource *)s_hw_ps_cb)++pixel_state_maps;
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
    method(NV097_SET_SURFACE_FORMAT,8|(2<<4));method(NV097_SET_SURFACE_PITCH,32);
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
        assert(fabsf(depth-(float)0x7f0000/16777215.0f)<1e-6f);fclose(dump);
    } else {
        ZetaBuffer *zeta=zbuf_current(0);assert(zeta && zeta->offset==0x80030000);
        assert(zeta->z[0]==(float)0x7f0000 && zeta->stencil[0]==0xa5);
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
        HwColor *color=s_hw_colors;assert(color && color->host_surface);
        D3D8Surface *surface=(D3D8Surface *)color->host_surface;
        assert(surface->width==color->width*d3d8_RenderScale() && surface->height==color->height*d3d8_RenderScale());
        size_t row=(size_t)surface->width*4,bytes=row*surface->height;unsigned char *expected=malloc(bytes);assert(expected);
        D3DLOCKED_RECT pixels;assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
        for(unsigned y=0;y<surface->height;y++)memcpy(expected+row*y,(unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,row);
        assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));
        FILE *snapshot=tmpfile();assert(snapshot && nv2a_gpu_save_checkpoint(snapshot));
        const float changed[4]={0,1,0,1};ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),surface->rtv,changed);
        rewind(snapshot);assert(nv2a_gpu_load_checkpoint(snapshot));fclose(snapshot);
        color=s_hw_colors;surface=(D3D8Surface *)color->host_surface;
        assert(SUCCEEDED(color->host_surface->lpVtbl->LockRect(color->host_surface,&pixels,NULL,D3DLOCK_READONLY)));
        for(unsigned y=0;y<surface->height;y++)assert(!memcmp(expected+row*y,(unsigned char *)pixels.pBits+(size_t)y*pixels.Pitch,row));
        assert(SUCCEEDED(color->host_surface->lpVtbl->UnlockRect(color->host_surface)));free(expected);
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
    D3DLOCKED_RECT locked;hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,D3DLOCK_READONLY),"test native upload pixels");
    for(unsigned y=0;y<256;y++)assert(!memcmp((uint8_t *)locked.pBits+(size_t)y*locked.Pitch,linear+y*128,128u*4));
    hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"test native upload unlock");
    const float rgba[4]={102.0f/255,85.0f/255,68.0f/255,1};
    D3D8Surface *surface=(D3D8Surface *)hw_render_surface(color);
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),surface->rtv,rgba);
    color->dirty=1;color->dirty_bounds=(RECT){17,129,119,233};hw_copy_color_to_memory(color);
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++) {
        uint32_t at=swizzle_offset(x,y,128,256),expected=x>=17&&x<119&&y>=129&&y<233?0xFF665544u:linear[y*128+x];
        assert(((const uint32_t *)memory)[at]==expected);
    }
    memcpy(memory,swizzled,bytes);hw_upload_native_color(color);
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
    hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,D3DLOCK_READONLY),"test retained native extent");
    for(unsigned y=0;y<256;y++)for(unsigned x=0;x<128;x++)
        assert(((const uint32_t *)((const uint8_t *)locked.pBits+(size_t)y*locked.Pitch))[x]==((const uint32_t *)memory)[swizzle_offset(x,y,128,128)]);
    hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"test retained native extent unlock");
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
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(unrelated))->rtv,rgba);
    unrelated->dirty=1;unrelated->dirty_bounds=(RECT){0,0,8,8};
    s_gpu.texs[0]=(Texture){.offset=source_color->address,.width=128,.height=256,.pitch=512,.color=6,.levels=1,.valid=1};
    s_gpu.rc.stage_program=1;
    uint64_t readbacks=s_gpu.hardware_color_readbacks;
    hw_texture_bind(0);
    assert(unrelated->dirty && s_gpu.hardware_color_readbacks==readbacks);
    assert(s_hw_textures && !memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    hw_defer_output((uint8_t *)(xbox_GetMemoryOffset()+source_color->address),0xffabcdef);
    hw_texture_bind(0);
    assert(!s_hw_query_count && memory[0]==0xffabcdef && !unrelated->dirty);
    assert(s_gpu.hardware_color_readbacks==readbacks+1);
    assert(!memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    readbacks=s_gpu.hardware_color_readbacks;
    unrelated->dirty=1;
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(source_color))->rtv,rgba);
    source_color->dirty=1;source_color->dirty_bounds=(RECT){0,0,128,256};
    hw_texture_bind(0);
    assert(!source_color->dirty && !unrelated->dirty && s_gpu.hardware_color_readbacks==readbacks+2);
    assert(memory[0]==0xffff0000 && !memcmp(s_hw_textures->raw,memory,128u*256u*4u));
    fprintf(stderr,"[CLEAN OVERLAP] clean texture avoids unrelated colour readback; dirty texture still publishes original pixels\n");
}
static LONG CALLBACK observe_output_fault(PEXCEPTION_POINTERS e) {
    if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->ExceptionInformation[0]<=1) {
        SetEvent(fault_observed);
        if(nv2a_pb_handle_memory_fault(e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionInformation[0])) return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI write_output(LPVOID address) {*(volatile uint32_t *)address=0x33445566;return *(volatile uint32_t *)address;}
static void verify_vertex_bytes(const unsigned char *expected,UINT bytes,UINT vertex_bytes,UINT expected_stride) {
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    ID3D11Buffer *vb=NULL,*ib=NULL;UINT stride,offset,index_offset;DXGI_FORMAT format;
    ID3D11DeviceContext_IAGetVertexBuffers(ctx,0,1,&vb,&stride,&offset);
    ID3D11DeviceContext_IAGetIndexBuffer(ctx,&ib,&format,&index_offset);
    fprintf(stderr,"[VERTEX PAYLOAD] stride=%u bytes=%u index_offset=%u\n",stride,vertex_upload_test_bytes,index_offset-offset);
    assert(stride==expected_stride && vertex_upload_test_bytes==bytes);
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
    int length=0;const uint32_t *program=nv2a_vsh_program(&length);
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
    method(NV097_SET_SURFACE_PITCH,640*4);
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
    ID3D11DeviceContext_ClearRenderTargetView(d3d8_GetD3D11Context(),((D3D8Surface *)hw_render_surface(color))->rtv,rgba);
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
static void pixel_state_bytes(uint32_t bytes[28]) {
    D3D11_BUFFER_DESC desc;ID3D11Buffer_GetDesc(s_hw_ps_cb,&desc);assert(desc.ByteWidth==112);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ID3D11Buffer *staging=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&staging)));
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)s_hw_ps_cb);
    D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
    memcpy(bytes,mapped.pData,112);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);ID3D11Buffer_Release(staging);
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
    uint32_t first[28],current[28],initial_value[28];pixel_state_bytes(first);memcpy(initial_value,first,sizeof first);unsigned initial=pixel_state_maps;assert(initial==1);
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
    D3D11_BUFFER_DESC desc={0};desc.ByteWidth=112;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *other=NULL,*bound=NULL;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&other)));
    unsigned before_rebind=pixel_state_maps;
    ID3D11DeviceContext_PSSetConstantBuffers(d3d8_GetD3D11Context(),1,1,&other);pixel_state_draw();assert(pixel_state_maps==before_rebind);
    ID3D11DeviceContext_PSGetConstantBuffers(d3d8_GetD3D11Context(),1,1,&bound);assert(bound==s_hw_ps_cb);ID3D11Buffer_Release(bound);ID3D11Buffer_Release(other);
    pixel_state_bytes(first);assert(!memcmp(first,current,sizeof first));
    rewind(checkpoint);assert(nv2a_gpu_load_checkpoint(checkpoint));fclose(checkpoint);
    unsigned before_restore_draw=pixel_state_maps;
    pixel_state_draw();pixel_state_bytes(current);assert(!memcmp(current,initial_value,sizeof current));assert(pixel_state_maps==before_restore_draw+1);
    fprintf(stderr,"[PIXEL STATE] initial_maps=%u equal64_maps=%u bytes112_equal;changed_clip,raw_depth_bits,rebind,hot_restore passed\n",initial,repeated);
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
        assert(*(const uint32_t *)(memory+offset)==expected);
        assert(*(const uint32_t *)(xbox_GetMemoryOffset()+color->address+offset)==expected);
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
        for(unsigned i=0;i<256;i++) {
            uint32_t value=(i<<24)|((255-i)<<16)|(((i*73)&255)<<8)|((i*151)&255);
            method(NV097_SET_COLOR_CLEAR_VALUE,value);method(NV097_CLEAR_SURFACE,0xf0);
            HwColor *color=hw_color(0);clear_transfer_pixels(color,opaque?value|0xff000000:value);
        }
        unsigned actual=(unsigned)(s_gpu.hardware_color_uploads-uploads);assert(actual==expected);
        uint32_t before=s_gpu.clear_color;if(opaque)before|=0xff000000;
        method(NV097_SET_COLOR_CLEAR_VALUE,0x87654321);uploads=s_gpu.hardware_color_uploads;
        method(NV097_CLEAR_SURFACE,0x10);clear_transfer_pixels(hw_color(0),(before&~0x00ff0000)|0x00650000);
        assert(s_gpu.hardware_color_uploads==uploads+1);
        pg->regs[NV_PGRAPH_CLEARRECTX]=(5u<<16)|2;pg->regs[NV_PGRAPH_CLEARRECTY]=(5u<<16)|2;
        uploads=s_gpu.hardware_color_uploads;method(NV097_CLEAR_SURFACE,0xf0);assert(s_gpu.hardware_color_uploads==uploads+1);
        HwColor *color=hw_color(0);const uint8_t *memory=color->memory->host_view+color->memory_offset;
        D3DLOCKED_RECT pixels;assert(SUCCEEDED(color->surface->lpVtbl->LockRect(color->surface,&pixels,NULL,D3DLOCK_READONLY)));
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
            uint32_t value=x>=2&&x<=5&&y>=2&&y<=5?(opaque?0xff654321:0x87654321):((before&~0x00ff0000)|0x00650000);
            assert(*(const uint32_t *)(memory+color_pixel_offset(color->format,color->pitch,4,x,y))==value);
            assert(((const uint32_t *)((const uint8_t *)pixels.pBits+(size_t)y*pixels.Pitch))[x]==value);
        }
        assert(SUCCEEDED(color->surface->lpVtbl->UnlockRect(color->surface)));
        pg->regs[NV_PGRAPH_CLEARRECTX]=7u<<16;pg->regs[NV_PGRAPH_CLEARRECTY]=7u<<16;
        fprintf(stderr,"[CLEAR TRANSFER] layout=%u opaque=%u full256_uploads=%u; CPU aliases, native/scaled GPU bytes, protection, partial channels and rectangle passed\n",layout,opaque,actual);
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
        method(NV097_SET_SURFACE_PITCH,pitch);
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
    method(NV097_SET_SURFACE_FORMAT,0x128);method(NV097_SET_SURFACE_PITCH,640*4);
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
    hw_complete_commands();assert(s_hw_completion);
    event_query_creates=0;
    query_context_getter_calls=0;
    query_status_calls=query_event_payload_calls=0;
    protection_test_inspect=0;QueryPerformanceCounter(&start);
    for(unsigned i=0;i<100;++i) {
        hw_defer_output(output,i+1);
        protection_test_calls=0;hw_collect_queries(1);
        calls+=protection_test_calls;
        assert(!s_hw_query_count && ldl_le_p(output)==i+1);
        for(unsigned v=0;v<views.view_count;++v) {
            MEMORY_BASIC_INFORMATION region;
            uint8_t *alias=(uint8_t *)views.cpu_views[v]+views.offset;
            assert(VirtualQuery(alias,&region,sizeof region) && region.Protect==PAGE_READWRITE);
            assert(ldl_le_p(alias)==i+1);
        }
    }
    QueryPerformanceCounter(&end);
    fprintf(stderr,"[OUTPUT WORK] completed100 protection_calls=%u views=%u seconds=%.7f\n",calls,views.view_count,
        (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart);
    fprintf(stderr,"[QUERY PAYLOAD] status_calls=%u event_payload_calls=%u\n",query_status_calls,query_event_payload_calls);
    fprintf(stderr,"[QUERY CONTEXT] getter_calls=%u poll_calls=%u\n",query_context_getter_calls,query_status_calls);
    assert(query_context_getter_calls==query_status_calls+200);
    assert(query_status_calls>=100 && query_event_payload_calls==0);
    /* Completion must directly establish the same final protection once. */
    assert(calls==100*views.view_count);
    fprintf(stderr,"[EVENT RESOURCE] completed100 creates=%u\n",event_query_creates);
    assert(event_query_creates==0);
    /* Neither a second pending output nor a synchronous barrier may reuse
     * an event whose original queue entry has not retired. */
    hw_defer_output(output,101);hw_defer_output(output+4,102);
    assert(s_hw_query_count==2 && s_hw_queries[0].query!=s_hw_queries[1].query);
    ID3D11Query *first=s_hw_queries[0].query,*second=s_hw_queries[1].query;
    hw_complete_commands();assert(s_hw_completion!=first && s_hw_completion!=second);
    hw_collect_queries(1);assert(!s_hw_query_count && ldl_le_p(output)==101 && ldl_le_p(output+4)==102);
    assert(s_hw_completion);
    protection_test_inspect=1;
}
static void verify_attribute_offset_work(void) {
    VertexAttr attr={0};float out[4],reference[4];
    const float defaults[4]={0,0,0,1},payload[4]={-1.25f,0.5f,17,2};
    const uint32_t address=0x80070000u;
    unsigned invalid_calls=0,inline_calls=0,array_calls=0;
    int (*volatile read_attribute)(const VertexAttr *,uint32_t,float *)=fetch_attr;
    memcpy((void *)(xbox_GetMemoryOffset()+address),payload,sizeof payload);
    memcpy(s_gpu.inline_buf,payload,sizeof payload);
    attribute_offset_calls=0;
    assert(!read_attribute(&attr,0,out) && !memcmp(out,defaults,sizeof out));
    attr.size=4;
    assert(!read_attribute(&attr,0,out) && !memcmp(out,defaults,sizeof out));
    attr.type=2;attr.stride=16;s_gpu.inline_active=0;
    assert(!read_attribute(&attr,0,out) && !memcmp(out,defaults,sizeof out));
    s_gpu.inline_active=1;s_gpu.inline_count=4;
    assert(!read_attribute(&attr,1,out) && !memcmp(out,defaults,sizeof out));
    invalid_calls=attribute_offset_calls;
    attribute_offset_calls=0;
    assert(read_attribute(&attr,0,out) && !memcmp(out,payload,sizeof out));
    inline_calls=attribute_offset_calls;
    s_gpu.inline_active=0;attr.offset=address;attribute_offset_calls=0;
    assert(read_attribute(&attr,0,out) && !memcmp(out,payload,sizeof out));
    array_calls=attribute_offset_calls;
    /* Both storage paths use the production decoder for every supported format
     * and component count, including signed and unknown-format payloads. */
    const uint32_t types[]={0,2,4,1,5,6,7};
    for(unsigned t=0;t<sizeof types/sizeof types[0];++t) {
        for(unsigned size=1;size<=4;++size) {
            attr.type=types[t];attr.size=size;attr.offset=address;s_gpu.inline_active=0;
            int expected=read_attribute(&attr,0,reference);
            attr.offset=0;s_gpu.inline_active=1;
            assert(read_attribute(&attr,0,out)==expected && !memcmp(out,reference,sizeof out));
        }
    }
    attr.type=2;attr.size=4;attr.offset=0;attr.stride=16;
    LARGE_INTEGER frequency,start,end;assert(QueryPerformanceFrequency(&frequency));
    attribute_offset_calls=0;double checksum=0;
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<1000000;++i) {assert(read_attribute(&attr,0,out));checksum+=out[0];}
    QueryPerformanceCounter(&end);
    fprintf(stderr,"[ATTRIBUTE] invalid=%u inline=%u array=%u million_inline_queries=%u seconds=%.7f checksum=%.0f\n",
        invalid_calls,inline_calls,array_calls,attribute_offset_calls,
        (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
    assert(checksum==-1250000 && invalid_calls==0 && inline_calls==0 && array_calls==0 && attribute_offset_calls==0);
    s_gpu.inline_active=0;attr.offset=address;attribute_offset_calls=0;checksum=0;
    assert(g_xbox_mem_offset==xbox_GetMemoryOffset());
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<1000000;++i) {assert(read_attribute(&attr,0,out));checksum+=out[0];}
    QueryPerformanceCounter(&end);
    fprintf(stderr,"[ATTRIBUTE BASE] million_array_queries=%u seconds=%.7f checksum=%.0f\n",
        attribute_offset_calls,(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
    assert(checksum==-1250000 && attribute_offset_calls==0);
    const unsigned measured_types[2]={2,0};
    for(unsigned t=0;t<2;++t) {
        attr.type=measured_types[t];
        for(unsigned round=0;round<5;++round) {
            checksum=0;QueryPerformanceCounter(&start);
            for(unsigned i=0;i<4000000;++i) {assert(read_attribute(&attr,0,out));checksum+=out[0];}
            QueryPerformanceCounter(&end);
            fprintf(stderr,"[ATTRIBUTE ALIAS] type=%u round=%u seconds=%.7f checksum=%.9f\n",
                attr.type,round,(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart,checksum);
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
        assert(g_vsh_fixed_cb && BRIDGE_MEM32(s_gpu.color_base)==0xffff0000);
        NV2ANativeFixed actual;ID3D11Buffer *copy=NULL;D3D11_BUFFER_DESC desc={sizeof actual,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0,0};
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&desc,NULL,&copy)));ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
        ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)copy,(ID3D11Resource *)g_vsh_fixed_cb);D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)copy,0,D3D11_MAP_READ,0,&mapped)));memcpy(&actual,mapped.pData,sizeof actual);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)copy,0);ID3D11Buffer_Release(copy);
        for(unsigned stage=0;stage<4;stage++){assert(actual.texture_matrix[stage]==(test==2));for(unsigned j=0;j<4;j++)assert(actual.texgen[stage][j]==(test==2?0:test?2:1));}
    }
    fprintf(stderr,"[FIXED TEXGEN] original methods route XYZRHW through fixed vertex consumer; all stage/component and matrix bindings exact; pixels preserved\n");
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
        if(!strcmp(producer,"array16")) {
            for(;i+1<NV2A_MAX_BATCH_LENGTH;i+=2)method(NV097_ARRAY_ELEMENT16,(i&0xffff)|(((i+1)&0xffff)<<16));
        } else if(!strcmp(producer,"draw")) {
            for(;i+255<NV2A_MAX_BATCH_LENGTH;i+=256)method(NV097_DRAW_ARRAYS,i|(255u<<24));
        }
        for(;i<NV2A_MAX_BATCH_LENGTH;i++)method(NV097_ARRAY_ELEMENT32,i);
        assert(s_gpu.idx_count==NV2A_MAX_BATCH_LENGTH && s_gpu.idx[s_gpu.idx_count-1]==NV2A_MAX_BATCH_LENGTH-1);
        fprintf(stderr,"[BATCH BOUNDARY] full index payload retained\n");
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
static void verify_fifo_worker_checkpoint(void) {
    NV2AState *gpu=nv2a_get_state();
    seed();method(NV097_SET_BEGIN_END,0);nv2a_pb_checkpoint_publish();
    gpu->vram_ptr=(uint8_t *)(xbox_GetMemoryOffset()+0x80000000u);
    uint32_t *object=(uint32_t *)(gpu->ramin_ptr+0x400);
    object[0]=2;object[1]=4095;object[2]=0x80000;
    uint32_t *commands=(uint32_t *)(gpu->vram_ptr+0x80000);
    commands[0]=(1u<<18)|NV097_SET_COLOR_CLEAR_VALUE;commands[1]=0xff445566;
    commands[2]=(1u<<18)|NV097_CLEAR_SURFACE;commands[3]=0xf0;
    commands[4]=(1u<<18)|NV097_SET_SEMAPHORE_OFFSET;commands[5]=0x70000;
    commands[6]=(1u<<18)|NV097_BACK_END_WRITE_SEMAPHORE_RELEASE;commands[7]=0x12345678;
    method(NV097_SET_CONTEXT_DMA_SEMAPHORE,0x100);
    BRIDGE_MEM32(0x80070000u)=0;
    uint32_t *r=gpu->pfifo.regs;
    r[NV_PFIFO_MODE]=1;r[NV_PFIFO_CACHE1_PUSH1]=0x100;r[NV_PFIFO_CACHE1_PUSH0]=1;
    r[NV_PFIFO_CACHE1_DMA_PUSH]=1;r[NV_PFIFO_CACHE1_PULL0]=1;r[NV_PFIFO_CACHE1_DMA_INSTANCE]=0x40;
    r[NV_PFIFO_CACHE1_DMA_GET]=r[NV_PFIFO_CACHE1_DMA_STATE]=0;
    gpu->pgraph.waiting_for_nop=false;gpu->pgraph.regs[NV_PGRAPH_FIFO]=1;
    /* Frozen submission remains pending in the canonical saved FIFO. */
    nv2a_mmio_write(gpu,0x800040,32,4);
    assert(r[NV_PFIFO_CACHE1_DMA_GET]==0 && BRIDGE_MEM32(0x80070000u)==0);
    FILE *snapshot=tmpfile(),*devices=tmpfile();assert(snapshot && devices);
    assert(nv2a_gpu_save_checkpoint(snapshot) && xbox_CheckpointWriteDevices(devices));
    rewind(devices);assert(xbox_CheckpointLoadDevices(devices));fclose(devices);
    for(unsigned pass=0;pass<2;pass++) {
        xbox_CheckpointEnd();nv2a_fifo_wait_idle(gpu);
        ULONGLONG deadline=GetTickCount64()+5000;
        while(BRIDGE_MEM32(0x80070000u)!=0x12345678 && GetTickCount64()<deadline) {
            nv2a_pb_request_progress();Sleep(1);
        }
        assert(BRIDGE_MEM32(0x80070000u)==0x12345678);
        assert(xbox_CheckpointBootstrapBegin());nv2a_pb_checkpoint_publish();
        assert(r[NV_PFIFO_CACHE1_DMA_GET]==32 && BRIDGE_MEM32(0x80020000u)==0xff445566);
        if(!pass) {
            BRIDGE_MEM32(0x80070000u)=0;rewind(snapshot);
            assert(nv2a_gpu_load_checkpoint(snapshot));
            assert(r[NV_PFIFO_CACHE1_DMA_GET]==0 && BRIDGE_MEM32(0x80070000u)==0);
        }
    }
    fclose(snapshot);
    fprintf(stderr,"[FIFO CHECKPOINT] frozen queued commands, device barrier, restored pending FIFO, native pixels and real semaphore completion passed\n");
}
int main(int argc,char **argv) {
    assert(argc==6);_putenv_s("RECOMP_GUEST_LOCK","1");_putenv_s("RECOMP_GPU_D3D11",argv[5]);
    const char *render_scale=getenv("RECOMP_RENDER_SCALE");
    d3d8_DisplayConfigure(render_scale?(unsigned)atoi(render_scale):1,4.0f/3.0f);
    FILE *file=fopen(argv[2],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    void *xbe=malloc(bytes);assert(xbe && fread(xbe,bytes,1,file)==1);fclose(file);
    assert(xbox_MemoryLayoutInit(xbe,bytes));free(xbe);
    NV2AState *gpu=nv2a_init_standalone((void *)xbox_GetMemoryOffset(),0x04000000,
                         (void *)(xbox_GetMemoryOffset()+0xfd700000u),0x100000);assert(gpu);
    gpu->pfifo.method_consumer=nv2a_pb_exec_method;
    if(!strcmp(argv[1],"fifo-worker"))assert(nv2a_fifo_start(gpu, 1));
    assert(xbox_CheckpointBootstrapBegin());
    if(!strcmp(argv[1],"fifo-worker")) {
        verify_fifo_worker_checkpoint();
    } else if(!strncmp(argv[1],"boundary-",9)) {
        verify_batch_boundary(argv[1]+9);
    } else if(!strncmp(argv[1],"large-",6)) {
        verify_large_batch(argv[1]+6);
    } else if(!strcmp(argv[1],"fixed-texgen")) {
        verify_fixed_texgen_route();
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
''',encoding='utf-8')
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


@pytest.mark.parametrize('backend,scale',[('D3D11',1),('D3D11',2),('Vulkan',1),('Vulkan',2),('OpenGL',1),('OpenGL',2)])
def test_clean_texture_overlap_keeps_unrelated_colour_deferred(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'clean-overlap',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[CLEAN OVERLAP]' in result.stderr


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


@pytest.mark.parametrize('backend,scale',[('D3D11',2),('D3D11',4),('Vulkan',2),('Vulkan',4),('OpenGL',2),('OpenGL',4)])
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


@pytest.mark.parametrize('backend',['D3D11','Vulkan','OpenGL'])
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


@pytest.mark.parametrize('backend',['D3D11','Vulkan','OpenGL'])
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


@pytest.mark.parametrize('producer',['array32','array16','draw','inline','immediate'])
@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_large_batches_preserve_geometry(native_gpu,tmp_path,producer,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'large-'+producer,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[LARGE BATCH]' in result.stderr


@pytest.mark.parametrize('producer',['array32','array16','draw','inline','immediate'])
def test_batch_capacity_boundary_is_explicit(native_gpu,tmp_path,producer):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'boundary-'+producer,str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==5,result.stdout+result.stderr
    assert '[BATCH BOUNDARY] full' in result.stderr
    assert 'batch exceeds NV2A capacity:' in result.stderr


@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_fifo_worker_preserves_pending_checkpoint_and_gpu_completion(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'fifo-worker',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],
        env={**os.environ,'RECOMP_GPU_BACKEND':backend,'DXVK_LOG_PATH':str(tmp_path)},
        capture_output=True,text=True,errors='replace',timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[FIFO CHECKPOINT]' in result.stderr
