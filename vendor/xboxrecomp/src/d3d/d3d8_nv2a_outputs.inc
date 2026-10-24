/* The original NV2A vertex interpreter has already produced screen-space
 * outputs. This binding carries those outputs to D3D11 without interpreting
 * an Xbox vertex program again. The upload keeps the fixed eight-vector
 * position/color/fog/texture protocol independent of interpreter output fields. */
const char *d3d8_shaders_nv2a_output_source(void)
{
    static const char source[] =
        "cbuffer ScreenCB:register(b0) { float4 screen; };\n"
        "struct VI { float4 pos:POSITION; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:TEXCOORD4; float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3; };\n"
        "struct VO { float4 pos:SV_POSITION; noperspective float4 color0:COLOR0; noperspective float4 color1:COLOR1; float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3; noperspective float fog:TEXCOORD4; noperspective float2 z:TEXCOORD5; };\n"
        "VO main(VI i) { VO o; o.pos=screen.z!=0 ? i.pos : float4((i.pos.x/screen.x*2-1)*i.pos.w,(1-i.pos.y/screen.y*2)*i.pos.w,0,i.pos.w); o.color0=i.d0; o.color1=i.d1; o.fog=i.fog.x; o.z=float2(i.pos.z,0); o.tc0=i.tc0; o.tc1=i.tc1; o.tc2=i.tc2; o.tc3=i.tc3; return o; }\n";
    return source;
}
HRESULT d3d8_shaders_bind_nv2a_outputs(float width, float height, int raw_position)
{
    const char *source=d3d8_shaders_nv2a_output_source();
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    HRESULT hr;
    if (!g_nv2a_output_vs) {
        ID3DBlob *code = NULL, *errors = NULL;
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",4,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,112,D3D11_INPUT_PER_VERTEX_DATA,0},
        };
        D3D11_BUFFER_DESC cb = {0};
        hr = D3DCompile(source, strlen(source), "nv2a_outputs", NULL, NULL,
                        "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            fprintf(stderr,"NV2A outputs: %s\n", errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "compile failed");
            if (errors) ID3D10Blob_Release(errors);
            return hr;
        }
        if (errors) ID3D10Blob_Release(errors);
        hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_nv2a_output_vs);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(device, layout, 8, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &g_nv2a_output_layout);
        ID3D10Blob_Release(code);
        if (FAILED(hr)) return hr;
        cb.ByteWidth=16; cb.Usage=D3D11_USAGE_DYNAMIC; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        hr=ID3D11Device_CreateBuffer(device,&cb,NULL,&g_nv2a_output_cb);
        if (FAILED(hr)) return hr;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    float screen[4] = {width,height,(float)raw_position,1.0f};
    hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)g_nv2a_output_cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData,screen,sizeof screen);
    ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)g_nv2a_output_cb,0);
    ID3D11DeviceContext_GSSetShader(ctx,NULL,NULL,0);
    ID3D11DeviceContext_VSSetShader(ctx,g_nv2a_output_vs,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&g_nv2a_output_cb);
    ID3D11DeviceContext_IASetInputLayout(ctx,g_nv2a_output_layout);
    return S_OK;
}
