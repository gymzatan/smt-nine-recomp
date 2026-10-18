/* The existing D3D11 command and shader contract is implemented by DXVK for
 * Vulkan, WineD3D for OpenGL, or the system D3D11 driver. */
#include "d3d8_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef HRESULT (WINAPI *CreateFactory)(REFIID,void **);
typedef HRESULT (WINAPI *CreateDevice)(IDXGIAdapter *,D3D_DRIVER_TYPE,HMODULE,UINT,
    const D3D_FEATURE_LEVEL *,UINT,UINT,ID3D11Device **,D3D_FEATURE_LEVEL *,ID3D11DeviceContext **);
static unsigned render_scale=1;
static float game_aspect=4.0f/3.0f;
static const char *active_backend="D3D11";
void d3d8_DisplayConfigure(unsigned scale,float aspect)
{
    render_scale=scale;game_aspect=aspect;
}
unsigned d3d8_RenderScale(void){return render_scale;}
float d3d8_GameAspect(void){return game_aspect;}
const char *d3d8_ActiveBackend(void){return active_backend;}

static HRESULT load_backend(const wchar_t *name,HMODULE *dxgi,HMODULE *d3d)
{
    wchar_t path[32768],*end;DWORD length=GetModuleFileNameW(NULL,path,32768);
    if(!length || length>=32768)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    end=wcsrchr(path,L'\\');if(!end)return E_INVALIDARG;
    size_t remaining=32768-(size_t)(end+1-path);
    const wchar_t *dxgi_name=!wcscmp(name,L"vulkan")?L"nvgi.dll":L"nogi.dll";
    if(swprintf_s(end+1,remaining,L"backends\\%s\\%s",name,dxgi_name)<0)return E_INVALIDARG;
    if(!*dxgi)*dxgi=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!*dxgi)return HRESULT_FROM_WIN32(GetLastError());
    if(swprintf_s(end+1,remaining,L"backends\\%s\\d3d11.dll",name)<0)return E_INVALIDARG;
    if(!*d3d)*d3d=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!*d3d)return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
static HRESULT create_hardware(CreateFactory factory_fn,CreateDevice device_fn,
    ID3D11Device **device,ID3D11DeviceContext **context)
{
    IDXGIFactory1 *factory=NULL;IDXGIAdapter1 *chosen=NULL;SIZE_T memory=0;HRESULT hr;
    hr=factory_fn(&IID_IDXGIFactory1,(void **)&factory);if(FAILED(hr))return hr;
    for(UINT i=0;;i++) {
        IDXGIAdapter1 *adapter=NULL;DXGI_ADAPTER_DESC1 desc;
        hr=IDXGIFactory1_EnumAdapters1(factory,i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND){hr=S_OK;break;}
        if(FAILED(hr))break;
        hr=IDXGIAdapter1_GetDesc1(adapter,&desc);
        if(FAILED(hr)){IDXGIAdapter1_Release(adapter);break;}
        if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && (!chosen || desc.DedicatedVideoMemory>memory)) {
            if(chosen)IDXGIAdapter1_Release(chosen);
            chosen=adapter;memory=desc.DedicatedVideoMemory;
        } else IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    if(SUCCEEDED(hr) && !chosen)hr=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(hr)) {
        D3D_FEATURE_LEVEL level;const D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0};
        hr=device_fn((IDXGIAdapter *)chosen,D3D_DRIVER_TYPE_UNKNOWN,NULL,0,requested,1,D3D11_SDK_VERSION,device,&level,context);
        if(SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC1 desc;HRESULT description=IDXGIAdapter1_GetDesc1(chosen,&desc);
            if(SUCCEEDED(description))fprintf(stderr,"[GPU %s] adapter %ls, dedicated memory %llu, feature level %04X\n",active_backend,desc.Description,(unsigned long long)desc.DedicatedVideoMemory,level);
        }
    }
    if(chosen)IDXGIAdapter1_Release(chosen);
    return hr;
}
HRESULT d3d8_backend_create_device(ID3D11Device **device,ID3D11DeviceContext **context)
{
    if(!device || !context)return E_POINTER;
    *device=NULL;*context=NULL;
    const char *requested=getenv("RECOMP_GPU_BACKEND");
    if(!requested)requested="D3D11";
    int automatic=!strcmp(requested,"Auto");
    const char *names[]={"Vulkan","OpenGL","D3D11"};
    const wchar_t *folders[]={L"vulkan",L"opengl"};
    HRESULT last=E_INVALIDARG;
    /* Driver dependencies have process lifetime: unloading a failed runtime
     * can invalidate shared native-driver state during subsequent draws.
     * Cache each load once; partial device/context objects are still released. */
    static HMODULE modules[2][2];
    for(unsigned index=0;index<3;index++) {
        if(!automatic && strcmp(requested,names[index]))continue;
        active_backend=names[index];HRESULT hr=S_OK;
        CreateFactory factory_fn=CreateDXGIFactory1;CreateDevice device_fn=D3D11CreateDevice;
        fprintf(stderr,"[GPU] trying %s\n",active_backend);
        if(index<2) {
            if(index==1 && (!SetEnvironmentVariableA("WINE_D3D_CONFIG","renderer=gl") || _putenv_s("WINE_D3D_CONFIG","renderer=gl")))hr=E_FAIL;
            if(SUCCEEDED(hr))hr=load_backend(folders[index],&modules[index][0],&modules[index][1]);
            if(SUCCEEDED(hr)) {
                factory_fn=(CreateFactory)GetProcAddress(modules[index][0],"CreateDXGIFactory1");
                device_fn=(CreateDevice)GetProcAddress(modules[index][1],"D3D11CreateDevice");
                if(!factory_fn || !device_fn)hr=HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            }
        }
        /* Let the selected implementation check its own driver requirements.
         * DXVK/WineD3D may return E_FAIL or another HRESULT for an unusable
         * driver, not just DXGI_ERROR_UNSUPPORTED. */
        if(SUCCEEDED(hr))hr=create_hardware(factory_fn,device_fn,device,context);
        if(SUCCEEDED(hr)){fprintf(stderr,"[GPU] active backend %s\n",active_backend);last=hr;break;}
        if(*context){ID3D11DeviceContext_Release(*context);*context=NULL;}
        if(*device){ID3D11Device_Release(*device);*device=NULL;}
        last=hr;
        fprintf(stderr,"[GPU] %s initialization failed: %08lX\n",names[index],(unsigned long)hr);
        if(!automatic)break;
    }
    return last;
}

static D3D8TextureAssetLookup texture_asset_lookup;
void d3d8_SetTextureAssetLookup(D3D8TextureAssetLookup lookup){texture_asset_lookup=lookup;}
const void *d3d8_TextureAsset(const void *raw,size_t bytes,unsigned width,unsigned height,unsigned *host_width,unsigned *host_height)
{ return texture_asset_lookup?texture_asset_lookup(raw,bytes,width,height,host_width,host_height):NULL; }
