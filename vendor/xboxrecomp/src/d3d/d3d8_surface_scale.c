/* Host resolution changes keep the original guest surface and its aliases.
 * Resampling runs on the selected GPU; the immediate pipeline is preserved. */
#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <string.h>

static ID3D11DeviceContext *deferred;
static ID3D11VertexShader *vertex_shader;
static ID3D11PixelShader *pixel_shader;
static ID3D11Buffer *dimensions;
static ID3D11SamplerState *sampler;
static ID3D11SamplerState *upload_sampler;
static HRESULT initialize(void)
{
    if(deferred)return S_OK;
    static const char source[]=
        "Texture2D pixels:register(t0);SamplerState linearSampler:register(s0);\n"
        "cbuffer Dimensions:register(b0){float4 size;}\n"
        "float4 vs(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}\n"
        "float4 ps(float4 p:SV_Position):SV_Target{return pixels.SampleLevel(linearSampler,p.xy/size.xy,0);}\n";
    ID3D11Device *device=d3d8_GetD3D11Device();ID3DBlob *vs=NULL,*ps=NULL;HRESULT hr;
    hr=D3DCompile(source,sizeof source-1,"surface_scale",NULL,NULL,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,NULL);
    if(SUCCEEDED(hr))hr=D3DCompile(source,sizeof source-1,"surface_scale",NULL,NULL,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,NULL);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),NULL,&vertex_shader);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),NULL,&pixel_shader);
    D3D11_BUFFER_DESC buffer={0};buffer.ByteWidth=16;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateBuffer(device,&buffer,NULL,&dimensions);
    D3D11_SAMPLER_DESC sampling={0};sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampling.MaxLOD=D3D11_FLOAT32_MAX;
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateSamplerState(device,&sampling,&sampler);
    sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateSamplerState(device,&sampling,&upload_sampler);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateDeferredContext(device,0,&deferred);
    if(vs)ID3D10Blob_Release(vs);if(ps)ID3D10Blob_Release(ps);
    return hr;
}
HRESULT d3d8_surface_resample(IDirect3DSurface8 *input,IDirect3DSurface8 *output)
{
    D3D8Surface *src=(D3D8Surface *)input,*dst=(D3D8Surface *)output;
    if(!src || !dst || src==dst || src->sample_count!=1 || dst->sample_count!=1 || !dst->rtv)return E_INVALIDARG;
    HRESULT hr=initialize();if(FAILED(hr))return hr;
    if(!src->scale_srv) {
        hr=ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),(ID3D11Resource *)src->d3d11_texture,NULL,&src->scale_srv);
        if(FAILED(hr))return hr;
    }
    float size[4]={(float)dst->width,(float)dst->height,0,0};
    D3D11_VIEWPORT viewport={0,0,(float)dst->width,(float)dst->height,0,1};
    ID3D11CommandList *commands=NULL;
    ID3D11DeviceContext_UpdateSubresource(deferred,(ID3D11Resource *)dimensions,0,NULL,size,0,0);
    ID3D11DeviceContext_RSSetViewports(deferred,1,&viewport);
    ID3D11DeviceContext_OMSetRenderTargets(deferred,1,&dst->rtv,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(deferred,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(deferred,vertex_shader,NULL,0);
    ID3D11DeviceContext_PSSetShader(deferred,pixel_shader,NULL,0);
    ID3D11DeviceContext_PSSetConstantBuffers(deferred,0,1,&dimensions);
    ID3D11DeviceContext_PSSetShaderResources(deferred,0,1,&src->scale_srv);
    ID3D11SamplerState *selected=dst->width>src->width?upload_sampler:sampler;
    ID3D11DeviceContext_PSSetSamplers(deferred,0,1,&selected);
    ID3D11DeviceContext_Draw(deferred,3,0);
    hr=ID3D11DeviceContext_FinishCommandList(deferred,FALSE,&commands);
    if(SUCCEEDED(hr)){ID3D11DeviceContext_ExecuteCommandList(d3d8_GetD3D11Context(),commands,TRUE);ID3D11CommandList_Release(commands);}
    return hr;
}
