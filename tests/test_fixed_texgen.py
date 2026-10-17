"""Execute the production fixed vertex shader and read its texture outputs."""
from pathlib import Path
import shutil
import subprocess


def test_linear_texgen_and_texture_matrices(tmp_path):
    compiler = shutil.which('cl')
    assert compiler, 'requires existing MSVC environment'
    root = Path(__file__).resolve().parents[1] / 'vendor/xboxrecomp/src'
    source = r'''
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "production_shader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <d3dcompiler.h>
static ID3DBlob *compile(const char *text,const char *entry,const char *target) {
    ID3DBlob *code=NULL,*error=NULL;
    HRESULT hr=D3DCompile(text,strlen(text),"texgen_test",NULL,NULL,entry,target,D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(FAILED(hr) && error)fprintf(stderr,"%s",(char *)ID3D10Blob_GetBufferPointer(error));
    assert(SUCCEEDED(hr));if(error)ID3D10Blob_Release(error);return code;
}
int main(void) {
    ID3D11Device *device;ID3D11DeviceContext *ctx;
    assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,NULL,&ctx)));
    char *text=malloc(sizeof nv2a_geometry_source+sizeof nv2a_fixed_vertex_source);
    strcpy(text,nv2a_geometry_source);strcat(text,nv2a_fixed_vertex_source);
    ID3DBlob *code=compile(text,"main_fixed","vs_5_0");free(text);
    ID3D11VertexShader *vs;assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&vs)));
    D3D11_INPUT_ELEMENT_DESC layout[13]={0};
    for(unsigned i=0;i<13;i++)layout[i]=(D3D11_INPUT_ELEMENT_DESC){"ATTR",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
    ID3D11InputLayout *il;assert(SUCCEEDED(ID3D11Device_CreateInputLayout(device,layout,13,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&il)));
    ID3D10Blob_Release(code);
    const char gs[]="struct V {float4 p:SV_POSITION,d0:COLOR0,d1:COLOR1,t0:TEXCOORD0,t1:TEXCOORD1,t2:TEXCOORD2,t3:TEXCOORD3;float f:FOG,sz:PSIZE;float4 b0:TEXCOORD4,b1:TEXCOORD5;};[maxvertexcount(1)]void main(point V v[1],inout PointStream<V> s){s.Append(v[0]);}";
    code=compile(gs,"main","gs_5_0");
    D3D11_SO_DECLARATION_ENTRY so[4]={0};for(unsigned i=0;i<4;i++)so[i]=(D3D11_SO_DECLARATION_ENTRY){0,"TEXCOORD",i,0,4,0};
    UINT stride=64;ID3D11GeometryShader *shader;
    assert(SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),so,4,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,NULL,&shader)));ID3D10Blob_Release(code);
    float constants[192][4]={0},input[13][4]={0};
    for(unsigned j=0;j<4;j++){constants[j][j]=1;constants[8+j][j]=(float)(j+2);constants[12+j][j]=1;}
    constants[8][3]=1;constants[9][3]=2;constants[10][3]=3;constants[11][3]=1;
    input[0][0]=2;input[0][1]=3;input[0][2]=4;input[0][3]=1;
    for(unsigned stage=0;stage<4;stage++) {
        const float tc[4]={7,8,9,1};memcpy(input[9+stage],tc,16);
        for(unsigned j=0;j<4;j++){constants[64+stage*8+j][0]=(float)(j+1);constants[64+stage*8+j][1]=(float)(stage+1);constants[68+stage*8+j][j]=1;}
        constants[68+stage*8][0]=2;constants[68+stage*8][3]=3;
    }
    NV2ANativeGeometry geometry={0};geometry.point_size[0]=1;
    NV2ANativeFixed fixed={0};
    ID3D11Buffer *cb[4]={0},*vb,*output,*readback;D3D11_BUFFER_DESC d={0};D3D11_SUBRESOURCE_DATA initial={0};
    d.ByteWidth=sizeof constants;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;initial.pSysMem=constants;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,&initial,&cb[1])));
    d.ByteWidth=sizeof geometry;initial.pSysMem=&geometry;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,&initial,&cb[2])));
    d.ByteWidth=sizeof fixed;initial.pSysMem=&fixed;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,&initial,&cb[3])));
    d.ByteWidth=sizeof input;d.BindFlags=D3D11_BIND_VERTEX_BUFFER;initial.pSysMem=input;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,&initial,&vb)));
    d.ByteWidth=64;d.BindFlags=D3D11_BIND_STREAM_OUTPUT;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,NULL,&output)));
    d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&d,NULL,&readback)));
    UINT offset=0;stride=sizeof input;ID3D11DeviceContext_IASetInputLayout(ctx,il);ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&vb,&stride,&offset);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,3,cb+1);ID3D11DeviceContext_GSSetShader(ctx,shader,NULL,0);
    for(unsigned mode=0;mode<=2;mode++)for(unsigned matrix=0;matrix<2;matrix++) {
        for(unsigned stage=0;stage<4;stage++){fixed.texture_matrix[stage]=matrix;for(unsigned j=0;j<4;j++)fixed.texgen[stage][j]=j==stage?0:mode;}
        ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource *)cb[3],0,NULL,&fixed,0,0);
        ID3D11DeviceContext_SOSetTargets(ctx,1,&output,&offset);ID3D11DeviceContext_Draw(ctx,1,0);ID3D11Buffer *empty=NULL;ID3D11DeviceContext_SOSetTargets(ctx,1,&empty,&offset);
        ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)readback,(ID3D11Resource *)output);D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)readback,0,D3D11_MAP_READ,0,&mapped)));
        const float *actual=mapped.pData;
        for(unsigned stage=0;stage<4;stage++) {
            float want[4];for(unsigned j=0;j<4;j++)want[j]=!mode || j==stage?input[9+stage][j]:mode==1?5*(j+1)+11*(stage+1):2*(j+1)+3*(stage+1);
            if(matrix)want[0]=2*want[0]+3*want[3];
            for(unsigned j=0;j<4;j++){if(actual[stage*4+j]!=want[j])fprintf(stderr,"mode%u matrix%u stage%u component%u actual%g expected%g\n",mode,matrix,stage,j,actual[stage*4+j],want[j]);assert(actual[stage*4+j]==want[j]);}
        }
        ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)readback,0);
    }
    return 0;
}
'''
    geometry = (root / 'd3d/d3d8_nv2a_geometry.inc').read_text(encoding='utf-8')
    fragments = []
    for start, end in [('static const char nv2a_geometry_source[]', 'static HRESULT bind_native_geometry'),
                       ('static const char nv2a_fixed_vertex_source[]', 'HRESULT d3d8_vsh_bind_fixed')]:
        fragments.append(geometry[geometry.index(start):geometry.index(end)])
    (tmp_path / 'production_shader.h').write_text('\n'.join(fragments), encoding='utf-8')
    path = tmp_path / 'texgen.c'
    path.write_text(source, encoding='utf-8')
    executable = tmp_path / 'texgen.exe'
    result = subprocess.run([compiler, '/nologo', '/O2', '/Gy', '/MD', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
        '/D_CRT_SECURE_NO_WARNINGS', '/I'+str(root), '/I'+str(root/'d3d'), str(path), '/Fe:'+str(executable),
        '/link', '/OPT:REF', 'd3d11.lib', 'd3dcompiler.lib', 'dxguid.lib'], cwd=tmp_path, capture_output=True, text=True, errors='replace')
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True, errors='replace', timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
