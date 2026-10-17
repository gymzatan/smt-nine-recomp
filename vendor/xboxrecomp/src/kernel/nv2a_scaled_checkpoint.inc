/* Persist the actual high-resolution colour surfaces, using their existing
 * physical identities. Guest RAM remains the authoritative console mapping. */
typedef struct HwColorCheckpoint {
    uint32_t address,pitch,width,height,bpp,format,scale;
    uint64_t bytes;
} HwColorCheckpoint;
static void hw_free_colors(HwColor *colors)
{
    while(colors) {
        HwColor *next=colors->next;
        if(colors->host_surface)colors->host_surface->lpVtbl->Release(colors->host_surface);
        if(colors->surface)colors->surface->lpVtbl->Release(colors->surface);
        free(colors);colors=next;
    }
}
static int hw_save_scaled_colors(FILE *out)
{
    uint32_t count=0,scale=d3d8_RenderScale();
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->host_surface)count++;
    if(fwrite(&count,4,1,out)!=1)return 0;
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->host_surface) {
        HwColorCheckpoint header={c->address,c->pitch,c->width,c->height,c->bpp,c->format,scale,(uint64_t)c->width*c->height*scale*scale*4};
        D3DLOCKED_RECT pixels;
        HRESULT hr=c->host_surface->lpVtbl->LockRect(c->host_surface,&pixels,NULL,D3DLOCK_READONLY);
        if(FAILED(hr))return 0;
        int ok=fwrite(&header,sizeof header,1,out)==1;
        for(unsigned y=0;ok && y<c->height*scale;y++)ok=fwrite((uint8_t *)pixels.pBits+(size_t)y*pixels.Pitch,(size_t)c->width*scale*4,1,out)==1;
        if(FAILED(c->host_surface->lpVtbl->UnlockRect(c->host_surface)))ok=0;
        if(!ok)return 0;
    }
    return 1;
}
static int hw_prepare_scaled_colors(FILE *in,HwColor **result)
{
    *result=NULL;uint32_t count;
    if(fread(&count,4,1,in)!=1 || count>65536)return 0;
    if(count)hw_initialize();
    HwColor **next=result;
    for(uint32_t i=0;i<count;i++) {
        HwColorCheckpoint header;
        if(fread(&header,sizeof header,1,in)!=1 || header.scale!=d3d8_RenderScale() || header.scale<2 || header.scale>4 ||
           !header.width || !header.height || header.width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/header.scale ||
           header.height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/header.scale || header.bpp!=4 || header.pitch<header.width*4 ||
           header.bytes!=(uint64_t)header.width*header.height*header.scale*header.scale*4 || header.bytes>SIZE_MAX)return 0;
        void *bytes=malloc((size_t)header.bytes);if(!bytes)return 0;
        if(fread(bytes,(size_t)header.bytes,1,in)!=1){free(bytes);return 0;}
        XboxSharedMemoryViews views;
        if(!xbox_GetSharedMemoryViews((uint8_t *)xbox_GetMemoryOffset()+header.address,(size_t)header.pitch*header.height,&views)){free(bytes);return 0;}
        HwColor *color=calloc(1,sizeof *color);if(!color){free(bytes);return 0;}
        *next=color;next=&color->next;
        color->address=header.address;color->pitch=header.pitch;color->width=header.width;color->height=header.height;
        color->bpp=header.bpp;color->format=header.format;color->memory=hw_memory(&views);color->memory_offset=views.offset;
        IDirect3DDevice8 *device=d3d8_GetDevice();
        HRESULT hr=device->lpVtbl->CreateRenderTarget(device,color->width,color->height,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->surface);
        if(SUCCEEDED(hr))hr=device->lpVtbl->CreateRenderTarget(device,color->width*header.scale,color->height*header.scale,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->host_surface);
        if(SUCCEEDED(hr))ID3D11DeviceContext_UpdateSubresource(d3d8_GetD3D11Context(),(ID3D11Resource *)((D3D8Surface *)color->host_surface)->d3d11_texture,0,NULL,bytes,color->width*header.scale*4,0);
        free(bytes);if(FAILED(hr))return 0;
    }
    return 1;
}
static D3D8Surface *hw_prepare_scaled_color(uint32_t address,uint32_t pitch)
{
    if(d3d8_RenderScale()==1)return NULL;
    const char *window=getenv("RECOMP_FB_WINDOW");if(!window || strcmp(window,"1"))return NULL;
    hw_enter();
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->address==address && c->pitch==pitch && c->host_surface) {
        D3D8Surface *surface=(D3D8Surface *)c->host_surface;
        hw_check(d3d8_surface_queue_readback(c->host_surface),"scaled display readback queue");
        hw_leave();return surface;
    }
    hw_leave();return NULL;
}
static void hw_present_scaled_color(D3D8Surface *surface)
{
    if(!surface)return;
    hw_enter();D3DLOCKED_RECT pixels;
    hw_check(d3d8_surface_lock_readback(&surface->iface,&pixels),"scaled display readback");
    extern void xbox_FramebufferWindowPresentHost(const void *,unsigned,unsigned,unsigned);
    xbox_FramebufferWindowPresentHost(pixels.pBits,(unsigned)pixels.Pitch,surface->width,surface->height);
    hw_check(surface->iface.lpVtbl->UnlockRect(&surface->iface),"scaled display unlock");
    hw_leave();
}
