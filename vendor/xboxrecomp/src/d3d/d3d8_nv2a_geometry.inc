/* Native screen-coordinate setup consumes the original translated VS outputs. */
static const char nv2a_geometry_source[] =
"cbuffer VSC:register(b1) { float4 C[192]; };\n"
"cbuffer Geometry:register(b2) { float4 screen,bounds,ts[4],fog; uint4 flags; float4 pointParams[2],pointSize; uint4 pointFlags,polygon; float4 raster; };\n"
"struct V { float4 p:SV_POSITION; float4 d0:COLOR0,d1:COLOR1,t0:TEXCOORD0,t1:TEXCOORD1,t2:TEXCOORD2,t3:TEXCOORD3; float f:FOG,sz:PSIZE; float4 b0:TEXCOORD4,b1:TEXCOORD5; };\n"
"struct O { float4 p:SV_POSITION; noperspective float4 d0:COLOR0,d1:COLOR1; float4 t0:TEXCOORD0,t1:TEXCOORD1,t2:TEXCOORD2,t3:TEXCOORD3; noperspective float f:TEXCOORD4; noperspective float2 z:TEXCOORD5; };\n"
"V toclip(V a) { [unroll] for(int j=0;j<3;j++) a.p[j]=C[58][j]!=0 ? (a.p[j]-C[59][j])/C[58][j]*a.p.w : 0; return a; }\n"
"V mixv(V a,V b,float t) { V o; o.p=a.p+(b.p-a.p)*t; o.d0=a.d0+(b.d0-a.d0)*t; o.d1=a.d1+(b.d1-a.d1)*t; o.t0=a.t0+(b.t0-a.t0)*t; o.t1=a.t1+(b.t1-a.t1)*t; o.t2=a.t2+(b.t2-a.t2)*t; o.t3=a.t3+(b.t3-a.t3)*t; o.f=a.f+(b.f-a.f)*t; o.sz=a.sz+(b.sz-a.sz)*t;o.b0=a.b0+(b.b0-a.b0)*t;o.b1=a.b1+(b.b1-a.b1)*t;return o; }\n"
"float ff(float d) { if(fog.w==0) return 1; float f; uint mode=(uint)fog.z; if(mode==0x800 || mode==0x802) f=fog.x+exp2(d*fog.y*16)-1.5; else if(mode==0x801 || mode==0x803) f=fog.x+exp2(-d*d*fog.y*fog.y*32)-1.5; else f=fog.x+d*fog.y-1; if(mode==0x802 || mode==0x803 || mode==0x804) f=abs(f); if(isnan(f)) f=1; return saturate(f); }\n"
"O convert(V v) { O o=(O)0; o.p=float4((v.p.x/screen.x*2-1)*v.p.w,(1-v.p.y/screen.y*2)*v.p.w,0,v.p.w); o.d0=v.d0;o.d1=v.d1;o.f=ff(v.f);o.z=float2(v.p.z,0);o.t0=v.t0*ts[0];o.t1=v.t1*ts[1];o.t2=v.t2*ts[2];o.t3=v.t3*ts[3];return o; }\n"
"\n"
"V from_output(O o) { V v=(V)0;v.p=o.p;v.d0=o.d0;v.d1=o.d1;v.t0=o.t0;v.t1=o.t1;v.t2=o.t2;v.t3=o.t3;v.f=o.f;v.sz=pointSize.x;return v; }\n"
"void output_vertices(O input[6],inout V v[6]) { [unroll] for(int i=0;i<6;i++)v[i]=from_output(input[i]); }\n"
"uint clip_polygon_xy(inout V p[9],inout uint edges[9],inout uint points[9],uint n,uint axis,float boundary,bool lower) {\n"
"    V output[9]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};uint oe[9]={0,0,0,0,0,0,0,0,0},op[9]={0,0,0,0,0,0,0,0,0};uint count=0;\n"
"    [loop] for(uint i=0;i<n;i++) {\n"
"        V a=p[i],b=p[(i+1)%n];float da=lower?a.p[axis]-boundary:boundary-a.p[axis],db=lower?b.p[axis]-boundary:boundary-b.p[axis];bool ain=da>=0,bin=db>=0;\n"
"        if(ain){output[count]=a;oe[count]=edges[i];op[count]=points[i];count++;}\n"
"        if(ain && !bin && da==0)oe[count-1]=1;\n"
"        if(ain!=bin && da!=0 && db!=0) {\n"
"            float t=da/(da-db);precise float clip_t=t*a.p.w/((1-t)*b.p.w+t*a.p.w);\n"
"            a.p.xyz*=a.p.w;b.p.xyz*=b.p.w;V v=mixv(a,b,clip_t);v.p.xyz/=v.p.w;v.p[axis]=boundary;\n"
"            output[count]=v;oe[count]=ain?1:edges[i];op[count]=edges[i];count++;\n"
"        }\n"
"    }\n"
"    [unroll] for(uint j=0;j<9;j++){p[j]=output[j];edges[j]=oe[j];points[j]=op[j];}return count;\n"
"}\n"
"uint prepare_polygon(V input[6],inout V poly[9],inout uint edges[9],inout uint points[9]) {\n"
"    V a[4]={(V)0,(V)0,(V)0,(V)0};a[0]=input[0];a[1]=input[2];a[2]=input[4];a[3]=input[3];\n"
"    uint count=(polygon.w&255)==4?4:3,edgeMask=(polygon.w>>8)&15,pointMask=(polygon.w>>16)&15;\n"
"    [unroll] for(uint i=0;i<9;i++){poly[i]=(V)0;edges[i]=points[i]=0;}\n"
"    if(flags.x!=0) [unroll] for(uint k=0;k<4;k++){a[k].d0=input[1].d0;a[k].d1=input[1].d1;a[k].b0=input[1].b0;a[k].b1=input[1].b1;}\n"
"    bool clipped=false,inside=true;[unroll] for(uint i=0;i<4;i++)if(i<count){if(a[i].p.w<=0.001)clipped=true;if(!(a[i].p.w>0.001))inside=false;}\n"
"    if(inside && polygon.x==0 && polygon.y==0){poly[0]=a[0];poly[1]=a[1];poly[2]=a[2];poly[3]=a[3];return count;}\n"
"    uint n=0;[loop] for(uint i=0;i<count;i++) {\n"
"        V p=a[i],q=a[(i+1)%count];uint edge=(edgeMask>>i)&1;bool pin=p.p.w>0.001,qin=q.p.w>0.001;\n"
"        if(pin){poly[n]=p;if(clipped)poly[n]=toclip(p);edges[n]=edge;points[n]=(pointMask>>i)&1;n++;}\n"
"        if(pin!=qin){precise float t=(0.001-p.p.w)/(q.p.w-p.p.w);poly[n]=mixv(toclip(p),toclip(q),t);edges[n]=pin?1:edge;points[n]=edge;n++;}\n"
"    }\n"
"    if(clipped) [loop] for(uint i=0;i<n;i++)poly[i].p.xyz=poly[i].p.xyz/poly[i].p.w*C[58].xyz+C[59].xyz;\n"
"    if(polygon.x!=0 || polygon.y!=0){n=clip_polygon_xy(poly,edges,points,n,0,0,true);n=clip_polygon_xy(poly,edges,points,n,0,screen.x,false);n=clip_polygon_xy(poly,edges,points,n,1,0,true);n=clip_polygon_xy(poly,edges,points,n,1,screen.y,false);}\n"
"    return n;\n"
"}\n"
"bool visible_polygon(inout V p[9],uint n,uint mode) {\n"
"    if(n<3)return false;[loop] for(uint j=0;j<n;j++)p[j].p.xy=trunc(p[j].p.xy*16)/16;precise float area=0;\n"
"    [loop] for(uint i=1;i+1<n;i++){area=(p[i].p.x-p[0].p.x)*(p[i+1].p.y-p[0].p.y)-(p[i].p.y-p[0].p.y)*(p[i+1].p.x-p[0].p.x);if(area!=0)break;}\n"
"    if(area==0 || isnan(area))return false;bool front=(area<0)==(flags.z!=0);\n"
"    if((front?polygon.x:polygon.y)!=mode)return false;\n"
"    if(flags.y!=0 && (flags.w==3 || (front && flags.w==1) || (!front && flags.w==2)))return false;\n"
"    [loop] for(uint i=0;i<n;i++){if(pointFlags.z!=0 && !front){p[i].d0=p[i].b0;p[i].d1=p[i].b1;}}\n"
"    return true;\n"
"}\n"
"float polygon_depth_slope(V p[9],uint n) {\n"
"    [loop] for(uint j=1;j+1<n;j++){float2 b=p[j].p.xy-p[0].p.xy,c=p[j+1].p.xy-p[0].p.xy;precise float area=b.x*c.y-b.y*c.x;if(area!=0){float db=p[j].p.z-p[0].p.z,dc=p[j+1].p.z-p[0].p.z;return max(abs((db*c.y-dc*b.y)/area),abs((dc*b.x-db*c.x)/area));}}return 0;\n"
"}\n"
"void emit_polygon_fill(V input[6],inout TriangleStream<O> stream) {\n"
"    V p[9]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};uint e[9]={0,0,0,0,0,0,0,0,0},v[9]={0,0,0,0,0,0,0,0,0};uint n=prepare_polygon(input,p,e,v);if(!visible_polygon(p,n,0))return;\n"
"    [loop] for(uint i=1;i+1<n;i++){stream.Append(convert(p[0]));stream.Append(convert(p[i]));stream.Append(convert(p[i+1]));stream.RestartStrip();}\n"
"}\n"
"[maxvertexcount(21)] void main(triangleadj V input[6],inout TriangleStream<O> stream){emit_polygon_fill(input,stream);}\n"
"[maxvertexcount(21)] void main_output(triangleadj O input[6],inout TriangleStream<O> stream){V v[6]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};output_vertices(input,v);emit_polygon_fill(v,stream);}\n"
"void emitpoint(O a,float size,inout TriangleStream<O> stream) { if(a.p.w<=0 || any(isnan(a.p)) || any(isinf(a.p)) || !isfinite(size) || any(abs(a.p.xy)>a.p.w)) return; size=max(1,floor(size+0.5)); float2 center=float2((a.p.x/a.p.w+1)*0.5*screen.x,(1-a.p.y/a.p.w)*0.5*screen.y); center=((uint)size&1)!=0 ? floor(center)+0.5 : floor(center+0.5); [unroll] for(int k=0;k<4;k++) { float2 uv=float2(k&1,k>>1); float2 pos=center+(uv-0.5)*size; O o=a; o.p.xy=float2(pos.x/screen.x*2-1,1-pos.y/screen.y*2)*a.p.w; if(pointFlags.y!=0) o.t3=float4(uv,1,1); stream.Append(o); } stream.RestartStrip(); }\n"
"void emit_polygon_points(V input[6],inout TriangleStream<O> stream) {\n"
"    V p[9]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};uint e[9]={0,0,0,0,0,0,0,0,0},v[9]={0,0,0,0,0,0,0,0,0};uint n=prepare_polygon(input,p,e,v);if(!visible_polygon(p,n,1))return;\n"
"    float slope=(polygon.z&1)!=0?polygon_depth_slope(p,n):0;\n"
"    [loop] for(uint i=0;i<n;i++)if(v[i]!=0 && (raster.y<0 || i==(uint)raster.y)){O a=convert(p[i]);a.z.y=slope;emitpoint(a,pointFlags.x!=0?p[i].sz:pointSize.x,stream);}\n"
"}\n"
"[maxvertexcount(4)] void main_polygon_point(triangleadj V input[6],inout TriangleStream<O> stream){emit_polygon_points(input,stream);}\n"
"[maxvertexcount(4)] void main_output_polygon_point(triangleadj O input[6],inout TriangleStream<O> stream){V v[6]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};output_vertices(input,v);emit_polygon_points(v,stream);}\n"
"uint line_width() {return (uint)max(1,floor(raster.x+0.5))*(uint)raster.w;}\n"
"void emit_wide_line(O a,O b,uint instance,uint width,inout LineStream<O> stream) {\n"
"    float2 delta=(b.p.xy/b.p.w-a.p.xy/a.p.w)*screen.xy;\n"
"    uint copy=(uint)raster.z+instance;float offset=(float(copy)-float(width-1)*0.5)/raster.w;float2 shift=abs(delta.x)>=abs(delta.y)?float2(0,-2*offset/screen.y):float2(2*offset/screen.x,0);O x=a,y=b;x.p.xy+=shift*x.p.w;y.p.xy+=shift*y.p.w;stream.Append(x);stream.Append(y);stream.RestartStrip();\n"
"}\n"
"void emit_polygon_lines(V input[6],uint instance,inout LineStream<O> stream) {\n"
"    uint width=line_width();bool edge_instance=raster.y<0 && raster.z==0 && width>=2 && width<=16;if(edge_instance ? instance>=9 : (uint)raster.z+instance>=width)return;\n"
"    V p[9]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};uint e[9]={0,0,0,0,0,0,0,0,0},v[9]={0,0,0,0,0,0,0,0,0};uint n=prepare_polygon(input,p,e,v);if(!visible_polygon(p,n,2))return;\n"
"    float slope=(polygon.z&2)!=0?polygon_depth_slope(p,n):0;\n"
"    [loop] for(uint i=0;i<n;i++)if(e[i]!=0 && (edge_instance ? i==instance : (raster.y<0 || i==(uint)raster.y))){O a=convert(p[i]),b=convert(p[(i+1)%n]);a.z.y=b.z.y=slope;if(edge_instance){[loop]for(uint copy=0;copy<width;copy++)emit_wide_line(a,b,copy,width,stream);}else emit_wide_line(a,b,instance,width,stream);}\n"
"}\n"
"[instance(32)][maxvertexcount(32)] void main_polygon_line(triangleadj V input[6],uint id:SV_GSInstanceID,inout LineStream<O> stream){emit_polygon_lines(input,id,stream);}\n"
"[instance(32)][maxvertexcount(32)] void main_output_polygon_line(triangleadj O input[6],uint id:SV_GSInstanceID,inout LineStream<O> stream){V v[6]={(V)0,(V)0,(V)0,(V)0,(V)0,(V)0};output_vertices(input,v);emit_polygon_lines(v,id,stream);}\n"
"void emit_line_input(V input[4],uint instance,inout LineStream<O> stream) {\n"
"    uint width=line_width();if((uint)raster.z+instance>=width)return;\n"
"    V a=input[1],b=input[2];if(flags.x!=0){a.d0=input[0].d0;b.d0=input[0].d0;a.d1=input[0].d1;b.d1=input[0].d1;}\n"
"    if(a.p.w<=0.001 && b.p.w<=0.001)return;\n"
"    if(a.p.w<=0.001 || b.p.w<=0.001){precise float t=(0.001-a.p.w)/(b.p.w-a.p.w);V clipped=mixv(toclip(a),toclip(b),t);clipped.p.xyz=clipped.p.xyz/clipped.p.w*C[58].xyz+C[59].xyz;if(a.p.w<=0.001)a=clipped;else b=clipped;}\n"
"    a.p.xy=trunc(a.p.xy*16)/16;b.p.xy=trunc(b.p.xy*16)/16;\n"
"    if(a.p.w<=0 || b.p.w<=0 || any(isnan(a.p)) || any(isnan(b.p)))return;\n"
"    emit_wide_line(convert(a),convert(b),instance,width,stream);\n"
"}\n"
"[instance(32)][maxvertexcount(2)] void main_line(lineadj V input[4],uint id:SV_GSInstanceID,inout LineStream<O> stream){emit_line_input(input,id,stream);}\n"
"[instance(32)][maxvertexcount(2)] void main_output_line(lineadj O input[4],uint id:SV_GSInstanceID,inout LineStream<O> stream){V v[4];[unroll]for(uint i=0;i<4;i++)v[i]=from_output(input[i]);emit_line_input(v,id,stream);}\n"
"[maxvertexcount(4)] void main_point(point V input[1],inout TriangleStream<O> stream){V a=input[0];a.p.xy=trunc(a.p.xy*16)/16;emitpoint(convert(a),pointFlags.x!=0?a.sz:pointSize.x,stream);}\n"
;

static HRESULT bind_native_geometry(const float *constants,const NV2ANativeGeometry *geometry,unsigned kind)
{
    static const char *entry_points[]={"main","main_line","main_point","main_polygon_line","main_polygon_point","main_output","main_output_line","main_output_polygon_line","main_output_polygon_point"};
    if(kind>=9)return E_INVALIDARG;
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
    ID3D11Buffer *geometry_buffers[]={g_vsh_cb,g_vsh_geometry_cb};
    ID3D11DeviceContext_GSSetConstantBuffers(ctx,1,2,geometry_buffers);
    return S_OK;
}

HRESULT d3d8_vsh_bind_geometry(const float *constants,const NV2ANativeGeometry *geometry,unsigned kind)
{
    return bind_native_geometry(constants,geometry,kind);
}

static VshCacheEntry *native_program(const DWORD *microcode,int length)
{
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
    return entry;
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
    VshCacheEntry *entry=native_program(microcode,length);
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
" float4 halfVector[8],direction[8],position[8],attenuation[8],spotFalloff[8],spotDirection[8];\n"
" float4 material; uint4 fixedFlags,sources,lightModes[2],texgen[4],textureMatrix,skinning;\n"
" float4 backScene,backEmission,backMaterial,backAmbient[8],backDiffuse[8],backSpecular[8]; uint4 backSources;float4 specularSecond,backSpecularSecond;\n"
"};\n"
"struct I { float4 a0:ATTR0,a1:ATTR1,a2:ATTR2,a3:ATTR3,a4:ATTR4,a5:ATTR5,a6:ATTR6,a7:ATTR7,a8:ATTR8,a9:ATTR9,a10:ATTR10,a11:ATTR11,a12:ATTR12; };\n"
"float4 transform4(float4 p,int base) { return float4(dot(p,C[base]),dot(p,C[base+1]),dot(p,C[base+2]),dot(p,C[base+3])); }\n"
"float4 fixedTexcoord(I v,float4 eye,float3 normal,uint stage,float4 t) { [unroll] for(uint j=0;j<4;j++) { if(texgen[stage][j]==1) t[j]=dot(C[64+stage*8+j],eye); else if(texgen[stage][j]==2) t[j]=dot(C[64+stage*8+j],v.a0); else if(texgen[stage][j]==4 && j<3) t[j]=normal[j]; else if((texgen[stage][j]==5 && j<3) || (texgen[stage][j]==3 && j<2)) { float3 u=skinning.y!=0 ? float3(0,0,1) : normalize(eye.xyz);float3 r=reflect(u,normal);t[j]=texgen[stage][j]==3 ? r[j]/(2*length(r+float3(0,0,1)))+.5 : r[j]; } } if(textureMatrix[stage]!=0) t=transform4(t,68+stage*8);return t; }\n"
"float3 sourceColour(uint source,float3 constantValue,I v) { return source==0 ? constantValue : source==1 ? v.a3.rgb : v.a4.rgb; }\n"
"float rationalFalloff(float x,float3 k,float ceiling) { if(x<=-k.x) return 0;float denominator=k.y*x+k.z;if(denominator==0) return ceiling;if(denominator<0) return 0;return clamp((x+k.x)/denominator,0,ceiling); }\n"
"float rationalHalfVector(float s,float hd,float3 k) { float ss=s*s;float numerator=ss+hd*k.x,denominator=ss*k.y+hd*k.z;if(s<0 || numerator<=0) return 0;if(denominator==0) return 10000;if(denominator<0) return 0;return clamp(numerator/denominator,0,10000); }\n"
"V main_fixed(I v) {\n"
" V o=(V)0; float4 eye=transform4(v.a0,8); float3 n=transform4(float4(v.a2.xyz,0),12).xyz;\n"
" if(skinning.x!=0) { eye=0;n=0;uint count=(skinning.x+3)/2;float remaining=1;[unroll] for(uint bone=0;bone<4;bone++) { if(bone>=count) break;float weight=(skinning.x&1)!=0 && bone==count-1 ? remaining : v.a1[bone];remaining-=weight;eye+=transform4(v.a0,8+bone*8)*weight;n+=transform4(float4(v.a2.xyz,0),12+bone*8).xyz*weight; } }\n"
" if(fixedFlags.y!=0) n=normalize(n);\n"
" o.d0=v.a3;o.d1=v.a4;o.b0=v.a7;o.b1=v.a8;\n"
" if(fixedFlags.x!=0) {\n"
"  float3 toEye=fixedFlags.z!=0 ? normalize(C[56].xyz/C[56].w-eye.xyz/eye.w) : float3(0,0,0);\n"
"  [unroll] for(uint face=0;face<2;face++) {\n"
"   if(face!=0 && pointFlags.z==0) break;\n"
"   uint4 src=face==0 ? sources : backSources;\n"
"   float4 mat=face==0 ? material : backMaterial;\n"
"   float3 sceneColor=face==0 ? scene.rgb : backScene.rgb;\n"
"   float3 emissionColor=face==0 ? emission.rgb : backEmission.rgb;\n"
"   float3 normal=face==0 ? n : -n;\n"
"   float alpha=src.z==0 ? mat.x : src.z==1 ? v.a3.a : v.a4.a;\n"
"   float3 sceneProduct;\n"
"   if(src.x==0 || src.y==0) { sceneProduct=sceneColor;if(src.x!=0)sceneProduct+=sourceColour(src.x,float3(0,0,0),v)*emissionColor;else if(src.y!=0)sceneProduct+=sourceColour(src.y,float3(0,0,0),v)*emissionColor; }\n"
"   else sceneProduct=sourceColour(src.x,float3(0,0,0),v)+sourceColour(src.y,float3(0,0,0),v)*emissionColor;\n"
"   float4 litDiffuse=float4(sceneProduct,alpha);\n"
"   float4 litSpecular=float4(0,0,0,v.a4.a);\n"
"   [unroll] for(uint i=0;i<8;i++) {\n"
"    uint mode=lightModes[i/4][i%4]; if(mode==0) continue;\n"
"    float3 vp; float att=1;float3 hv;\n"
"    if(mode==1) { vp=normalize(direction[i].xyz); hv=fixedFlags.z!=0 ? vp+toEye : halfVector[i].xyz; }\n"
"    else { vp=position[i].xyz-eye.xyz/eye.w;float d=length(vp);if(d>attenuation[i].w) continue;vp=normalize(vp);att=1/(attenuation[i].x+attenuation[i].y*d+attenuation[i].z*d*d);hv=vp+toEye; }\n"
"    if(mode==3) { float spot=dot(spotDirection[i].xyz,vp)+spotDirection[i].w;att*=spot<=0 ? 0 : spot>=1 ? 1 : rationalFalloff(spot,spotFalloff[i].xyz,1); }\n"
"    float ndv=max(0,dot(normal,vp));float pf=0;if(ndv>0) { if(mode==1 && fixedFlags.z==0)pf=rationalFalloff(max(0,dot(normal,hv)),mat.yzw,10000);else pf=rationalHalfVector(dot(normal,hv),dot(hv,hv),face==0?specularSecond.xyz:backSpecularSecond.xyz); }\n"
"    float3 amb=face==0 ? ambient[i].rgb : backAmbient[i].rgb;\n"
"    float3 dif=face==0 ? diffuse[i].rgb : backDiffuse[i].rgb;\n"
"    float3 spec=face==0 ? specular[i].rgb : backSpecular[i].rgb;\n"
"    litDiffuse.rgb+=sourceColour(src.y,float3(1,1,1),v)*amb*att+sourceColour(src.z,float3(1,1,1),v)*dif*att*ndv;\n"
"    litSpecular.rgb+=sourceColour(src.w,float3(1,1,1),v)*spec*att*pf;\n"
"   }\n"
"   if(face==0) { o.d0=litDiffuse;o.d1=litSpecular; } else { o.b0=litDiffuse;o.b1=litSpecular; }\n"
"  }\n"
" }\n"
" if(pointSize.y==0) { o.d1=float4(0,0,0,1);o.b1=float4(0,0,0,1); }\n"
" else { if(fixedFlags.w==0) { if(fixedFlags.x!=0) { o.d0.rgb+=o.d1.rgb;o.b0.rgb+=o.b1.rgb; } o.d1=v.a4;o.b1=fixedFlags.x!=0 ? v.a4 : v.a8; } if(pointSize.z!=0) { o.d1.a=1;o.b1.a=1; } }\n"
" if(fixedFlags.x==0 && pointFlags.z!=0) { o.b0=float4(0,0,0,1);o.b1=float4(0,0,0,1); }\n"
" o.d0=saturate(o.d0);o.d1=saturate(o.d1);o.b0=saturate(o.b0);o.b1=saturate(o.b1);\n"
" float4 p=skinning.x==0 ? transform4(v.a0,0) : transform4(eye,0);\n"
" if(p.w>0 || asuint(p.w)==0) p.w=clamp(p.w,asfloat(0x1F800000),asfloat(0x5F800000));else p.w=clamp(p.w,asfloat(0xDF800000),asfloat(0x9F800000));\n"
" o.p=float4(p.xy/p.w+C[59].xy,p.z/p.w,p.w);\n"
" o.f=v.a5.x;if(pointFlags.w==0) o.f=saturate(v.a4.a);else if(pointFlags.w==1) o.f=length(eye.xyz);else if(pointFlags.w==2 || pointFlags.w==3) { o.f=dot(C[57].xyz,eye.xyz)+C[57].w;if(pointFlags.w==3) o.f=abs(o.f); }\n"
" o.sz=max(1,pointSize.x);if(pointFlags.x!=0) { float d=length(eye);float mn=min(pointParams[1].w,63.875),mx=min(pointParams[0].w+mn,63.875);o.sz=clamp((rsqrt(pointParams[0].x+pointParams[0].y*d+pointParams[0].z*d*d)+pointParams[1].z)*pointParams[0].w+pointParams[1].w,mn,mx); }\n"
" o.t0=fixedTexcoord(v,eye,n,0,v.a9);o.t1=fixedTexcoord(v,eye,n,1,v.a10);o.t2=fixedTexcoord(v,eye,n,2,v.a11);o.t3=fixedTexcoord(v,eye,n,3,v.a12);return o;\n"
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
        char *source=d3d8_vsh_fixed_source();if(!source) return E_OUTOFMEMORY;
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
    ID3D11Buffer *fixed_buffers[]={g_vsh_cb,g_vsh_geometry_cb,g_vsh_fixed_cb};
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,3,fixed_buffers);
    return S_OK;
}
