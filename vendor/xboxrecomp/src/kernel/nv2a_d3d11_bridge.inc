/* Physical resource connection for the existing NV097 consumer. Native
 * program outputs consume the existing VSH cache and geometry setup;
 * attribute, primitive and texture identities remain in nv2a_pb_exec.c. */
#include <d3dcompiler.h>
extern void xbox_FramebufferWindowPresentHost(const void *,unsigned,unsigned,unsigned);
static ID3D11VertexShader *s_hw_clear_vs;
static ID3D11Buffer *s_hw_clear_cb;
static ID3D11RasterizerState *s_hw_clear_rs;
static ID3D11DepthStencilState *s_hw_clear_ds[4];
typedef struct HwMemory {
    XboxSharedMemoryViews views;
    uint8_t *host_view;
    DWORD *write_guard_protections;
    struct HwMemory *next;
} HwMemory;
static HwMemory *s_hw_memories;
typedef struct HwColor {
    uint32_t address, pitch, width, height, bpp, format;
    IDirect3DSurface8 *surface,*host_surface;
    NvkImage *native_surface,*native_host;
    HwMemory *memory;
    size_t memory_offset;
    int upload_needed;
    int dirty;
    RECT dirty_bounds;
    struct HwColor *next;
} HwColor;
typedef struct HwDepth {
    uint32_t address, width, height, format, pitch, surface_format;
    HwMemory *memory;
    size_t memory_offset;
    int dirty, upload_needed;
    ID3D11Texture2D *texture;
    ID3D11DepthStencilView *view;
    NvkImage *native;
    struct HwDepth *next;
} HwDepth;
typedef struct HwTexture {
    Texture identity;
    size_t raw_size;
    uint8_t *raw;
    uint32_t palette[256];
    HwMemory *memory, *palette_memory;
    size_t memory_offset, palette_offset;
    int content_valid;
    IDirect3DBaseTexture8 *texture;
    NvkImage *native;
    unsigned host_width,host_height;
    struct HwTexture *next, *hash_next, *valid_next;
    struct HwTexture **valid_link;
} HwTexture;

/* Retain only the existing attribute conversion and index-to-row results.
 * The original primitive assembly, upload ring and shader binding still run. */
enum { HW_VERTEX_CACHE_SETS=128, HW_VERTEX_CACHE_WAYS=4,
       HW_VERTEX_CACHE_ENTRY_BYTES=65536 };
typedef struct HwVertexRange { HwMemory *memory;size_t offset,bytes; } HwVertexRange;
typedef struct HwVertexBatch {
    VertexAttr attr[16];
    float current[16][4];
    uint32_t inputs,count,vertices,bytes,hash;
    uint64_t used,upload_generation;
    uint32_t upload_offset,upload_primitive,upload_provoking,upload_indices;
    int valid;
    unsigned range_count;
    HwVertexRange ranges[16];
    uint8_t *data;
    struct HwVertexBatch *next;
} HwVertexBatch;
static HwVertexBatch s_hw_vertex_batches[HW_VERTEX_CACHE_SETS][HW_VERTEX_CACHE_WAYS];
static HwVertexBatch *s_hw_vertex_buckets[HW_VERTEX_CACHE_SETS];
static HwVertexBatch *s_hw_vertex_batch;
static uint64_t s_hw_vertex_cache_clock;
static uint64_t s_hw_vertex_cache_hits,s_hw_vertex_cache_misses;

static HwColor *s_hw_colors;
static HwDepth *s_hw_depths;
static size_t hw_depth_bytes(const HwDepth *d) {return (size_t)d->pitch*d->height;}
static void hw_copy_depth_to_memory(HwDepth *depth);
static void hw_upload_depth(HwDepth *depth);
static void hw_free_depths(HwDepth *depth);
static HwTexture *s_hw_textures;
enum { HW_TEXTURE_BUCKETS=256 };
static HwTexture *s_hw_texture_buckets[HW_TEXTURE_BUCKETS];
/* Index the original content-valid entries; invalid history needs no write checks. */
static HwTexture *s_hw_valid_textures;
typedef struct HwVertexOutput { float pos[4],d0[4],d1[4],fog[4],tex[4][4]; } HwVertexOutput;
static void hw_pack_output(HwVertexOutput *dst,const Nv2aVshOutput *src)
{
    memcpy(dst->pos,src->pos,16);memcpy(dst->d0,src->d0,16);memcpy(dst->d1,src->d1,16);memcpy(dst->fog,src->fog,16);
    memcpy(dst->tex,src->tex,sizeof dst->tex);
}
static HwVertexOutput *s_hw_vertices;
static uint32_t s_hw_count, s_hw_capacity;
static RECT s_hw_bounds;
static int s_hw_program_active, s_hw_fixed_active, s_hw_output_active;
static uint32_t s_hw_polygon_info[NV_MAX_INDICES];
static D3D11_PRIMITIVE_TOPOLOGY s_hw_topology;
static uint32_t s_hw_program_vertex_count;
static uint32_t s_hw_program_inputs;
/* Valid only during raster_batch_vertices and its immediate flush. */
static const uint32_t *s_hw_program;
static int s_hw_program_length;
static uint32_t s_hw_program_vertex_rows[NV_MAX_INDICES];
static struct { float vertex[NV_MAX_INDICES*16][4]; uint32_t indices[NV_MAX_INDICES*6]; } s_hw_program_upload;
static int hw_polygon_modes_active(void)
{
    return s_gpu.prim>NV_PRIM_LINE_STRIP && (nv2a_get_state()->pgraph.regs[NV_PGRAPH_SETUPRASTER]&
        (NV_PGRAPH_SETUPRASTER_FRONTFACEMODE|NV_PGRAPH_SETUPRASTER_BACKFACEMODE))!=0;
}
static void hw_program_triangle(uint32_t a,uint32_t b,uint32_t c,uint32_t p)
{
    s_hw_polygon_info[s_hw_count/6]=primitive_polygon_info(a,b,c,0,3);
    uint32_t *indices=s_hw_program_upload.indices+s_hw_count;
    indices[0]=s_hw_program_vertex_rows[a];indices[1]=s_hw_program_vertex_rows[p];
    indices[2]=s_hw_program_vertex_rows[b];indices[3]=s_hw_program_vertex_rows[p];
    indices[4]=s_hw_program_vertex_rows[c];indices[5]=s_hw_program_vertex_rows[p];
    if(!s_hw_count) note_drawn();
    s_hw_count+=6;
}
static void hw_program_quad(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t p)
{
    hw_program_triangle(a,b,c,p);
    s_hw_program_upload.indices[s_hw_count-3]=s_hw_program_vertex_rows[d];
    s_hw_polygon_info[s_hw_count/6-1]=primitive_polygon_info(a,b,c,d,4);
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
    float depth_offset[4];
    uint32_t stipple_flags[4],stipple_pattern[32];
} HwPixelState;
static HwPixelState s_hw_ps_value;
static int s_hw_ps_value_valid;
static int s_hw_initialized;
typedef struct HwQuery {
    ID3D11Query *query;
    NvkQuery *native_query;
    uint64_t native_serial;
    int pending;
    HwMemory *memory;
    size_t offset;
    uint32_t value;
} HwQuery;
static HwQuery *s_hw_queries;
static volatile LONG64 s_hw_query_count;
static size_t s_hw_query_capacity;
static void hw_check(HRESULT hr, const char *operation)
{
    if (FAILED(hr)) {
        fprintf(stderr,"[STOP] D3D11 %s failed: %08lX\n",operation,(unsigned long)hr);
        exit(5);
    }
}
static void hw_vk_check(int ok,const char *operation)
{
    if(!ok){fprintf(stderr,"[STOP] Vulkan %s: %s\n",operation,nvk_error());exit(5);}
}
static NvkImage *hw_vk_image(NvkImage *image,const char *operation)
{ hw_vk_check(image!=NULL,operation);return image; }
static NvkImage *hw_vk_render(HwColor *color)
{ return color->native_host?color->native_host:color->native_surface; }
static NvkImage *s_hw_vk_textures[4];
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
static void hw_context_enter(void) {
    if(!InitOnceExecuteOnce(&s_hw_lock_once,hw_lock_initialize,NULL,NULL)) hw_check(E_FAIL,"memory lock initialization");
    EnterCriticalSection(&s_hw_lock);
}
static void hw_enter(void) {
    NV2AState *d=nv2a_get_state();if(d)qemu_mutex_lock(&d->pfifo.lock);
    hw_context_enter();
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
    m->write_guard_protections=calloc(((views->view_size+4095)/4096)*views->view_count,sizeof *m->write_guard_protections);
    if(!m->write_guard_protections) hw_check(E_OUTOFMEMORY,"CPU write protection ownership");
    m->next=s_hw_memories;s_hw_memories=m;return m;
}
static void hw_texture_invalidate(HwTexture *texture)
{
    if(!texture->content_valid) return;
    *texture->valid_link=texture->valid_next;
    if(texture->valid_next) texture->valid_next->valid_link=texture->valid_link;
    texture->valid_link=NULL;texture->valid_next=NULL;texture->content_valid=0;
}
static void hw_texture_validate(HwTexture *texture)
{
    texture->valid_next=s_hw_valid_textures;
    texture->valid_link=&s_hw_valid_textures;
    if(texture->valid_next) texture->valid_next->valid_link=&texture->valid_next;
    s_hw_valid_textures=texture;texture->content_valid=1;
}
static unsigned hw_texture_bucket(const Texture *source,size_t bytes)
{
    const uint32_t words[]={source->offset,source->width,source->height,source->color,
        source->pitch,source->cube,source->levels,texture_volume(source),texture_depth(source),(uint32_t)bytes,(uint32_t)((uint64_t)bytes>>32)};
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<sizeof words/sizeof *words;i++) hash=(hash^words[i])*16777619u;
    hash^=hash>>16;
    return hash%HW_TEXTURE_BUCKETS;
}
static void hw_invalidate_texture_range(HwMemory *memory,size_t offset,size_t bytes)
{
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    for(unsigned set=0;set<HW_VERTEX_CACHE_SETS;set++) for(unsigned way=0;way<HW_VERTEX_CACHE_WAYS;way++) {
        HwVertexBatch *entry=&s_hw_vertex_batches[set][way];
        if(!entry->valid) continue;
        for(unsigned i=0;i<entry->range_count;i++) {
            HwVertexRange *range=&entry->ranges[i];
            if(range->memory==memory && lo<range->offset+range->bytes && range->offset<hi) {entry->valid=0;break;}
        }
    }
    for(HwTexture *t=s_hw_valid_textures,*next;t;t=next) {
        next=t->valid_next;
        if((t->memory==memory && lo<t->memory_offset+t->raw_size && t->memory_offset<hi) ||
           (t->palette_memory==memory && lo<t->palette_offset+sizeof t->palette && t->palette_offset<hi))
            hw_texture_invalidate(t);
    }
}
static void hw_protect_views(HwMemory *memory,size_t offset,size_t bytes,DWORD protection)
{
    if(!bytes || offset>memory->views.view_size || bytes>memory->views.view_size-offset) hw_check(E_INVALIDARG,"shared colour protection extent");
    if(protection==PAGE_NOACCESS) hw_invalidate_texture_range(memory,offset,bytes);
    if(protection==PAGE_READWRITE) {
        hw_invalidate_texture_range(memory,offset,bytes);
        for(size_t page=offset/4096;page<(offset+bytes+4095)/4096;page++)
            for(unsigned i=0;i<memory->views.view_count;i++) memory->write_guard_protections[page*memory->views.view_count+i]=0;
    }
    for(unsigned i=0;i<memory->views.view_count;i++) {
        DWORD previous;
        if(!VirtualProtect((uint8_t *)memory->views.cpu_views[i]+offset,bytes,protection,&previous)) hw_check(HRESULT_FROM_WIN32(GetLastError()),"shared colour protection");
    }
    if(protection==PAGE_NOACCESS)return; /* The successful full-span write is already strongest. */
    for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==memory && (d->dirty || !d->upload_needed)) {
        size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
        size_t dlo=d->memory_offset&~(size_t)4095,dhi=(d->memory_offset+hw_depth_bytes(d)+4095)&~(size_t)4095;
        if(lo<dlo)lo=dlo;if(hi>dhi)hi=dhi;
        for(unsigned i=0;i<memory->views.view_count;i++)for(size_t page=lo;page<hi;page+=4096) {
            MEMORY_BASIC_INFORMATION info;DWORD old;
            if(!VirtualQuery((uint8_t *)memory->views.cpu_views[i]+page,&info,sizeof info))hw_check(E_FAIL,"depth page query");
            DWORD guard=d->dirty?PAGE_NOACCESS:PAGE_READONLY;
            if(info.Protect==PAGE_NOACCESS || info.Protect==guard)continue;
            if(!VirtualProtect((uint8_t *)memory->views.cpu_views[i]+page,4096,guard,&old))hw_check(E_FAIL,"depth page protection");
        }
    }
}
static size_t hw_color_bytes(const HwColor *color) { return (size_t)color->pitch*color->height; }
static int hw_ranges_overlap(size_t a,size_t an,size_t b,size_t bn) { return a<b+bn && b<a+an; }
static int hw_pending_output(HwMemory *memory,size_t offset,size_t bytes)
{
    for(size_t i=0;i<s_hw_query_count;i++) {
        HwQuery *q=&s_hw_queries[i];
        if(q->memory && q->pending && (!memory || (q->memory==memory && hw_ranges_overlap(offset,bytes,q->offset,4)))) return 1;
    }
    return 0;
}
static void hw_guard_cpu_write_range(HwMemory *memory,size_t offset,size_t bytes);
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
                if(q->memory==query->memory && q->pending && q!=query &&
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
        for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==query->memory && hw_ranges_overlap(page,4096,d->memory_offset,hw_depth_bytes(d))) {
            colour=1;if(d->dirty){protection=PAGE_NOACCESS;break;}if(!d->upload_needed)protection=PAGE_READONLY;
        }
        /* Other pending outputs still own this already read-only page. */
        if(!colour) {
            /* Keep the existing owned write guard until an actual CPU write.
             * The host view publishes completion without making aliases writable. */
            if(transition==HW_OUTPUT_COMPLETE) {
                hw_invalidate_texture_range(query->memory,page,4096);continue;
            }
            if(protection==PAGE_READONLY) {
                DWORD *permissions=query->memory->write_guard_protections+
                    (page/4096)*query->memory->views.view_count;
                unsigned i=0;while(i<query->memory->views.view_count && permissions[i])++i;
                if(i<query->memory->views.view_count)hw_guard_cpu_write_range(query->memory,page,4096);
                continue;
            }
        }
        if(transition==HW_OUTPUT_COMPLETE && protection==PAGE_READONLY)
            hw_invalidate_texture_range(query->memory,page,4096);
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
        if(q->memory==memory && q->pending && (completed || hw_ranges_overlap(lo,hi-lo,q->offset,4)))
            hw_output_page_protection(q,HW_OUTPUT_REFRESH);
    }
}
static void hw_append_query(HwQuery query)
{
    size_t count=(size_t)s_hw_query_count;
    if(count==s_hw_query_capacity) {
        if(s_hw_query_capacity>SIZE_MAX/sizeof(HwQuery)/2) hw_check(E_OUTOFMEMORY,"query output capacity");
        size_t capacity=s_hw_query_capacity?s_hw_query_capacity*2:16;
        HwQuery *queries=(HwQuery *)realloc(s_hw_queries,capacity*sizeof *queries);
        if(!queries) hw_check(E_OUTOFMEMORY,"query output storage");
        s_hw_queries=queries;s_hw_query_capacity=capacity;
    }
    query.pending=1;
    s_hw_queries[count]=query;InterlockedIncrement64(&s_hw_query_count);
}
static void hw_initialize(void);
static void hw_defer_output(uint8_t *output,uint32_t value)
{
    hw_initialize();hw_enter();uint64_t start=gpu_timing_clock();
    XboxSharedMemoryViews views;
    if(!xbox_GetSharedMemoryViews(output,4,&views)) hw_check(E_INVALIDARG,"semaphore shared output identity");
    HwQuery query={0};query.memory=hw_memory(&views);query.offset=views.offset;query.value=value;
    if(d3d8_IsNativeVulkan()) query.native_serial=nvk_fence();
    else {
    /* Transfer the existing idle EVENT; pending entries keep sole ownership. */
    if(s_hw_completion) {query.query=s_hw_completion;s_hw_completion=NULL;}
    else {
        D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};
        hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&query.query),"semaphore completion event");
    }
    ID3D11DeviceContext_End(d3d8_GetD3D11Context(),(ID3D11Asynchronous *)query.query);
    }
    hw_append_query(query);hw_output_page_protection(&s_hw_queries[s_hw_query_count-1],HW_OUTPUT_APPEND);
    gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);
    hw_leave();
}
static void hw_mark_cpu_write(HwMemory *memory,size_t offset,size_t bytes,HwQuery *completed)
{
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    for(HwColor *c=s_hw_colors;c;c=c->next) if(c->memory==memory && hw_ranges_overlap(lo,hi-lo,c->memory_offset,hw_color_bytes(c))) c->upload_needed=1;
    for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==memory && hw_ranges_overlap(lo,hi-lo,d->memory_offset,hw_depth_bytes(d))) d->upload_needed=1;
    hw_protect(memory,offset,bytes,PAGE_READWRITE,completed);
}
/* Keep existing colour/query protections stronger than an owned write guard.
 * Per-view records retain only permissions changed here; content validity stays
 * in the original texture cache, and guest-owned read-only views stay intact. */
static void hw_guard_cpu_write_range(HwMemory *memory,size_t offset,size_t bytes)
{
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    for(unsigned i=0;i<memory->views.view_count;i++) {
        uint8_t *base=(uint8_t *)memory->views.cpu_views[i];
        for(size_t at=lo;at<hi;) {
            MEMORY_BASIC_INFORMATION page;
            if(!VirtualQuery(base+at,&page,sizeof page)) hw_check(HRESULT_FROM_WIN32(GetLastError()),"texture page identity");
            size_t end=(size_t)((uint8_t *)page.BaseAddress+page.RegionSize-base);
            if(end>hi) end=hi;
            if(end<=at) hw_check(E_FAIL,"texture page extent");
            DWORD access=page.Protect&0xff;
            if(access==PAGE_READWRITE || access==PAGE_WRITECOPY ||
               access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY) {
                DWORD previous;
                DWORD read_only=(access==PAGE_EXECUTE_READWRITE || access==PAGE_EXECUTE_WRITECOPY)?PAGE_EXECUTE_READ:PAGE_READONLY;
                if(!VirtualProtect(base+at,end-at,read_only|(page.Protect&~0xffu),&previous))
                    hw_check(HRESULT_FROM_WIN32(GetLastError()),"texture read protection");
                for(size_t index=at/4096;index<end/4096;index++)
                    memory->write_guard_protections[index*memory->views.view_count+i]=page.Protect;
            }
            at=end;
        }
    }
}
static void hw_release_cpu_write_range(HwMemory *memory,size_t offset,size_t bytes)
{
    hw_invalidate_texture_range(memory,offset,bytes);
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    for(size_t page=lo;page<hi;page+=4096) {
        DWORD *permissions=memory->write_guard_protections+(page/4096)*memory->views.view_count;
        int owned=0;for(unsigned i=0;i<memory->views.view_count;i++) if(permissions[i]){owned=1;break;}
        if(!owned) continue;
        int output=hw_pending_output(memory,page,4096);
        DWORD protection=output?PAGE_READONLY:0;
        for(HwColor *c=s_hw_colors;c;c=c->next) if(c->memory==memory && hw_ranges_overlap(page,4096,c->memory_offset,hw_color_bytes(c))) {
            if(c->dirty) {protection=PAGE_NOACCESS;break;}
            if(!c->upload_needed) protection=PAGE_READONLY;
        }
        for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==memory && hw_ranges_overlap(page,4096,d->memory_offset,hw_depth_bytes(d))) {
            if(d->dirty){protection=PAGE_NOACCESS;break;}if(!d->upload_needed)protection=PAGE_READONLY;
        }
        for(unsigned i=0;i<memory->views.view_count;i++) if(permissions[i]) {
            DWORD previous;
            if(!VirtualProtect((uint8_t *)memory->views.cpu_views[i]+page,4096,protection?protection:permissions[i],&previous))
                hw_check(HRESULT_FROM_WIN32(GetLastError()),"texture protection release");
            if(!output)permissions[i]=0;
        }
    }
}

static void hw_vertex_batch_discard(HwVertexBatch *entry)
{
    entry->valid=0;
    for(unsigned i=0;i<entry->range_count;i++)
        hw_release_cpu_write_range(entry->ranges[i].memory,entry->ranges[i].offset,entry->ranges[i].bytes);
    free(entry->data);memset(entry,0,sizeof *entry);
}
static void hw_free_vertex_batches(void)
{
    for(unsigned set=0;set<HW_VERTEX_CACHE_SETS;set++) for(unsigned way=0;way<HW_VERTEX_CACHE_WAYS;way++)
        hw_vertex_batch_discard(&s_hw_vertex_batches[set][way]);
    memset(s_hw_vertex_buckets,0,sizeof s_hw_vertex_buckets);
    s_hw_vertex_batch=NULL;
}
static HwVertexBatch *hw_vertex_batch_begin(uint32_t inputs,unsigned attribute_count,int *hit)
{
    uint32_t n=s_gpu.idx_count;
    *hit=0;
    /* A fixed bound includes both original indices and converted row mapping.
     * Inline inputs remain owned by their original transient command batch. */
    /* Tiny triangles/quads cost less to convert than to guard their source
     * pages; keep them in the existing uncached conversion path. */
    if(s_gpu.inline_active || n<=4 ||
       (uint64_t)n*(attribute_count*16u+8u)>HW_VERTEX_CACHE_ENTRY_BYTES) return NULL;
    VertexAttr attributes[16]={0};
    float current[16][4]={0};
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<16;i++) if(inputs&(1u<<i)) {
        attributes[i]=s_gpu.attr[i];
        const uint32_t *words=(const uint32_t *)&attributes[i];
        for(unsigned word=0;word<sizeof(VertexAttr)/4;word++) hash=(hash^words[word])*16777619u;
        if(!attributes[i].size) {
            memcpy(current[i],s_gpu.imm_attr[i],sizeof current[i]);
            uint32_t bits[4];memcpy(bits,current[i],sizeof bits);
            for(unsigned word=0;word<4;word++) hash=(hash^bits[word])*16777619u;
        }
    }
    hash=(hash^inputs)*16777619u;hash=(hash^n)*16777619u;
    hash=(hash^s_gpu.idx[0])*16777619u;
    hash=(hash^s_gpu.idx[n/2])*16777619u;
    hash=(hash^s_gpu.idx[n-1])*16777619u;
    hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
    HwVertexBatch *selected=NULL;
    for(HwVertexBatch *entry=s_hw_vertex_buckets[hash%HW_VERTEX_CACHE_SETS];entry;entry=entry->next) {
        if(entry->data && entry->hash==hash && entry->inputs==inputs && entry->count==n &&
           !memcmp(entry->attr,attributes,sizeof attributes) && !memcmp(entry->current,current,sizeof current) && !memcmp(entry->data,s_gpu.idx,n*4)) {
            entry->used=++s_hw_vertex_cache_clock;
            if(entry->valid) {
                memcpy(s_hw_program_vertex_rows,entry->data+n*4,n*4);
                s_hw_program_vertex_count=entry->vertices;
                s_gpu.vertices_reused+=n-entry->vertices;
                ++s_hw_vertex_cache_hits;*hit=1;return entry;
            }
            for(unsigned i=0;i<entry->range_count;i++)
                hw_guard_cpu_write_range(entry->ranges[i].memory,entry->ranges[i].offset,entry->ranges[i].bytes);
            ++s_hw_vertex_cache_misses;return entry;
        }
    }
    for(unsigned set=0;set<HW_VERTEX_CACHE_SETS;set++) for(unsigned way=0;way<HW_VERTEX_CACHE_WAYS;way++) {
        HwVertexBatch *entry=&s_hw_vertex_batches[set][way];
        if(!selected || entry->used<selected->used) selected=entry;
    }
    if(selected->data) {
        HwVertexBatch **link=&s_hw_vertex_buckets[selected->hash%HW_VERTEX_CACHE_SETS];
        while(*link!=selected)link=&(*link)->next;
        *link=selected->next;
    }
    hw_vertex_batch_discard(selected);
    uint32_t first=s_gpu.idx[0],last=first;
    for(uint32_t i=1;i<n;i++) {
        if(s_gpu.idx[i]<first)first=s_gpu.idx[i];
        if(s_gpu.idx[i]>last)last=s_gpu.idx[i];
    }
    for(unsigned i=0;i<16;i++) if(inputs&(1u<<i)) {
        const VertexAttr *attr=&attributes[i];
        if(!attr->size || !attr->stride || !attr->offset) continue;
        /* Guard a conservative four-component span; fetch_attr remains the
         * only format decoder and continues to read the original CPU view. */
        size_t at=(size_t)attr->offset+(size_t)first*attr->stride;
        size_t bytes=(size_t)(last-first)*attr->stride+16;
        XboxSharedMemoryViews views;
        if(!xbox_GetSharedMemoryViews((uint8_t *)g_xbox_mem_offset+at,bytes,&views)) {
            hw_vertex_batch_discard(selected);return NULL;
        }
        HwMemory *memory=hw_memory(&views);
        unsigned range;
        for(range=0;range<selected->range_count;range++) {
            HwVertexRange *current=&selected->ranges[range];
            if(current->memory==memory && current->offset<=views.offset+bytes && views.offset<=current->offset+current->bytes) {
                size_t end=current->offset+current->bytes;
                if(end<views.offset+bytes)end=views.offset+bytes;
                if(current->offset>views.offset)current->offset=views.offset;
                current->bytes=end-current->offset;break;
            }
        }
        if(range==selected->range_count) selected->ranges[selected->range_count++]=(HwVertexRange){memory,views.offset,bytes};
    }
    for(unsigned i=0;i<selected->range_count;i++)
        hw_guard_cpu_write_range(selected->ranges[i].memory,selected->ranges[i].offset,selected->ranges[i].bytes);
    selected->data=malloc((size_t)n*(attribute_count*16u+8u));
    if(!selected->data) hw_check(E_OUTOFMEMORY,"vertex conversion cache");
    memcpy(selected->attr,attributes,sizeof attributes);
    memcpy(selected->current,current,sizeof current);
    selected->inputs=inputs;selected->count=n;selected->hash=hash;
    selected->used=++s_hw_vertex_cache_clock;
    memcpy(selected->data,s_gpu.idx,n*4);
    selected->next=s_hw_vertex_buckets[hash%HW_VERTEX_CACHE_SETS];
    s_hw_vertex_buckets[hash%HW_VERTEX_CACHE_SETS]=selected;
    ++s_hw_vertex_cache_misses;return selected;
}
static void hw_vertex_batch_end(HwVertexBatch *entry,unsigned attribute_count)
{
    if(!entry) return;
    entry->upload_generation=0;
    entry->vertices=s_hw_program_vertex_count;entry->bytes=entry->vertices*attribute_count*16u;
    memcpy(entry->data+entry->count*4,s_hw_program_vertex_rows,entry->count*4);
    memcpy(entry->data+entry->count*8,s_hw_program_upload.vertex,entry->bytes);
    for(unsigned i=0;i<entry->range_count;i++)
        hw_guard_cpu_write_range(entry->ranges[i].memory,entry->ranges[i].offset,entry->ranges[i].bytes);
    entry->valid=1;
}

static void hw_disarm_texture(HwTexture *texture)
{
    if(texture->memory) hw_release_cpu_write_range(texture->memory,texture->memory_offset,texture->raw_size);
    if(texture->palette_memory) hw_release_cpu_write_range(texture->palette_memory,texture->palette_offset,sizeof texture->palette);
}
static void hw_free_textures(void)
{
    memset(s_hw_texture_buckets,0,sizeof s_hw_texture_buckets);
    while(s_hw_textures) {
        HwTexture *texture=s_hw_textures;s_hw_textures=texture->next;
        hw_texture_invalidate(texture);
        hw_disarm_texture(texture);
        if(texture->native)nvk_image_destroy(texture->native);
        if(texture->texture) texture->texture->lpVtbl->Release(texture->texture);
        free(texture->raw);free(texture);
    }
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
    if(d3d8_IsNativeVulkan()){s_hw_initialized=1;return;}
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
        if(color->native_surface) {
            locked.Pitch=color->width*4;locked.pBits=malloc((size_t)locked.Pitch*color->height);
            if(!locked.pBits)hw_check(E_OUTOFMEMORY,"colour upload storage");
        } else hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,NULL,0),"swizzled colour upload lock");
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
        if(color->native_surface){hw_vk_check(nvk_image_upload(color->native_surface,locked.pBits,locked.Pitch),"swizzled colour upload");free(locked.pBits);}
        else hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"swizzled colour upload unlock");
    } else if(color->native_surface)hw_vk_check(nvk_image_upload(color->native_surface,memory,color->pitch),"colour upload");
    else ID3D11DeviceContext_UpdateSubresource(d3d8_GetD3D11Context(),
        (ID3D11Resource *)((D3D8Surface *)color->surface)->d3d11_texture,0,NULL,memory,color->pitch,0);
    color->upload_needed=0;++s_gpu.hardware_color_uploads;
}
static void hw_clear_color_image(HwColor *color)
{
    uint32_t value=s_gpu.clear_color;
    if((color->format&15)==NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) value|=0xff000000u;
    const float rgba[4]={((value>>16)&255)/255.0f,((value>>8)&255)/255.0f,(value&255)/255.0f,(value>>24)/255.0f};
    if(color->native_surface) {
        hw_vk_check(nvk_image_clear(color->native_surface,rgba),"colour clear");
        if(color->native_host)hw_vk_check(nvk_image_clear(color->native_host,rgba),"scaled colour clear");
    } else {
    ID3D11DeviceContext *context=d3d8_GetD3D11Context();
    ID3D11DeviceContext_ClearRenderTargetView(context,((D3D8Surface *)color->surface)->rtv,rgba);
    if(color->host_surface) ID3D11DeviceContext_ClearRenderTargetView(context,((D3D8Surface *)color->host_surface)->rtv,rgba);
    }
}
static void hw_upload_color(HwColor *color,uint32_t cleared_channels)
{
    ClearBounds bounds=clear_bounds();
    if((cleared_channels&NV097_CLEAR_COLOR_MASK)==NV097_CLEAR_COLOR_MASK &&
       !bounds.x0 && !bounds.y0 && bounds.x1==color->width && bounds.y1==color->height) {
        /* The original CPU clear has already written every target byte. */
        if(hw_pending_output(color->memory,color->memory_offset,hw_color_bytes(color))) hw_collect_queries(1);
        hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_READONLY,NULL);
        hw_clear_color_image(color);
        color->upload_needed=0;
    } else {
        hw_upload_native_color(color);
        if(color->native_host)hw_vk_check(nvk_image_copy(color->native_surface,color->native_host,NULL,0),"scaled colour upload");
        if(color->host_surface)hw_check(d3d8_surface_resample(color->surface,color->host_surface),"scaled colour upload");
    }
}
static IDirect3DSurface8 *hw_render_surface(HwColor *color) { return color->host_surface?color->host_surface:color->surface; }
static void hw_copy_color_to_memory(HwColor *color)
{
    if(!color->dirty) return;
    uint64_t timing_start=gpu_timing_clock();D3DLOCKED_RECT locked;
    if(color->native_host)hw_vk_check(nvk_image_copy(color->native_host,color->native_surface,NULL,1),"guest colour resolve");
    if(color->host_surface)hw_check(d3d8_surface_resample(color->host_surface,color->surface),"guest colour resolve");
    const RECT *bounds=&color->dirty_bounds;
    uint8_t *native_pixels=NULL;
    if(color->native_surface) {
        native_pixels=malloc((size_t)color->width*color->height*4);
        if(!native_pixels)hw_check(E_OUTOFMEMORY,"colour readback storage");
        hw_vk_check(nvk_image_read(color->native_surface,native_pixels,color->width*4),"colour readback");
        locked.Pitch=color->width*4;locked.pBits=native_pixels+(size_t)bounds->top*locked.Pitch+bounds->left*4;
    } else hw_check(color->surface->lpVtbl->LockRect(color->surface,&locked,bounds,D3DLOCK_READONLY),"surface readback");
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
    if(native_pixels)free(native_pixels);
    else hw_check(color->surface->lpVtbl->UnlockRect(color->surface),"surface unlock");
    s_gpu.hardware_color_readback_bytes+=row_bytes*(size_t)(bounds->bottom-bounds->top);
    hw_invalidate_texture_range(color->memory,color->memory_offset,hw_color_bytes(color));
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
    for(HwDepth *depth=s_hw_depths;depth;depth=depth->next) if(depth->memory && depth->dirty) {
        hw_copy_depth_to_memory(depth);
        hw_protect(depth->memory,depth->memory_offset,hw_depth_bytes(depth),depth->upload_needed?PAGE_READWRITE:PAGE_READONLY,NULL);
    }
    if(protection_needed) for(HwColor *color=s_hw_colors;color;color=color->next) if(color->height) hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),color->upload_needed?PAGE_READWRITE:PAGE_READONLY,NULL);
    hw_leave();
}
static void hw_preserve_cpu_write(HwMemory *memory,size_t offset,size_t bytes,int overwrite)
{
    size_t lo=offset&~(size_t)4095,hi=(offset+bytes+4095)&~(size_t)4095;
    /* Every owner of pages made writable must retain its untouched
     * bytes. A complete overwrite needs none of that owner's pixels. */
    for(HwColor *c=s_hw_colors;c;c=c->next)
        if(c->memory==memory && c->dirty && hw_ranges_overlap(lo,hi-lo,c->memory_offset,hw_color_bytes(c))) {
            if(overwrite && c->memory_offset>=offset && c->memory_offset<=offset+bytes &&
               hw_color_bytes(c)<=offset+bytes-c->memory_offset)c->dirty=0;
            else hw_copy_color_to_memory(c);
        }
    for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==memory && d->dirty && hw_ranges_overlap(lo,hi-lo,d->memory_offset,hw_depth_bytes(d))) {
        if(overwrite && d->memory_offset>=offset && d->memory_offset<=offset+bytes && hw_depth_bytes(d)<=offset+bytes-d->memory_offset)d->dirty=0;
        else hw_copy_depth_to_memory(d);
    }
}
static void hw_cpu_write_begin(uint32_t address,size_t bytes,int overwrite)
{
    if(!s_hw_initialized) return;
    hw_enter();
    if(hw_pending_output(NULL,0,0))hw_collect_queries(1);
    uintptr_t p=(uintptr_t)xbox_GetMemoryOffset()+address;
    for(HwMemory *m=s_hw_memories;m;m=m->next) for(unsigned i=0;i<m->views.view_count;i++) {
        uintptr_t b=(uintptr_t)m->views.cpu_views[i];
        if(p>=b && p-b<m->views.view_size && bytes<=m->views.view_size-(p-b)) {
            size_t offset=p-b;
            hw_preserve_cpu_write(m,offset,bytes,overwrite);
            hw_mark_cpu_write(m,offset,bytes,NULL);hw_leave();return;
        }
    }
    hw_leave();
}
int nv2a_pb_host_write_begin(void *address,size_t bytes)
{
    if(!s_hw_initialized || !bytes) return 0;
    hw_enter();
    uintptr_t p=(uintptr_t)address;
    for(HwMemory *m=s_hw_memories;m;m=m->next) for(unsigned i=0;i<m->views.view_count;i++) {
        uintptr_t b=(uintptr_t)m->views.cpu_views[i];
        if(p>=b && p-b<m->views.view_size && bytes<=m->views.view_size-(p-b)) {
            size_t lo=(p-b)&~(size_t)4095,hi=(p-b+bytes+4095)&~(size_t)4095;
            if(hw_pending_output(m,lo,hi-lo))hw_collect_queries(1);
            /* Host reads may be short or fail: preserve every dirty owner on
             * pages made writable, without publishing unrelated surfaces. */
            hw_preserve_cpu_write(m,p-b,bytes,0);
            hw_release_cpu_write_range(m,p-b,bytes);
            for(HwColor *c=s_hw_colors;c;c=c->next) if(c->memory==m && hw_ranges_overlap(lo,hi-lo,c->memory_offset,hw_color_bytes(c))) {
                size_t color_lo=c->memory_offset&~(size_t)4095,color_hi=(c->memory_offset+hw_color_bytes(c)+4095)&~(size_t)4095;
                size_t begin=lo>color_lo?lo:color_lo,end=hi<color_hi?hi:color_hi;
                hw_mark_cpu_write(m,begin,end-begin,NULL);
            }
            for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->memory==m && hw_ranges_overlap(lo,hi-lo,d->memory_offset,hw_depth_bytes(d)))hw_mark_cpu_write(m,lo,hi-lo,NULL);
            return 1;
        }
    }
    hw_leave();return 0;
}
void nv2a_pb_host_write_end(int held) { if(held) hw_leave(); }
int nv2a_pb_handle_memory_fault(uintptr_t address,int is_write)
{
    hw_enter();
    if(!s_hw_initialized || !hw_enabled()) {hw_leave();return 0;}
    HwColor *owner=NULL;HwMemory *memory=NULL;size_t offset=0;int texture_fault=0;
    for(HwColor *c=s_hw_colors;c && !owner;c=c->next) {
        size_t lo=c->memory_offset&~(size_t)4095,hi=(c->memory_offset+hw_color_bytes(c)+4095)&~(size_t)4095;
        for(unsigned i=0;i<c->memory->views.view_count;i++) {
            uintptr_t b=(uintptr_t)c->memory->views.cpu_views[i];
            if(address>=b+lo && address<b+hi) { owner=c;offset=address-b;break; }
        }
    }
    int depth_owner=0;
    if(owner) memory=owner->memory;
    for(HwDepth *d=s_hw_depths;d && !memory;d=d->next) {
        size_t lo=d->memory_offset&~(size_t)4095,hi=(d->memory_offset+hw_depth_bytes(d)+4095)&~(size_t)4095;
        for(unsigned i=0;i<d->memory->views.view_count;i++) {
            uintptr_t b=(uintptr_t)d->memory->views.cpu_views[i];
            if(address>=b+lo && address<b+hi){memory=d->memory;offset=address-b;depth_owner=1;break;}
        }
    }
    for(size_t n=0;n<s_hw_query_count && !memory;n++) {
        HwQuery *q=&s_hw_queries[n];if(!q->memory) continue;
        size_t lo=q->offset&~(size_t)4095,hi=(q->offset+4+4095)&~(size_t)4095;
        for(unsigned i=0;i<q->memory->views.view_count;i++) {
            uintptr_t b=(uintptr_t)q->memory->views.cpu_views[i];
            if(address>=b+lo && address<b+hi) {memory=q->memory;offset=address-b;break;}
        }
    }
    if(is_write) for(HwMemory *m=s_hw_memories;m && !memory;m=m->next)
        for(unsigned i=0;i<m->views.view_count;i++) {
            uintptr_t b=(uintptr_t)m->views.cpu_views[i];
            if(address>=b && address-b<m->views.view_size &&
               m->write_guard_protections[((address-b)/4096)*m->views.view_count+i]) {
                memory=m;offset=address-b;texture_fault=1;break;
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
    /* An owned write guard was found after excluding all colour and pending
     * output pages. Its write cannot require an unrelated output completion. */
    if(owner || depth_owner) hw_publish_all();else if(!texture_fault) hw_collect_queries(1);
    if(is_write) {
        if(texture_fault) hw_release_cpu_write_range(memory,offset&~(size_t)4095,4096);
        else if(owner && offset>=owner->memory_offset && offset<owner->memory_offset+hw_color_bytes(owner)) hw_mark_cpu_write(owner->memory,owner->memory_offset,hw_color_bytes(owner),NULL);
        else hw_mark_cpu_write(memory,offset&~(size_t)4095,4096,NULL);
        ++s_gpu.hardware_cpu_write_faults;
    } else ++s_gpu.hardware_cpu_read_faults;
    gpu_timing_finish(started,&s_gpu.hardware_cpu_access_ticks);hw_leave();return 1;
}
void nv2a_pb_release_memory_views(void)
{
    if(!s_hw_initialized) return;
    hw_enter();hw_publish_all();
    hw_free_vertex_batches();
    hw_free_textures();
    for(HwColor *c=s_hw_colors;c;) {
        HwColor *next=c->next;if(c->height) hw_protect(c->memory,c->memory_offset,hw_color_bytes(c),PAGE_READWRITE,NULL);
        if(c->native_host)nvk_image_destroy(c->native_host);
        if(c->native_surface)nvk_image_destroy(c->native_surface);
        if(c->host_surface)c->host_surface->lpVtbl->Release(c->host_surface);
        if(c->surface) c->surface->lpVtbl->Release(c->surface);free(c);c=next;
    }
    s_hw_colors=NULL;
    for(HwDepth *d=s_hw_depths;d;d=d->next){d->dirty=0;d->upload_needed=1;hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_READWRITE,NULL);}
    hw_free_depths(s_hw_depths);s_hw_depths=NULL;
    for(HwMemory *m=s_hw_memories;m;) { HwMemory *next=m->next;
        hw_release_cpu_write_range(m,0,m->views.view_size);
        UnmapViewOfFile(m->host_view);free(m->write_guard_protections);free(m);m=next;
    }
    s_hw_memories=NULL;
    if(s_hw_clear_vs) { ID3D11VertexShader_Release(s_hw_clear_vs);s_hw_clear_vs=NULL; }
    if(s_hw_clear_cb) { ID3D11Buffer_Release(s_hw_clear_cb);s_hw_clear_cb=NULL; }
    if(s_hw_clear_rs) { ID3D11RasterizerState_Release(s_hw_clear_rs);s_hw_clear_rs=NULL; }
    for(unsigned i=0;i<4;i++) if(s_hw_clear_ds[i]) { ID3D11DepthStencilState_Release(s_hw_clear_ds[i]);s_hw_clear_ds[i]=NULL; }
    hw_leave();
}
static HwColor *hw_color_target(void)
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
        unsigned scale=d3d8_RenderScale();
        if(d3d8_IsNativeVulkan()) {
            nvk_image_destroy(color->native_host);nvk_image_destroy(color->native_surface);
            color->native_host=NULL;
            color->native_surface=hw_vk_image(nvk_image_create(width,height,1,VK_FORMAT_B8G8R8A8_UNORM,0),"physical render target");
            if(scale>1)color->native_host=hw_vk_image(nvk_image_create(width*scale,height*scale,1,VK_FORMAT_B8G8R8A8_UNORM,0),"scaled render target");
        } else {
        IDirect3DDevice8 *device=d3d8_GetDevice();
        hw_check(device->lpVtbl->CreateRenderTarget(device,width,height,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->surface),"physical render target");
        if(scale>1) {
            if(width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale)hw_check(E_INVALIDARG,"scaled colour extent");
            hw_check(device->lpVtbl->CreateRenderTarget(device,width*scale,height*scale,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&color->host_surface),"scaled render target");
            fprintf(stderr,"[GPU RESOLUTION] colour %08X guest=%ux%u host=%ux%u\n",address,width,height,width*scale,height*scale);
        }
        }
        color->upload_needed=1;
    }
    return color;
}
static HwColor *hw_color(uint32_t cleared_channels)
{
    HwColor *color=hw_color_target();
    for(HwDepth *d=s_hw_depths;d;d=d->next)if(d->memory==color->memory && d->dirty && hw_ranges_overlap(d->memory_offset,hw_depth_bytes(d),color->memory_offset,hw_color_bytes(color)))hw_copy_depth_to_memory(d);
    if(color->upload_needed)hw_upload_color(color,cleared_channels);
    return color;
}
static void hw_complete_commands_locked(void)
{
    uint64_t start=gpu_timing_clock();
    if(d3d8_IsNativeVulkan()) {
        hw_vk_check(nvk_wait(nvk_fence()),"command completion");
        ++s_gpu.hardware_gpu_completions;gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);return;
    }
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    if(!s_hw_completion) { D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&s_hw_completion),"command completion event"); }
    ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous *)s_hw_completion);
    HRESULT hr;
    do { hr=ID3D11DeviceContext_GetData(ctx,(ID3D11Asynchronous *)s_hw_completion,NULL,0,0);if(hr==S_FALSE) SwitchToThread(); } while(hr==S_FALSE);
    hw_check(hr,"command completion result");++s_gpu.hardware_gpu_completions;gpu_timing_finish(start,&s_gpu.hardware_gpu_completion_ticks);
}
static void hw_complete_commands(void)
{
    if(!s_hw_initialized)return;
    hw_enter();hw_complete_commands_locked();hw_leave();
}

/* The finished image belongs to the existing context. The window never takes
 * PFIFO while holding this context lock, or accesses guest render state. */
static IDirect3DSurface8 *s_hw_front;
static NvkImage *s_hw_vk_front;
static unsigned s_hw_vk_front_width,s_hw_vk_front_height;
static int s_hw_native_present;
static void hw_front_size(unsigned width,unsigned height)
{
    if(d3d8_IsNativeVulkan()) {
        if(s_hw_vk_front && width==s_hw_vk_front_width && height==s_hw_vk_front_height)return;
        nvk_image_destroy(s_hw_vk_front);s_hw_vk_front=NULL;
        s_hw_vk_front_width=width;s_hw_vk_front_height=height;
        if(width && height)s_hw_vk_front=hw_vk_image(nvk_image_create(width,height,1,VK_FORMAT_B8G8R8A8_UNORM,0),"finished display surface");
        return;
    }
    D3D8Surface *front=(D3D8Surface *)s_hw_front;
    if(front && front->width==width && front->height==height)return;
    if(front)front->iface.lpVtbl->Release(&front->iface);
    s_hw_front=NULL;
    if(width && height) {
        IDirect3DDevice8 *device=d3d8_GetDevice();
        hw_check(device->lpVtbl->CreateRenderTarget(device,width,height,D3DFMT_LIN_A8R8G8B8,D3DMULTISAMPLE_NONE,TRUE,&s_hw_front),"finished display surface");
    }
}
static void hw_finish_native_frame(uint32_t address,uint32_t pitch)
{
    hw_enter();
    XboxSharedMemoryViews views;
    unsigned scale=d3d8_RenderScale(),width=d3d8_GetBackbufferWidth(),height=d3d8_GetBackbufferHeight();
    if(!xbox_GetSharedMemoryViews((uint8_t *)xbox_GetMemoryOffset()+address,(size_t)height*pitch,&views))
        hw_check(E_INVALIDARG,"finished display identity");
    HwColor *color;
    for(color=s_hw_colors;color;color=color->next)
        if(color->memory->views.section==views.section && color->memory_offset==views.offset && color->pitch==pitch)break;
    if(!color || color->width<width || color->height<height) {
        fprintf(stderr,"[DISPLAY] address=%08X pitch=%u expected=%ux%u actual=%ux%u\n",address,pitch,width,height,color?color->width:0,color?color->height:0);
        hw_check(E_INVALIDARG,"finished display owner");
    }
    if(color->upload_needed)hw_upload_color(color,0);
    hw_front_size(width*scale,height*scale);
    if(d3d8_IsNativeVulkan()) {
        int32_t bounds[4]={0,0,width*scale,height*scale};
        hw_vk_check(nvk_image_copy_rect(hw_vk_render(color),s_hw_vk_front,bounds,bounds,0),"finished display copy");
    } else {
    D3D11_BOX box={0,0,0,width*scale,height*scale,1};
    ID3D11DeviceContext_CopySubresourceRegion(d3d8_GetD3D11Context(),
        (ID3D11Resource *)((D3D8Surface *)s_hw_front)->d3d11_texture,0,0,0,0,
        (ID3D11Resource *)((D3D8Surface *)hw_render_surface(color))->d3d11_texture,0,&box);
    }
    /* Preserve the original flip's real GPU completion before acknowledgement.
     * RAM publication remains demand driven by the existing memory guards. */
    hw_complete_commands_locked();hw_collect_queries(1);hw_leave();
}
static int hw_display_native(void *window,unsigned width,unsigned height,const int32_t bounds[4])
{
    if(!width || !height)return 0;
    hw_context_enter();
    if(s_hw_vk_front) {
        int result=nvk_window_present(window,s_hw_vk_front,width,height,bounds,d3d8_DisplayVSync());
        hw_vk_check(result,"native window present");
        LeaveCriticalSection(&s_hw_lock);return result==1;
    }
    int ready=s_hw_front!=NULL;
    if(ready) {
        if(d3d8_window_needs_resize(width,height))hw_complete_commands_locked();
        RECT rectangle={bounds[0],bounds[1],bounds[2],bounds[3]};
        hw_check(d3d8_window_draw((HWND)window,s_hw_front,&rectangle,width,height),"native window draw");
    }
    /* DXGI Present also accesses the immediate context. Keep it serialized;
     * the nonblocking swap avoids waiting here for desktop pacing. */
    HRESULT result=S_FALSE;
    if(ready) {result=d3d8_window_present();hw_check(result,"native window present");}
    LeaveCriticalSection(&s_hw_lock);return result!=S_FALSE;
}
static int hw_capture_native(void)
{
    hw_context_enter();
    if(s_hw_vk_front) {
        uint32_t *pixels=malloc((size_t)s_hw_vk_front_width*s_hw_vk_front_height*4);
        if(!pixels)hw_check(E_OUTOFMEMORY,"finished capture pixels");
        hw_vk_check(nvk_image_read(s_hw_vk_front,pixels,s_hw_vk_front_width*4),"finished display capture");
        xbox_FramebufferWindowPresentHost(pixels,s_hw_vk_front_width*4,s_hw_vk_front_width,s_hw_vk_front_height);free(pixels);
    } else if(s_hw_front) {
        D3D8Surface *front=(D3D8Surface *)s_hw_front;D3DLOCKED_RECT pixels;
        hw_check(s_hw_front->lpVtbl->LockRect(s_hw_front,&pixels,NULL,D3DLOCK_READONLY),"finished display capture");
        xbox_FramebufferWindowPresentHost(pixels.pBits,pixels.Pitch,front->width,front->height);
        hw_check(s_hw_front->lpVtbl->UnlockRect(s_hw_front),"finished display capture unlock");
    }
    LeaveCriticalSection(&s_hw_lock);return 1;
}
static int hw_restore_native(const uint32_t *pixels,unsigned width,unsigned height)
{
    hw_context_enter();
    if(pixels)hw_initialize();
    hw_front_size(pixels?width:0,pixels?height:0);
    if(pixels && s_hw_vk_front)hw_vk_check(nvk_image_upload(s_hw_vk_front,pixels,width*4),"finished display restore");
    else if(pixels)ID3D11DeviceContext_UpdateSubresource(d3d8_GetD3D11Context(),
        (ID3D11Resource *)((D3D8Surface *)s_hw_front)->d3d11_texture,0,NULL,pixels,width*4,0);
    LeaveCriticalSection(&s_hw_lock);return 1;
}
static void hw_close_native(void)
{
    hw_context_enter();
    if(s_hw_initialized)hw_complete_commands_locked();
    if(d3d8_IsNativeVulkan())nvk_window_close();else d3d8_window_release();
    hw_front_size(0,0);
    LeaveCriticalSection(&s_hw_lock);
}
const FramebufferNativeRenderer *nv2a_pb_native_renderer(void)
{
    static const FramebufferNativeRenderer renderer={hw_display_native,hw_capture_native,hw_restore_native,hw_close_native};
    s_hw_native_present=hw_enabled() && getenv("RECOMP_FB_WINDOW") && !getenv("RECOMP_FB_VA");
    return s_hw_native_present?&renderer:NULL;
}
/* Transfer the existing native depth image, never a second depth cache. */
static int hw_depth_transfer(HwDepth *d,void *pixels,int upload,unsigned read_step)
{
    if(!read_step || (upload && read_step!=1) || (d->width*d3d8_RenderScale())%read_step || (d->height*d3d8_RenderScale())%read_step)hw_check(E_INVALIDARG,"depth sampling step");
    if(d->native)return nvk_depth_transfer(d->native,pixels,upload,read_step);
    ID3D11Device *device=d3d8_GetD3D11Device();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    unsigned scale=d3d8_RenderScale(),width=d->width*scale,height=d->height*scale;
    D3D11_TEXTURE2D_DESC desc={0};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    if(upload) {
        ID3D11Texture2D *texture=NULL;ID3D11DepthStencilView *view=NULL;
        D3D11_SUBRESOURCE_DATA data={pixels,width*8,0};
        HRESULT hr=ID3D11Device_CreateTexture2D(device,&desc,&data,&texture);
        if(SUCCEEDED(hr))hr=ID3D11Device_CreateDepthStencilView(device,(ID3D11Resource *)texture,NULL,&view);
        if(FAILED(hr)){if(texture)ID3D11Texture2D_Release(texture);return 0;}
        ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
        if(d->view)ID3D11DepthStencilView_Release(d->view);if(d->texture)ID3D11Texture2D_Release(d->texture);
        d->texture=texture;d->view=view;return 1;
    }
    desc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging=NULL;HRESULT hr=ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging);if(FAILED(hr))return 0;
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)d->texture);
    D3D11_MAPPED_SUBRESOURCE map;hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&map);
    if(SUCCEEDED(hr)) {
        if(read_step==1)for(unsigned y=0;y<height;y++)memcpy((uint8_t *)pixels+(size_t)y*width*8,(uint8_t *)map.pData+(size_t)y*map.RowPitch,width*8);
        else {size_t k=0;for(unsigned y=0;y<height;y+=read_step)for(unsigned x=0;x<width;x+=read_step,++k)memcpy((uint8_t *)pixels+k*8,(uint8_t *)map.pData+(size_t)y*map.RowPitch+x*8,8);}
        ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);
    }
    ID3D11Texture2D_Release(staging);return SUCCEEDED(hr);
}
static size_t hw_depth_offset(const HwDepth *d,unsigned x,unsigned y)
{
    unsigned bpp=d->format==1?2:4;
    return color_pixel_offset(d->surface_format,d->pitch,bpp,x,y);
}
static void hw_upload_depth(HwDepth *d)
{
    if(!d->upload_needed)return;
    if(d->dirty)hw_copy_depth_to_memory(d);
    unsigned scale=d3d8_RenderScale(),width=d->width*scale,height=d->height*scale;
    uint32_t *pixels=malloc((size_t)width*height*8);if(!pixels)hw_check(E_OUTOFMEMORY,"depth upload storage");
    const uint8_t *mem=d->memory->host_view+d->memory_offset;float maximum=d->format==1?65536.0f:16777216.0f;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
        size_t at=hw_depth_offset(d,x/scale,y/scale);uint32_t raw=d->format==1?*(const uint16_t *)(mem+at):*(const uint32_t *)(mem+at);
        float z=(float)(d->format==1?raw:raw>>8)/maximum;size_t k=((size_t)y*width+x)*2;
        memcpy(pixels+k,&z,4);pixels[k+1]=d->format==1?0:raw&255;
    }
    hw_vk_check(hw_depth_transfer(d,pixels,1,1),"depth RAM upload");free(pixels);d->upload_needed=0;
    hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_READONLY,NULL);
}
static void hw_copy_depth_to_memory(HwDepth *d)
{
    if(!d->dirty)return;
    unsigned scale=d3d8_RenderScale(),width=d->width,height=d->height;
    uint32_t *pixels=malloc((size_t)width*height*8);if(!pixels)hw_check(E_OUTOFMEMORY,"depth readback storage");
    hw_vk_check(hw_depth_transfer(d,pixels,0,scale),"depth RAM readback");
    uint8_t *mem=d->memory->host_view+d->memory_offset;float maximum=d->format==1?65536.0f:16777216.0f;
    for(unsigned y=0;y<d->height;y++)for(unsigned x=0;x<d->width;x++) {
        size_t k=((size_t)y*width+x)*2,at=hw_depth_offset(d,x,y);float z;memcpy(&z,pixels+k,4);
        uint32_t raw=(uint32_t)fmin((double)maximum-1,floor(fmax(0,(double)z*maximum)+0.5));
        if(d->format==1)*(uint16_t *)(mem+at)=(uint16_t)raw;else *(uint32_t *)(mem+at)=(raw<<8)|(pixels[k+1]&255);
    }
    free(pixels);d->dirty=0;hw_invalidate_texture_range(d->memory,d->memory_offset,hw_depth_bytes(d));
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->memory==d->memory && hw_ranges_overlap(c->memory_offset,hw_color_bytes(c),d->memory_offset,hw_depth_bytes(d)))c->upload_needed=1;
    for(HwDepth *other=s_hw_depths;other;other=other->next)if(other!=d && other->memory==d->memory && hw_ranges_overlap(other->memory_offset,hw_depth_bytes(other),d->memory_offset,hw_depth_bytes(d)))other->upload_needed=1;
}
static void hw_begin_depth_write(HwDepth *d)
{
    if(!d)return;
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->memory==d->memory && c->dirty && hw_ranges_overlap(c->memory_offset,hw_color_bytes(c),d->memory_offset,hw_depth_bytes(d))){hw_copy_color_to_memory(c);d->upload_needed=1;}
    hw_upload_depth(d);d->dirty=1;hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_NOACCESS,NULL);
}
static void hw_depth_draw_write(HwDepth *d)
{
    if(!d)return;
    const DWORD *rs=d3d8_GetRenderStates();
    int zwrite=rs[D3DRS_ZENABLE] && rs[D3DRS_ZWRITEENABLE];
    int swrite=rs[D3DRS_STENCILENABLE] && (rs[D3DRS_STENCILWRITEMASK]&255) &&
        (rs[D3DRS_STENCILFAIL]!=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP || rs[D3DRS_STENCILZFAIL]!=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP || rs[D3DRS_STENCILPASS]!=NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP);
    if(!zwrite && !swrite)return;
    if(d->dirty)return; /* All protection writers preserve dirty-depth ownership. */
    d->dirty=1;hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_NOACCESS,NULL);
}
static HwDepth *hw_depth(void)
{
    hw_initialize();
    uint32_t width=color_width(s_gpu.format,s_gpu.pitch,surface_bpp()),height=color_height(s_gpu.format,s_gpu.clip_y+s_gpu.clip_h);
    uint32_t format=(s_gpu.format>>4)&15,bpp=format==1?2:4;
    uint32_t layout=s_gpu.format&(NV097_SET_SURFACE_FORMAT_TYPE|NV097_SET_SURFACE_FORMAT_WIDTH|NV097_SET_SURFACE_FORMAT_HEIGHT);
    uint32_t pitch=color_swizzled(s_gpu.format)?width*bpp:s_gpu.zeta_pitch;
    if((format!=1 && format!=2) || !width || !height || pitch<width*bpp || width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/d3d8_RenderScale() || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/d3d8_RenderScale())hw_check(E_INVALIDARG,"depth surface extent/format");
    uint32_t address=dma_resolve(nv2a_get_state()->pgraph.dma_zeta,s_gpu.zeta_offset,(size_t)pitch*height);
    HwDepth *d;for(d=s_hw_depths;d;d=d->next)if(d->address==address)break;
    if(!d){d=calloc(1,sizeof *d);if(!d)hw_check(E_OUTOFMEMORY,"depth identity");d->address=address;d->next=s_hw_depths;s_hw_depths=d;}
    int reinterpret=d->pitch!=pitch || d->format!=format || d->surface_format!=layout || d->width!=width;
    if(!d->memory || reinterpret || d->height<height) {
        hw_publish_all();
        if(d->native){nvk_image_destroy(d->native);d->native=NULL;}
        if(d->view){ID3D11DepthStencilView_Release(d->view);d->view=NULL;}
        if(d->texture){ID3D11Texture2D_Release(d->texture);d->texture=NULL;}
        if(d->memory){d->upload_needed=1;hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_READWRITE,NULL);}
        d->width=width;d->height=height;d->format=format;d->pitch=pitch;d->surface_format=layout;
        XboxSharedMemoryViews views;if(!xbox_GetSharedMemoryViews((uint8_t *)xbox_GetMemoryOffset()+address,hw_depth_bytes(d),&views))hw_check(E_INVALIDARG,"depth shared identity");
        d->memory=hw_memory(&views);d->memory_offset=views.offset;d->upload_needed=1;
        if(d3d8_IsNativeVulkan())d->native=hw_vk_image(nvk_image_create(width*d3d8_RenderScale(),height*d3d8_RenderScale(),1,VK_FORMAT_D32_SFLOAT_S8_UINT,0),"physical depth target");
    }
    for(HwDepth *other=s_hw_depths;other;other=other->next)if(other!=d && other->memory==d->memory && other->dirty && hw_ranges_overlap(other->memory_offset,hw_depth_bytes(other),d->memory_offset,hw_depth_bytes(d))){hw_copy_depth_to_memory(other);d->upload_needed=1;}
    for(HwColor *c=s_hw_colors;c;c=c->next)if(c->memory==d->memory && c->dirty && hw_ranges_overlap(c->memory_offset,hw_color_bytes(c),d->memory_offset,hw_depth_bytes(d))){hw_copy_color_to_memory(c);d->upload_needed=1;}
    hw_upload_depth(d);return d;
}
#include "nv2a_scaled_checkpoint.inc"
typedef struct HwDepthCheckpoint { uint32_t address,width,height,format,scale,pitch,surface_format,normalization;uint64_t bytes; } HwDepthCheckpoint;
static int hw_save_depths(FILE *out) {
    uint32_t count=0;for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->texture || d->native) count++;
    if(fwrite(&count,4,1,out)!=1) return 0;
    ID3D11Device *device=d3d8_GetD3D11Device();ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    for(HwDepth *d=s_hw_depths;d;d=d->next) if(d->texture || d->native) {
        hw_upload_depth(d);
        unsigned scale=d3d8_RenderScale();
        HwDepthCheckpoint header={d->address,d->width,d->height,d->format,scale,d->pitch,d->surface_format,d->format==1?65536u:16777216u,(uint64_t)d->width*d->height*scale*scale*8};
        if(d->native) {
            void *pixels=malloc((size_t)header.bytes);if(!pixels)return 0;
            int ok=nvk_depth_transfer(d->native,pixels,0,1) && fwrite(&header,sizeof header,1,out)==1 && fwrite(pixels,(size_t)header.bytes,1,out)==1;
            free(pixels);if(!ok)return 0;continue;
        }
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
    while(depth) { HwDepth *next=depth->next;if(depth->native)nvk_image_destroy(depth->native);
        if(depth->view)ID3D11DepthStencilView_Release(depth->view);
        if(depth->texture)ID3D11Texture2D_Release(depth->texture);free(depth);depth=next; }
}
static int hw_prepare_depths(FILE *in,HwDepth **result,HwColor **colors) {
    *colors=NULL;*result=NULL;uint32_t count;if(fread(&count,4,1,in)!=1) return 0;
    if(count) hw_initialize();
    HwDepth **next=result;
    for(uint32_t i=0;i<count;i++) {
        HwDepthCheckpoint header;
        if(fread(&header,sizeof header,1,in)!=1 || !header.width || !header.height ||
            header.width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/d3d8_RenderScale() || header.height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/d3d8_RenderScale() ||
            (header.format!=1 && header.format!=2) || header.normalization!=(header.format==1?65536u:16777216u) || header.scale!=d3d8_RenderScale() || header.bytes!=(uint64_t)header.width*header.height*header.scale*header.scale*8 || header.bytes>SIZE_MAX) goto failed;
        void *bytes=malloc((size_t)header.bytes);if(!bytes) goto failed;
        if(fread(bytes,(size_t)header.bytes,1,in)!=1) {free(bytes);goto failed;}
        HwDepth *d=calloc(1,sizeof *d);if(!d){free(bytes);goto failed;}
        *next=d;next=&d->next;d->address=header.address;d->width=header.width;d->height=header.height;d->format=header.format;d->pitch=header.pitch;d->surface_format=header.surface_format;
        XboxSharedMemoryViews views;
        if(d->pitch<d->width*(d->format==1?2u:4u) || !xbox_GetSharedMemoryViews((uint8_t *)xbox_GetMemoryOffset()+d->address,hw_depth_bytes(d),&views)){free(bytes);goto failed;}
        d->memory=hw_memory(&views);d->memory_offset=views.offset;
        if(d3d8_IsNativeVulkan()) {
            d->native=nvk_image_create(d->width*header.scale,d->height*header.scale,1,VK_FORMAT_D32_SFLOAT_S8_UINT,0);
            int ok=d->native && nvk_depth_transfer(d->native,bytes,1,1);free(bytes);
            if(!ok)goto failed;continue;
        }
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
    if(s_hw_initialized && !d3d8_IsNativeVulkan()) ID3D11DeviceContext_OMSetRenderTargets(d3d8_GetD3D11Context(),0,NULL,NULL);
    for(HwDepth *d=s_hw_depths;d;d=d->next){d->dirty=0;d->upload_needed=1;hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_READWRITE,NULL);}
    hw_free_depths(s_hw_depths);s_hw_depths=depths;
    for(HwDepth *d=s_hw_depths;d;d=d->next)hw_protect(d->memory,d->memory_offset,hw_depth_bytes(d),PAGE_READONLY,NULL);
    for(HwColor *color=s_hw_colors;color;color=color->next) { color->dirty=0;color->upload_needed=1;
        if(color->height)hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_READWRITE,NULL); }
    hw_free_colors(s_hw_colors);s_hw_colors=colors;
    for(HwColor *color=s_hw_colors;color;color=color->next)hw_upload_native_color(color);
    hw_free_vertex_batches();
    hw_free_textures();
    s_hw_count=0;s_hw_program_active=0;s_hw_fixed_active=0;s_hw_output_active=0;
}
static void hw_clear_depth_rect(HwDepth *depth,ClearBounds bounds,UINT flags,float z,UINT8 stencil)
{
    if(depth->native) {
        unsigned scale=d3d8_RenderScale();
        int32_t rect[4]={bounds.x0*scale,bounds.y0*scale,bounds.x1*scale,bounds.y1*scale};
        VkImageAspectFlags mask=((flags&D3D11_CLEAR_DEPTH)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|((flags&D3D11_CLEAR_STENCIL)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
        hw_vk_check(nvk_depth_clear(depth->native,mask,z,stencil,rect),"partial depth/stencil clear");return;
    }
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
    hw_begin_depth_write(depth);
    float scale=depth->format==1?65536.0f:16777216.0f;
    float normalized_depth=zclear_value()/scale;
    if(!bounds.x0 && !bounds.y0 && bounds.x1==depth->width && bounds.y1==depth->height)
        if(depth->native) {
            VkImageAspectFlags mask=((flags&D3D11_CLEAR_DEPTH)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|((flags&D3D11_CLEAR_STENCIL)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
            hw_vk_check(nvk_depth_clear(depth->native,mask,normalized_depth,(UINT8)s_gpu.zstencil_clear,NULL),"depth/stencil clear");
        } else ID3D11DeviceContext_ClearDepthStencilView(d3d8_GetD3D11Context(),depth->view,flags,normalized_depth,(UINT8)s_gpu.zstencil_clear);
    else {
        static unsigned log_count;
        if(log_count++<8) fprintf(stderr,"[DEPTH RECT] address=%08X size=%ux%u bounds=%u,%u,%u,%u flags=%u value=%08X\n",depth->address,depth->width,depth->height,bounds.x0,bounds.y0,bounds.x1,bounds.y1,flags,s_gpu.zstencil_clear);
        hw_clear_depth_rect(depth,bounds,flags,normalized_depth,(UINT8)s_gpu.zstencil_clear);
    }
    hw_leave();
}
static void hw_begin_color_write(HwColor *color)
{
    for(HwDepth *d=s_hw_depths;d;d=d->next)if(d->memory==color->memory && hw_ranges_overlap(d->memory_offset,hw_depth_bytes(d),color->memory_offset,hw_color_bytes(color))) {
        if(d->dirty)hw_copy_depth_to_memory(d);d->upload_needed=1;
    }
    if(!color->dirty)hw_protect(color->memory,color->memory_offset,hw_color_bytes(color),PAGE_NOACCESS,NULL);
    for(HwColor *other=s_hw_colors;other;other=other->next) if(other!=color && other->memory==color->memory && hw_ranges_overlap(other->memory_offset,hw_color_bytes(other),color->memory_offset,hw_color_bytes(color))) other->upload_needed=1;
}
static void hw_end_color_write(HwColor *color,RECT bounds)
{
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
}
static int hw_clear_full_color(void)
{
    hw_enter();
    /* Keep the original full clear's completion boundary before changing ownership. */
    if(hw_pending_output(NULL,0,0))hw_collect_queries(1);
    HwColor *color=hw_color_target();
    ClearBounds bounds=clear_bounds();
    /* A retained taller target still needs its untouched rows. */
    if(bounds.x1!=color->width || bounds.y1!=color->height) {hw_leave();return 0;}
    hw_begin_color_write(color);
    hw_clear_color_image(color);
    color->upload_needed=0;
    hw_end_color_write(color,(RECT){0,0,(LONG)color->width,(LONG)color->height});
    hw_leave();return 1;
}
static void hw_colour_cleared(uint32_t param)
{
    if(!s_hw_initialized) return;
    hw_enter();(void)hw_color(param);hw_leave();
}
static void hw_texture_bind(unsigned stage,HwColor *current_target)
{
    Texture *source=&s_gpu.texs[stage];
    unsigned mode=(s_gpu.rc.stage_program>>(stage*5))&31;
    ID3D11DeviceContext *context=d3d8_GetD3D11Context();
    if(!texture_stage_samples(mode)) {
        if(d3d8_IsNativeVulkan())s_hw_vk_textures[stage]=NULL;
        else { ID3D11ShaderResourceView *empty=NULL;
        ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&empty); }
        return;
    }
    if(!source->valid) hw_check(E_NOTIMPL,"enabled texture identity");
    unsigned shadow=texture_shadow_mode(stage),luma=texture_luma_stage(stage),float_texture=shadow||luma;
    unsigned volume=texture_volume(source),depth=texture_depth(source);
    uint32_t stride=tex_face_stride(source);
    size_t bytes=volume?texture_volume_bytes(source):(size_t)stride*(source->cube?6:1);
    if(!bytes) hw_check(E_FAIL,"texture storage span");
    for(HwDepth *d=s_hw_depths;d;d=d->next)if(d->dirty && source->offset<d->address+hw_depth_bytes(d) && d->address<source->offset+bytes)hw_publish_all();
    for(HwColor *color=s_hw_colors;color;color=color->next) {
        if(!volume && !float_texture && !source->cube && !tex_size_from_format(source->color) && color->bpp==4 &&
           source->offset==color->address && source->pitch==color->pitch &&
           source->width==color->width && source->height==color->height) {
            D3D8Surface *surface=(D3D8Surface *)hw_render_surface(color);
            /* Earlier texture stages can complete writes on a shared page. */
            if(current_target->upload_needed)hw_upload_color(current_target,0);
            if(color==current_target)continue;
            if(color->upload_needed)hw_upload_color(color,0);
            if(color->native_surface){s_hw_vk_textures[stage]=hw_vk_render(color);return;}
            if(!surface->scale_srv)hw_check(ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),(ID3D11Resource *)surface->d3d11_texture,NULL,&surface->scale_srv),"render target texture view");
            ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&surface->scale_srv);
            static unsigned reported;
            if(reported++<8)fprintf(stderr,"[GPU TARGET TEXTURE] original=%08X native=%ux%u host=%ux%u\n",color->address,color->width,color->height,surface->width,surface->height);
            return;
        }
    }
    for(HwColor *color=s_hw_colors;color;color=color->next)
        if(source->offset < color->address+color->height*color->pitch && color->address < source->offset+bytes &&
           color->dirty) hw_publish_all();
    unsigned bucket=hw_texture_bucket(source,bytes);
    HwTexture *entry,**link;
    for(link=&s_hw_texture_buckets[bucket];(entry=*link)!=NULL;link=&entry->hash_next)
        if(entry->identity.offset==source->offset && entry->identity.width==source->width && entry->identity.height==source->height && entry->identity.color==source->color && entry->identity.pitch==source->pitch && entry->identity.cube==source->cube && entry->identity.levels==source->levels && texture_volume(&entry->identity)==volume && texture_depth(&entry->identity)==depth && entry->raw_size==bytes) break;
    if(entry && link!=&s_hw_texture_buckets[bucket]) {
        *link=entry->hash_next;entry->hash_next=s_hw_texture_buckets[bucket];s_hw_texture_buckets[bucket]=entry;
    }
    const uint8_t *raw=(uint8_t *)xbox_GetMemoryOffset()+source->offset;
    const uint32_t *palette=source->color==0x0B ? (uint32_t *)((uint8_t *)xbox_GetMemoryOffset()+source->palette) : NULL;
    int created=!entry;
    if(!entry) {
        entry=(HwTexture *)calloc(1,sizeof *entry);
        if(!entry) hw_check(E_OUTOFMEMORY,"texture identity allocation");
        entry->identity=*source; entry->raw_size=bytes; entry->raw=(uint8_t *)malloc(bytes);
        if(!entry->raw) hw_check(E_OUTOFMEMORY,"texture publication storage");
        XboxSharedMemoryViews views;
        if(!xbox_GetSharedMemoryViews(raw,bytes,&views)) hw_check(E_INVALIDARG,"texture shared memory identity");
        entry->memory=hw_memory(&views);entry->memory_offset=views.offset;
        entry->next=s_hw_textures; s_hw_textures=entry;
        entry->hash_next=s_hw_texture_buckets[bucket];s_hw_texture_buckets[bucket]=entry;
    }
    if(palette && (!entry->palette_memory || entry->identity.palette!=source->palette)) {
        if(entry->palette_memory) hw_release_cpu_write_range(entry->palette_memory,entry->palette_offset,sizeof entry->palette);
        XboxSharedMemoryViews views;
        if(!xbox_GetSharedMemoryViews(palette,sizeof entry->palette,&views)) hw_check(E_INVALIDARG,"palette shared memory identity");
        entry->palette_memory=hw_memory(&views);entry->palette_offset=views.offset;
        entry->identity.palette=source->palette;hw_texture_invalidate(entry);
    }
    if(hw_pending_output(entry->memory,entry->memory_offset,bytes) ||
       (palette && hw_pending_output(entry->palette_memory,entry->palette_offset,sizeof entry->palette)))
        hw_collect_queries(1);
    int changed=created;
    if(!entry->content_valid) {
        hw_guard_cpu_write_range(entry->memory,entry->memory_offset,bytes);
        if(palette) hw_guard_cpu_write_range(entry->palette_memory,entry->palette_offset,sizeof entry->palette);
        changed=created || memcmp(entry->raw,raw,bytes)!=0 || (palette && memcmp(entry->palette,palette,sizeof entry->palette)!=0);
    }
    if(changed) {
        unsigned width=source->width,height=source->height;
        const void *asset=(volume || source->cube || float_texture)?NULL:d3d8_TextureAsset(raw,bytes,width,height,&width,&height);
        if((!entry->texture && !entry->native) || width!=entry->host_width || height!=entry->host_height) {
            if(entry->texture)entry->texture->lpVtbl->Release(entry->texture);
            entry->texture=NULL;
            if(d3d8_IsNativeVulkan()) {
                nvk_image_destroy(entry->native);
                entry->native=hw_vk_image(volume?nvk_image_create_volume(width,height,depth,VK_FORMAT_B8G8R8A8_UNORM):nvk_image_create(width,height,source->cube?6:1,float_texture?VK_FORMAT_R32_SFLOAT:VK_FORMAT_B8G8R8A8_UNORM,source->cube),"texture");
            } else if(volume) {
                hw_check(d3d8_CreateVolumeTextureImpl(width,height,depth,1,0,D3DFMT_LIN_A8R8G8B8,(IDirect3DVolumeTexture8 **)&entry->texture),"volume texture");
            } else if(source->cube) {
                if(width!=height)hw_check(E_FAIL,"cube dimensions");
                hw_check(d3d8_CreateCubeTextureImpl(width,1,0,D3DFMT_LIN_A8R8G8B8,(IDirect3DCubeTexture8 **)&entry->texture),"cube texture");
            } else hw_check(d3d8_CreateTextureImpl(width,height,1,0,float_texture?D3DFMT_LIN_R32F:D3DFMT_LIN_A8R8G8B8,(IDirect3DTexture8 **)&entry->texture),"texture");
            entry->host_width=width;entry->host_height=height;
        }
        ++s_gpu.hardware_texture_uploads;
        uint8_t *native_pixels=NULL;
        if(entry->native) {
            native_pixels=malloc((size_t)width*height*4*(volume?depth:source->cube?6:1));
            if(!native_pixels)hw_check(E_OUTOFMEMORY,"native texture pixels");
        }
        if(volume) {
            D3DLOCKED_BOX locked;IDirect3DVolumeTexture8 *texture=(IDirect3DVolumeTexture8 *)entry->texture;
            if(native_pixels){locked.pBits=native_pixels;locked.RowPitch=width*4;locked.SlicePitch=width*height*4;}
            else hw_check(texture->lpVtbl->LockBox(texture,0,&locked,NULL,0),"volume upload lock");
            for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
                uint32_t pixel;if(!sample_tex_value(source,0,x,y,z,0,&pixel,NULL))hw_check(E_NOTIMPL,"volume texture decoder");
                ((uint32_t *)((uint8_t *)locked.pBits+(size_t)z*locked.SlicePitch+(size_t)y*locked.RowPitch))[x]=pixel;
            }
            if(!native_pixels)hw_check(texture->lpVtbl->UnlockBox(texture,0),"volume upload");
        } else
        for(unsigned face=0;face<(source->cube?6u:1u);face++) {
            D3DLOCKED_RECT locked;
            if(native_pixels){locked.pBits=native_pixels+(size_t)face*width*height*4;locked.Pitch=width*4;}
            else if(source->cube) {
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
                if(float_texture) {
                    float value[4];if(!sample_tex_value(source,face*stride,x,y,0,0,&pixel,value))hw_check(E_NOTIMPL,"float texture decoder");
                    ((float *)((uint8_t *)locked.pBits+(size_t)y*locked.Pitch))[x]=value[0];
                } else {
                    if(!sample_tex(source,face*stride,x,y,&pixel)) hw_check(E_NOTIMPL,"original texture decoder");
                    ((uint32_t *)((uint8_t *)locked.pBits+(size_t)y*locked.Pitch))[x]=pixel;
                }
            }
            if(native_pixels)continue;
            if(source->cube) { IDirect3DCubeTexture8 *cube=(IDirect3DCubeTexture8 *)entry->texture; hw_check(cube->lpVtbl->UnlockRect(cube,(D3DCUBEMAP_FACES)face,0),"cube upload"); }
            else { IDirect3DTexture8 *texture=(IDirect3DTexture8 *)entry->texture; hw_check(texture->lpVtbl->UnlockRect(texture,0),"texture upload"); }
        }
        if(native_pixels){hw_vk_check(nvk_image_upload(entry->native,native_pixels,width*4),"texture upload");free(native_pixels);}
        memcpy(entry->raw,raw,bytes);
        if(palette) memcpy(entry->palette,palette,1024);
    }
    if(!entry->content_valid) {
        hw_guard_cpu_write_range(entry->memory,entry->memory_offset,bytes);
        if(palette) hw_guard_cpu_write_range(entry->palette_memory,entry->palette_offset,sizeof entry->palette);
        hw_texture_validate(entry);
    }
    if(entry->native)s_hw_vk_textures[stage]=entry->native;
    else { ID3D11ShaderResourceView *view=d3d8_base_srv(entry->texture);
    ID3D11DeviceContext_PSSetShaderResources(context,stage,1,&view); }
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
        if(d3d8_IsNativeVulkan()) {
            int result;
            if(q->memory) {
                if(wait)hw_vk_check(nvk_wait(q->native_serial),"output completion");
                result=nvk_completed(q->native_serial);
            } else result=nvk_query_result(q->native_query,&pixels,wait);
            hw_vk_check(result>=0,"query output result");hr=result?S_OK:S_FALSE;
        } else do {
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
        if(d3d8_IsNativeVulkan()){if(q->native_query)nvk_query_destroy(q->native_query);}
        else if(q->memory && !s_hw_completion) s_hw_completion=q->query;
        else ID3D11Query_Release(q->query);
        q->query=NULL;q->pending=0;
        if(q->memory) {
            ++s_gpu.hardware_gpu_completions;
            for(HwColor *c=s_hw_colors;c;c=c->next)
                if(c->memory==q->memory && hw_ranges_overlap(c->memory_offset,hw_color_bytes(c),q->offset,4)) hw_copy_color_to_memory(c);
            for(HwDepth *d=s_hw_depths;d;d=d->next)if(d->memory==q->memory && hw_ranges_overlap(d->memory_offset,hw_depth_bytes(d),q->offset,4))hw_copy_depth_to_memory(d);
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
        HwVertexOutput *vertices=(HwVertexOutput *)realloc(s_hw_vertices,(size_t)capacity*sizeof *vertices);
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
        HwVertexOutput *output=&s_hw_vertices[s_hw_count++];
        hw_pack_output(output,input[k]); memcpy(output->pos,pos[k],sizeof output->pos);
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
        HwVertexOutput *vertices=(HwVertexOutput *)realloc(s_hw_vertices,(size_t)capacity*sizeof *vertices);
        if(!vertices) hw_check(E_OUTOFMEMORY,"transformed line batch");
        s_hw_vertices=vertices;s_hw_capacity=capacity;
    }
    const Nv2aVshOutput *input[2]={a,b};
    for(unsigned k=0;k<2;k++) {
        HwVertexOutput *output=&s_hw_vertices[s_hw_count++];hw_pack_output(output,input[k]);
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
static NvkShader *s_hw_vk_geometry[9],*s_hw_vk_fixed,*s_hw_vk_outputs;
static NvkShader *hw_vk_shader(NvkShader *shader,const char *operation)
{ hw_vk_check(shader!=NULL,operation);return shader; }
static VkPrimitiveTopology hw_vk_topology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
    switch(topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    default:hw_check(E_INVALIDARG,"native topology");return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
static VkBlendFactor hw_vk_blend(DWORD factor)
{
    static const VkBlendFactor map[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if(factor>=sizeof map/sizeof *map)hw_check(E_INVALIDARG,"native blend factor");
    return map[factor];
}
static VkStencilOp hw_vk_stencil(DWORD value)
{
    static const VkStencilOp map[]={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_ZERO,VK_STENCIL_OP_REPLACE,
        VK_STENCIL_OP_INCREMENT_AND_CLAMP,VK_STENCIL_OP_DECREMENT_AND_CLAMP,VK_STENCIL_OP_INVERT,
        VK_STENCIL_OP_INCREMENT_AND_WRAP,VK_STENCIL_OP_DECREMENT_AND_WRAP};
    if(value>=sizeof map/sizeof *map)hw_check(E_INVALIDARG,"native stencil operation");
    return map[value];
}
static void hw_vk_draw_state(NvkDraw *draw,HwColor *color,HwDepth *depth)
{
    const DWORD *rs=d3d8_GetRenderStates();
    draw->color=hw_vk_render(color);draw->depth=depth?depth->native:NULL;
    draw->topology=hw_vk_topology(s_hw_topology);
    draw->wireframe=rs[D3DRS_FILLMODE]==D3DFILL_WIREFRAME;
    draw->depth_test=rs[D3DRS_ZENABLE]!=0;draw->depth_write=rs[D3DRS_ZWRITEENABLE]!=0;
    draw->depth_compare=(VkCompareOp)(rs[D3DRS_ZFUNC]-1);
    draw->stencil=rs[D3DRS_STENCILENABLE]!=0;
    draw->stencil_state=(VkStencilOpState){hw_vk_stencil(rs[D3DRS_STENCILFAIL]),hw_vk_stencil(rs[D3DRS_STENCILPASS]),
        hw_vk_stencil(rs[D3DRS_STENCILZFAIL]),(VkCompareOp)(rs[D3DRS_STENCILFUNC]?rs[D3DRS_STENCILFUNC]-1:VK_COMPARE_OP_ALWAYS),
        rs[D3DRS_STENCILMASK]&255,rs[D3DRS_STENCILWRITEMASK]&255,rs[D3DRS_STENCILREF]};
    draw->logic=rs[D3DRS_NV2ALOGICOP]!=0;
    draw->logic_op=(VkLogicOp)(rs[D3DRS_NV2ALOGICOP]&15);
    draw->blend=rs[D3DRS_ALPHABLENDENABLE]!=0 && !draw->logic;draw->source_blend=hw_vk_blend(rs[D3DRS_SRCBLEND]);
    draw->destination_blend=hw_vk_blend(rs[D3DRS_DESTBLEND]);draw->blend_op=(VkBlendOp)(rs[D3DRS_BLENDOP]-1);
    draw->color_mask=rs[D3DRS_COLORWRITEENABLE]&15;
    static const VkSamplerAddressMode address[]={VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    for(unsigned i=0;i<4;i++) {
        const DWORD *tss=d3d8_GetTSS(i);
        draw->textures[i].image=s_hw_vk_textures[i];
        draw->textures[i].u=address[tss[D3DTSS_ADDRESSU]];draw->textures[i].v=address[tss[D3DTSS_ADDRESSV]];
        draw->textures[i].w=address[tss[D3DTSS_NV2AADDRESSW]];
        nv2a_rc_unpack(s_gpu.texs[i].border_color,draw->textures[i].border);
        draw->textures[i].filter=tss[D3DTSS_MINFILTER]==D3DTEXF_LINEAR?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    }
}
static unsigned hw_geometry_kind(unsigned mode)
{
    if(s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_POINTLIST)return 2;
    if(s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ)return s_hw_output_active?6:1;
    return s_hw_output_active?(mode==2?7:mode==1?8:5):(mode==2?3:mode==1?4:0);
}
static NvkShader *hw_vk_geometry_shader(unsigned kind)
{
    static const char *entries[]={"main","main_line","main_point","main_polygon_line","main_polygon_point","main_output","main_output_line","main_output_polygon_line","main_output_polygon_point"};
    if(!s_hw_vk_geometry[kind])s_hw_vk_geometry[kind]=hw_vk_shader(nvk_shader_create(d3d8_vsh_geometry_source(),entries[kind],VK_SHADER_STAGE_GEOMETRY_BIT),"geometry shader");
    return s_hw_vk_geometry[kind];
}
static int hw_outputs_have_constant_w(void)
{
    /* Only original complete outputs are inspectable here. Native GPU-only
     * program/fixed transforms are not executed again for this proof. */
    if(s_hw_program_active && !s_hw_output_active)return 0;
    const HwVertexOutput *outputs=s_hw_vertices;
    unsigned count=s_hw_count;
    if(s_hw_program_active) {
        outputs=(const HwVertexOutput *)s_hw_program_upload.vertex;
        count=s_hw_program_vertex_count;
    }
    if(!count || !isfinite(outputs[0].pos[3]) || outputs[0].pos[3]<=0)return 0;
    for(unsigned i=1;i<count;i++)if(outputs[i].pos[3]!=outputs[0].pos[3])return 0;
    return 1;
}

static void hw_polygon_offset(HwPixelState *native,unsigned mode,int constant_w)
{
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    native->stipple_flags[0]=mode==0 && (pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_STIPPLEENABLE)!=0;
    native->depth_offset[0]=native->depth_offset[1]=0;
    native->depth_offset[2]=mode!=0;
    uint32_t enabled=mode==1?NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE:mode==2?NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE:NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE;
    if(pg->regs[NV_PGRAPH_SETUPRASTER]&enabled) {
        memcpy(&native->depth_offset[0],&pg->regs[NV_PGRAPH_ZOFFSETFACTOR],4);
        memcpy(&native->depth_offset[1],&pg->regs[NV_PGRAPH_ZOFFSETBIAS],4);
        if(native->depth_offset[0]!=0 && (pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)) {
            if(!constant_w || !isfinite(native->depth_offset[0]))hw_check(E_NOTIMPL,"W-buffer polygon slope convention");
            /* The proven finite constant W plane has zero slope; bias remains. */
            native->depth_offset[0]=0;
        }
    }
}
static void hw_bind_pixel_state(ID3D11DeviceContext *ctx,const HwPixelState *native)
{
    if(!s_hw_ps_value_valid || memcmp(&s_hw_ps_value,native,sizeof *native)) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hw_check(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)s_hw_ps_cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"pixel state upload");
        memcpy(mapped.pData,native,sizeof *native);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)s_hw_ps_cb,0);
        memcpy(&s_hw_ps_value,native,sizeof *native);s_hw_ps_value_valid=1;
    }
    ID3D11DeviceContext_PSSetConstantBuffers(ctx,1,1,&s_hw_ps_cb);
}
static void hw_issue_draw(ID3D11DeviceContext *ctx,NvkDraw *draw,unsigned first,unsigned count)
{
    if(d3d8_IsNativeVulkan()) {
        draw->draw_first=first;draw->draw_count=count;draw->query=NULL;
        if(s_gpu.zpass_enable){draw->query=nvk_query_create();hw_vk_check(draw->query!=NULL,"visibility query");}
        hw_vk_check(nvk_draw(draw),"draw");
        s_gpu.hardware_vertex_stream_uploads+=draw->uploaded;s_gpu.hardware_vertex_stream_starts+=draw->upload_started;
        if(draw->query){HwQuery result={0};result.native_query=draw->query;hw_append_query(result);}
    } else if(s_hw_program_active)ID3D11DeviceContext_DrawIndexed(ctx,count,first,0);
    else ID3D11DeviceContext_Draw(ctx,count,first);
}
static unsigned hw_line_copies(const NV2ANativeGeometry *geometry)
{
    return (unsigned)fmaxf(1,floorf(geometry->raster[0]+0.5f))*(unsigned)geometry->raster[3];
}
static void hw_issue_geometry_draw(ID3D11DeviceContext *ctx,NvkDraw *draw,NV2ANativeGeometry *geometry,const HwPixelState *native,unsigned kind,unsigned first,unsigned count)
{
    int lines=kind==1 || kind==3 || kind==6 || kind==7;
    unsigned copies=lines?hw_line_copies(geometry):1;
    for(unsigned copy=0;copy<copies;copy+=32) {
        geometry->raster[2]=(float)copy;
        if(d3d8_IsNativeVulkan())draw->geometry=hw_vk_geometry_shader(kind);
        else {hw_bind_pixel_state(ctx,native);hw_check(d3d8_vsh_bind_geometry(nv2a_vsh_constant(0),geometry,kind),"raster geometry state");}
        hw_issue_draw(ctx,draw,first,count);
    }
}
static void hw_flush_batch(void)
{
    if(!s_hw_count) { s_hw_program_active=s_hw_output_active=0;return; }
    hw_enter();
    uint64_t timing_start=gpu_timing_clock();
    HwColor *color=hw_color(0);
    HwDepth *depth=(s_gpu.depth_test || (nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_1]&NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE))?hw_depth():NULL;
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    NvkDraw native_draw={0};NV2ANativeGeometry geometry={0};NV2ANativeFixed fixed={0};
    int constant_w=0;
    struct PGRAPHState *offset_pg=&nv2a_get_state()->pgraph;
    if(s_gpu.prim>=NV_PRIM_TRIANGLES && (offset_pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)) {
        float factor;memcpy(&factor,&offset_pg->regs[NV_PGRAPH_ZOFFSETFACTOR],4);
        if(factor!=0 && isfinite(factor)) {
            uint32_t raster=offset_pg->regs[NV_PGRAPH_SETUPRASTER];
            unsigned modes[2]={GET_MASK(raster,NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),GET_MASK(raster,NV_PGRAPH_SETUPRASTER_BACKFACEMODE)};
            unsigned enables[3]={NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE,NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE,NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE};
            if((modes[0]<3 && (raster&enables[modes[0]])) || (modes[1]<3 && (raster&enables[modes[1]])))constant_w=hw_outputs_have_constant_w();
        }
    }
    float output_screen[4]={(float)color->width,(float)color->height,0,1};
    if(!d3d8_IsNativeVulkan()) {
    ID3D11RenderTargetView *view=((D3D8Surface *)hw_render_surface(color))->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&view,depth?depth->view:NULL);
    D3D11_VIEWPORT vp={0,0,(FLOAT)(color->width*d3d8_RenderScale()),(FLOAT)(color->height*d3d8_RenderScale()),0,1};
    ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
    }
    gpu_timing_finish(timing_start,&s_gpu.hardware_surface_ticks);
    timing_start=gpu_timing_clock();
    IDirect3DDevice8 *device=d3d8_GetDevice();
    const IDirect3DDevice8Vtbl *v=device->lpVtbl;
    v->SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE);
    DWORD fill_mode=D3DFILL_SOLID;
    int polygon_modes=s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ && hw_polygon_modes_active();
    v->SetRenderState(device,D3DRS_FILLMODE,fill_mode);
    v->SetRenderState(device,D3DRS_ZENABLE,s_gpu.depth_test);
    v->SetRenderState(device,D3DRS_ZFUNC,s_gpu.depth_test?s_gpu.depth_func-0x1FF:D3DCMP_ALWAYS);
    v->SetRenderState(device,D3DRS_ZWRITEENABLE,s_gpu.depth_test && s_gpu.depth_mask);
    uint32_t logic_state=nv2a_get_state()->pgraph.regs[NV_PGRAPH_BLEND];
    v->SetRenderState(device,D3DRS_NV2ALOGICOP,(logic_state&NV_PGRAPH_BLEND_LOGICOP_ENABLE)?
        0x1500|GET_MASK(logic_state,NV_PGRAPH_BLEND_LOGICOP):0);
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
    uint32_t stencil_ops=nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_2];
    uint32_t stencil_write=nv2a_get_state()->pgraph.regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE;
    v->SetRenderState(device,D3DRS_STENCILWRITEMASK,stencil_write?GET_MASK(stencil,NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE):0);
    v->SetRenderState(device,D3DRS_STENCILFAIL,GET_MASK(stencil_ops,NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL));
    v->SetRenderState(device,D3DRS_STENCILZFAIL,GET_MASK(stencil_ops,NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL));
    v->SetRenderState(device,D3DRS_STENCILPASS,GET_MASK(stencil_ops,NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS));
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
    unsigned combiner_count=s_gpu.rc.control&255;if(combiner_count>8)combiner_count=8;
    combiner.num_stages=(int)combiner_count;
    d3d8_combiners_from_render_states(states,&combiner);
    combiner.num_stages=(int)combiner_count;
    combiner.native_coordinates=1; combiner.final_enabled=s_gpu.rc.final0 || s_gpu.rc.final1;
    combiner.final_flags=s_gpu.rc.final1&255; combiner.flags=(s_gpu.rc.control>>8)&1;
    combiner.clip_plane_mode=s_gpu.clip_plane_mode;
    NV2APSConstants constants={0};
    for(unsigned stage=0;stage<(combiner_count?combiner_count:1);stage++) {
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
        if(mode>7 && mode!=13 && mode!=18 && mode!=9 && mode!=11 && mode!=12 && mode!=14 && mode!=17 && mode!=15 && mode!=16) hw_check(E_NOTIMPL,"texture stage operation");
        combiner.tex_mode[stage]=mode==0?NV2A_TEXMODE_NONE:mode>=4?(NV2ATextureMode)mode:mode==3 && s_gpu.texs[stage].cube?NV2A_TEXMODE_CUBEMAP:NV2A_TEXMODE_2D;
        if(mode==2 && texture_volume(&s_gpu.texs[stage])) { combiner.tex_mode[stage]=NV2A_TEXMODE_3D;combiner.volume_mask|=1u<<stage; }
        if(mode==13 || mode==18 || mode==9 || mode==11 || mode==12 || mode==14 || mode==17) {
            combiner.tex_mode[stage]=mode==13?NV2A_TEXMODE_DOT_STR:mode==18?NV2A_TEXMODE_DOT_RFLCT_SPEC_CONST:mode==11?NV2A_TEXMODE_DOT_RFLCT_DIFF:mode==9?NV2A_TEXMODE_DOT_ST:mode==14?NV2A_TEXMODE_DOT_STR_CUBE:mode==12?NV2A_TEXMODE_DOT_RFLCT_SPEC:NV2A_TEXMODE_DOTPRODUCT;
            combiner.dependent_source[stage]=texture_dot_source(stage,mode);
            combiner.dot_mapping[stage]=texture_dot_mapping(stage);
            if(mode==13 && texture_volume(&s_gpu.texs[stage]))combiner.volume_mask|=1u<<stage;
            if(mode==18)texture_eye_vector(constants.bump_matrix[3]);
            if((mode==11 || mode==12 || mode==14 || mode==18) && !s_gpu.texs[stage].cube)combiner.dot_2d_mask|=1u<<stage;
            if(mode==9 || mode==13) {
                const Texture *target=&s_gpu.texs[stage];
                constants.bump_lum_coord[stage][2]=tex_size_from_format(target->color)?1.0f:1.0f/target->width;
                constants.bump_lum_coord[stage][3]=tex_size_from_format(target->color)?1.0f:1.0f/target->height;
            }
        }
        if(mode==6 || mode==7) {
            combiner.tex_mode[stage]=mode==6?NV2A_TEXMODE_BUMP:NV2A_TEXMODE_BUMP_LUM;
            combiner.dependent_source[stage]=texture_bump_source(stage);
            texture_bump_parameters(stage,constants.bump_matrix[stage],constants.bump_lum_coord[stage]);
        }
        if(mode==15 || mode==16) {
            combiner.tex_mode[stage]=mode==15?NV2A_TEXMODE_DEPENDENT_AR:NV2A_TEXMODE_DEPENDENT_GB;
            combiner.dependent_source[stage]=texture_dependent_source(stage);
        }
        if(mode==1 || mode==2) combiner.projected_mask|=1u<<stage;
        if(texture_stage_samples(mode) && texture_luma_stage(stage))combiner.luma_mask|=1u<<stage;
        unsigned shadow=texture_shadow_mode(stage);
        if(shadow && (mode==1 || mode==2)) {
            combiner.shadow_bits[stage]=shadow;combiner.shadow_mask|=1u<<stage;
            if(mode==2)combiner.shadow_project_mask|=1u<<stage;
            combiner.shadow_function=nv2a_get_state()->pgraph.regs[NV_PGRAPH_SHADOWCTL]&7;
        }
        if(texture_stage_samples(mode) && texture_alpha_kill_enabled(stage)) combiner.alpha_kill_mask|=1u<<stage;
        if(texture_stage_samples(mode)) {
            combiner.color_key_mode[stage]=texture_color_key_mode(stage);
            constants.color_key[stage]=nv2a_get_state()->pgraph.regs[NV_PGRAPH_COLORKEYCOLOR0+stage*4];
            constants.color_key_mask[stage]=texture_color_key_mask(stage);
        }
        hw_texture_bind(stage,color);
        Texture *texture=&s_gpu.texs[stage];
        DWORD address_u=texture->addr_u,address_v=texture->addr_v;
        if(address_u>5 || address_v>5) hw_check(E_NOTIMPL,"texture address mode");
        v->SetTextureStageState(device,stage,D3DTSS_ADDRESSU,address_u);
        v->SetTextureStageState(device,stage,D3DTSS_ADDRESSV,address_v);
        v->SetTextureStageState(device,stage,D3DTSS_NV2AADDRESSW,texture_volume(texture) && texture_stage_samples(mode)?texture_address_w(stage):D3DTADDRESS_WRAP);
        v->SetTextureStageState(device,stage,D3DTSS_BORDERCOLOR,texture->border_color);
        uint32_t mag=(texture->filter>>24)&15,min=(texture->filter>>16)&255;
        int linear=mag==2 || mag==4 || min==2 || min==4 || min==6;
        v->SetTextureStageState(device,stage,D3DTSS_MINFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        v->SetTextureStageState(device,stage,D3DTSS_MAGFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
        v->SetTextureStageState(device,stage,D3DTSS_MIPFILTER,D3DTEXF_NONE);
    }
    gpu_timing_finish(timing_start,&s_gpu.hardware_texture_ticks);
    timing_start=gpu_timing_clock();
    if(!d3d8_IsNativeVulkan())hw_check(d3d8_states_apply_checked(),"render state binding");
    if(s_hw_program_active) {
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
        geometry.point_flags[2]=(point_pg->regs[NV_PGRAPH_CSV0_C]&NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE)!=0;
        geometry.point_flags[3]=GET_MASK(point_pg->regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_FOGGENMODE);
        geometry.polygon[0]=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
        geometry.polygon[1]=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
        float polygon_factor;memcpy(&polygon_factor,&point_pg->regs[NV_PGRAPH_ZOFFSETFACTOR],4);
        geometry.polygon[2]=polygon_factor!=0 && !(point_pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)?((raster&NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE)?1u:0u)|((raster&NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)?2u:0u):0;
        geometry.raster[0]=GET_MASK(raster,NV_PGRAPH_SETUPRASTER_LINEWIDTH)/8.0f;geometry.raster[1]=-1;geometry.raster[3]=(float)d3d8_RenderScale();
        if(s_hw_output_active && geometry.point_flags[0] && (geometry.polygon[0]==1 || geometry.polygon[1]==1))hw_check(E_NOTIMPL,"screen polygon point parameter size");
        /* Sprite UV replacement happens after convert() has normalized the
         * standing vertex coordinates. Its generated [0,1] coordinates feed
         * the same native sampler for both rectangle and swizzled images. */
        if(s_hw_output_active) {
            output_screen[2]=1;
            if(d3d8_IsNativeVulkan()) {
                if(!s_hw_vk_outputs)s_hw_vk_outputs=hw_vk_shader(nvk_shader_create(d3d8_shaders_nv2a_output_source(),"main",VK_SHADER_STAGE_VERTEX_BIT),"vertex outputs");
                native_draw.vertex=s_hw_vk_outputs;
            } else hw_check(d3d8_shaders_bind_nv2a_outputs((float)color->width,(float)color->height,1),"screen output binding");
        } else if(!s_hw_fixed_active) {
            const DWORD *program=(const DWORD *)s_hw_program;int length=s_hw_program_length;
            if(d3d8_IsNativeVulkan())native_draw.vertex=hw_vk_shader(d3d8_vsh_vulkan_program(program,length,&s_hw_program_inputs),"vertex program");
            else hw_check(d3d8_vsh_bind_native(program,length,nv2a_vsh_constant(0),&geometry,s_hw_topology,&s_hw_program_inputs),"native vertex program binding");
        } else {
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
            memcpy(&fixed.material[1],pg->specular_params,12);
            memcpy(fixed.specular_second,pg->specular_params+3,12);
            memcpy(fixed.back_scene,pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BR_AMB],sizeof fixed.back_scene);
            memcpy(fixed.back_emission,pg->ltctxa[NV_IGRAPH_XF_LTCTXA_BCM_COL],sizeof fixed.back_emission);
            fixed.back_material[0]=pg->back_material_alpha;
            memcpy(&fixed.back_material[1],pg->back_specular_params,12);
            memcpy(fixed.back_specular_second,pg->back_specular_params+3,12);
            for(unsigned source=0;source<4;source++)fixed.back_sources[source]=(pg->back_material_sources>>(source*2))&3u;
            fixed.skinning[0]=GET_MASK(d,NV_PGRAPH_CSV0_D_SKIN);
            fixed.skinning[1]=GET_MASK(d,NV_PGRAPH_CSV0_D_TEXGEN_REF);
            for(unsigned i=0;i<8;i++) {
                unsigned mode=GET_MASK(d,NV_PGRAPH_CSV0_D_LIGHT0<<(i*2));
                fixed.light_modes[i/4][i%4]=mode;
                memcpy(fixed.ambient[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_AMB+i*6],16);
                memcpy(fixed.diffuse[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_DIF+i*6],16);
                memcpy(fixed.specular[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_SPC+i*6],16);
                memcpy(fixed.back_ambient[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BAMB+i*6],16);
                memcpy(fixed.back_diffuse[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BDIF+i*6],16);
                memcpy(fixed.back_specular[i],pg->ltctxb[NV_IGRAPH_XF_LTCTXB_L0_BSPC+i*6],16);
                memcpy(&fixed.attenuation[i][3],&pg->ltc1[NV_IGRAPH_XF_LTC1_r0+i][0],4);
                memcpy(fixed.spot_falloff[i],pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_K+i*2],16);
                memcpy(fixed.spot_direction[i],pg->ltctxa[NV_IGRAPH_XF_LTCTXA_L0_SPT+i*2],16);
            }
            if(d3d8_IsNativeVulkan()) {
                if(!s_hw_vk_fixed) {
                    char *source=d3d8_vsh_fixed_source();if(!source)hw_check(E_OUTOFMEMORY,"fixed shader source");
                    s_hw_vk_fixed=hw_vk_shader(nvk_shader_create(source,"main_fixed",VK_SHADER_STAGE_VERTEX_BIT),"fixed vertex shader");free(source);
                }
                native_draw.vertex=s_hw_vk_fixed;
            } else hw_check(d3d8_vsh_bind_fixed(nv2a_vsh_constant(0),&geometry,&fixed,s_hw_topology),"native fixed vertex binding");
        }
    } else if(d3d8_IsNativeVulkan()) {
        if(!s_hw_vk_outputs)s_hw_vk_outputs=hw_vk_shader(nvk_shader_create(d3d8_shaders_nv2a_output_source(),"main",VK_SHADER_STAGE_VERTEX_BIT),"vertex outputs");
        native_draw.vertex=s_hw_vk_outputs;
    } else hw_check(d3d8_shaders_bind_nv2a_outputs((float)color->width,(float)color->height,0),"vertex output binding");
    if(d3d8_IsNativeVulkan())native_draw.pixel=hw_vk_shader(d3d8_combiners_vulkan_shader(&combiner),"combiner shader");
    else hw_check(d3d8_combiners_bind(&combiner,&constants),"original combiner shader binding");
    HwPixelState native={0};
    struct PGRAPHState *pg=&nv2a_get_state()->pgraph;
    if(!polygon_modes && (s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ))hw_polygon_offset(&native,0,constant_w);
    memcpy(native.stipple_pattern,pg->polygon_stipple,sizeof native.stipple_pattern);
    for(unsigned i=0;i<8;i++) { native.cx[i]=pg->regs[NV_PGRAPH_WINDOWCLIPX0+i*4];native.cy[i]=pg->regs[NV_PGRAPH_WINDOWCLIPY0+i*4]; }
    memcpy(&native.depth[0],&pg->regs[NV_PGRAPH_ZCLIPMIN],4);memcpy(&native.depth[1],&pg->regs[NV_PGRAPH_ZCLIPMAX],4);
    native.depth[2]=1.0f/(((s_gpu.format>>4)&15)==1?65536.0f:16777216.0f);
    if(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_Z_FORMAT)
        native.depth[3]=((s_gpu.format>>4)&15)==1?16:24;
    native.flags[0]=(pg->regs[NV_PGRAPH_SETUPRASTER]&NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE)!=0;
    native.flags[1]=GET_MASK(pg->regs[NV_PGRAPH_ZCOMPRESSOCCLUDE],NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN)==NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN_CULL;
    native.flags[2]=(pg->regs[NV_PGRAPH_CONTROL_0]&NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)!=0;
    native.flags[3]=d3d8_RenderScale();
    native.bounds[0]=s_gpu.clip_x;native.bounds[1]=s_gpu.clip_y;native.bounds[2]=s_gpu.clip_x+s_gpu.clip_w;native.bounds[3]=s_gpu.clip_y+s_gpu.clip_h;
    if(d3d8_IsNativeVulkan()) {
        native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){&constants,sizeof constants,16};
        native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){&native,sizeof native,17};
        if(s_hw_program_active) {
            native_draw.geometry=hw_vk_geometry_shader(hw_geometry_kind(0));
            if(s_hw_output_active)native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){output_screen,sizeof output_screen,0};
            native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){nv2a_vsh_constant(0),192*16,1};
            native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){nv2a_vsh_constant(0),192*16,9};
            native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){&geometry,sizeof geometry,10};
            if(s_hw_fixed_active) {
                native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){&geometry,sizeof geometry,2};
                native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){&fixed,sizeof fixed,3};
            }
            unsigned locations[16],attributes[16];
            native_draw.attribute_count=nvk_shader_inputs(native_draw.vertex,locations,attributes,16);
            if(native_draw.attribute_count>16)hw_check(E_INVALIDARG,"vertex shader inputs");
            for(unsigned i=0;i<native_draw.attribute_count;i++) {
                unsigned offset=0,mask=s_hw_program_inputs&((1u<<attributes[i])-1);
                while(mask){offset+=16;mask&=mask-1;}
                native_draw.attributes[i]=(VkVertexInputAttributeDescription){locations[i],0,VK_FORMAT_R32G32B32A32_SFLOAT,offset};
            }
            if(s_hw_output_active) {
                const unsigned output_locations[]={2,0,1,7,3,4,5,6};native_draw.attribute_count=8;
                for(unsigned i=0;i<8;i++)native_draw.attributes[i]=(VkVertexInputAttributeDescription){output_locations[i],0,VK_FORMAT_R32G32B32A32_SFLOAT,i*16};
            }
        } else {
            native_draw.uniforms[native_draw.uniform_count++]=(NvkUniform){output_screen,sizeof output_screen,0};
            const unsigned locations[]={2,0,1,7,3,4,5,6};native_draw.attribute_count=8;
            for(unsigned i=0;i<8;i++)native_draw.attributes[i]=(VkVertexInputAttributeDescription){locations[i],0,VK_FORMAT_R32G32B32A32_SFLOAT,i*16};
        }
        hw_vk_draw_state(&native_draw,color,depth);
    } else if(!s_hw_program_active) {
        hw_bind_pixel_state(ctx,&native);
    }
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
    uint32_t vertex_bytes=s_hw_program_active?s_hw_program_vertex_count*vertex_stride:s_hw_count*sizeof *s_hw_vertices;
    uint32_t index_bytes=s_hw_program_active?s_hw_count*sizeof(uint32_t):0;
    uint32_t bytes=vertex_bytes+index_bytes;
    ID3D11Buffer *vertices_buffer=NULL;
    UINT offset=0;
    HwVertexBatch *entry=s_hw_program_active?s_hw_vertex_batch:NULL;
    /* Both backends consume the original cached rows without staging a copy. */
    const void *vertex_data=s_hw_program_active?(entry?(const void *)(entry->data+entry->count*8):(const void *)s_hw_program_upload.vertex):(const void *)s_hw_vertices;
    uint32_t provoking=pg->regs[NV_PGRAPH_CONTROL_3]&NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX;
    if(d3d8_IsNativeVulkan()) {
        native_draw.vertices=vertex_data;
        native_draw.vertex_bytes=vertex_bytes;native_draw.stride=vertex_stride;native_draw.count=s_hw_count;
        if(s_hw_program_active)native_draw.indices=s_hw_program_upload.indices;
        if(entry && entry->valid) {
            if(entry->upload_primitive!=s_gpu.prim || entry->upload_provoking!=provoking || entry->upload_indices!=s_hw_count)
                entry->upload_generation=0;
            native_draw.upload_generation=&entry->upload_generation;
            native_draw.upload_offset=&entry->upload_offset;
            entry->upload_primitive=s_gpu.prim;entry->upload_provoking=provoking;entry->upload_indices=s_hw_count;
        }
    } else if(entry && entry->valid && entry->upload_primitive==s_gpu.prim &&
       entry->upload_provoking==provoking && entry->upload_indices==s_hw_count &&
       d3d8_reuse_vertex_upload(entry->upload_generation,entry->upload_offset,bytes,&vertices_buffer)) {
        offset=entry->upload_offset;
    } else {
        offset=d3d8_upload_vertices(vertex_data,vertex_bytes,s_hw_program_upload.indices,index_bytes,&vertices_buffer);
        if(offset==(UINT)-1) hw_check(E_OUTOFMEMORY,"original vertex stream upload");
        ++s_gpu.hardware_vertex_stream_uploads;
        if(!offset) ++s_gpu.hardware_vertex_stream_starts;
        if(entry && entry->valid) {
            entry->upload_generation=d3d8_vertex_upload_generation();entry->upload_offset=offset;
            entry->upload_primitive=s_gpu.prim;entry->upload_provoking=provoking;entry->upload_indices=s_hw_count;
        }
    }
    if(!d3d8_IsNativeVulkan()) {
    ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&vertices_buffer,&vertex_stride,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,s_hw_topology);
    if(s_hw_program_active) ID3D11DeviceContext_IASetIndexBuffer(ctx,vertices_buffer,DXGI_FORMAT_R32_UINT,offset+vertex_bytes);
    }
    ID3D11Query *query=NULL;
    if(s_gpu.zpass_enable) {
        if(!d3d8_IsNativeVulkan()) {
        D3D11_QUERY_DESC desc={D3D11_QUERY_OCCLUSION,0};
        hw_check(ID3D11Device_CreateQuery(d3d8_GetD3D11Device(),&desc,&query),"visibility query");
        ID3D11DeviceContext_Begin(ctx,(ID3D11Asynchronous *)query);
        }
    }
    hw_begin_color_write(color);
    hw_depth_draw_write(depth);
    if(polygon_modes) {
        /* Keep original primitive order. Face-specific modes are separate
         * native raster consumers of each primitive, never reordered batches. */
        uint64_t upload_generation=0;uint32_t upload_offset=0;
        if(d3d8_IsNativeVulkan() && !native_draw.upload_generation){native_draw.upload_generation=&upload_generation;native_draw.upload_offset=&upload_offset;}
        for(unsigned first=0;first<s_hw_count;first+=6) {
            geometry.polygon[3]=s_hw_polygon_info[first/6];
            for(unsigned mode=0;mode<3;mode++) {
                if(mode!=geometry.polygon[0] && mode!=geometry.polygon[1])continue;
                hw_polygon_offset(&native,mode,constant_w);
                unsigned passes=mode==1 || (mode==2 && hw_line_copies(&geometry)>16)?9:1;
                for(unsigned edge=0;edge<passes;edge++) {
                    geometry.raster[1]=passes==1?-1:(float)edge;
                    hw_issue_geometry_draw(ctx,&native_draw,&geometry,&native,hw_geometry_kind(mode),first,6);
                }
            }
        }
    } else if(s_hw_program_active) {
        if(s_hw_topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && hw_line_copies(&geometry)>32) {
            for(unsigned first=0;first<s_hw_count;first+=4)hw_issue_geometry_draw(ctx,&native_draw,&geometry,&native,hw_geometry_kind(0),first,4);
        } else hw_issue_geometry_draw(ctx,&native_draw,&geometry,&native,hw_geometry_kind(0),0,s_hw_count);
    } else hw_issue_draw(ctx,&native_draw,0,s_hw_count);
    if(query) {
        ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous *)query);
        HwQuery result={0};result.query=query;hw_append_query(result);
    }
    RECT bounds=s_hw_bounds;
    if(fill_mode==D3DFILL_WIREFRAME || polygon_modes) {
        /* Edge coverage extends half a pixel beyond filled-triangle bounds. */
        if(bounds.left>(LONG)s_gpu.clip_x)--bounds.left;
        if(bounds.top>(LONG)s_gpu.clip_y)--bounds.top;
        if(bounds.right<(LONG)(s_gpu.clip_x+s_gpu.clip_w))++bounds.right;
        if(bounds.bottom<(LONG)(s_gpu.clip_y+s_gpu.clip_h))++bounds.bottom;
    }
    if(bounds.right>(LONG)color->width) bounds.right=(LONG)color->width;
    if(bounds.bottom>(LONG)color->height) bounds.bottom=(LONG)color->height;
    hw_end_color_write(color,bounds);
    s_hw_count=0;s_hw_program_active=0;s_hw_fixed_active=0;
    if(!d3d8_IsNativeVulkan())hw_check(ID3D11Device_GetDeviceRemovedReason(d3d8_GetD3D11Device()),"hardware device status");
    gpu_timing_finish(timing_start,&s_gpu.hardware_submit_ticks);
    hw_leave();
}

