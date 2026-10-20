/* Physical resource connection for the existing NV097 consumer. Native
 * program outputs consume the existing VSH cache and geometry setup;
 * attribute, primitive and texture identities remain in nv2a_pb_exec.c. */
#include <d3dcompiler.h>
static ID3D11VertexShader *s_hw_clear_vs;
static ID3D11Buffer *s_hw_clear_cb;
static ID3D11RasterizerState *s_hw_clear_rs;
static ID3D11DepthStencilState *s_hw_clear_ds[4];
typedef struct HwMemory {
    XboxSharedMemoryViews views;
    uint8_t *host_view;
    struct HwMemory *next;
} HwMemory;
static HwMemory *s_hw_memories;
typedef struct HwColor {
    uint32_t address, pitch, width, height, bpp, format;
    IDirect3DSurface8 *surface,*host_surface;
    HwMemory *memory;
    size_t memory_offset;
    int upload_needed;
    int dirty;
    RECT dirty_bounds;
    struct HwColor *next;
} HwColor;
typedef struct HwDepth {
    uint32_t address, width, height, format;
    ID3D11Texture2D *texture;
    ID3D11DepthStencilView *view;
    struct HwDepth *next;
} HwDepth;
typedef struct HwTexture {
    Texture identity;
    size_t raw_size;
    uint8_t *raw;
    uint32_t palette[256];
    IDirect3DBaseTexture8 *texture;
    unsigned host_width,host_height;
    struct HwTexture *next;
} HwTexture;
static HwColor *s_hw_colors;
static HwDepth *s_hw_depths;
static HwTexture *s_hw_textures;
static Nv2aVshOutput *s_hw_vertices;
static uint32_t s_hw_count, s_hw_capacity;
static RECT s_hw_bounds;
static int s_hw_program_active, s_hw_fixed_active;
static D3D11_PRIMITIVE_TOPOLOGY s_hw_topology;
static uint32_t s_hw_program_vertex_count;
static uint32_t s_hw_program_inputs;
static uint32_t s_hw_program_vertex_rows[NV_MAX_INDICES];
static struct { float vertex[NV_MAX_INDICES*16][4]; uint32_t indices[NV_MAX_INDICES*6]; } s_hw_program_upload;
static void hw_program_triangle(uint32_t a,uint32_t b,uint32_t c,uint32_t p)
{
    uint32_t *indices=s_hw_program_upload.indices+s_hw_count;
    indices[0]=s_hw_program_vertex_rows[a];indices[1]=s_hw_program_vertex_rows[p];
    indices[2]=s_hw_program_vertex_rows[b];indices[3]=s_hw_program_vertex_rows[p];
    indices[4]=s_hw_program_vertex_rows[c];indices[5]=s_hw_program_vertex_rows[p];
    if(!s_hw_count) note_drawn();
    s_hw_count+=6;
}
static void hw_program_line(uint32_t a,uint32_t b,uint32_t p)
{
    uint32_t *indices=s_hw_program_upload.indices+s_hw_count;
    indices[0]=s_hw_program_vertex_rows[p];indices[1]=s_hw_program_vertex_rows[a];
    indices[2]=s_hw_program_vertex_rows[b];indices[3]=s_hw_program_vertex_rows[p];
    if(!s_hw_count) note_drawn();
    s_hw_count+=4;
}
static void hw_program_point(uint32_t i)
{
    if(!s_hw_count) note_drawn();
    s_hw_program_upload.indices[s_hw_count++]=s_hw_program_vertex_rows[i];
}
static ID3D11Buffer *s_hw_ps_cb;
typedef struct HwPixelState {
    uint32_t cx[8],cy[8];
    float depth[4];
    uint32_t flags[4],bounds[4];
} HwPixelState;
static HwPixelState s_hw_ps_value;
static int s_hw_ps_value_valid;
static int s_hw_initialized;
typedef struct HwQuery {
    ID3D11Query *query;
    HwMemory *memory;
    size_t offset;
    uint32_t value;
} HwQuery;
static HwQuery *s_hw_queries;
static volatile LONG64 s_hw_query_count;
static void hw_check(HRESULT hr, const char *operation)
{
    if (FAILED(hr)) {
        fprintf(stderr,"[STOP] D3D11 %s failed: %08lX\n",operation,(unsigned long)hr);
        exit(5);
    }
}
static int hw_enabled(void)
{
    static int selected = -1;
    if (selected < 0) {
        const char *selection = getenv("RECOMP_GPU_D3D11");
        selected = selection && strcmp(selection,"1") == 0;
    }
    return selected;
}
static INIT_ONCE s_hw_lock_once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_hw_lock;
static ID3D11Query *s_hw_completion;
static BOOL CALLBACK hw_lock_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;(void)parameter;(void)context;InitializeCriticalSection(&s_hw_lock);return TRUE;
}
/* CPU memory faults and the command consumer share this lock order. The
 * outer device lock also protects the render state used by publication. */
static void hw_enter(void) {
    NV2AState *d=nv2a_get_state();if(d)qemu_mutex_lock(&d->pfifo.lock);
    if(!InitOnceExecuteOnce(&s_hw_lock_once,hw_lock_initialize,NULL,NULL)) hw_check(E_FAIL,"memory lock initialization");
    EnterCriticalSection(&s_hw_lock);
}
static void hw_leave(void) {
    LeaveCriticalSection(&s_hw_lock);
    NV2AState *d=nv2a_get_state();if(d)qemu_mutex_unlock(&d->pfifo.lock);
}
static HwMemory *hw_memory(const XboxSharedMemoryViews *views)
{
    for(HwMemory *m=s_hw_memories;m;m=m->next) if(m->views.section==views->section) return m;
    HwMemory *m=(HwMemory *)calloc(1,sizeof *m);if(!m) hw_check(E_OUTOFMEMORY,"shared memory identity");
    m->views=*views;m->host_view=(uint8_t *)MapViewOfFile((HANDLE)views->section,FILE_MAP_ALL_ACCESS,0,0,views->view_size);
    if(!m->host_view) hw_check(HRESULT_FROM_WIN32(GetLastError()),"shared memory host view");
    m->next=s_hw_memories;s_hw_memories=m;return m;
}
static void hw_protect_views(HwMemory *memory,size_t offset,size_t bytes,DWORD protection)
{
    if(!bytes || offset>memory->views.view_size || bytes>memory->views.view_size-offset) hw_check(E_INVALIDARG,"shared colour protection extent");
    for(unsigned i=0;i<memory->views.view_count;i++) {
        DWORD previous;
        if(!VirtualProtect((uint8_t *)memory->views.cpu_views[i]+offset,bytes,protection,&previous)) hw_check(HRESULT_FROM_WIN32(GetLastError()),"shared colour protection");
    }
}
static size_t hw_color_bytes(const HwColor *color) { return (size_t)color->pitch*color->height; }
static int hw_ranges_overlap(size_t a,size_t an,size_t b,size_t bn) { return a<b+bn && b<a+an; }
static int hw_pending_output(HwMemory *memory,size_t offset,size_t bytes)
{
    for(size_t i=0;i<s_hw_query_count;i++) {
        HwQuery *q=&s_hw_queries[i];
        if(q->memory && q->query && (!memory || (q->memory==memory && hw_ranges_overlap(offset,bytes,q->offset,4)))) return 1;
    }
    return 0;
}
enum HwOutputProtection { HW_OUTPUT_REFRESH, HW_OUTPUT_APPEND, HW_OUTPUT_COMPLETE };
static void hw_output_page_protection(HwQuery *query, enum HwOutputProtection transition)
{
    size_t lo=query->offset&~(size_t)4095,hi=(query->offset+4+4095)&~(size_t)4095;
    for(size_t page=lo;page<hi;page+=4096) {
        /* Appending an output cannot change a page already protected by a
         * pending output. The original queue is the sole pending identity. */
        if(transition==HW_OUTPUT_APPEND) {
            int protected=0;
            for(size_t i=0;i<(size_t)s_hw_query_count;i++) {
                HwQuery *q=&s_hw_queries[i];
                if(q->memory==query->memory && q->query && q->query!=query->query &&
                   hw_ranges_overlap(page,4096,q->offset,4)) {protected=1;break;}
            }
            if(protected) continue;
        }
        int pending=hw_pending_output(query->memory,page,4096),colour=0;
        DWORD protection=pending?PAGE_READONLY:PAGE_READWRITE;
        for(HwColor *c=s_hw_colors;c;c=c->next) if(c->memory==query->memory && hw_ranges_overlap(page,4096,c->memory_offset,hw_color_bytes(c))) {
            colour=1;
            if(c->dirty) {protection=PAGE_NOACCESS;break;}
            if(!c->upload_needed) protection=PAGE_READONLY;
        }
        /* Other pending outputs still own this already read-only page. */
        if(transition==HW_OUTPUT_COMPLETE && pending && !colour) continue;
        hw_protect_views(query->memory,page,4096,protection);
    }
}
static void hw_protect(HwMemory *memory,size_t offset,size_t bytes,DWORD protection,HwQuery *completed)
{
    /* Completion already resolves every owner of its output pages. No
     * other page was changed, so pending outputs elsewhere stay protected. */
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    if(completed) {
        hw_output_page_protection(completed,HW_OUTPUT_COMPLETE);
        /* A colour readback can change ownership on its other pages too. */
        int colour=0;
        for(HwColor *c=s_hw_colors;c;c=c->next)
            if(c->memory==memory && hw_ranges_overlap(lo,hi-lo,c->memory_offset,hw_color_bytes(c))) {colour=1;break;}
        if(!colour)return;
    } else hw_protect_views(memory,offset,bytes,protection);
    for(size_t i=0;i<(size_t)s_hw_query_count;i++) {
        HwQuery *q=&s_hw_queries[i];
        if(q->memory==memory && q->query && (completed || hw_ranges_overlap(lo,hi-lo,q->offset,4)))
            hw_output_page_protection(q,HW_OUTPUT_REFRESH);
    }
}
static void hw_append_query(HwQuery query)
{
    HwQuery *queries=(HwQuery *)realloc(s_hw_queries,(s_hw_query_count+1)*sizeof *queries);
    if(!queries) hw_check(E_OUTOFMEMORY,"query output storage");
    s_hw_queries=queries;s_hw_queries[s_hw_query_count]=query;InterlockedIncrement64(&s_hw_query_count);
}
static void hw_initialize(void);
static void hw_defer_output(uint8_t *output,uint32_t value)
{
    hw_initialize();hw_enter();uint64_t start=gpu_timing_clock();
    XboxSharedMemoryViews views;
    if(!xbox_GetSharedMemoryViews(output,4,&views)) hw_check(E_INVALIDARG,"semaphore shared output identity");
    HwQuery query={0};query.memory=hw_memory(&views);query.offset=views.offset;query.value=value;
    /* Transfer the existing idle EVENT; pending entries keep sole ownership. */
    if(s_hw_completion) {query.query=s_hw_completion;s_hw_completion=NULL;}
    else {
        D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};
        hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&query.query),"semaphore completion event");
    }
    ID3D11DeviceContext_End(d3d8_GetD3D11Context(),(ID3D11Asynchronous *)query.query);
    hw_append_query(query);hw_output_page_protection(&query,HW_OUTPUT_APPEND);
    gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);
    hw_leave();
}
static void hw_mark_cpu_write(HwMemory *memory,size_t offset,size_t bytes,HwQuery *completed)
{
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    for(HwColor *c=s_hw_colors;c;c=c->next) if(c->memory==memory && hw_ranges_overlap(lo,hi-lo,c->memory_offset,hw_color_bytes(c))) c->upload_needed=1;
    hw_protect(memory,offset,bytes,PAGE_READWRITE,completed);
}
static void hw_initialize(void)
{
    if(s_hw_initialized) return;
    HRESULT initialized=d3d8_InitializeOffscreenDevice(640,480);
    if(FAILED(initialized) && getenv("RECOMP_GPU_ERROR_DIALOG")) {
        wchar_t message[512];
        swprintf_s(message,512,L"\u663e\u5361\u521d\u59cb\u5316\u5931\u8d25 (0x%08lX)\u3002\u8bf7\u67e5\u770b runtime/startup.log\uff0c\u5e76\u5728 Config.exe \u68c0\u67e5\u56fe\u5f62\u540e\u7aef\u3002\nGraphics initialization failed. See runtime/startup.log and the graphics backend in Config.exe.",(unsigned long)initialized);
        MessageBoxW(NULL,message,L"NINE",MB_OK|MB_ICONERROR);
    }
    hw_check(initialized,"device initialization");
    D3D11_BUFFER_DESC cb={0};
    cb.ByteWidth=sizeof(HwPixelState); cb.Usage=D3D11_USAGE_DYNAMIC; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    hw_check(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(),&cb,NULL,&s_hw_ps_cb),"native pixel constants");
    s_hw_ps_value_valid=0;
    s_hw_initialized=1;
}
static void hw_upload_native_color(HwColor *color)
{
    if(hw_pending_output(color->memory,color->memory_offset,hw_color_bytes(color))) hw_collect_queries(1);
    hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_READONLY,NULL);
    const uint8_t *memory=color->memory->host_view+color->memory_offset;
    if(color_swizzled(color->format)) {
        D3DLOCKED_RECT locked;
        hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,0),"swizzled colour upload lock");
        /* Same masked Morton walk as xbox_unswizzle_rect, with locked row pitch. */
        uint32_t mask_x,mask_y,off_y=0;
        xbox_swizzle_masks(color_width(color->format,color->pitch,color->bpp),color_height(color->format,0),&mask_x,&mask_y);
        for(uint32_t y=0;y<color->height;y++) {
            uint32_t off_x=0;
            uint32_t *row=(uint32_t *)((uint8_t *)locked.pBits+(size_t)y*locked.Pitch);
            for(uint32_t x=0;x<color->width;x++) {
                row[x]=*(const uint32_t *)(memory+(size_t)(off_x|off_y)*color->bpp);
                off_x=(off_x-mask_x)&mask_x;
            }
            off_y=(off_y-mask_y)&mask_y;
        }
        hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"swizzled colour upload unlock");
    } else ID3D11DeviceContext_UpdateSubresource(d3d8_GetD3D11Context(),
        (ID3D11Resource *)((D3D8Surface *)color->surface)->d3d11_texture,0,NULL,memory,color->pitch,0);
    color->upload_needed=0;++s_gpu.hardware_color_uploads;
}
static void hw_upload_color(HwColor *color,uint32_t cleared_channels)
{
    ClearBounds bounds=clear_bounds();
    if((cleared_channels&NV097_CLEAR_COLOR_MASK)==NV097_CLEAR_COLOR_MASK &&
       !bounds.x0 && !bounds.y0 && bounds.x1==color->width && bounds.y1==color->height) {
        /* The original CPU clear has already written every target byte. */
        if(hw_pending_output(color->memory,color->memory_offset,hw_color_bytes(color))) hw_collect_queries(1);
        hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_READONLY,NULL);
        uint32_t value=s_gpu.clear_color;
        if((color->format&15)==NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) value|=0xff000000u;
        const float rgba[4]={((value>>16)&255)/255.0f,((value>>8)&255)/255.0f,(value&255)/255.0f,(value>>24)/255.0f};
        ID3D11DeviceContext *context=d3d8_GetD3D11Context();
        ID3D11DeviceContext_ClearRenderTargetView(context,((D3D8Surface *)color->surface)->rtv,rgba);
        if(color->host_surface) ID3D11DeviceContext_ClearRenderTargetView(context,((D3D8Surface *)color->host_surface)->rtv,rgba);
        color->upload_needed=0;
    } else {
        hw_upload_native_color(color);
        if(color->host_surface)hw_check(d3d8_surface_resample(color->surface,color->host_surface),"scaled colour upload");
    }
}
static IDirect3DSurface8 *hw_render_surface(HwColor *color) { return color->host_surface?color->host_surface:color->surface; }
static void hw_copy_color_to_memory(HwColor *color)
{
    if(!color->dirty) return;
    uint64_t timing_start=gpu_timing_clock();D3DLOCKED_RECT locked;
    if(color->host_surface)hw_check(d3d8_surface_resample(color->host_surface,color->surface),"guest colour resolve");
    const RECT *bounds=&color->dirty_bounds;
    hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,bounds,D3DLOCK_READONLY),"surface readback");
    uint8_t *mem=color->memory->host_view+color->memory_offset;
    size_t row_bytes=(size_t)(bounds->right-bounds->left)*color->bpp;
    /* Seed the existing masked walk once at this dirty rectangle's origin. */
    uint32_t mask_x=0,mask_y=0,first_x=0,off_y=0;
    if(color_swizzled(color->format)) {
        xbox_swizzle_masks(color_width(color->format,color->pitch,color->bpp),color_height(color->format,0),&mask_x,&mask_y);
        first_x=swizzle_deposit((uint32_t)bounds->left,mask_x);
        off_y=swizzle_deposit((uint32_t)bounds->top,mask_y);
    }
    for(LONG y=bounds->top;y<bounds->bottom;y++) {
        const uint8_t *row=(uint8_t *)locked.pBits+(size_t)(y-bounds->top)*locked.Pitch;
        if(color_swizzled(color->format)) {
            uint32_t off_x=first_x;
            LONG x=bounds->left;
            if((x&1) && x<bounds->right) {
                *(uint32_t *)(mem+(size_t)(off_x|off_y)*color->bpp)=
                    ((const uint32_t *)row)[x-bounds->left];
                off_x=(off_x-mask_x)&mask_x;
                ++x;
            }
            /* The original Morton mask assigns bit zero to X whenever a
             * row has a pair. Copy both raw pixels before advancing X by two. */
            uint32_t pair_mask=mask_x&~1u;
            for(;x+1<bounds->right;x+=2) {
                memcpy(mem+(size_t)(off_x|off_y)*color->bpp,
                       row+(size_t)(x-bounds->left)*color->bpp,8);
                off_x=(off_x-pair_mask)&pair_mask;
            }
            if(x<bounds->right)
                *(uint32_t *)(mem+(size_t)(off_x|off_y)*color->bpp)=
                    ((const uint32_t *)row)[x-bounds->left];
            off_y=(off_y-mask_y)&mask_y;
        } else {
            size_t at=(size_t)y*color->pitch+(size_t)bounds->left*color->bpp;
            memcpy(mem+at,row,row_bytes);
        }
    }
    hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"surface unlock");
    s_gpu.hardware_color_readback_bytes+=row_bytes*(size_t)(bounds->bottom-bounds->top);
    color->dirty=0;++s_gpu.hardware_color_readbacks;gpu_timing_finish(timing_start,&s_gpu.hardware_publish_ticks);
}
static void hw_publish_all(void)
{
    if(!hw_enabled() || !s_hw_initialized) return;
    hw_enter();
    int protection_needed=hw_pending_output(NULL,0,0);
    if(protection_needed) hw_collect_queries(1);
    for(HwColor *color=s_hw_colors;color;color=color->next) {
        if(color->dirty || color->upload_needed) protection_needed=1;
        hw_copy_color_to_memory(color);
    }
    if(protection_needed) for(HwColor *color=s_hw_colors;color;color=color->next) if(color->height) hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),color->upload_needed?PAGE_READWRITE:PAGE_READONLY,NULL);
    hw_leave();
}
static void hw_cpu_write_begin(uint32_t address,size_t bytes)
{
    if(!s_hw_initialized) return;
    hw_enter();hw_publish_all();
    uintptr_t p=(uintptr_t)xbox_GetMemoryOffset()+address;
    for(HwMemory *m=s_hw_memories;m;m=m->next) for(unsigned i=0;i<m->views.view_count;i++) {
        uintptr_t b=(uintptr_t)m->views.cpu_views[i];
        if(p>=b && p-b<m->views.view_size && bytes<=m->views.view_size-(p-b)) { hw_mark_cpu_write(m,p-b,bytes,NULL);hw_leave();return; }
    }
    hw_leave();
}
int nv2a_pb_handle_memory_fault(uintptr_t address,int is_write)
{
    hw_enter();
    if(!s_hw_initialized || !hw_enabled()) {hw_leave();return 0;}
    HwColor *owner=NULL;HwMemory *memory=NULL;size_t offset=0;
    for(HwColor *c=s_hw_colors;c && !owner;c=c->next) {
        size_t lo=c->memory_offset&~(size_t)4095,hi=(c->memory_offset+hw_color_bytes(c)+4095)&~(size_t)4095;
        for(unsigned i=0;i<c->memory->views.view_count;i++) {
            uintptr_t b=(uintptr_t)c->memory->views.cpu_views[i];
            if(address>=b+lo && address<b+hi) { owner=c;offset=address-b;break; }
        }
    }
    if(owner) memory=owner->memory;
    for(size_t n=0;n<s_hw_query_count && !memory;n++) {
        HwQuery *q=&s_hw_queries[n];if(!q->memory) continue;
        size_t lo=q->offset&~(size_t)4095,hi=(q->offset+4+4095)&~(size_t)4095;
        for(unsigned i=0;i<q->memory->views.view_count;i++) {
            uintptr_t b=(uintptr_t)q->memory->views.cpu_views[i];
            if(address>=b+lo && address<b+hi) {memory=q->memory;offset=address-b;break;}
        }
    }
    if(!memory) {
        /* Another CPU access may have collected the output while this fault
         * was waiting for the original memory lock. Recheck the owned view. */
        for(HwMemory *m=s_hw_memories;m;m=m->next) for(unsigned i=0;i<m->views.view_count;i++) {
            uintptr_t b=(uintptr_t)m->views.cpu_views[i];
            if(address>=b && address-b<m->views.view_size) {
                MEMORY_BASIC_INFORMATION page;
                if(VirtualQuery((void *)address,&page,sizeof page) &&
                   (page.Protect==PAGE_READWRITE || (!is_write && page.Protect==PAGE_READONLY))) {hw_leave();return 1;}
            }
        }
        hw_leave();return 0;
    }
    uint64_t started=gpu_timing_clock();
    if(owner) hw_publish_all();else hw_collect_queries(1);
    if(is_write) {
        if(owner && offset>=owner->memory_offset && offset<owner->memory_offset+hw_color_bytes(owner)) hw_mark_cpu_write(owner->memory,owner->memory_offset,hw_color_bytes(owner),NULL);
        else hw_mark_cpu_write(memory,offset&~(size_t)4095,4096,NULL);
        ++s_gpu.hardware_cpu_write_faults;
    } else ++s_gpu.hardware_cpu_read_faults;
    gpu_timing_finish(started,&s_gpu.hardware_cpu_access_ticks);hw_leave();return 1;
}
void nv2a_pb_release_memory_views(void)
{
    if(!s_hw_initialized) return;
    hw_enter();hw_publish_all();
    for(HwColor *c=s_hw_colors;c;) {
        HwColor *next=c->next;if(c->height) hw_protect(c->memory,c->memory_offset,hw_color_bytes(c),PAGE_READWRITE,NULL);
        if(c->host_surface)c->host_surface->lpVtbl->Release(c->host_surface);
        if(c->surface) c->surface->lpVtbl->Release(c->surface);free(c);c=next;
    }
    s_hw_colors=NULL;
    for(HwMemory *m=s_hw_memories;m;) { HwMemory *next=m->next;UnmapViewOfFile(m->host_view);free(m);m=next; }
    s_hw_memories=NULL;
    if(s_hw_clear_vs) { ID3D11VertexShader_Release(s_hw_clear_vs);s_hw_clear_vs=NULL; }
    if(s_hw_clear_cb) { ID3D11Buffer_Release(s_hw_clear_cb);s_hw_clear_cb=NULL; }
    if(s_hw_clear_rs) { ID3D11RasterizerState_Release(s_hw_clear_rs);s_hw_clear_rs=NULL; }
    for(unsigned i=0;i<4;i++) if(s_hw_clear_ds[i]) { ID3D11DepthStencilState_Release(s_hw_clear_ds[i]);s_hw_clear_ds[i]=NULL; }
    hw_leave();
}
static HwColor *hw_color(uint32_t cleared_channels)
{
    hw_initialize();
    uint32_t bpp=surface_bpp(),width=color_width(s_gpu.format,s_gpu.pitch,bpp),height=color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
    uint32_t pitch=color_pitch(s_gpu.format,s_gpu.pitch,bpp);
    uint32_t address=color_dma_resolve(s_gpu.color_offset,color_extent(s_gpu.format,s_gpu.pitch,bpp,s_gpu.clip_y+s_gpu.clip_h));
    unsigned format=s_gpu.format&15;
    if(bpp!=4 || !width || !height ||
       (uint64_t)s_gpu.clip_x+s_gpu.clip_w>width || (uint64_t)s_gpu.clip_y+s_gpu.clip_h>height ||
       (format!=NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 && format!=NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8)) {
        fprintf(stderr,"[COLOUR TARGET] address=%08X format=%08X bpp=%u pitch=%u size=%ux%u clip=%u,%u,%u,%u xf_mode=%08X program=%d pending=%u mask=%08X blend=%u,%08X,%08X\n",address,s_gpu.format,bpp,s_gpu.pitch,width,height,s_gpu.clip_x,s_gpu.clip_y,s_gpu.clip_w,s_gpu.clip_h,s_gpu.xf_mode,s_hw_program_active,s_hw_count,s_gpu.color_mask,s_gpu.blend_enable,s_gpu.blend_sfactor,s_gpu.blend_dfactor);
        hw_check(E_NOTIMPL,"colour surface format");
    }
    HwColor *color;
    for(color=s_hw_colors;color;color=color->next) if(color->address==address && color->pitch==pitch) break;
    if(!color) {
        XboxSharedMemoryViews views;
        if(!xbox_GetSharedMemoryViews((uint8_t *)xbox_GetMemoryOffset()+address,(size_t)height*pitch,&views)) hw_check(E_NOTIMPL,"physical colour shared views");
        HwMemory *memory=hw_memory(&views);
        for(color=s_hw_colors;color;color=color->next) if(color->memory==memory && color->memory_offset==views.offset && color->pitch==pitch) break;
        if(!color) {
            hw_publish_all();color=(HwColor *)calloc(1,sizeof *color);if(!color) hw_check(E_OUTOFMEMORY,"colour identity allocation");
            color->address=address;color->pitch=pitch;color->bpp=bpp;color->memory=memory;color->memory_offset=views.offset;color->upload_needed=1;
            color->next=s_hw_colors;s_hw_colors=color;
        }
    }
    if(hw_pending_output(color->memory,color->memory_offset,(size_t)height*pitch)) hw_collect_queries(1);
    if((color->format^s_gpu.format)&0xFFFF0F00u) {
        hw_publish_all();color->upload_needed=1;
    }
    color->format=s_gpu.format;
    for(HwColor *other=s_hw_colors;other;other=other->next) if(other!=color && other->memory==color->memory && other->dirty && hw_ranges_overlap(other->memory_offset,hw_color_bytes(other),color->memory_offset,(size_t)height*pitch)) { hw_publish_all();break; }
    if(color->height<height || color->width!=width) {
        hw_publish_all();if(color->host_surface){color->host_surface->lpVtbl->Release(color->host_surface);color->host_surface=NULL;}
        if(color->surface) color->surface->lpVtbl->Release(color->surface);
        color->width=width;color->height=height;
        if(color->memory_offset>color->memory->views.view_size || hw_color_bytes(color)>color->memory->views.view_size-color->memory_offset) hw_check(E_INVALIDARG,"physical colour shared extent");
        IDirect3DDevice8 *device=d3d8_GetDevice();
        hw_check(device->lpVtbl->CreateRenderTarget(device,width,height,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->surface),"physical render target");
        unsigned scale=d3d8_RenderScale();
        if(scale>1) {
            if(width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale)hw_check(E_INVALIDARG,"scaled colour extent");
            hw_check(device->lpVtbl->CreateRenderTarget(device,width*scale,height*scale,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->host_surface),"scaled render target");
            fprintf(stderr,"[GPU RESOLUTION] colour %08X guest=%ux%u host=%ux%u\n",address,width,height,width*scale,height*scale);
        }
        hw_upload_color(color,cleared_channels);
    } else if(color->upload_needed) hw_upload_color(color,cleared_channels);
    return color;
}
static void hw_complete_commands(void)
{
    if(!s_hw_initialized) return;
    hw_enter();uint64_t start=gpu_timing_clock();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    if(!s_hw_completion) { D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&s_hw_completion),"command completion event"); }
    ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous *)s_hw_completion);
    HRESULT hr;
    do { hr=ID3D11DeviceContext_GetData(ctx,(ID3D11Asynchronous *)s_hw_completion,NULL,0,0);if(hr==S_FALSE) SwitchToThread(); } while(hr==S_FALSE);
    hw_check(hr,"command completion result");++s_gpu.hardware_gpu_completions;gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);hw_leave();
}
static HwDepth *hw_depth(void)
{
    hw_initialize();
    uint32_t width=s_gpu.pitch/surface_bpp(),height=s_gpu.clip_y+s_gpu.clip_h;
    uint32_t format=(s_gpu.format>>4)&15;
    uint32_t address=dma_resolve(nv2a_get_state()->pgraph.dma_zeta,s_gpu.zeta_offset,1);
    if(format!=1 && format!=2) hw_check(E_NOTIMPL,"depth surface format");
    HwDepth *depth;
    for(depth=s_hw_depths;depth;depth=depth->next) if(depth->address==address) break;
    if(!depth) {
        depth=(HwDepth *)calloc(1,sizeof *depth);
        if(!depth) hw_check(E_OUTOFMEMORY,"depth identity allocation");
        depth->address=address; depth->next=s_hw_depths; s_hw_depths=depth;
    }
    if(!depth->texture) {
        depth->width=width; depth->height=height; depth->format=format;
        D3D11_TEXTURE2D_DESC desc={0};
        desc.Width=width*d3d8_RenderScale(); desc.Height=height*d3d8_RenderScale(); desc.MipLevels=desc.ArraySize=1; desc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        hw_check(ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(),&desc,NULL,&depth->texture),"physical depth target");
        hw_check(ID3D11Device_CreateDepthStencilView(d3d8_GetD3D11Device(),(ID3D11Resource *)depth->texture,NULL,&depth->view),"depth view");
        ID3D11DeviceContext_ClearDepthStencilView(d3d8_GetD3D11Context(),depth->view,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    }
    if(depth->width<width || depth->height<height || depth->format!=format) {
        fprintf(stderr,"[DEPTH IDENTITY] address=%08X existing=%ux%u format=%u requested=%ux%u format=%u colour_pitch=%u colour_format=%08X clip=%u,%u,%u,%u\n",address,depth->width,depth->height,depth->format,width,height,format,s_gpu.pitch,s_gpu.format,s_gpu.clip_x,s_gpu.clip_y,s_gpu.clip_w,s_gpu.clip_h);
        hw_check(E_NOTIMPL,"depth identity resize");
    }
    return depth;
}
#include "nv2a_scaled_checkpoint.inc"
typedef struct HwDepthCheckpoint { uint32_t address,width,height,format,scale;uint64_t bytes; } HwDepthCheckpoint;
static int hw_save_depths(FILE *out) {
    uint32_t count=0;for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->texture) count++;
    if(fwrite(&count,4,1,out)!=1) return 0;
    ID3D11Device *device=d3d8_GetD3D11Device();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->texture) {
        unsigned scale=d3d8_RenderScale();
        HwDepthCheckpoint header={d->address,d->width,d->height,d->format,scale,(uint64_t)d->width*d->height*scale*scale*8};
        D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(d->texture,&desc);
        desc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D *staging=NULL;
        HRESULT result=ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging);if(FAILED(result)) return 0;
        ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)d->texture);
        D3D11_MAPPED_SUBRESOURCE mapped;result=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped);
        int ok=SUCCEEDED(result) && fwrite(&header,sizeof header,1,out)==1;
        if(SUCCEEDED(result)) {
            for(uint32_t y=0;ok && y<d->height*scale;y++) ok=fwrite((const char *)mapped.pData+(size_t)y*mapped.RowPitch,d->width*scale*8,1,out)==1;
            ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);
        }
        ID3D11Texture2D_Release(staging);if(!ok) return 0;
    }
    return hw_save_scaled_colors(out);
}
static void hw_free_depths(HwDepth *depth) {
    while(depth) { HwDepth *next=depth->next;if(depth->view)ID3D11DepthStencilView_Release(depth->view);
        if(depth->texture)ID3D11Texture2D_Release(depth->texture);free(depth);depth=next; }
}
static int hw_prepare_depths(FILE *in,HwDepth **result,HwColor **colors) {
    *colors=NULL;*result=NULL;uint32_t count;if(fread(&count,4,1,in)!=1) return 0;
    if(count) hw_initialize();
    HwDepth **next=result;
    for(uint32_t i=0;i<count;i++) {
        HwDepthCheckpoint header;
        if(fread(&header,sizeof header,1,in)!=1 || !header.width || !header.height ||
            header.width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || header.height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            (header.format!=1 && header.format!=2) || header.scale!=d3d8_RenderScale() || header.bytes!=(uint64_t)header.width*header.height*header.scale*header.scale*8 || header.bytes>SIZE_MAX) goto failed;
        void *bytes=malloc((size_t)header.bytes);if(!bytes) goto failed;
        if(fread(bytes,(size_t)header.bytes,1,in)!=1) {free(bytes);goto failed;}
        HwDepth *d=calloc(1,sizeof *d);if(!d){free(bytes);goto failed;}
        *next=d;next=&d->next;d->address=header.address;d->width=header.width;d->height=header.height;d->format=header.format;
        D3D11_TEXTURE2D_DESC desc={0};desc.Width=d->width*header.scale;desc.Height=d->height*header.scale;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_SUBRESOURCE_DATA data={bytes,d->width*header.scale*8,0};
        HRESULT hr=ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(),&desc,&data,&d->texture);free(bytes);
        if(FAILED(hr) || FAILED(ID3D11Device_CreateDepthStencilView(d3d8_GetD3D11Device(),(ID3D11Resource *)d->texture,NULL,&d->view))) goto failed;
    }
    if(!hw_prepare_scaled_colors(in,colors))goto failed;
    return 1;
failed:hw_free_colors(*colors);*colors=NULL;hw_free_depths(*result);*result=NULL;return 0;
}
static void hw_apply_depths(HwDepth *depths,HwColor *colors) {
    if(s_hw_initialized) ID3D11DeviceContext_OMSetRenderTargets(d3d8_GetD3D11Context(),0,NULL,NULL);
    hw_free_depths(s_hw_depths);s_hw_depths=depths;
    for(HwColor *color=s_hw_colors;color;color=color->next) { color->dirty=0;color->upload_needed=1;
        if(color->height)hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_READWRITE,NULL); }
    if(d3d8_RenderScale()>1) {
        hw_free_colors(s_hw_colors);s_hw_colors=colors;
        for(HwColor *color=s_hw_colors;color;color=color->next)hw_upload_native_color(color);
    }
    while(s_hw_textures) {
        HwTexture *texture=s_hw_textures;s_hw_textures=texture->next;
        if(texture->texture)texture->texture->lpVtbl->Release(texture->texture);free(texture->raw);free(texture);
    }
    s_hw_count=0;s_hw_program_active=0;s_hw_fixed_active=0;
}
static void hw_clear_depth_rect(HwDepth *depth,ClearBounds bounds,UINT flags,float z,UINT8 stencil)
{
    ID3D11Device *device=d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    if(!s_hw_clear_vs) {
        static const char source[]="cbuffer ClearCB:register(b0){float4 value;} float4 main(uint id:SV_VertexID):SV_POSITION {float2 p=id==0?float2(-1,-1):(id==1?float2(-1,3):float2(3,-1));return float4(p,value.x,1);}";
        ID3DBlob *code=NULL,*errors=NULL;
        HRESULT hr=D3DCompile(source,strlen(source),"nv2a_depth_clear",NULL,NULL,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr) && errors) fprintf(stderr,"Depth clear: %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
        if(errors) ID3D10Blob_Release(errors);
        hw_check(hr,"depth clear shader compilation");
        hw_check(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&s_hw_clear_vs),"depth clear vertex shader");
        ID3D10Blob_Release(code);
        D3D11_BUFFER_DESC cb={0};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        hw_check(ID3D11Device_CreateBuffer(device,&cb,NULL,&s_hw_clear_cb),"depth clear constants");
        D3D11_RASTERIZER_DESC rs={0};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;rs.ScissorEnable=TRUE;
        hw_check(ID3D11Device_CreateRasterizerState(device,&rs,&s_hw_clear_rs),"depth clear raster state");
    }
    if(!s_hw_clear_ds[flags]) {
        D3D11_DEPTH_STENCIL_DESC ds={0};
        ds.DepthEnable=(flags&D3D11_CLEAR_DEPTH)!=0;ds.DepthWriteMask=ds.DepthEnable?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        ds.StencilEnable=(flags&D3D11_CLEAR_STENCIL)!=0;ds.StencilReadMask=ds.StencilWriteMask=0xFF;
        ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.BackFace=ds.FrontFace;
        hw_check(ID3D11Device_CreateDepthStencilState(device,&ds,&s_hw_clear_ds[flags]),"depth clear write mask");
    }
    ID3D11RenderTargetView *old_rt[8]={0};ID3D11DepthStencilView *old_dsv=NULL;
    ID3D11DepthStencilState *old_ds=NULL;UINT old_ref;
    ID3D11BlendState *old_blend=NULL;FLOAT old_factor[4];UINT old_sample_mask;
    ID3D11RasterizerState *old_rs=NULL;
    ID3D11VertexShader *old_vs=NULL;ID3D11GeometryShader *old_gs=NULL;ID3D11PixelShader *old_ps=NULL;
    ID3D11Buffer *old_cb=NULL;ID3D11InputLayout *old_layout=NULL;D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT old_vp[16];UINT old_vp_count=16;
    D3D11_RECT old_rect[16];UINT old_rect_count=16;
    ID3D11DeviceContext_OMGetRenderTargets(ctx,8,old_rt,&old_dsv);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx,&old_ds,&old_ref);
    ID3D11DeviceContext_OMGetBlendState(ctx,&old_blend,old_factor,&old_sample_mask);
    ID3D11DeviceContext_RSGetState(ctx,&old_rs);
    ID3D11DeviceContext_RSGetViewports(ctx,&old_vp_count,old_vp);
    ID3D11DeviceContext_RSGetScissorRects(ctx,&old_rect_count,old_rect);
    ID3D11DeviceContext_VSGetShader(ctx,&old_vs,NULL,NULL);
    ID3D11DeviceContext_GSGetShader(ctx,&old_gs,NULL,NULL);
    ID3D11DeviceContext_PSGetShader(ctx,&old_ps,NULL,NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(ctx,0,1,&old_cb);
    ID3D11DeviceContext_IAGetInputLayout(ctx,&old_layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx,&old_topology);
    unsigned scale=d3d8_RenderScale();
    float value[4]={z,0,0,0};D3D11_VIEWPORT viewport={0,0,(float)(depth->width*scale),(float)(depth->height*scale),0,1};
    D3D11_RECT rect={(LONG)(bounds.x0*scale),(LONG)(bounds.y0*scale),(LONG)(bounds.x1*scale),(LONG)(bounds.y1*scale)};
    ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource *)s_hw_clear_cb,0,NULL,value,0,0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,depth->view);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,s_hw_clear_ds[flags],stencil);
    ID3D11DeviceContext_OMSetBlendState(ctx,NULL,NULL,0xFFFFFFFF);
    ID3D11DeviceContext_RSSetState(ctx,s_hw_clear_rs);
    ID3D11DeviceContext_RSSetViewports(ctx,1,&viewport);
    ID3D11DeviceContext_RSSetScissorRects(ctx,1,&rect);
    ID3D11DeviceContext_VSSetShader(ctx,s_hw_clear_vs,NULL,0);
    ID3D11DeviceContext_GSSetShader(ctx,NULL,NULL,0);
    ID3D11DeviceContext_PSSetShader(ctx,NULL,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&s_hw_clear_cb);
    ID3D11DeviceContext_IASetInputLayout(ctx,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_Draw(ctx,3,0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx,8,old_rt,old_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,old_ds,old_ref);
    ID3D11DeviceContext_OMSetBlendState(ctx,old_blend,old_factor,old_sample_mask);
    ID3D11DeviceContext_RSSetState(ctx,old_rs);
    ID3D11DeviceContext_RSSetViewports(ctx,old_vp_count,old_vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx,old_rect_count,old_rect);
    ID3D11DeviceContext_VSSetShader(ctx,old_vs,NULL,0);
    ID3D11DeviceContext_GSSetShader(ctx,old_gs,NULL,0);
    ID3D11DeviceContext_PSSetShader(ctx,old_ps,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&old_cb);
    ID3D11DeviceContext_IASetInputLayout(ctx,old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,old_topology);
    for(unsigned i=0;i<8;i++) if(old_rt[i]) ID3D11RenderTargetView_Release(old_rt[i]);
    if(old_dsv) ID3D11DepthStencilView_Release(old_dsv);
    if(old_ds) ID3D11DepthStencilState_Release(old_ds);
    if(old_blend) ID3D11BlendState_Release(old_blend);
    if(old_rs) ID3D11RasterizerState_Release(old_rs);
    if(old_vs) ID3D11VertexShader_Release(old_vs);
    if(old_gs) ID3D11GeometryShader_Release(old_gs);
    if(old_ps) ID3D11PixelShader_Release(old_ps);
    if(old_cb) ID3D11Buffer_Release(old_cb);
    if(old_layout) ID3D11InputLayout_Release(old_layout);
}
static void hw_clear_depth(uint32_t param)
{
    hw_enter();
    ClearBounds bounds=clear_bounds();
    if(bounds.x0>=bounds.x1 || bounds.y0>=bounds.y1) { hw_leave();return; }
    HwDepth *depth=hw_depth();
    if(bounds.x1>depth->width || bounds.y1>depth->height) hw_check(E_INVALIDARG,"depth clear extent");
    UINT flags=((param&NV097_CLEAR_SURFACE_Z)?D3D11_CLEAR_DEPTH:0)|((param&NV097_CLEAR_SURFACE_STENCIL)?D3D11_CLEAR_STENCIL:0);
    float scale=depth->format==1?65535.0f:16777215.0f;
    if(!bounds.x0 && !bounds.y0 && bounds.x1==depth->width && bounds.y1==depth->height)
        ID3D11DeviceContext_ClearDepthStencilView(d3d8_GetD3D11Context(),depth->view,flags,zclear_value()/scale,(UINT8)s_gpu.zstencil_clear);
    else {
        static unsigned log_count;
        if(log_count++<8) fprintf(stderr,"[DEPTH RECT] address=%08X size=%ux%u bounds=%u,%u,%u,%u flags=%u value=%08X\n",depth->address,depth->width,depth->height,bounds.x0,bounds.y0,bounds.x1,bounds.y1,flags,s_gpu.zstencil_clear);
        hw_clear_depth_rect(depth,bounds,flags,zclear_value()/scale,(UINT8)s_gpu.zstencil_clear);
    }
    hw_leave();
}
static void hw_colour_cleared(uint32_t param)
{
    if(!s_hw_initialized) return;
    hw_enter();(void)hw_color(param);hw_leave();
}
static void hw_texture_bind(unsigned stage)
{
    Texture *source=&s_gpu.texs[stage];
    unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
    ID3D11DeviceContext *context=d3d8_GetD3D11Context();
    if(mode==0 || mode>=4) {
        ID3D11ShaderResourceView *empty=NULL;
        ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&empty);
        return;
    }
    if(!source->valid) hw_check(E_NOTIMPL,"enabled texture identity");
    uint32_t stride=tex_face_stride(source);
    size_t bytes=(size_t)stride*(source->cube?6:1);
    if(!bytes) hw_check(E_FAIL,"texture storage span");
    for(HwColor *color=s_hw_colors;color;color=color->next) {
        if(!source->cube && !tex_size_from_format(source->color) && color->bpp==4 &&
           source->offset==color->address && source->pitch==color->pitch &&
           source->width==color->width && source->height==color->height) {
            D3D8Surface *surface=(D3D8Surface *)hw_render_surface(color);
            if(color==hw_color(0))continue;
            if(!surface->scale_srv)hw_check(ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),(ID3D11Resource *)surface->d3d11_texture,NULL,&surface->scale_srv),"render target texture view");
            ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&surface->scale_srv);
            static unsigned reported;
            if(reported++<8)fprintf(stderr,"[GPU TARGET TEXTURE] original=%08X native=%ux%u host=%ux%u\n",color->address,color->width,color->height,surface->width,surface->height);
            return;
        }
    }
    for(HwColor *color=s_hw_colors;color;color=color->next)
        if(source->offset < color->address+color->height*color->pitch && color->address < source->offset+bytes &&
           (color->dirty || hw_pending_output(NULL,0,0))) hw_publish_all();
    HwTexture *entry;
    for(entry=s_hw_textures;entry;entry=entry->next)
        if(entry->identity.offset==source->offset && entry->identity.width==source->width && entry->identity.height==source->height && entry->identity.color==source->color && entry->identity.pitch==source->pitch && entry->identity.cube==source->cube && entry->identity.levels==source->levels && entry->raw_size==bytes) break;
    const uint8_t *raw=(uint8_t *)xbox_GetMemoryOffset()+source->offset;
    const uint32_t *palette=source->color==0x0B ? (uint32_t *)((uint8_t *)xbox_GetMemoryOffset()+source->palette) : NULL;
    int changed;
    if(!entry) {
        entry=(HwTexture *)calloc(1,sizeof *entry);
        if(!entry) hw_check(E_OUTOFMEMORY,"texture identity allocation");
        entry->identity=*source; entry->raw_size=bytes; entry->raw=(uint8_t *)malloc(bytes);
        if(!entry->raw) hw_check(E_OUTOFMEMORY,"texture publication storage");
        entry->next=s_hw_textures; s_hw_textures=entry; changed=1;
    } else changed=memcmp(entry->raw,raw,bytes)!=0 || (palette && memcmp(entry->palette,palette,1024)!=0);
    if(changed) {
        unsigned width=source->width,height=source->height;
        const void *asset=source->cube?NULL:d3d8_TextureAsset(raw,bytes,width,height,&width,&height);
        if(!entry->texture || width!=entry->host_width || height!=entry->host_height) {
            if(entry->texture)entry->texture->lpVtbl->Release(entry->texture);
            entry->texture=NULL;
            if(source->cube) {
                if(width!=height)hw_check(E_FAIL,"cube dimensions");
                hw_check(d3d8_CreateCubeTextureImpl(width,1,0,D3DFMT_LIN_A8R8G8B8,(IDirect3DCubeTexture8 **)&entry->texture),"cube texture");
            } else hw_check(d3d8_CreateTextureImpl(width,height,1,0,D3DFMT_LIN_A8R8G8B8,(IDirect3DTexture8 **)&entry->texture),"texture");
            entry->host_width=width;entry->host_height=height;
        }
        ++s_gpu.hardware_texture_uploads;
        for(unsigned face=0;face<(source->cube?6u:1u);face++) {
            D3DLOCKED_RECT locked;
            if(source->cube) {
                IDirect3DCubeTexture8 *cube=(IDirect3DCubeTexture8 *)entry->texture;
                hw_check(cube->lpVtbl->LockRect(cube,(D3DCUBEMAP_FACES)face,0,&locked,NULL,0),"cube upload lock");
            } else {
                IDirect3DTexture8 *texture=(IDirect3DTexture8 *)entry->texture;
                hw_check(texture->lpVtbl->LockRect(texture,0,&locked,NULL,0),"texture upload lock");
            }
            if(asset) {
                for(unsigned y=0;y<height;y++)memcpy((uint8_t *)locked.pBits+(size_t)y*locked.Pitch,(const uint8_t *)asset+(size_t)y*width*4,(size_t)width*4);
            } else for(uint32_t y=0;y<source->height;y++) for(uint32_t x=0;x<source->width;x++) {
                uint32_t pixel;
                if(!sample_tex(source,face*stride,x,y,&pixel)) hw_check(E_NOTIMPL,"original texture decoder");
                ((uint32_t *)((uint8_t *)locked.pBits+(size_t)y*locked.Pitch))[x]=pixel;
            }
            if(source->cube) { IDirect3DCubeTexture8 *cube=(IDirect3DCubeTexture8 *)entry->texture; hw_check(cube->lpVtbl->UnlockRect(cube,(D3DCUBEMAP_FACES)face,0),"cube upload"); }
            else { IDirect3DTexture8 *texture=(IDirect3DTexture8 *)entry->texture; hw_check(texture->lpVtbl->UnlockRect(texture,0),"texture upload"); }
        }
        memcpy(entry->raw,raw,bytes);
        if(palette) memcpy(entry->palette,palette,1024);
    }
    ID3D11ShaderResourceView *view=d3d8_base_srv(entry->texture);
    ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&view);
}
static DWORD hw_blend_enum(uint32_t value)
{
    switch(value) {
    case 0: return D3DBLEND_ZERO; case 1:return D3DBLEND_ONE;
    case 0x300:return D3DBLEND_SRCCOLOR; case 0x301:return D3DBLEND_INVSRCCOLOR;
    case 0x302:return D3DBLEND_SRCALPHA; case 0x303:return D3DBLEND_INVSRCALPHA;
    case 0x304:return D3DBLEND_DESTALPHA; case 0x305:return D3DBLEND_INVDESTALPHA;
    case 0x306:return D3DBLEND_DESTCOLOR; case 0x307:return D3DBLEND_INVDESTCOLOR;
    case 0x308:return D3DBLEND_SRCALPHASAT;
    default:hw_check(E_NOTIMPL,"blend factor");return 0;
    }
}
static void hw_collect_queries(int wait)
{
    hw_enter();size_t consumed=0,count=(size_t)s_hw_query_count;
    for(;consumed<count;consumed++) {
        HwQuery *q=&s_hw_queries[consumed];uint64_t pixels=0;HRESULT hr;
        uint64_t start=q->memory?gpu_timing_clock():0;
        do {
            ID3D11DeviceContext *context=d3d8_GetD3D11Context();
            hr=ID3D11DeviceContext_GetData(context,(ID3D11Asynchronous *)q->query,
                q->memory?NULL:(void *)&pixels,q->memory?0:sizeof pixels,0);
            if(hr!=S_FALSE) break;
            if(!wait) break;
            SwitchToThread();
        } while(1);
        hw_check(hr,"query output result");
        if(q->memory) gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);
        if(hr==S_FALSE) break;
        if(q->memory && !s_hw_completion) s_hw_completion=q->query;
        else ID3D11Query_Release(q->query);
        q->query=NULL;
        if(q->memory) {
            ++s_gpu.hardware_gpu_completions;
            for(HwColor *c=s_hw_colors;c;c=c->next)
                if(c->memory==q->memory && hw_ranges_overlap(c->memory_offset,hw_color_bytes(c),q->offset,4)) hw_copy_color_to_memory(c);
            memcpy(q->memory->host_view+q->offset,&q->value,4);
            hw_mark_cpu_write(q->memory,q->offset,4,q);
        } else s_gpu.zpass_count+=(uint32_t)pixels;
    }
    if(consumed) {
        memmove(s_hw_queries,s_hw_queries+consumed,(count-consumed)*sizeof *s_hw_queries);
        InterlockedExchange64(&s_hw_query_count,(LONG64)(count-consumed));
    }
    hw_leave();
}
static void hw_triangle(const XfTri *triangle)
{
    if(s_hw_count+3>s_hw_capacity) {
        uint32_t capacity=s_hw_capacity?s_hw_capacity*2:3072;
        Nv2aVshOutput *vertices=(Nv2aVshOutput *)realloc(s_hw_vertices,(size_t)capacity*sizeof *vertices);
        if(!vertices) hw_check(E_OUTOFMEMORY,"transformed vertex batch");
        s_hw_vertices=vertices; s_hw_capacity=capacity;
    }
    /* The original raster bounds already include setup rounding and clips. */
    if(!s_hw_count) {
        s_hw_bounds=(RECT){triangle->minx,triangle->miny,triangle->maxx,triangle->maxy};
    } else {
        if(triangle->minx<s_hw_bounds.left) s_hw_bounds.left=triangle->minx;
        if(triangle->miny<s_hw_bounds.top) s_hw_bounds.top=triangle->miny;
        if(triangle->maxx>s_hw_bounds.right) s_hw_bounds.right=triangle->maxx;
        if(triangle->maxy>s_hw_bounds.bottom) s_hw_bounds.bottom=triangle->maxy;
    }
    const Nv2aVshOutput *input[]={triangle->va,triangle->vb,triangle->vc};
    const float *pos[]={triangle->a,triangle->b,triangle->c};
    for(unsigned k=0;k<3;k++) {
        Nv2aVshOutput *output=&s_hw_vertices[s_hw_count++];
        *output=*input[k]; memcpy(output->pos,pos[k],sizeof output->pos);
        output->fog[0]=triangle->vfog[k];
        for(unsigned stage=0;stage<4;stage++) {
            unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
            if((mode==1 || mode==2 || (mode==3 && !s_gpu.texs[stage].cube)) && !tex_size_from_format(s_gpu.texs[stage].color)) {
                output->tex[stage][0]/=s_gpu.texs[stage].width;
                output->tex[stage][1]/=s_gpu.texs[stage].height;
            }
        }
    }
}
static void hw_fixed_line(const Nv2aVshOutput *a,const Nv2aVshOutput *b)
{
    if(s_hw_count+2>s_hw_capacity) {
        uint32_t capacity=s_hw_capacity?s_hw_capacity*2:3072;
        Nv2aVshOutput *vertices=(Nv2aVshOutput *)realloc(s_hw_vertices,(size_t)capacity*sizeof *vertices);
        if(!vertices) hw_check(E_OUTOFMEMORY,"transformed line batch");
        s_hw_vertices=vertices;s_hw_capacity=capacity;
    }
    const Nv2aVshOutput *input[2]={a,b};
    for(unsigned k=0;k<2;k++) {
        Nv2aVshOutput *output=&s_hw_vertices[s_hw_count++];*output=*input[k];
        output->fog[0]=fog_factor(output->fog[0]);
        for(unsigned stage=0;stage<4;stage++) {
            unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
            if((mode==1 || mode==2 || (mode==3 && !s_gpu.texs[stage].cube)) && !tex_size_from_format(s_gpu.texs[stage].color)) {
                output->tex[stage][0]/=s_gpu.texs[stage].width;
                output->tex[stage][1]/=s_gpu.texs[stage].height;
            }
        }
    }
    s_hw_bounds=(RECT){(LONG)s_gpu.clip_x,(LONG)s_gpu.clip_y,(LONG)(s_gpu.clip_x+s_gpu.clip_w),(LONG)(s_gpu.clip_y+s_gpu.clip_h)};
    s_hw_topology=D3D11_PRIMITIVE_TOPOLOGY_LINELIST;note_drawn();
}
static uint32_t hw_reverse_inputs(uint32_t word)
{
    return (word>>24)|((word>>8)&0xFF00)|((word<<8)&0xFF0000)|(word<<24);
}
static void hw_flush_batch(void)
{
    if(!s_hw_count) { s_hw_program_active=0;return; }
    hw_enter();
    uint64_t timing_start=gpu_timing_clock();
    HwColor *color=hw_color(0);
    HwDepth *depth=(s_gpu.depth_test || (nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_1]&NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE))?hw_depth():NULL;
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    ID3D11RenderTargetView *view=((D3D8Surface *)hw_render_surface(color))->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&view,depth?depth->view:NULL);
    D3D11_VIEWPORT vp={0,0,(FLOAT)(color->width*d3d8_RenderScale()),(FLOAT)(color->height*d3d8_RenderScale()),0,1};
    ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
    gpu_timing_finish(timing_start,&s_gpu.hardware_surface_ticks);
    timing_start=gpu_timing_clock();
    IDirect3DDevice8 *device=d3d8_GetDevice();
    const IDirect3DDevice8Vtbl *v=device->lpVtbl;
    v->SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE);
    v->SetRenderState(device,D3DRS_ZENABLE,s_gpu.depth_test);
    v->SetRenderState(device,D3DRS_ZFUNC,s_gpu.depth_test?s_gpu.depth_func-0x1FF:D3DCMP_ALWAYS);
    v->SetRenderState(device,D3DRS_ZWRITEENABLE,s_gpu.depth_test && s_gpu.depth_mask);
    v->SetRenderState(device,D3DRS_ALPHABLENDENABLE,s_gpu.blend_enable);
    v->SetRenderState(device,D3DRS_SRCBLEND,hw_blend_enum(s_gpu.blend_sfactor));
    v->SetRenderState(device,D3DRS_DESTBLEND,hw_blend_enum(s_gpu.blend_dfactor));
    DWORD equation;
    switch(s_gpu.blend_equation) { case 0:case 0x8006:equation=1;break;case 0x800A:equation=2;break;case 0x800B:equation=3;break;case 0x8007:equation=4;break;case 0x8008:equation=5;break;default:hw_check(E_NOTIMPL,"blend equation");equation=0; }
    v->SetRenderState(device,D3DRS_BLENDOP,equation);
    uint32_t mask=s_gpu.color_mask;
    v->SetRenderState(device,D3DRS_COLORWRITEENABLE,((mask>>16)&1)|(((mask>>8)&1)<<1)|((mask&1)<<2)|(((mask>>24)&1)<<3));
    uint32_t stencil=nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_1];
    v->SetRenderState(device,D3DRS_STENCILENABLE,(stencil&NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE)!=0);
    v->SetRenderState(device,D3DRS_STENCILFUNC,GET_MASK(stencil,NV_PGRAPH_CONTROL_1_STENCIL_FUNC)+1);
    v->SetRenderState(device,D3DRS_STENCILREF,GET_MASK(stencil,NV_PGRAPH_CONTROL_1_STENCIL_REF));
    v->SetRenderState(device,D3DRS_STENCILMASK,GET_MASK(stencil,NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ));
    DWORD states[256]={0};
    for(unsigned stage=0;stage<8;stage++) {
        states[D3DRS_PSRGBINPUTS0+stage]=hw_reverse_inputs(s_gpu.rc.color_icw[stage]);
        states[D3DRS_PSALPHAINPUTS0+stage]=hw_reverse_inputs(s_gpu.rc.alpha_icw[stage]);
        uint32_t rgb=s_gpu.rc.color_ocw[stage],alpha=s_gpu.rc.alpha_ocw[stage];
        states[D3DRS_PSRGBOUTPUTS0+stage]=(rgb&~255)|((rgb&15)<<4)|((rgb>>4)&15);
        states[D3DRS_PSALPHAOUTPUTS0+stage]=(alpha&~255)|((alpha&15)<<4)|((alpha>>4)&15);
        states[D3DRS_PSCONSTANT0_0+stage]=s_gpu.rc.factor0[(s_gpu.rc.control&0x1000)?stage:0];
        states[D3DRS_PSCONSTANT1_0+stage]=s_gpu.rc.factor1[(s_gpu.rc.control&0x10000)?stage:0];
    }
    states[D3DRS_PSFINALCOMBINERINPUTSABCD]=hw_reverse_inputs(s_gpu.rc.final0);
    states[D3DRS_PSFINALCOMBINERINPUTSEFG]=hw_reverse_inputs(s_gpu.rc.final1)>>0;
    NV2ACombinerState combiner={0};
    d3d8_combiners_from_render_states(states,&combiner);
    combiner.num_stages=s_gpu.rc.control&255; if(combiner.num_stages>8) combiner.num_stages=8;
    combiner.native_coordinates=1; combiner.final_enabled=s_gpu.rc.final0 || s_gpu.rc.final1;
    combiner.final_flags=s_gpu.rc.final1&255; combiner.flags=(s_gpu.rc.control>>8)&1;
    combiner.clip_plane_mode=s_gpu.clip_plane_mode;
    NV2APSConstants constants={0};
    for(unsigned stage=0;stage<8;stage++) {
        nv2a_rc_unpack(states[D3DRS_PSCONSTANT0_0+stage],constants.c0[stage]);
        nv2a_rc_unpack(states[D3DRS_PSCONSTANT1_0+stage],constants.c1[stage]);
        combiner.stages[stage].rgb_output.ab_blue_alpha=(s_gpu.rc.color_ocw[stage]>>19)&1;
        combiner.stages[stage].rgb_output.cd_blue_alpha=(s_gpu.rc.color_ocw[stage]>>18)&1;
        if(((s_gpu.rc.color_ocw[stage]>>15)&7)==6) combiner.stages[stage].rgb_output.output_map=NV2A_OUT_SHIFTRIGHT_1;
        if(((s_gpu.rc.alpha_ocw[stage]>>15)&7)==6) combiner.stages[stage].alpha_output.output_map=NV2A_OUT_SHIFTRIGHT_1;
    }
    nv2a_rc_unpack(s_gpu.rc.final_c0,constants.final_c0); nv2a_rc_unpack(s_gpu.rc.final_c1,constants.final_c1);
    nv2a_rc_unpack(s_gpu.fog_color,constants.fog_color); float red=constants.fog_color[0];constants.fog_color[0]=constants.fog_color[2];constants.fog_color[2]=red;
    constants.alpha_ref=(s_gpu.alpha_ref&255)/255.0f;constants.alpha_func=s_gpu.alpha_func-0x1FF;constants.alpha_test_enable=s_gpu.alpha_test;
    if(!s_gpu.rc_seen) hw_check(E_NOTIMPL,"unprogrammed combiner state");
    for(unsigned stage=0;stage<4;stage++) {
        unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
        if(mode>5) hw_check(E_NOTIMPL,"texture stage operation");
        combiner.tex_mode[stage]=mode==0?NV2A_TEXMODE_NONE:mode>=4?(NV2ATextureMode)mode:mode==3 && s_gpu.texs[stage].cube?NV2A_TEXMODE_CUBEMAP:NV2A_TEXMODE_2D;
        if(mode==1 || mode==2) combiner.projected_mask|=1u<<stage;
        if(mode>=1 && mode<=3 && texture_alpha_kill_enabled(stage)) combiner.alpha_kill_mask|=1u<<stage;
        hw_texture_bind(stage);
        Texture *texture=&s_gpu.texs[stage];
        DWORD address_u=texture->addr_u,address_v=texture->addr_v;
        if(address_u>5 || address_v>5) hw_check(E_NOTIMPL,"texture address mode");
        v->SetTextureStageState(device,stage,D3DTSS_ADDRESSU,address_u);
        v->SetTextureStageState(device,stage,D3DTSS_ADDRESSV,address_v);
        uint32_t mag=(texture->filter>>24)&15,min=(texture->filter>>16)&255;
        int linear=mag==2 || mag==4 || min==2 || min==4 || min==6;
        v->SetTextureStageState(device,stage,D3DTSS_MINFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        v->SetTextureStageState(device,stage,D3DTSS_MAGFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        v->SetTextureStageState(device,stage,D3DTSS_MIPFILTER,D3DTEXF_NONE);
    }
    gpu_timing_finish(timing_start,&s_gpu.hardware_texture_ticks);
    timing_start=gpu_timing_clock();
    hw_check(d3d8_states_apply_checked(),"render state binding");
    if(s_hw_program_active) {
        NV2ANativeGeometry geometry={0};
        geometry.screen[0]=(float)color->width;geometry.screen[1]=(float)color->height;
        geometry.screen[2]=(float)s_gpu.clip_x;geometry.screen[3]=(float)s_gpu.clip_y;
        geometry.bounds[0]=(float)(s_gpu.clip_x+s_gpu.clip_w);geometry.bounds[1]=(float)(s_gpu.clip_y+s_gpu.clip_h);
        if(geometry.bounds[0]>NV_ZBUF_W) geometry.bounds[0]=NV_ZBUF_W;
        if(geometry.bounds[1]>NV_ZBUF_H) geometry.bounds[1]=NV_ZBUF_H;
        for(unsigned stage=0;stage<4;stage++) {
            for(unsigned component=0;component<4;component++) geometry.tex_scale[stage][component]=1;
            unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
            if((mode==1 || mode==2 || (mode==3 && !s_gpu.texs[stage].cube)) && !tex_size_from_format(s_gpu.texs[stage].color)) {
                geometry.tex_scale[stage][0]=1.0f/s_gpu.texs[stage].width;geometry.tex_scale[stage][1]=1.0f/s_gpu.texs[stage].height;
            }
        }
        geometry.fog[0]=s_gpu.fog_param[0];geometry.fog[1]=s_gpu.fog_param[1];geometry.fog[2]=(float)s_gpu.fog_mode;geometry.fog[3]=(float)s_gpu.fog_enable;
        uint32_t raster=nv2a_get_state()->pgraph.regs[NV_PGRAPH_SETUPRASTER];
        geometry.flags[0]=(nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_3]&NV_PGRAPH_CONTROL_3_SHADEMODE)==0;
        geometry.flags[1]=(raster&NV_PGRAPH_SETUPRASTER_CULLENABLE)!=0;
        geometry.flags[2]=(raster&NV_PGRAPH_SETUPRASTER_FRONTFACE)!=0;
        geometry.flags[3]=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_CULLCTRL);
        struct PGRAPHState *point_pg=&nv2a_get_state()->pgraph;
        memcpy(geometry.point_params,point_pg->point_params,sizeof geometry.point_params);
        float point_size=point_pg->regs[NV_PGRAPH_POINTSIZE]/8.0f;
        geometry.point_size[0]=point_size<=0?1.0f:point_size;
        if(s_hw_fixed_active && geometry.point_size[0]<1) geometry.point_size[0]=1;
        geometry.point_size[1]=(point_pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_SPECULAR_ENABLE)!=0;
        geometry.point_size[2]=(point_pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_ALPHA_FROM_MATERIAL_SPECULAR)==0;
        geometry.point_flags[0]=(point_pg->regs[NV_PGRAPH_CSV0_D]&NV_PGRAPH_CSV0_D_POINTPARAMSENABLE)!=0;
        geometry.point_flags[1]=(raster&NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE)!=0;
        geometry.point_flags[3]=GET_MASK(point_pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_FOGGENMODE);
        if(s_gpu.prim==NV_PRIM_POINTS && geometry.point_flags[1] && geometry.tex_scale[3][0]!=1)
            hw_check(E_NOTIMPL,"rectangle texture on point sprite stage3");
        if(!s_hw_fixed_active) {
            int length=0;const DWORD *program=(const DWORD *)nv2a_vsh_program(&length);
            if(!program) hw_check(E_NOTIMPL,"vertex program context writes or missing final");
            hw_check(d3d8_vsh_bind_native(program,length,nv2a_vsh_constant(0),&geometry,s_hw_topology,&s_hw_program_inputs),"native vertex program binding");
        } else {
            NV2ANativeFixed fixed={0};
            struct PGRAPHState *pg=point_pg;
            memcpy(fixed.scene,pg->ltctxa[NV_IGRAPH_XF_LTCTXA_FR_AMB],sizeof fixed.scene);
            memcpy(fixed.emission,pg->ltctxa[NV_IGRAPH_XF_LTCTXA_CM_COL],sizeof fixed.emission);
            memcpy(fixed.half_vector,pg->light_infinite_half_vector,sizeof fixed.half_vector);
            memcpy(fixed.direction,pg->light_infinite_direction,sizeof fixed.direction);
            memcpy(fixed.position,pg->light_local_position,sizeof fixed.position);
            memcpy(fixed.attenuation,pg->light_local_attenuation,sizeof fixed.attenuation);
            uint32_t c=pg->regs[NV_PGRAPH_CSV0_C],d=pg->regs[NV_PGRAPH_CSV0_D];
            fixed.flags[0]=(c&NV_PGRAPH_CSV0_C_LIGHTING)!=0;
            fixed.flags[1]=(c&NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE)!=0;
            fixed.flags[2]=(c&NV_PGRAPH_CSV0_C_LOCALEYE)!=0;
            fixed.flags[3]=(c&NV_PGRAPH_CSV0_C_SEPARATE_SPECULAR)!=0;
            for(unsigned stage=0;stage<4;stage++) {
                fixed.texture_matrix[stage]=pg->texture_matrix_enable[stage];
                for(unsigned component=0;component<4;component++) {
                    uint32_t mask=NV_PGRAPH_CSV1_A_T0_S<<(component*3+(stage%2)*16);
                    fixed.texgen[stage][component]=GET_MASK(pg->regs[stage<2?NV_PGRAPH_CSV1_A:NV_PGRAPH_CSV1_B],mask);
                }
            }
            fixed.sources[0]=GET_MASK(c,NV_PGRAPH_CSV0_C_EMISSION);
            fixed.sources[1]=GET_MASK(c,NV_PGRAPH_CSV0_C_AMBIENT);
            fixed.sources[2]=GET_MASK(c,NV_PGRAPH_CSV0_C_DIFFUSE);
            fixed.sources[3]=GET_MASK(c,NV_PGRAPH_CSV0_C_SPECULAR);
            fixed.material[0]=pg->material_alpha;
            for(unsigned i=0;i<8;i++) {
                unsigned mode=GET_MASK(d,NV_PGRAPH_CSV0_D_LIGHT0<<(i*2));
                fixed.light_modes[i/4][i%4]=mode;
                memcpy(fixed.ambient[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_AMB+i*6],16);
                memcpy(fixed.diffuse[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_DIF+i*6],16);
                memcpy(fixed.specular[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_SPC+i*6],16);
                memcpy(&fixed.attenuation[i][3],&pg->ltc1[NV_IGRAPH_XF_LTC1_r0+i][0],4);
                if(!fixed.flags[0] || !mode) continue;
                if(mode==3) hw_check(E_NOTIMPL,"fixed spot falloff coefficients");
                if(geometry.point_size[1] && (fixed.specular[i][0]!=0 || fixed.specular[i][1]!=0 || fixed.specular[i][2]!=0)) {
                    const float *p=pg->specular_params;
                    if(!(p[1]>0 && p[2]<1 && isfinite(p[2]-2*p[0]) && p[2]-2*p[0]>=0))
                        hw_check(E_NOTIMPL,"fixed specular coefficient reconstruction");
                    fixed.material[1]=p[2]-2*p[0];
                }
            }
            hw_check(d3d8_vsh_bind_fixed(nv2a_vsh_constant(0),&geometry,&fixed,s_hw_topology),"native fixed vertex binding");
        }
    } else hw_check(d3d8_shaders_bind_nv2a_outputs((float)color->width,(float)color->height,0),"vertex output binding");
    hw_check(d3d8_combiners_bind(&combiner,&constants),"original combiner shader binding");
    HwPixelState native={0};
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    for(unsigned i=0;i<8;i++) { native.cx[i]=pg->regs[NV_PGRAPH_WINDOWCLIPX0+i*4];native.cy[i]=pg->regs[NV_PGRAPH_WINDOWCLIPY0+i*4]; }
    memcpy(&native.depth[0],&pg->regs[NV_PGRAPH_ZCLIPMIN],4);memcpy(&native.depth[1],&pg->regs[NV_PGRAPH_ZCLIPMAX],4);
    native.depth[2]=1.0f/(((s_gpu.format>>4)&15)==1?65535.0f:16777215.0f);
    native.flags[0]=(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE)!=0;
    native.flags[1]=GET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE],NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN)==NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN_CULL;
    native.flags[2]=(pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)!=0;
    native.flags[3]=d3d8_RenderScale();
    native.bounds[0]=s_gpu.clip_x;native.bounds[1]=s_gpu.clip_y;native.bounds[2]=s_gpu.clip_x+s_gpu.clip_w;native.bounds[3]=s_gpu.clip_y+s_gpu.clip_h;
    if(!s_hw_ps_value_valid || memcmp(&s_hw_ps_value,&native,sizeof native)) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hw_check(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)s_hw_ps_cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"pixel state upload");
        memcpy(mapped.pData,&native,sizeof native); ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)s_hw_ps_cb,0);
        memcpy(&s_hw_ps_value,&native,sizeof native);s_hw_ps_value_valid=1;
    }
    ID3D11DeviceContext_PSSetConstantBuffers(ctx,1,1,&s_hw_ps_cb);
    gpu_timing_finish(timing_start,&s_gpu.hardware_state_ticks);
    timing_start=gpu_timing_clock();
    UINT vertex_stride=sizeof *s_hw_vertices;
    if(s_hw_program_active) {
        unsigned count=0;
        uint32_t remaining=s_hw_program_inputs;
        while(remaining) {
            ++count;
            remaining&=remaining-1;
        }
        vertex_stride=count*16;
    }
    uint32_t vertex_bytes=s_hw_program_vertex_count*vertex_stride;
    uint32_t bytes=s_hw_program_active ? vertex_bytes+s_hw_count*sizeof(uint32_t) : s_hw_count*sizeof *s_hw_vertices;
    if(s_hw_program_active) memmove((uint8_t *)&s_hw_program_upload+vertex_bytes,s_hw_program_upload.indices,s_hw_count*sizeof(uint32_t));
    ID3D11Buffer *vertices_buffer=NULL;
    UINT offset=d3d8_upload_vertices(s_hw_program_active?(const void *)&s_hw_program_upload:(const void *)s_hw_vertices,bytes,&vertices_buffer);
    if(offset==(UINT)-1) hw_check(E_OUTOFMEMORY,"original vertex stream upload");
    ++s_gpu.hardware_vertex_stream_uploads;
    if(!offset) ++s_gpu.hardware_vertex_stream_starts;
    ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&vertices_buffer,&vertex_stride,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,s_hw_topology);
    if(s_hw_program_active) ID3D11DeviceContext_IASetIndexBuffer(ctx,vertices_buffer,DXGI_FORMAT_R32_UINT,offset+vertex_bytes);
    ID3D11Query *query=NULL;
    if(s_gpu.zpass_enable) {
        D3D11_QUERY_DESC desc={D3D11_QUERY_OCCLUSION,0};
        hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&query),"visibility query");
        ID3D11DeviceContext_Begin(ctx,(ID3D11Asynchronous *)query);
    }
    if(!color->dirty) hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_NOACCESS,NULL);
    for(HwColor *other=s_hw_colors;other;other=other->next) if(other!=color && other->memory==color->memory && hw_ranges_overlap(other->memory_offset,hw_color_bytes(other),color->memory_offset,hw_color_bytes(color))) other->upload_needed=1;
    if(s_hw_program_active) ID3D11DeviceContext_DrawIndexed(ctx,s_hw_count,0,0);
    else ID3D11DeviceContext_Draw(ctx,s_hw_count,0);
    if(query) {
        ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous *)query);
        HwQuery result={0};result.query=query;hw_append_query(result);
    }
    RECT bounds=s_hw_bounds;
    if(bounds.right>(LONG)color->width) bounds.right=(LONG)color->width;
    if(bounds.bottom>(LONG)color->height) bounds.bottom=(LONG)color->height;
    if(bounds.left<bounds.right && bounds.top<bounds.bottom) {
        if(!color->dirty) color->dirty_bounds=bounds;
        else {
            if(bounds.left<color->dirty_bounds.left) color->dirty_bounds.left=bounds.left;
            if(bounds.top<color->dirty_bounds.top) color->dirty_bounds.top=bounds.top;
            if(bounds.right>color->dirty_bounds.right) color->dirty_bounds.right=bounds.right;
            if(bounds.bottom>color->dirty_bounds.bottom) color->dirty_bounds.bottom=bounds.bottom;
        }
        color->dirty=1;
    }
    s_hw_count=0;s_hw_program_active=0;s_hw_fixed_active=0;
    hw_check(ID3D11Device_GetDeviceRemovedReason(d3d8_GetD3D11Device()),"hardware device status");
    gpu_timing_finish(timing_start,&s_gpu.hardware_submit_ticks);
    hw_leave();
}
