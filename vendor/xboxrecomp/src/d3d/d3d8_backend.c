/* Vulkan owns a native device and queue. The other selections keep their
 * existing D3D11, D3D11On12 or WineD3D implementation. */
#include "d3d8_internal.h"
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef HRESULT (WINAPI *CreateFactory)(REFIID,void **);
typedef HRESULT (WINAPI *CreateDevice)(IDXGIAdapter *,D3D_DRIVER_TYPE,HMODULE,UINT,
    const D3D_FEATURE_LEVEL *,UINT,UINT,ID3D11Device **,D3D_FEATURE_LEVEL *,ID3D11DeviceContext **);
static unsigned render_scale=1;
static int display_vsync;
void d3d8_DisplayVSyncConfigure(int enabled){display_vsync=enabled!=0;}
int d3d8_DisplayVSync(void){return display_vsync;}
static float game_aspect=4.0f/3.0f;
static const char *active_backend="D3D11";
static int native_vulkan;
int d3d8_IsNativeVulkan(void){return native_vulkan;}
void d3d8_DisplayConfigure(unsigned scale,float aspect)
{
    render_scale=scale;game_aspect=aspect;
}
unsigned d3d8_RenderScale(void){return render_scale;}
float d3d8_GameAspect(void){return game_aspect;}
const char *d3d8_ActiveBackend(void){return active_backend;}

static IDXGIAdapter3 *memory_adapter;
static unsigned memory_segments;
static ULONGLONG memory_last_tick;
static int memory_sampled;
void d3d8_video_memory_shutdown(void)
{
    if(memory_adapter)IDXGIAdapter3_Release(memory_adapter);
    memory_adapter=NULL;memory_segments=0;memory_sampled=0;
}
void d3d8_LogVideoMemory(void)
{
    if(!memory_adapter || !memory_segments)return;
    ULONGLONG now=GetTickCount64();
    if(memory_sampled && now-memory_last_tick<5000)return;
    memory_sampled=1;memory_last_tick=now;
    for(unsigned segment=0;segment<2;segment++) {
        if(!(memory_segments&(1u<<segment)))continue;
        const char *name=segment ? "nonlocal" : "local";
        DXGI_QUERY_VIDEO_MEMORY_INFO info={0};
        HRESULT hr=IDXGIAdapter3_QueryVideoMemoryInfo(memory_adapter,0,
            (DXGI_MEMORY_SEGMENT_GROUP)segment,&info);
        if(SUCCEEDED(hr)) {
            fprintf(stderr,"[GPU MEMORY %s] segment=%s current_bytes=%llu budget_bytes=%llu\n",
                active_backend,name,(unsigned long long)info.CurrentUsage,
                (unsigned long long)info.Budget);
        } else {
            fprintf(stderr,"[GPU MEMORY %s] segment=%s unavailable hr=%08lX\n",
                active_backend,name,(unsigned long)hr);
            memory_segments&=~(1u<<segment);
        }
    }
}
static void attach_memory_adapter(IDXGIAdapter1 *adapter)
{
    d3d8_video_memory_shutdown();
    HRESULT hr=IDXGIAdapter1_QueryInterface(adapter,&IID_IDXGIAdapter3,(void **)&memory_adapter);
    if(FAILED(hr)) {
        memory_adapter=NULL;
        fprintf(stderr,"[GPU MEMORY %s] unavailable IDXGIAdapter3 hr=%08lX\n",
            active_backend,(unsigned long)hr);
        return;
    }
    memory_segments=3;
    d3d8_LogVideoMemory();
}

static HRESULT load_backend(const wchar_t *name,HMODULE *dxgi,HMODULE *d3d)
{
    wchar_t path[32768],*end;DWORD length=GetModuleFileNameW(NULL,path,32768);
    if(!length || length>=32768)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    end=wcsrchr(path,L'\\');if(!end)return E_INVALIDARG;
    size_t remaining=32768-(size_t)(end+1-path);
    const wchar_t *dxgi_name=L"nogi.dll";
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
            if(SUCCEEDED(description))fprintf(stderr,"[GPU %s] adapter %ls, dedicated memory %llu, feature level %04X, shared system memory %llu, dedicated system memory %llu, vendor %04X device %04X\n",active_backend,desc.Description,(unsigned long long)desc.DedicatedVideoMemory,level,(unsigned long long)desc.SharedSystemMemory,(unsigned long long)desc.DedicatedSystemMemory,desc.VendorId,desc.DeviceId);
            attach_memory_adapter(chosen);
        }
    }
    if(chosen)IDXGIAdapter1_Release(chosen);
    return hr;
}
static HRESULT WINAPI create_d3d12(IDXGIAdapter *adapter,D3D_DRIVER_TYPE driver,
    HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,UINT sdk,
    ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context)
{
    (void)driver;(void)software;(void)sdk;
    ID3D12Device *native=NULL;ID3D12CommandQueue *queue=NULL;
    HRESULT hr=D3D12CreateDevice((IUnknown *)adapter,D3D_FEATURE_LEVEL_11_0,
        &IID_ID3D12Device,(void **)&native);
    if(SUCCEEDED(hr)) {
        D3D12_COMMAND_QUEUE_DESC desc={0};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr=ID3D12Device_CreateCommandQueue(native,&desc,&IID_ID3D12CommandQueue,(void **)&queue);
    }
    if(SUCCEEDED(hr)) {
        IUnknown *queues[]={(IUnknown *)queue};
        hr=D3D11On12CreateDevice((IUnknown *)native,flags,levels,count,queues,1,0,
            device,context,level);
    }
    if(queue)ID3D12CommandQueue_Release(queue);
    if(native)ID3D12Device_Release(native);
    return hr;
}
static void release_opengl_timer(void) { timeEndPeriod(1); }
HRESULT d3d8_backend_create_device(ID3D11Device **device,ID3D11DeviceContext **context)
{
    if(!device || !context)return E_POINTER;
    *device=NULL;*context=NULL;
    const char *requested=getenv("RECOMP_GPU_BACKEND");
    if(!requested)requested="D3D11";
    int automatic=!strcmp(requested,"Auto");
    const char *names[]={"Vulkan","OpenGL","D3D12","D3D11"};
    HRESULT last=E_INVALIDARG;
    /* Driver dependencies have process lifetime: unloading a failed runtime
     * can invalidate shared native-driver state during subsequent draws.
     * Cache each load once; partial device/context objects are still released. */
    static HMODULE modules[2];
    for(unsigned index=0;index<4;index++) {
        if(!automatic && strcmp(requested,names[index]))continue;
        active_backend=names[index];HRESULT hr=S_OK;
        CreateFactory factory_fn=CreateDXGIFactory1;CreateDevice device_fn=D3D11CreateDevice;
        if(index==2)device_fn=create_d3d12;
        fprintf(stderr,"[GPU] trying %s\n",active_backend);
        if(index==0) {
            wchar_t compiler[MAX_PATH];DWORD length=GetModuleFileNameW(NULL,compiler,MAX_PATH);
            if(!length || length>=MAX_PATH)hr=E_FAIL;
            else {
                wchar_t *slash=wcsrchr(compiler,L'\\');
                if(!slash)hr=E_FAIL;
                else {
                    slash[1]=0;
                    if(wcscat_s(compiler,MAX_PATH,L"backends\\vulkan\\dxcompiler.dll"))hr=E_FAIL;
                    else if(!nvk_initialize(compiler)) {
                        fprintf(stderr,"[GPU Vulkan] %s\n",nvk_error());nvk_shutdown();hr=E_FAIL;
                    } else native_vulkan=1;
                }
            }
        }
        if(index==1) {
            if(!SetEnvironmentVariableA("WINE_D3D_CONFIG","renderer=gl") || _putenv_s("WINE_D3D_CONFIG","renderer=gl"))hr=E_FAIL;
            if(SUCCEEDED(hr))hr=load_backend(L"opengl",&modules[0],&modules[1]);
            if(SUCCEEDED(hr)) {
                factory_fn=(CreateFactory)GetProcAddress(modules[0],"CreateDXGIFactory1");
                device_fn=(CreateDevice)GetProcAddress(modules[1],"D3D11CreateDevice");
                if(!factory_fn || !device_fn)hr=HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            }
        }
        /* Let the selected implementation check its own driver requirements.
         * WineD3D may return E_FAIL or another HRESULT for an unusable
         * driver, not just DXGI_ERROR_UNSUPPORTED. */
        if(SUCCEEDED(hr) && index!=0)hr=create_hardware(factory_fn,device_fn,device,context);
        /* WineD3D polls EVENT queries on its command thread. Its short waits
         * otherwise round to the default Windows timer interval, stalling
         * NINE's per-glyph completion fences. Pair this process request at exit. */
        if(SUCCEEDED(hr) && index==1) {
            if(timeBeginPeriod(1)!=TIMERR_NOERROR)hr=E_FAIL;
            else if(atexit(release_opengl_timer)!=0){timeEndPeriod(1);hr=E_OUTOFMEMORY;}
        }
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
