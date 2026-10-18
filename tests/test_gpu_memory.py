"""Exercise the actual memory logger with supported and unavailable DXGI queries."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def memory_probe(tmp_path_factory):
    compiler = shutil.which('cl')
    assert compiler, 'requires the existing MSVC environment'
    work = tmp_path_factory.mktemp('gpu-memory')
    source = (ROOT/'vendor/xboxrecomp/src/d3d/d3d8_backend.c').read_text()
    body = source[source.index('static IDXGIAdapter3 *memory_adapter;'):source.index('static HRESULT load_backend(')]
    (work/'probe.c').write_text(r'''
#define COBJMACROS
#include <windows.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static const char *active_backend="D3D11";
static ULONGLONG tick=10000;
static unsigned calls[2],releases;
static int unsupported,local_failure;
static ULONGLONG test_tick(void){return tick;}
#define GetTickCount64 test_tick
static HRESULT STDMETHODCALLTYPE query_memory(IDXGIAdapter3 *self,UINT node,
    DXGI_MEMORY_SEGMENT_GROUP segment,DXGI_QUERY_VIDEO_MEMORY_INFO *info){
    assert(node==0 && segment<=1);++calls[segment];
    if(segment==0 && local_failure)return E_NOTIMPL;
    info->CurrentUsage=segment?67108864:32505856;
    info->Budget=segment?4294967296ULL:268435456;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE release_adapter(IDXGIAdapter3 *self){++releases;return 0;}
static IDXGIAdapter3Vtbl memory_vtable={.Release=release_adapter,.QueryVideoMemoryInfo=query_memory};
static IDXGIAdapter3 fake_memory={&memory_vtable};
static HRESULT STDMETHODCALLTYPE query_interface(IDXGIAdapter1 *self,REFIID iid,void **out){
    if(unsupported){*out=NULL;return E_NOINTERFACE;}
    *out=&fake_memory;return S_OK;
}
static IDXGIAdapter1Vtbl adapter_vtable={.QueryInterface=query_interface};
static IDXGIAdapter1 fake_adapter={&adapter_vtable};
''' + body + r'''
int main(int argc,char **argv){
    assert(argc==2);
    unsupported=!strcmp(argv[1],"unsupported");
    local_failure=!strcmp(argv[1],"local-unavailable");
    attach_memory_adapter(&fake_adapter);
    if(unsupported){assert(!calls[0] && !calls[1]);d3d8_LogVideoMemory();return 0;}
    assert(calls[0]==1 && calls[1]==1);
    tick=14999;d3d8_LogVideoMemory();assert(calls[0]==1 && calls[1]==1);
    tick=15000;d3d8_LogVideoMemory();assert(calls[0]==(local_failure?1:2) && calls[1]==2);
    unsupported=1;attach_memory_adapter(&fake_adapter);assert(releases==1);
    tick=20000;d3d8_LogVideoMemory();assert(calls[1]==2);
    return 0;
}
''', encoding='utf-8')
    result = subprocess.run([compiler, '/nologo', '/O2', '/MD', '/utf-8', '/std:c11', str(work/'probe.c'),
        '/Fe:'+str(work/'probe.exe'), '/link', 'dxguid.lib'], cwd=work, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return work/'probe.exe'


@pytest.mark.parametrize('mode', ['supported', 'unsupported', 'local-unavailable'])
def test_actual_memory_queries_and_throttling(memory_probe, mode):
    result = subprocess.run([str(memory_probe), mode], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    if mode == 'unsupported':
        assert 'unavailable IDXGIAdapter3' in result.stderr
        assert 'current_bytes=' not in result.stderr
    else:
        assert 'segment=nonlocal current_bytes=67108864 budget_bytes=4294967296' in result.stderr
        if mode == 'local-unavailable':
            assert result.stderr.count('segment=local unavailable') == 1
            assert 'segment=local current_bytes=' not in result.stderr
        else:
            assert result.stderr.count('segment=local current_bytes=32505856 budget_bytes=268435456') == 2
