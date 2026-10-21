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
    body = source[source.index('static void release_backend_timer('):source.index('static D3D8TextureAssetLookup')]
    (work/'probe.c').write_text(r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <mmsystem.h>
#include <wchar.h>
static unsigned timer_begins,timer_ends;
static int timer_failure,exit_failure;
static void (*timer_cleanup)(void);
static MMRESULT test_begin(UINT period){assert(period==1);++timer_begins;return timer_failure?TIMERR_NOCANDO:TIMERR_NOERROR;}
static MMRESULT test_end(UINT period){assert(period==1);++timer_ends;return TIMERR_NOERROR;}
static int test_exit(void (*callback)(void)){if(exit_failure)return 1;timer_cleanup=callback;return 0;}
#define timeBeginPeriod test_begin
#define timeEndPeriod test_end
#define atexit test_exit
typedef void *CreateFactory;
typedef void *CreateDevice;
static const char *active_backend="D3D11";
static unsigned attempts,loads,releases,native_shutdowns;
static int native_vulkan;
static HRESULT results[4],load_result=S_OK;
static int nvk_initialize(const wchar_t *compiler) {
    assert(wcsstr(compiler,L"backends\\vulkan\\dxcompiler.dll"));
    ++attempts;return SUCCEEDED(results[0]);
}
static const char *nvk_error(void){return "native device unavailable";}
static void nvk_shutdown(void){++native_shutdowns;}
static HRESULT load_backend(const wchar_t *folder,HMODULE *dxgi,HMODULE *d3d) {
    ++loads;*dxgi=(HMODULE)1;if(FAILED(load_result))return load_result;
    *d3d=(HMODULE)2;return S_OK;
}
static HRESULT create_hardware(CreateFactory factory,CreateDevice create,ID3D11Device **device,ID3D11DeviceContext **context) {
    unsigned i=!strcmp(active_backend,"Vulkan")?0:!strcmp(active_backend,"OpenGL")?1:!strcmp(active_backend,"D3D12")?2:3;
    ++attempts;*device=(ID3D11Device *)1;*context=(ID3D11DeviceContext *)2;return results[i];
}
#undef ID3D11DeviceContext_Release
#undef ID3D11Device_Release
#define ID3D11DeviceContext_Release(p) (++releases)
#define ID3D11Device_Release(p) (++releases)
#define GetProcAddress(h,n) ((FARPROC)1)
#define create_d3d12 ((CreateDevice)2)
''' + body + r'''
int main(int argc,char **argv) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    assert(argc==2);ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    _putenv_s("RECOMP_GPU_BACKEND",!strcmp(argv[1],"explicit")?"Vulkan":!strcmp(argv[1],"d3d12")?"D3D12":"Auto");
    results[0]=E_FAIL;results[1]=E_INVALIDARG;results[2]=DXGI_ERROR_UNSUPPORTED;results[3]=S_OK;
    if(!strcmp(argv[1],"load"))load_result=HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if(!strcmp(argv[1],"all"))results[3]=DXGI_ERROR_UNSUPPORTED;
    if(!strcmp(argv[1],"d3d12") || !strcmp(argv[1],"auto-d3d12"))results[2]=S_OK;
    if(!strncmp(argv[1],"gl",2))results[1]=S_OK;
    timer_failure=!strcmp(argv[1],"timer-fail");exit_failure=!strcmp(argv[1],"exit-fail");
    if(!strcmp(argv[1],"vulkan") || !strcmp(argv[1],"explicit-vulkan")) {
        results[0]=S_OK;
        if(!strcmp(argv[1],"explicit-vulkan"))_putenv_s("RECOMP_GPU_BACKEND","Vulkan");
    }
    HRESULT hr=d3d8_backend_create_device(&device,&context);
    if(timer_failure || exit_failure) {
        assert(hr==(timer_failure?E_FAIL:E_OUTOFMEMORY));
        assert(!attempts && !loads && !releases && !native_shutdowns && !native_vulkan && !device && !context);
    }
    else if(!strcmp(argv[1],"vulkan") || !strcmp(argv[1],"explicit-vulkan")) {
        assert(SUCCEEDED(hr) && native_vulkan && attempts==1 && !loads && !releases && !native_shutdowns && !device && !context);
        assert(!strcmp(active_backend,"Vulkan"));
    }
    else {
        assert(!native_vulkan && native_shutdowns==(!strcmp(argv[1],"d3d12")?0u:1u));
        if(!strcmp(argv[1],"explicit")){assert(hr==E_FAIL && attempts==1 && releases==0 && !device && !context);}
        else if(!strcmp(argv[1],"load")){assert(SUCCEEDED(hr) && attempts==3 && loads==1 && !strcmp(active_backend,"D3D11"));}
        else if(!strcmp(argv[1],"all")){assert(FAILED(hr) && attempts==4 && releases==6 && !device && !context);}
        else if(!strcmp(argv[1],"d3d12")){assert(SUCCEEDED(hr) && attempts==1 && !releases && !strcmp(active_backend,"D3D12"));}
        else if(!strcmp(argv[1],"auto-d3d12")){assert(SUCCEEDED(hr) && attempts==3 && releases==2 && !strcmp(active_backend,"D3D12"));}
        else if(!strcmp(argv[1],"gl")){assert(SUCCEEDED(hr) && attempts==2 && releases==0 && !strcmp(active_backend,"OpenGL"));}
        else {assert(SUCCEEDED(hr) && attempts==4 && releases==4 && !strcmp(active_backend,"D3D11"));}
    }
    if(timer_failure){assert(timer_begins==1 && timer_ends==0 && !timer_cleanup);}
    else if(exit_failure){assert(timer_begins==1 && timer_ends==1 && !timer_cleanup);}
    else {assert(timer_begins==1 && timer_ends==0 && timer_cleanup);timer_cleanup();assert(timer_ends==1);}
    return 0;
}
''',encoding='utf-8')
    result=subprocess.run([compiler,'/nologo','/O2','/MD','/utf-8',str(work/'probe.c'),'/Fe:'+str(work/'probe.exe'),'/link','d3d11.lib','dxgi.lib'],cwd=work,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return work/'probe.exe'


@pytest.mark.parametrize('mode',['auto','explicit','load','all','gl','timer-fail','exit-fail','d3d12','auto-d3d12','vulkan','explicit-vulkan'])
def test_startup_failure_selection_and_cleanup(backend_probe,mode):
    result=subprocess.run([str(backend_probe),mode],capture_output=True,text=True,timeout=10)
    assert result.returncode==0,result.stdout+result.stderr
