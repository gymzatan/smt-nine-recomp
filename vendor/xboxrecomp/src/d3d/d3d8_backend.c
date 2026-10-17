/* The existing D3D11 command and shader contract is implemented by DXVK for
 * Vulkan, WineD3D for OpenGL, or the system D3D11 driver. */
#include "d3d8_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"

typedef HRESULT (WINAPI *CreateFactory)(REFIID,void **);
typedef HRESULT (WINAPI *CreateDevice)(IDXGIAdapter *,D3D_DRIVER_TYPE,HMODULE,UINT,
    const D3D_FEATURE_LEVEL *,UINT,UINT,ID3D11Device **,D3D_FEATURE_LEVEL *,ID3D11DeviceContext **);
static unsigned render_scale=1;
static float game_aspect=4.0f/3.0f;
static const char *active_backend="D3D11";
static HRESULT vulkan_available(void)
{
    HMODULE loader=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!loader) {
        DWORD error=GetLastError();
        return error==ERROR_MOD_NOT_FOUND?DXGI_ERROR_UNSUPPORTED:HRESULT_FROM_WIN32(error);
    }
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(loader,"vkGetInstanceProcAddr");
    if(!get){FreeLibrary(loader);return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);}
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties extensions=(PFN_vkEnumerateInstanceExtensionProperties)get(VK_NULL_HANDLE,"vkEnumerateInstanceExtensionProperties");
    if(!create || !extensions){FreeLibrary(loader);return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);}
    uint32_t extension_count=0;VkResult extension_result=extensions(NULL,&extension_count,NULL);
    if(extension_result!=VK_SUCCESS){FreeLibrary(loader);return extension_result==VK_ERROR_INCOMPATIBLE_DRIVER?DXGI_ERROR_UNSUPPORTED:E_FAIL;}
    VkExtensionProperties *available=extension_count?malloc(sizeof *available*extension_count):NULL;
    if(extension_count && !available){FreeLibrary(loader);return E_OUTOFMEMORY;}
    extension_result=extensions(NULL,&extension_count,available);
    int surface=0,win32=0;
    if(extension_result==VK_SUCCESS)for(unsigned i=0;i<extension_count;i++) {
        if(!strcmp(available[i].extensionName,"VK_KHR_surface"))surface=1;
        if(!strcmp(available[i].extensionName,"VK_KHR_win32_surface"))win32=1;
    }
    free(available);
    if(extension_result!=VK_SUCCESS){FreeLibrary(loader);return E_FAIL;}
    if(!surface || !win32){FreeLibrary(loader);return DXGI_ERROR_UNSUPPORTED;}
    VkApplicationInfo application={VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="NINE";application.pEngineName="DXVK";application.apiVersion=VK_API_VERSION_1_4;
    VkInstanceCreateInfo info={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&application;
    VkInstance instance=VK_NULL_HANDLE;VkResult result=create(&info,NULL,&instance);
    if(result!=VK_SUCCESS){FreeLibrary(loader);return result==VK_ERROR_INCOMPATIBLE_DRIVER?DXGI_ERROR_UNSUPPORTED:result==VK_ERROR_OUT_OF_HOST_MEMORY?E_OUTOFMEMORY:E_FAIL;}
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)get(instance,"vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    uint32_t count=0;result=enumerate(instance,&count,NULL);HRESULT hr=E_FAIL;
    if(result==VK_SUCCESS) {
        hr=DXGI_ERROR_UNSUPPORTED;
        VkPhysicalDevice *devices=count?malloc(sizeof *devices*count):NULL;
        if(count && !devices)hr=E_OUTOFMEMORY;
        else if(count) {
            result=enumerate(instance,&count,devices);
            if(result!=VK_SUCCESS)hr=E_FAIL;
            else for(unsigned i=0;i<count;i++) {
                VkPhysicalDeviceProperties caps;properties(devices[i],&caps);
                if(caps.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU && caps.apiVersion>=VK_API_VERSION_1_4){hr=S_OK;break;}
            }
        }
        free(devices);
    }
    destroy(instance,NULL);FreeLibrary(loader);return hr;
}
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
    *dxgi=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!*dxgi)return HRESULT_FROM_WIN32(GetLastError());
    if(swprintf_s(end+1,remaining,L"backends\\%s\\d3d11.dll",name)<0)return E_INVALIDARG;
    *d3d=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!*d3d){HRESULT hr=HRESULT_FROM_WIN32(GetLastError());FreeLibrary(*dxgi);*dxgi=NULL;return hr;}
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
    const char *requested=getenv("RECOMP_GPU_BACKEND");
    if(!requested)requested="D3D11";
    int automatic=!strcmp(requested,"Auto");
    const char *names[]={"Vulkan","OpenGL","D3D11"};
    const wchar_t *folders[]={L"vulkan",L"opengl"};
    for(unsigned index=0;index<3;index++) {
        if(!automatic && strcmp(requested,names[index]))continue;
        active_backend=names[index];HMODULE dxgi=NULL,d3d=NULL;HRESULT hr;
        CreateFactory factory_fn=CreateDXGIFactory1;CreateDevice device_fn=D3D11CreateDevice;
        if(index==0) {
            hr=vulkan_available();
            if(hr==DXGI_ERROR_UNSUPPORTED && automatic){fprintf(stderr,"[GPU] Vulkan loader/driver capabilities unavailable; trying OpenGL\n");continue;}
            if(FAILED(hr))return hr;
        }
        if(index<2) {
            if(index==1 && (!SetEnvironmentVariableA("WINE_D3D_CONFIG","renderer=gl") || _putenv_s("WINE_D3D_CONFIG","renderer=gl")))return E_FAIL;
            hr=load_backend(folders[index],&dxgi,&d3d);
            /* Missing/corrupt deployment is not a hardware capability failure. */
            if(FAILED(hr)){fprintf(stderr,"[STOP] %s runtime load failed: %08lX\n",active_backend,(unsigned long)hr);return hr;}
            factory_fn=(CreateFactory)GetProcAddress(dxgi,"CreateDXGIFactory1");
            device_fn=(CreateDevice)GetProcAddress(d3d,"D3D11CreateDevice");
            if(!factory_fn || !device_fn){FreeLibrary(d3d);FreeLibrary(dxgi);return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);}
        }
        hr=create_hardware(factory_fn,device_fn,device,context);
        if(SUCCEEDED(hr)){fprintf(stderr,"[GPU] active backend %s\n",active_backend);return hr;}
        if(*context){ID3D11DeviceContext_Release(*context);*context=NULL;}
        if(*device){ID3D11Device_Release(*device);*device=NULL;}
        if(d3d)FreeLibrary(d3d);if(dxgi)FreeLibrary(dxgi);
        if(!automatic || hr!=DXGI_ERROR_UNSUPPORTED)return hr;
        fprintf(stderr,"[GPU] %s required hardware capabilities unavailable; trying %s\n",names[index],index<2?names[index+1]:"none");
    }
    return DXGI_ERROR_UNSUPPORTED;
}

static D3D8TextureAssetLookup texture_asset_lookup;
void d3d8_SetTextureAssetLookup(D3D8TextureAssetLookup lookup){texture_asset_lookup=lookup;}
const void *d3d8_TextureAsset(const void *raw,size_t bytes,unsigned width,unsigned height,unsigned *host_width,unsigned *host_height)
{ return texture_asset_lookup?texture_asset_lookup(raw,bytes,width,height,host_width,host_height):NULL; }
