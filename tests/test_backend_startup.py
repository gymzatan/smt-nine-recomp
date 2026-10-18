"""Execute the production selection loop with failed driver/API boundaries."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def backend_probe(tmp_path_factory):
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    work = tmp_path_factory.mktemp('backend-startup')
    source = (ROOT/'vendor/xboxrecomp/src/d3d/d3d8_backend.c').read_text()
    body = source[source.index('HRESULT d3d8_backend_create_device('):source.index('static D3D8TextureAssetLookup')]
    (work/'probe.c').write_text(r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef void *CreateFactory;
typedef void *CreateDevice;
static const char *active_backend="D3D11";
static unsigned attempts,loads,releases;
static HRESULT results[3],load_result=S_OK;
static HRESULT load_backend(const wchar_t *folder,HMODULE *dxgi,HMODULE *d3d) {
    ++loads;*dxgi=(HMODULE)1;if(FAILED(load_result))return load_result;
    *d3d=(HMODULE)2;return S_OK;
}
static HRESULT create_hardware(CreateFactory factory,CreateDevice create,ID3D11Device **device,ID3D11DeviceContext **context) {
    unsigned i=!strcmp(active_backend,"Vulkan")?0:!strcmp(active_backend,"OpenGL")?1:2;
    ++attempts;*device=(ID3D11Device *)1;*context=(ID3D11DeviceContext *)2;return results[i];
}
#undef ID3D11DeviceContext_Release
#undef ID3D11Device_Release
#define ID3D11DeviceContext_Release(p) (++releases)
#define ID3D11Device_Release(p) (++releases)
#define GetProcAddress(h,n) ((FARPROC)1)
''' + body + r'''
int main(int argc,char **argv) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(argc==2);ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    _putenv_s("RECOMP_GPU_BACKEND",!strcmp(argv[1],"explicit")?"Vulkan":"Auto");
    results[0]=E_FAIL;results[1]=E_INVALIDARG;results[2]=S_OK;
    if(!strcmp(argv[1],"load"))load_result=HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if(!strcmp(argv[1],"all"))results[2]=DXGI_ERROR_UNSUPPORTED;
    HRESULT hr=d3d8_backend_create_device(&device,&context);
    if(!strcmp(argv[1],"explicit")){assert(hr==E_FAIL && attempts==1 && releases==2 && !device && !context);}
    else if(!strcmp(argv[1],"load")){assert(SUCCEEDED(hr) && attempts==1 && loads==2 && !strcmp(active_backend,"D3D11"));}
    else if(!strcmp(argv[1],"all")){assert(FAILED(hr) && attempts==3 && releases==6 && !device && !context);}
    else {assert(SUCCEEDED(hr) && attempts==3 && releases==4 && !strcmp(active_backend,"D3D11"));}
    return 0;
}
''',encoding='utf-8')
    result=subprocess.run([compiler,'/nologo','/O2','/MD','/utf-8',str(work/'probe.c'),'/Fe:'+str(work/'probe.exe'),'/link','d3d11.lib','dxgi.lib'],cwd=work,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return work/'probe.exe'


@pytest.mark.parametrize('mode',['auto','explicit','load','all'])
def test_startup_failure_selection_and_cleanup(backend_probe,mode):
    result=subprocess.run([str(backend_probe),mode],capture_output=True,text=True,timeout=10)
    assert result.returncode==0,result.stdout+result.stderr
