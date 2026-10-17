/* Native screen-coordinate setup consumes the original translated VS outputs. */
static const char nv2a_geometry_source[] =
"cbuffer VSC:register(b1) { float4 C[192]; };\n"
"cbuffer Geometry:register(b2) { float4 screen,bounds,ts[4],fog; uint4 flags; float4 pointParams[2],pointSize; uint4 pointFlags; };\n"
"struct V { float4 p:SV_POSITION; float4 d0:COLOR0,d1:COLOR1,t0:TEXCOORD0,t1:TEXCOORD1,t2:TEXCOORD2,t3:TEXCOORD3; float f:FOG,sz:PSIZE; float4 b0:TEXCOORD4,b1:TEXCOORD5; };\n"
"struct O { float4 p:SV_POSITION; noperspective float4 d0:COLOR0,d1:COLOR1; float4 t0:TEXCOORD0,t1:TEXCOORD1,t2:TEXCOORD2,t3:TEXCOORD3; noperspective float f:TEXCOORD4,z:TEXCOORD5; };\n"
"V toclip(V a) { [unroll] for(int j=0;j<3;j++) a.p[j]=C[58][j]!=0 ? (a.p[j]-C[59][j])/C[58][j]*a.p.w : 0; return a; }\n"
"V mixv(V a,V b,float t) { V o; o.p=a.p+(b.p-a.p)*t; o.d0=a.d0+(b.d0-a.d0)*t; o.d1=a.d1+(b.d1-a.d1)*t; o.t0=a.t0+(b.t0-a.t0)*t; o.t1=a.t1+(b.t1-a.t1)*t; o.t2=a.t2+(b.t2-a.t2)*t; o.t3=a.t3+(b.t3-a.t3)*t; o.f=a.f+(b.f-a.f)*t; o.sz=0;o.b0=0;o.b1=0;return o; }\n"
"float ff(float d) { if(fog.w==0) return 1; float f; uint mode=(uint)fog.z; if(mode==0x800 || mode==0x802) f=fog.x+exp2(d*fog.y*16)-1.5; else if(mode==0x801 || mode==0x803) f=fog.x+exp2(-d*d*fog.y*fog.y*32)-1.5; else f=fog.x+d*fog.y-1; if(mode==0x802 || mode==0x803 || mode==0x804) f=abs(f); if(isnan(f)) f=1; return saturate(f); }\n"
"O convert(V v) { O o; o.p=float4((v.p.x/screen.x*2-1)*v.p.w,(1-v.p.y/screen.y*2)*v.p.w,0,v.p.w); o.d0=v.d0;o.d1=v.d1;o.f=ff(v.f);o.z=v.p.z;o.t0=v.t0*ts[0];o.t1=v.t1*ts[1];o.t2=v.t2*ts[2];o.t3=v.t3*ts[3];return o; }\n"
"void emittri(V a,V b,V c,inout TriangleStream<O> stream) { a.p.xy=trunc(a.p.xy*16)/16;b.p.xy=trunc(b.p.xy*16)/16;c.p.xy=trunc(c.p.xy*16)/16; if(a.p.w<=0 || b.p.w<=0 || c.p.w<=0) return; precise float area=(b.p.x-a.p.x)*(c.p.y-a.p.y)-(b.p.y-a.p.y)*(c.p.x-a.p.x); if(area==0 || isnan(area)) return; bool front=(area<0)==(flags.z!=0); if(flags.y!=0 && (flags.w==3 || (front && flags.w==1) || (!front && flags.w==2))) return; float2 lo=floor(min(a.p.xy,min(b.p.xy,c.p.xy))),hi=ceil(max(a.p.xy,max(b.p.xy,c.p.xy))); if(any(max(lo,screen.zw)>=min(hi,bounds.xy))) return; O oa=convert(a),ob=convert(b),oc=convert(c);stream.Append(oa);stream.Append(ob);stream.Append(oc);stream.RestartStrip(); }\n"
"[maxvertexcount(6)] void main(triangleadj V input[6],inout TriangleStream<O> stream) { V a[3]; a[0]=input[0];a[1]=input[2];a[2]=input[4]; if(flags.x!=0) { [unroll] for(int k=0;k<3;k++) { a[k].d0=input[1].d0;a[k].d1=input[1].d1; } } if(a[0].p.w>0.001 && a[1].p.w>0.001 && a[2].p.w>0.001) { emittri(a[0],a[1],a[2],stream);return; } if(a[0].p.w<=0.001 && a[1].p.w<=0.001 && a[2].p.w<=0.001) return; V poly[4]; [unroll] for(int z=0;z<4;z++) poly[z]=(V)0; int n=0; [unroll] for(int i=0;i<3;i++) { V p=a[i],q=a[(i+1)%3]; bool pin=p.p.w>0.001,qin=q.p.w>0.001; if(pin) poly[n++]=toclip(p); if(pin!=qin) { precise float t=(0.001-p.p.w)/(q.p.w-p.p.w); poly[n++]=mixv(toclip(p),toclip(q),t); } } [loop] for(int j=0;j<n;j++) poly[j].p.xyz=poly[j].p.xyz/poly[j].p.w*C[58].xyz+C[59].xyz; [loop] for(int k=1;k+1<n;k++) emittri(poly[0],poly[k],poly[k+1],stream); }\n"
"[maxvertexcount(2)] void main_line(lineadj V input[4],inout LineStream<O> stream) { V a=input[1],b=input[2]; if(flags.x!=0) { a.d0=input[0].d0;b.d0=input[0].d0;a.d1=input[0].d1;b.d1=input[0].d1; } if(a.p.w<=0.001 && b.p.w<=0.001) return; if(a.p.w<=0.001 || b.p.w<=0.001) { precise float t=(0.001-a.p.w)/(b.p.w-a.p.w); V clipped=mixv(toclip(a),toclip(b),t); clipped.p.xyz=clipped.p.xyz/clipped.p.w*C[58].xyz+C[59].xyz; if(a.p.w<=0.001) a=clipped; else b=clipped; } a.p.xy=trunc(a.p.xy*16)/16;b.p.xy=trunc(b.p.xy*16)/16; if(a.p.w<=0 || b.p.w<=0 || any(isnan(a.p)) || any(isnan(b.p))) return; stream.Append(convert(a));stream.Append(convert(b));stream.RestartStrip(); }\n"
"void emitpoint(O a,float size,inout TriangleStream<O> stream) { if(a.p.w<=0 || any(isnan(a.p)) || any(isinf(a.p)) || !isfinite(size) || any(abs(a.p.xy)>a.p.w)) return; size=max(1,floor(size+0.5)); float2 center=float2((a.p.x/a.p.w+1)*0.5*screen.x,(1-a.p.y/a.p.w)*0.5*screen.y); center=((uint)size&1)!=0 ? floor(center)+0.5 : floor(center+0.5); [unroll] for(int k=0;k<4;k++) { float2 uv=float2(k&1,k>>1); float2 pos=center+(uv-0.5)*size; O o=a; o.p.xy=float2(pos.x/screen.x*2-1,1-pos.y/screen.y*2)*a.p.w; if(pointFlags.y!=0) o.t3=float4(uv,1,1); stream.Append(o); } stream.RestartStrip(); }\n"
"[maxvertexcount(4)] void main_point(point V input[1],inout TriangleStream<O> stream) { V a=input[0]; a.p.xy=trunc(a.p.xy*16)/16; emitpoint(convert(a),pointFlags.x!=0 ? a.sz : pointSize.x,stream); }\n"
;

static HRESULT bind_native_geometry(const float *constants,const NV2ANativeGeometry *geometry,unsigned kind)
{
    static const char *entry_points[]={"main","main_line","main_point"};
    ID3D11Device *device=d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    HRESULT hr;
    if(!g_vsh_geometry_shader[kind]) {
        ID3DBlob *code=NULL,*errors=NULL;
        hr=D3DCompile(nv2a_geometry_source,strlen(nv2a_geometry_source),"nv2a_geometry",NULL,NULL,entry_points[kind],"gs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
        if(FAILED(hr)) { fprintf(stderr,"NV2A geometry: %s\n",errors?(char *)ID3D10Blob_GetBufferPointer(errors):"compile failed"); if(errors) ID3D10Blob_Release(errors); return hr; }
        if(errors) ID3D10Blob_Release(errors);
        hr=ID3D11Device_CreateGeometryShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g_vsh_geometry_shader[kind]);
        ID3D10Blob_Release(code); if(FAILED(hr)) return hr;
    }
    if(!g_vsh_geometry_cb) {
        D3D11_BUFFER_DESC desc={0};desc.ByteWidth=sizeof *geometry;desc.Usage=D3D11_USAGE_DYNAMIC;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        hr=ID3D11Device_CreateBuffer(device,&desc,NULL,&g_vsh_geometry_cb);if(FAILED(hr)) return hr;
        g_vsh_geometry_valid=FALSE;
    }
    if(memcmp(&g_vsh_constants,constants,sizeof g_vsh_constants)!=0) d3d8_vsh_set_constant(0,constants,192);
    hr=vsh_upload_constants(ctx);if(FAILED(hr)) return hr;
    if(!g_vsh_geometry_valid || memcmp(&g_vsh_geometry_value,geometry,sizeof *geometry)) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)g_vsh_geometry_cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);if(FAILED(hr)) return hr;
        memcpy(mapped.pData,geometry,sizeof *geometry);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)g_vsh_geometry_cb,0);
        memcpy(&g_vsh_geometry_value,geometry,sizeof *geometry);
        g_vsh_geometry_valid=TRUE;
    }
    ID3D11DeviceContext_GSSetShader(ctx,g_vsh_geometry_shader[kind],NULL,0);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx,1,1,&g_vsh_cb);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx,2,1,&g_vsh_geometry_cb);
    return S_OK;
}

HRESULT d3d8_vsh_bind_native(const DWORD *microcode, int length, const float *constants, const NV2ANativeGeometry *geometry, D3D11_PRIMITIVE_TOPOLOGY topology, uint32_t *inputs_read)
{
    if(!inputs_read) return E_INVALIDARG;
    unsigned kind;
    if(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ) kind=0;
    else if(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ) kind=1;
    else if(topology==D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) kind=2;
    else return E_INVALIDARG;
    ID3D11Device *device=d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    static DWORD previous_program[NV2A_VS_MAX_INSTRUCTIONS * 4];
    static int previous_length;
    static uint32_t previous_hash;
    size_t program_bytes=(size_t)length*16;
    if(previous_length!=length || memcmp(previous_program,microcode,program_bytes)) {
        previous_hash=fnv1a_hash(microcode,program_bytes);
        memcpy(previous_program,microcode,program_bytes);
        previous_length=length;
    }
    uint32_t hash=previous_hash;
    VshCacheEntry *entry=cache_lookup(hash);
    if(!entry) entry=compile_shader(microcode,length,hash);
    if(!entry) return E_FAIL;
    HRESULT hr;
    if(!entry->native_layout) {
        D3D11_INPUT_ELEMENT_DESC elements[16]={0};
        unsigned count=0;
        for(unsigned i=0;i<16;i++) if(entry->inputs_read&(1u<<i)) {
            elements[count]=(D3D11_INPUT_ELEMENT_DESC){"ATTR",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,count*16,D3D11_INPUT_PER_VERTEX_DATA,0};
            ++count;
        }
        hr=ID3D11Device_CreateInputLayout(device,elements,count,ID3D10Blob_GetBufferPointer(entry->vs_blob),ID3D10Blob_GetBufferSize(entry->vs_blob),&entry->native_layout);
        if(FAILED(hr)) return hr;
    }
    ID3D11DeviceContext_VSSetShader(ctx,entry->vs,NULL,0);
    ID3D11DeviceContext_IASetInputLayout(ctx,entry->native_layout);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&g_vsh_cb);
    hr=bind_native_geometry(constants,geometry,kind);
    if(SUCCEEDED(hr)) *inputs_read=entry->inputs_read;
    return hr;
}

static const char nv2a_fixed_vertex_source[] =
"\n"
"// Fixed-function NV products use the original LT contract (xemu vsh-ff,\n"
"// espes, Jannik Vogel, Matt Borgerson, LGPL-2.0-or-later).\n"
"cbuffer Fixed:register(b3) {\n"
" float4 scene,emission;\n"
" float4 ambient[8],diffuse[8],specular[8];\n"
" float4 halfVector[8],direction[8],position[8],attenuation[8];\n"
" float4 material; uint4 fixedFlags,sources,lightModes[2];\n"
"};\n"
"struct I { float4 a0:ATTR0,a1:ATTR1,a2:ATTR2,a3:ATTR3,a4:ATTR4,a5:ATTR5,a6:ATTR6,a7:ATTR7,a8:ATTR8,a9:ATTR9,a10:ATTR10,a11:ATTR11,a12:ATTR12; };\n"
"float4 transform4(float4 p,int base) { return float4(dot(p,C[base]),dot(p,C[base+1]),dot(p,C[base+2]),dot(p,C[base+3])); }\n"
"float3 sourceColour(uint source,float3 constantValue,I v) { return source==0 ? constantValue : source==1 ? v.a3.rgb : v.a4.rgb; }\n"
"V main_fixed(I v) {\n"
" V o=(V)0; float4 eye=transform4(v.a0,8); float3 n=transform4(float4(v.a2.xyz,0),12).xyz;\n"
" if(fixedFlags.y!=0) n=normalize(n);\n"
" o.d0=v.a3;o.d1=v.a4;o.b0=v.a7;o.b1=v.a8;\n"
" if(fixedFlags.x!=0) {\n"
"  float alpha=sources.z==0 ? material.x : sources.z==1 ? v.a3.a : v.a4.a;\n"
"  o.d0=float4(sourceColour(sources.y,scene.rgb,v)*emission.rgb+sourceColour(sources.x,scene.rgb,v),alpha);\n"
"  o.d1=float4(0,0,0,v.a4.a);\n"
"  float3 toEye=fixedFlags.z!=0 ? normalize(C[56].xyz/C[56].w-eye.xyz/eye.w) : float3(0,0,0);\n"
"  [unroll] for(uint i=0;i<8;i++) {\n"
"   uint mode=lightModes[i/4][i%4]; if(mode==0) continue;\n"
"   float3 vp; float att=1;float ndh;\n"
"   if(mode==1) { vp=normalize(direction[i].xyz); ndh=max(0,dot(n,fixedFlags.z!=0 ? normalize(vp+toEye) : halfVector[i].xyz)); }\n"
"   else { vp=position[i].xyz-eye.xyz/eye.w;float d=length(vp);if(d>attenuation[i].w) continue;vp=normalize(vp);att=1/(attenuation[i].x+attenuation[i].y*d+attenuation[i].z*d*d);ndh=max(0,dot(n,normalize(vp+toEye))); }\n"
"   float ndv=max(0,dot(n,vp));float pf=(ndv==0 || ndh==0) ? 0 : pow(ndh,material.y);\n"
"   o.d0.rgb+=ambient[i].rgb*att+sourceColour(sources.z,float3(1,1,1),v)*diffuse[i].rgb*att*ndv;\n"
"   o.d1.rgb+=sourceColour(sources.w,float3(1,1,1),v)*specular[i].rgb*att*pf;\n"
"  }\n"
" }\n"
" if(pointSize.y==0) { o.d1=float4(0,0,0,1);o.b1=float4(0,0,0,1); }\n"
" else { if(fixedFlags.w==0) { if(fixedFlags.x!=0) { o.d0.rgb+=o.d1.rgb;o.b0.rgb+=o.b1.rgb; } o.d1=v.a4;o.b1=v.a8; } if(pointSize.z!=0) { o.d1.a=1;o.b1.a=1; } }\n"
" o.d0=saturate(o.d0);o.d1=saturate(o.d1);o.b0=saturate(o.b0);o.b1=saturate(o.b1);\n"
" float4 p=transform4(v.a0,0);\n"
" if(p.w>0 || asuint(p.w)==0) p.w=clamp(p.w,asfloat(0x1F800000),asfloat(0x5F800000));else p.w=clamp(p.w,asfloat(0xDF800000),asfloat(0x9F800000));\n"
" o.p=float4(p.xy/p.w+C[59].xy,p.z/p.w,p.w);\n"
" o.f=v.a5.x;if(pointFlags.w==0) o.f=saturate(v.a4.a);else if(pointFlags.w==1) o.f=length(eye.xyz);else if(pointFlags.w==2 || pointFlags.w==3) { o.f=dot(C[57].xyz,eye.xyz)+C[57].w;if(pointFlags.w==3) o.f=abs(o.f); }\n"
" o.sz=max(1,pointSize.x);if(pointFlags.x!=0) { float d=length(eye);float mn=min(pointParams[1].w,63.875),mx=min(pointParams[0].w+mn,63.875);o.sz=clamp((rsqrt(pointParams[0].x+pointParams[0].y*d+pointParams[0].z*d*d)+pointParams[1].z)*pointParams[0].w+pointParams[1].w,mn,mx); }\n"
" o.t0=v.a9;o.t1=v.a10;o.t2=v.a11;o.t3=v.a12;return o;\n"
"}\n"
;


HRESULT d3d8_vsh_bind_fixed(const float *constants,const NV2ANativeGeometry *geometry,const NV2ANativeFixed *fixed,D3D11_PRIMITIVE_TOPOLOGY topology)
{
    unsigned kind;
    if(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ) kind=0;
    else if(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ) kind=1;
    else if(topology==D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) kind=2;
    else return E_INVALIDARG;
    ID3D11Device *device=d3d8_GetD3D11Device();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();HRESULT hr;
    if(!g_vsh_fixed_shader) {
        size_t bytes=sizeof nv2a_geometry_source+sizeof nv2a_fixed_vertex_source;
        char *source=(char *)malloc(bytes);if(!source) return E_OUTOFMEMORY;
        strcpy(source,nv2a_geometry_source);strcat(source,nv2a_fixed_vertex_source);
        ID3DBlob *code=NULL,*errors=NULL;
        hr=D3DCompile(source,strlen(source),"nv2a_fixed_vertex",NULL,NULL,"main_fixed","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);free(source);
        if(FAILED(hr)) { fprintf(stderr,"NV2A fixed vertex: %s\n",errors?(char *)ID3D10Blob_GetBufferPointer(errors):"compile failed");if(errors) ID3D10Blob_Release(errors);return hr; }
        if(errors) ID3D10Blob_Release(errors);
        hr=ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g_vsh_fixed_shader);
        if(SUCCEEDED(hr)) {
            D3D11_INPUT_ELEMENT_DESC elements[13]={0};
            for(unsigned i=0;i<13;i++) elements[i]=(D3D11_INPUT_ELEMENT_DESC){"ATTR",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
            hr=ID3D11Device_CreateInputLayout(device,elements,13,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&g_vsh_fixed_layout);
        }
        ID3D10Blob_Release(code);if(FAILED(hr)) return hr;
    }
    if(!g_vsh_fixed_cb) {
        D3D11_BUFFER_DESC desc={0};desc.ByteWidth=sizeof *fixed;desc.Usage=D3D11_USAGE_DYNAMIC;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        hr=ID3D11Device_CreateBuffer(device,&desc,NULL,&g_vsh_fixed_cb);if(FAILED(hr)) return hr;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)g_vsh_fixed_cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);if(FAILED(hr)) return hr;
    memcpy(mapped.pData,fixed,sizeof *fixed);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)g_vsh_fixed_cb,0);
    hr=bind_native_geometry(constants,geometry,kind);if(FAILED(hr)) return hr;
    ID3D11DeviceContext_VSSetShader(ctx,g_vsh_fixed_shader,NULL,0);ID3D11DeviceContext_IASetInputLayout(ctx,g_vsh_fixed_layout);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&g_vsh_cb);ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&g_vsh_geometry_cb);ID3D11DeviceContext_VSSetConstantBuffers(ctx,3,1,&g_vsh_fixed_cb);
    return S_OK;
}
