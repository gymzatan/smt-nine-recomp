#define VK_NO_PROTOTYPES
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <bcrypt.h>
#include "nv2a_vulkan.h"
#include "vulkan/vulkan_win32.h"
#include "dxc/dxcapi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define NVK_INSTANCE_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(EnumerateDeviceExtensionProperties) \
 X(CreateDevice) X(GetDeviceProcAddr) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) \
 X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
 X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR)
#define NVK_DEVICE_FUNCTIONS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CreateSemaphore) X(DestroySemaphore) X(GetSemaphoreCounterValue) X(WaitSemaphores) \
 X(QueueSubmit) X(QueueWaitIdle) X(CreateBuffer) X(DestroyBuffer) \
 X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
 X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
 X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CmdPipelineBarrier) \
 X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdBlitImage) X(CmdCopyImage) \
 X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdBeginRendering) X(CmdEndRendering) \
 X(CmdClearAttachments) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreatePipelineCache) X(DestroyPipelineCache) \
 X(CreateSampler) X(DestroySampler) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
 X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) \
 X(CmdSetDepthTestEnable) X(CmdSetDepthWriteEnable) X(CmdSetDepthCompareOp) \
 X(CmdDraw) X(CmdDrawIndexed) X(CreateQueryPool) X(DestroyQueryPool) \
 X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults) \
 X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
 X(AcquireNextImageKHR) X(QueuePresentKHR)
#define DECLARE(name) static PFN_vk##name vk##name;
NVK_INSTANCE_FUNCTIONS(DECLARE)
NVK_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

#pragma comment(lib,"bcrypt.lib")

extern "C" { uint64_t g_nvk_timings[8]{}; }
namespace {
bool timing_enabled;
uint64_t timing_clock() {
    if(!timing_enabled)return 0;
    LARGE_INTEGER value;QueryPerformanceCounter(&value);return value.QuadPart;
}
void timing_finish(uint64_t start,unsigned slot) {
    if(start)g_nvk_timings[slot]+=timing_clock()-start;
}
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
std::string error;
void require(bool condition,const char *message) { if(!condition)throw Failure(message); }
void check(VkResult result,const char *operation) {
    if(result!=VK_SUCCESS)throw Failure(std::string(operation)+": "+std::to_string(result));
}
#define CHECK(call) check((call),#call)
struct Buffer { VkBuffer buffer{};VkDeviceMemory memory{};void *mapped{};VkDeviceSize size{};bool coherent{}; };
struct UniformRange {
    std::vector<unsigned char> value;
    uint64_t generation{};
    uint32_t offset{};
};
template<size_t Capacity> struct StateKey {
    // Key comparisons and consumers only observe the appended prefix.
    std::array<unsigned char,Capacity> data;
    size_t size{};

    StateKey()=default;
    StateKey(const StateKey &other):size(other.size) {
        std::memcpy(data.data(),other.data.data(),size);
    }
    StateKey &operator=(const StateKey &)=delete;

    bool operator<(const StateKey &other) const {
        int order=std::memcmp(data.data(),other.data.data(),std::min(size,other.size));
        return order<0 || (!order && size<other.size);
    }
};
using PipelineKey=StateKey<512>;
using SamplerKey=StateKey<64>;
using PipelineCache=std::map<PipelineKey,VkPipeline>;
using DescriptorSets = std::map<std::array<uint64_t,16>,VkDescriptorSet>;
struct Chunk {
    VkCommandBuffer commands{};
    VkDescriptorPool descriptors{};
    VkSemaphore acquired{};
    Buffer upload;
    VkDeviceSize used{};
    uint64_t serial{},acquire_serial{},last_read{},generation{};
    unsigned sets{},read_chunks{};
    DescriptorSets descriptor_sets;
    std::array<UniformRange,8> uniforms;
};
constexpr unsigned uniform_bindings[]={0,1,2,3,9,10,16,17};
constexpr auto uniform_slots=[] {
    constexpr size_t count=sizeof uniform_bindings/sizeof uniform_bindings[0];
    std::array<uint8_t,uniform_bindings[count-1]+1> slots{};
    for(size_t i=0;i<count;i++)slots[uniform_bindings[i]]=static_cast<uint8_t>(i+1);
    return slots;
}();
unsigned uniform_slot(unsigned binding) {
    if(binding<uniform_slots.size()) {
        const unsigned slot=uniform_slots[binding];
        if(slot)return slot-1;
    }
    throw Failure("Unknown original shader uniform binding");
}
constexpr unsigned chunk_count=4;
constexpr uint32_t depth_test_bit=1u,depth_write_bit=2u,depth_compare_shift=2u;
constexpr VkDeviceSize upload_size=16*1024*1024;
using ShaderDigest=std::array<uint8_t,32>;
struct Context {
    HMODULE loader{},compiler_module{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkBool32 logic_op;
    VkBool32 fill_mode_non_solid{};
    VkBool32 custom_border_colors{};
    VkDevice device{};
    VkQueue queue{};
    unsigned family{};
    VkCommandPool pool{};
    VkSemaphore timeline{};
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout pipeline_layout{};
    VkPipelineCache pipeline_cache{};
    std::array<Chunk,chunk_count> chunks{};
    Buffer sync_transfer;
    unsigned active{};
    bool recording{};
    NvkImage *render_color{},*render_depth{};
    const PipelineCache::value_type *bound_pipeline{};
    uint32_t bound_depth_state=UINT32_MAX;
    const DescriptorSets::value_type *bound_descriptors{};
    std::array<uint32_t,8> bound_uniform_offsets{};
    VkBuffer bound_vertex{},bound_index{};
    VkDeviceSize bound_vertex_offset{},bound_index_offset{};
    unsigned viewport_width{},viewport_height{};
    uint64_t submitted{},completed{},generation{};
    IDxcCompiler3 *compiler{};
    std::wstring shader_cache_directory;
    ShaderDigest shader_compiler_digest{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    HWND window{};
    unsigned window_width{},window_height{};
    VkPresentModeKHR window_mode{};
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> ready;
    std::vector<bool> presented;
} context;

/* The process-local shader owners still own VkShaderModule objects. These
 * files hold only the bytecode produced by the same DXC call. */
constexpr DWORD shader_cache_max_bytes=4*1024*1024;
struct CacheFile {
    HANDLE handle;
    ~CacheFile() {if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
bool shader_digest(const void *data,DWORD bytes,ShaderDigest &digest) {
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        reinterpret_cast<PUCHAR>(const_cast<void *>(data)),bytes,
        digest.data(),static_cast<ULONG>(digest.size()))==0;
}
bool compiler_digest(HANDLE file,ShaderDigest &digest) {
    LARGE_INTEGER size;
    if(file==INVALID_HANDLE_VALUE || !GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>MAXDWORD)
        return false;
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!mapping)return false;
    const void *image=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    const bool hashed=image && shader_digest(image,static_cast<DWORD>(size.QuadPart),digest);
    if(image)UnmapViewOfFile(image);
    CloseHandle(mapping);
    return hashed;
}
bool cache_directory(const std::wstring &path) {
    if(CreateDirectoryW(path.c_str(),nullptr))return true;
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
}
void initialize_shader_cache(HANDLE compiler_file) {
    wchar_t app_data[MAX_PATH];
    const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",app_data,MAX_PATH);
    std::wstring directory;
    if(!length || length>=MAX_PATH)return;
    directory.assign(app_data,length);
    directory+=L"\\SMT NINE";
    // File names contain a 64-character digest and an extension.
    if(directory.size()+sizeof("\\VulkanShaders\\")+68>=MAX_PATH ||
       !compiler_digest(compiler_file,context.shader_compiler_digest) || !cache_directory(directory))return;
    directory+=L"\\VulkanShaders";
    if(cache_directory(directory))context.shader_cache_directory=std::move(directory);
}
std::wstring shader_cache_entry(const DxcBuffer &source,const std::vector<LPCWSTR> &args,ShaderDigest &key) {
    if(context.shader_cache_directory.empty())return {};
    const auto &compiler=context.shader_compiler_digest;
    std::string identity(reinterpret_cast<const char *>(compiler.data()),compiler.size());
    // The literal namespaces the file layout and DXC invocation inputs that
    // are not command line options (UTF-8, no include handler).
    identity.append("NINE SPIR-V UTF-8",sizeof("NINE SPIR-V UTF-8"));
    for(const wchar_t *arg:args)
        identity.append(reinterpret_cast<const char *>(arg),(std::wcslen(arg)+1)*sizeof(wchar_t));
    identity.append(2,'\0');
    identity.append(static_cast<const char *>(source.Ptr),source.Size);
    if(identity.size()>MAXDWORD || !shader_digest(identity.data(),static_cast<DWORD>(identity.size()),key))return {};
    static constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring path=context.shader_cache_directory+L'\\';
    for(uint8_t byte:key) {
        path+=hex[byte>>4];
        path+=hex[byte&15];
    }
    return path+L".bin";
}
bool shader_record_digest(const ShaderDigest &key,const std::vector<uint32_t> &code,ShaderDigest &digest) {
    std::string content(reinterpret_cast<const char *>(key.data()),key.size());
    content.append(reinterpret_cast<const char *>(code.data()),code.size()*sizeof(uint32_t));
    return shader_digest(content.data(),static_cast<DWORD>(content.size()),digest);
}
bool read_shader_cache(const std::wstring &path,const ShaderDigest &key,std::vector<uint32_t> &code) {
    if(path.empty())return false;
    CacheFile file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
                               OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    LARGE_INTEGER size;
    ShaderDigest saved,calculated;
    if(file.handle==INVALID_HANDLE_VALUE || !GetFileSizeEx(file.handle,&size) ||
       size.QuadPart<static_cast<LONGLONG>(saved.size()+5*sizeof(uint32_t)) ||
       size.QuadPart>shader_cache_max_bytes || (size.QuadPart-saved.size())%sizeof(uint32_t))return false;
    const DWORD bytes=static_cast<DWORD>(size.QuadPart-saved.size());
    std::vector<uint32_t> words(bytes/sizeof(uint32_t));
    DWORD received;
    if(!ReadFile(file.handle,saved.data(),static_cast<DWORD>(saved.size()),&received,nullptr) || received!=saved.size() ||
       !ReadFile(file.handle,words.data(),bytes,&received,nullptr) || received!=bytes ||
       words[0]!=0x07230203 || !shader_record_digest(key,words,calculated) || saved!=calculated)return false;
    code.swap(words);
    return true;
}
void write_shader_cache(const std::wstring &path,const ShaderDigest &key,const std::vector<uint32_t> &code) {
    ShaderDigest digest;
    const size_t bytes=code.size()*sizeof(uint32_t);
    if(path.empty() || bytes>shader_cache_max_bytes-digest.size() || !shader_record_digest(key,code,digest))return;
    wchar_t temporary[MAX_PATH];
    if(!GetTempFileNameW(context.shader_cache_directory.c_str(),L"spv",0,temporary))return;
    bool written;
    {
        CacheFile file{CreateFileW(temporary,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        DWORD sent;
        written=file.handle!=INVALID_HANDLE_VALUE &&
            WriteFile(file.handle,digest.data(),static_cast<DWORD>(digest.size()),&sent,nullptr) && sent==digest.size() &&
            WriteFile(file.handle,code.data(),static_cast<DWORD>(bytes),&sent,nullptr) && sent==bytes;
    }
    // Publish a complete file in one rename. Readers of the previous file
    // may finish using their open handle.
    if(!written || !MoveFileExW(temporary,path.c_str(),MOVEFILE_REPLACE_EXISTING))DeleteFileW(temporary);
}
unsigned memory_type(unsigned bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred=0) {
    unsigned selected=VK_MAX_MEMORY_TYPES;
    for(unsigned i=0;i<context.memory.memoryTypeCount;i++) {
        VkMemoryPropertyFlags flags=context.memory.memoryTypes[i].propertyFlags;
        if(!(bits&(1u<<i)) || (flags&required)!=required)continue;
        if(selected==VK_MAX_MEMORY_TYPES)selected=i;
        if((flags&preferred)==preferred)return i;
    }
    if(selected!=VK_MAX_MEMORY_TYPES)return selected;
    throw Failure("Vulkan device has no compatible memory type");
}
void destroy(Buffer &buffer) {
    if(buffer.mapped)vkUnmapMemory(context.device,buffer.memory);
    if(buffer.buffer)vkDestroyBuffer(context.device,buffer.buffer,nullptr);
    if(buffer.memory)vkFreeMemory(context.device,buffer.memory,nullptr);
    buffer={};
}
Buffer buffer_create(VkDeviceSize size,VkBufferUsageFlags usage) {
    Buffer b{};b.size=size;
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size=size;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(context.device,&info,nullptr,&b.buffer));
        VkMemoryRequirements req;vkGetBufferMemoryRequirements(context.device,b.buffer,&req);
        unsigned index=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            (usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT)?VK_MEMORY_PROPERTY_HOST_CACHED_BIT:
            (usage&(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))?VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:0);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=index;
        CHECK(vkAllocateMemory(context.device,&alloc,nullptr,&b.memory));
        CHECK(vkBindBufferMemory(context.device,b.buffer,b.memory,0));
        CHECK(vkMapMemory(context.device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped));
        b.coherent=(context.memory.memoryTypes[index].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        return b;
    } catch(...) {destroy(b);throw;}
}
Buffer &sync_transfer_storage(VkDeviceSize bytes) {
    Buffer &staging=context.sync_transfer;
    if(staging.size<bytes) {
        Buffer larger=buffer_create(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        destroy(staging);staging=larger;
    }
    return staging;
}
void host_memory(Buffer &b,bool read) {
    if(b.coherent)return;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=b.memory;range.size=VK_WHOLE_SIZE;
    if(read)CHECK(vkInvalidateMappedMemoryRanges(context.device,1,&range));
    else CHECK(vkFlushMappedMemoryRanges(context.device,1,&range));
}
void wait_serial(uint64_t serial) {
    if(serial<=context.completed)return;
    require(serial<=context.submitted,"Cannot wait for unsubmitted Vulkan commands");
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount=1;info.pSemaphores=&context.timeline;info.pValues=&serial;
    uint64_t started=timing_clock();
    CHECK(vkWaitSemaphores(context.device,&info,UINT64_MAX));
    timing_finish(started,1);if(timing_enabled)++g_nvk_timings[5];
    context.completed=serial;
}
Chunk &begin() {
    Chunk &chunk=context.chunks[context.active];
    if(!context.recording) {
        wait_serial(chunk.serial);
        CHECK(vkResetCommandBuffer(chunk.commands,0));
        context.bound_descriptors=nullptr;
        if(chunk.sets>=2048) {
            CHECK(vkResetDescriptorPool(context.device,chunk.descriptors,0));
            chunk.sets=0;chunk.descriptor_sets.clear();
        }
        chunk.read_chunks=0;
        if(!chunk.generation)chunk.generation=++context.generation;
        context.bound_pipeline={};context.bound_depth_state=UINT32_MAX;
        context.viewport_width=context.viewport_height=0;
        context.bound_vertex=context.bound_index={};
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(chunk.commands,&info));
        context.recording=true;
    }
    return chunk;
}
void end_rendering() {
    if(context.render_color) {
        vkCmdEndRendering(context.chunks[context.active].commands);
        context.render_color=context.render_depth=nullptr;
    }
}
uint64_t submit(VkSemaphore acquired={},VkSemaphore ready={}) {
    if(!context.recording)return context.submitted;
    Chunk &chunk=context.chunks[context.active];
    end_rendering();
    host_memory(chunk.upload,false);
    CHECK(vkEndCommandBuffer(chunk.commands));
    uint64_t serial=context.submitted+1,wait_value=0;
    VkSemaphore signals[2]={context.timeline,ready};uint64_t values[2]={serial,0};
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount=acquired?1:0;timeline.pWaitSemaphoreValues=&wait_value;
    timeline.signalSemaphoreValueCount=ready?2:1;timeline.pSignalSemaphoreValues=values;
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&timeline;
    info.waitSemaphoreCount=acquired?1:0;info.pWaitSemaphores=&acquired;info.pWaitDstStageMask=&wait_stage;
    info.commandBufferCount=1;info.pCommandBuffers=&chunk.commands;
    info.signalSemaphoreCount=ready?2:1;info.pSignalSemaphores=signals;
    uint64_t started=timing_clock();
    CHECK(vkQueueSubmit(context.queue,1,&info,{}));
    timing_finish(started,0);if(timing_enabled)++g_nvk_timings[4];
    for(unsigned i=0;i<chunk_count;i++)if(chunk.read_chunks&(1u<<i))context.chunks[i].last_read=serial;
    chunk.serial=context.submitted=serial;context.recording=false;
    context.active=(context.active+1)%chunk_count;
    return serial;
}
template<class Write>
VkDeviceSize upload_range(Chunk &chunk,size_t bytes,VkDeviceSize alignment,Write write) {
    VkDeviceSize offset=(chunk.used+alignment-1)&~(alignment-1);
    require(offset+bytes<=chunk.upload.size,"Vulkan draw exceeded its reserved upload range");
    write(static_cast<char *>(chunk.upload.mapped)+offset);
    if(timing_enabled)g_nvk_timings[7]+=bytes;
    chunk.used=offset+bytes;return offset;
}
VkDeviceSize upload(Chunk &chunk,const void *data,size_t bytes,VkDeviceSize alignment=16) {
    return upload_range(chunk,bytes,alignment,[&](char *destination) {
        std::memcpy(destination,data,bytes);
    });
}
uint32_t upload_uniform(Chunk &chunk,unsigned slot,const void *data,size_t bytes,VkDeviceSize alignment) {
    auto &range=chunk.uniforms[slot];
    if(range.generation==chunk.generation && range.value.size()==bytes &&
       !std::memcmp(range.value.data(),data,bytes))return range.offset;
    range.offset=(uint32_t)upload(chunk,data,bytes,alignment);
    range.value.assign(static_cast<const unsigned char *>(data),static_cast<const unsigned char *>(data)+bytes);
    range.generation=chunk.generation;
    return range.offset;
}
Chunk &reserve(size_t bytes) {
    Chunk &current=context.chunks[context.active];
    if(current.used+bytes<=current.upload.size) {
        begin();
        if(current.sets<2048)return current;
    }
    /* Capacity alone does not create GPU work. An idle, full segment can
     * advance without opening and submitting an empty command buffer. */
    if(context.recording)submit();
    else context.active=(context.active+1)%chunk_count;
    Chunk &next=begin();
    if(next.used+bytes>next.upload.size) {
        /* Only overwriting data waits for later submissions that reused its
         * vertex ranges. Appending data and resetting commands do not. */
        wait_serial(next.last_read);
        next.used=0;next.generation=++context.generation;
        if(bytes>next.upload.size) {
            Buffer larger=buffer_create(bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            destroy(next.upload);next.upload=larger;
            CHECK(vkResetDescriptorPool(context.device,next.descriptors,0));
            next.sets=0;next.descriptor_sets.clear();
        }
    }
    return next;
}
VkImageAspectFlags aspects(VkFormat format) {
    return format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
}
struct ImageAccess { VkPipelineStageFlags stages;VkAccessFlags access; };
ImageAccess image_access(VkImageLayout layout) {
    switch(layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0};
    default:throw Failure("Unsupported Vulkan image access layout");
    }
}
void barrier(VkImage image,VkImageLayout before,VkImageLayout after,VkImageAspectFlags aspect,unsigned layers) {
    end_rendering();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    const auto source=image_access(before),destination=image_access(after);
    b.srcAccessMask=source.access;b.dstAccessMask=destination.access;
    b.oldLayout=before;b.newLayout=after;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;
    b.subresourceRange={aspect,0,1,0,layers};
    vkCmdPipelineBarrier(begin().commands,source.stages,destination.stages,0,0,nullptr,0,nullptr,1,&b);
}
PipelineCache pipelines;
std::map<SamplerKey,VkSampler> samplers;
void retire_pipelines(VkShaderModule module={}) {
    bool waited=false;
    for(auto it=pipelines.begin();it!=pipelines.end();) {
        VkShaderModule modules[3];std::memcpy(modules,it->first.data.data(),sizeof modules);
        if(!module || modules[0]==module || modules[1]==module || modules[2]==module) {
            if(!waited){wait_serial(submit());waited=true;}
            if(context.bound_pipeline==&*it)context.bound_pipeline=nullptr;
            vkDestroyPipeline(context.device,it->second,nullptr);it=pipelines.erase(it);
        } else ++it;
    }
}
template<size_t Capacity,class T> void append(StateKey<Capacity> &key,const T &value) {
    require(key.size+sizeof value<=Capacity,"Native Vulkan state key capacity exceeded");
    std::memcpy(key.data.data()+key.size,&value,sizeof value);key.size+=sizeof value;
}
}

struct NvkImage {
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};
    VkFormat format{};unsigned width{},height{},layers{},depth{};bool volume{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};
struct NvkShader {
    VkShaderModule module{};VkShaderStageFlagBits stage{};std::string entry;
    std::vector<uint32_t> code;
    std::vector<std::pair<unsigned,unsigned>> inputs;
    std::array<VkVertexInputAttributeDescription,16> packed_attributes;
    uint32_t packed_input_mask{};
    unsigned texture_slots{};
    bool packed_attributes_valid{};
};
namespace { struct OcclusionPool; }
struct NvkQuery {
    OcclusionPool *pool{};
    uint64_t serial{};
    uint32_t index{};
    bool reset_recorded{};
    NvkQuery *next_free{};
};

namespace {
constexpr uint32_t occlusion_queries_per_pool=64;
struct OcclusionPool {
    VkQueryPool handle{};
    std::array<NvkQuery,occlusion_queries_per_pool> queries{};
};
std::list<OcclusionPool> occlusion_pools;
NvkQuery *free_occlusion_query{};

void add_occlusion_pool() {
    auto &pool=occlusion_pools.emplace_back();
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType=VK_QUERY_TYPE_OCCLUSION;
    info.queryCount=occlusion_queries_per_pool;
    VkResult result=vkCreateQueryPool(context.device,&info,nullptr,&pool.handle);
    if(result!=VK_SUCCESS) {
        occlusion_pools.pop_back();
        check(result,"vkCreateQueryPool");
    }
    for(uint32_t i=occlusion_queries_per_pool;i>0;--i) {
        auto &slot=pool.queries[i-1];
        slot.pool=&pool;
        slot.index=i-1;
        slot.next_free=free_occlusion_query;
        free_occlusion_query=&slot;
    }
}

void prepare_occlusion_query(Chunk &chunk,NvkQuery &query) {
    if(query.reset_recorded)return;
    auto &pool=*query.pool;
    const auto needs_reset=[](const NvkQuery &slot) {
        return !slot.serial && !slot.reset_recorded;
    };
    uint32_t first=query.index,last=query.index+1;
    while(first && needs_reset(pool.queries[first-1]))--first;
    while(last<occlusion_queries_per_pool && needs_reset(pool.queries[last]))++last;
    end_rendering();
    vkCmdResetQueryPool(chunk.commands,pool.handle,first,last-first);
    /* All recordings submit in order on one queue. A reset orders later
     * commands referencing its slots, also across submissions. */
    for(uint32_t i=first;i<last;++i)pool.queries[i].reset_recorded=true;
}
void transition(NvkImage *image,VkImageLayout layout) {
    require(image!=nullptr,"Missing Vulkan image");
    if(image->layout==layout && (layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return;
    barrier(image->image,image->layout,layout,aspects(image->format),image->layers);image->layout=layout;
}
void image_destroy(NvkImage *image) {
    if(!image)return;
    context.bound_descriptors=nullptr;
    for(auto &chunk:context.chunks)chunk.descriptor_sets.clear();
    if(image->view)vkDestroyImageView(context.device,image->view,nullptr);
    if(image->image)vkDestroyImage(context.device,image->image,nullptr);
    if(image->memory)vkFreeMemory(context.device,image->memory,nullptr);
    delete image;
}
NvkImage *image_create(unsigned width,unsigned height,unsigned layers,unsigned depth,VkFormat format,bool cube,bool volume) {
    require(width && height && depth && (cube?layers==6:layers==1) && (volume?!cube && layers==1:depth==1),"Invalid Vulkan image dimensions");
    require(!volume || aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT,"Volume image must be a color texture");
    auto *image=new NvkImage{};image->width=width;image->height=height;image->layers=layers;image->depth=depth;image->volume=volume;image->format=format;
    try {
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(context.physical,format,&properties);
        VkFormatFeatureFlags required=aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|(volume?0:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT):VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        require((properties.optimalTilingFeatures&required)==required,"Unsupported Vulkan image format");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;info.imageType=volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;
        info.format=format;info.extent={width,height,depth};info.mipLevels=1;info.arrayLayers=layers;
        info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT)info.usage|=VK_IMAGE_USAGE_SAMPLED_BIT|(volume?0:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        else info.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        CHECK(vkCreateImage(context.device,&info,nullptr,&image->image));
        VkMemoryRequirements req;vkGetImageMemoryRequirements(context.device,image->image,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
        alloc.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(context.device,&alloc,nullptr,&image->memory));
        CHECK(vkBindImageMemory(context.device,image->image,image->memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image->image;view.format=format;
        view.viewType=volume?VK_IMAGE_VIEW_TYPE_3D:cube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange={aspects(format),0,1,0,layers};
        CHECK(vkCreateImageView(context.device,&view,nullptr,&image->view));
        return image;
    } catch(...) {image_destroy(image);throw;}
}
void window_close() {
    if(context.queue && vkQueueWaitIdle) {wait_serial(submit());CHECK(vkQueueWaitIdle(context.queue));}
    for(auto s:context.ready)vkDestroySemaphore(context.device,s,nullptr);
    context.ready.clear();context.swap_images.clear();context.presented.clear();
    if(context.swapchain)vkDestroySwapchainKHR(context.device,context.swapchain,nullptr);
    if(context.surface)vkDestroySurfaceKHR(context.instance,context.surface,nullptr);
    context.swapchain={};context.surface={};context.window={};context.window_width=context.window_height=0;
    context.swap_extent={};
}
}

#define NVK_BEGIN try {
#define NVK_END(value) } catch(const std::exception &e) {error=e.what();return value;}

const char *nvk_error(void) { return error.c_str(); }
int nvk_initialize(const wchar_t *compiler_path) {
    NVK_BEGIN
    timing_enabled=std::getenv("RECOMP_GPU_TIMING")!=nullptr;
    require(!context.instance,"Vulkan device already initialized");
    context.loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(context.loader!=nullptr,"Cannot load the system Vulkan loader");
    auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(context.loader,"vkGetInstanceProcAddr"));
    require(get!=nullptr,"Missing vkGetInstanceProcAddr");
    auto create=reinterpret_cast<PFN_vkCreateInstance>(get({}, "vkCreateInstance"));
    require(create!=nullptr,"Missing vkCreateInstance");
    const char *extensions[]={"VK_KHR_surface","VK_KHR_win32_surface"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="SMT NINE";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo=&app;instance.enabledExtensionCount=2;instance.ppEnabledExtensionNames=extensions;
    CHECK(create(&instance,nullptr,&context.instance));
#define LOAD_INSTANCE(name) vk##name=reinterpret_cast<PFN_vk##name>(get(context.instance,"vk" #name));require(vk##name!=nullptr,"Missing vk" #name);
    NVK_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    unsigned count=0;CHECK(vkEnumeratePhysicalDevices(context.instance,&count,nullptr));
    std::vector<VkPhysicalDevice> devices(count);CHECK(vkEnumeratePhysicalDevices(context.instance,&count,devices.data()));
    unsigned best=0;
    for(auto device:devices) {
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(device,&props);
        if(props.apiVersion<VK_API_VERSION_1_3 || props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&f12;
        vkGetPhysicalDeviceFeatures2(device,&features);
        if(!features.features.geometryShader || !features.features.occlusionQueryPrecise ||
           !f12.timelineSemaphore || !f12.samplerMirrorClampToEdge || !f13.dynamicRendering)continue;
        unsigned families=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&families,nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);vkGetPhysicalDeviceQueueFamilyProperties(device,&families,queues.data());
        unsigned score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;
        for(unsigned i=0;i<families;i++)if((queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && score>best) {
            best=score;context.physical=device;context.family=i;context.properties=props;
            context.fill_mode_non_solid=features.features.fillModeNonSolid;
            context.logic_op=features.features.logicOp;break;
        }
    }
    require(context.physical!=VkPhysicalDevice{},"No Vulkan 1.3 graphics device with geometry shaders, timelines and dynamic rendering");
    vkGetPhysicalDeviceMemoryProperties(context.physical,&context.memory);
    float priority=1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex=context.family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.dynamicRendering=VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f13;f12.timelineSemaphore=f12.samplerMirrorClampToEdge=VK_TRUE;
    VkPhysicalDeviceFeatures features{};features.geometryShader=features.occlusionQueryPrecise=VK_TRUE;
    features.fillModeNonSolid=context.fill_mode_non_solid;features.logicOp=context.logic_op;
    unsigned extension_count=0;
    CHECK(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&extension_count,nullptr));
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    CHECK(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&extension_count,available_extensions.data()));
    bool border_extension=false;
    for(const auto &extension:available_extensions)
        if(!std::strcmp(extension.extensionName,VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))border_extension=true;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if(border_extension) {
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported.pNext=&border_features;
        vkGetPhysicalDeviceFeatures2(context.physical,&supported);
        context.custom_border_colors=border_features.customBorderColors;
    }
    border_features.customBorderColorWithoutFormat=VK_FALSE;
    if(context.custom_border_colors)f13.pNext=&border_features;
    const char *device_extensions[]={"VK_KHR_swapchain",VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.pNext=&f12;info.queueCreateInfoCount=1;info.pQueueCreateInfos=&queue;
    info.pEnabledFeatures=&features;info.enabledExtensionCount=context.custom_border_colors?2:1;info.ppEnabledExtensionNames=device_extensions;
    CHECK(vkCreateDevice(context.physical,&info,nullptr,&context.device));
#define LOAD_DEVICE(name) vk##name=reinterpret_cast<PFN_vk##name>(vkGetDeviceProcAddr(context.device,"vk" #name));require(vk##name!=nullptr,"Missing vk" #name);
    NVK_DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vkGetDeviceQueue(context.device,context.family,0,&context.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex=context.family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHECK(vkCreateCommandPool(context.device,&pool,nullptr,&context.pool));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};semaphore.pNext=&type;
    CHECK(vkCreateSemaphore(context.device,&semaphore,nullptr,&context.timeline));semaphore.pNext=nullptr;
    std::array<VkDescriptorSetLayoutBinding,20> bindings{};unsigned binding_count=0;
    for(unsigned binding:uniform_bindings)
        bindings[binding_count++]={binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_ALL_GRAPHICS,nullptr};
    for(unsigned i=0;i<4;i++)bindings[binding_count++]={24+i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    for(unsigned i=0;i<4;i++)bindings[binding_count++]={28+i,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount=binding_count;layout.pBindings=bindings.data();
    CHECK(vkCreateDescriptorSetLayout(context.device,&layout,nullptr,&context.descriptor_layout));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&context.descriptor_layout;
    CHECK(vkCreatePipelineLayout(context.device,&pl,nullptr,&context.pipeline_layout));
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    CHECK(vkCreatePipelineCache(context.device,&cache,nullptr,&context.pipeline_cache));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,2048*8},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2048*4},{VK_DESCRIPTOR_TYPE_SAMPLER,2048*4}};
    for(auto &chunk:context.chunks) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool=context.pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
        CHECK(vkAllocateCommandBuffers(context.device,&alloc,&chunk.commands));
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2048;dp.poolSizeCount=3;dp.pPoolSizes=sizes;
        CHECK(vkCreateDescriptorPool(context.device,&dp,nullptr,&chunk.descriptors));
        CHECK(vkCreateSemaphore(context.device,&semaphore,nullptr,&chunk.acquired));
        chunk.upload=buffer_create(upload_size,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    CacheFile compiler_file{CreateFileW(compiler_path,GENERIC_READ,FILE_SHARE_READ,nullptr,
                                        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    context.compiler_module=LoadLibraryExW(compiler_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(context.compiler_module!=nullptr,"Cannot load the Vulkan SPIR-V compiler");
    auto factory=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(context.compiler_module,"DxcCreateInstance"));
    require(factory!=nullptr,"Missing DxcCreateInstance");
    require(SUCCEEDED(factory(CLSID_DxcCompiler,IID_PPV_ARGS(&context.compiler))),"Cannot create SPIR-V compiler");
    initialize_shader_cache(compiler_file.handle);
    std::fprintf(stderr,"[GPU Vulkan native] %s; Vulkan %u.%u; direct Vulkan queue/resources, no D3D device\n",
        context.properties.deviceName,VK_API_VERSION_MAJOR(context.properties.apiVersion),VK_API_VERSION_MINOR(context.properties.apiVersion));
    return 1;
    NVK_END(0)
}

uint64_t nvk_submit(void) {NVK_BEGIN return submit();NVK_END(0)}
uint64_t nvk_fence(void) {return context.submitted+(context.recording?1:0);}
int nvk_wait(uint64_t serial) {NVK_BEGIN if(serial>context.submitted)submit();wait_serial(serial);return 1;NVK_END(0)}
int nvk_completed(uint64_t serial,int submit_pending) {
    NVK_BEGIN
    if(serial<=context.completed)return 1;
    if(serial>context.submitted) {
        if(!submit_pending)return 0;
        submit();
    }
    uint64_t done;CHECK(vkGetSemaphoreCounterValue(context.device,context.timeline,&done));
    context.completed=done;return done>=serial?1:0;
    NVK_END(-1)
}
NvkImage *nvk_image_create(unsigned w,unsigned h,unsigned layers,VkFormat format,int cube) {
    NVK_BEGIN return image_create(w,h,layers,1,format,cube!=0,false);NVK_END(nullptr)
}
NvkImage *nvk_image_create_volume(unsigned w,unsigned h,unsigned depth,VkFormat format) {
    NVK_BEGIN return image_create(w,h,1,depth,format,false,true);NVK_END(nullptr)
}
VkFormat nvk_image_format(const NvkImage *image) {
    return image->format;
}
void nvk_image_destroy(NvkImage *image) {
    try {if(image){wait_serial(submit());image_destroy(image);}}catch(const std::exception &e){error=e.what();}
}
template<class Write>
static void upload_image(NvkImage *image,Write write) {
    const size_t row=image->width*4;
    const size_t rows=(size_t)image->height*image->layers*image->depth;
    const size_t bytes=row*rows;
    auto &chunk=reserve(bytes+3);
    const VkDeviceSize offset=upload_range(chunk,bytes,4,[&](char *destination) {
        write(destination,row,rows,bytes);
    });
    transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};copy.bufferOffset=offset;
    copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,image->layers};copy.imageExtent={image->width,image->height,image->depth};
    vkCmdCopyBufferToImage(chunk.commands,chunk.upload.buffer,image->image,image->layout,1,&copy);
}
int nvk_image_upload(NvkImage *image,const void *pixels,size_t pitch) {
    NVK_BEGIN
    require(image && pixels && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT && pitch>=image->width*4,"Invalid Vulkan color upload");
    upload_image(image,[&](char *destination,size_t row,size_t rows,size_t bytes) {
        if(pitch==row)std::memcpy(destination,pixels,bytes);
        else for(size_t y=0;y<rows;y++)
            std::memcpy(destination+y*row,static_cast<const char *>(pixels)+y*pitch,row);
    });
    return 1;
    NVK_END(0)
}
int nvk_image_upload_written(NvkImage *image,NvkTextureWriter write,const void *source) {
    NVK_BEGIN
    require(image && write && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT,"Invalid Vulkan color upload writer");
    upload_image(image,[&](char *destination,size_t row,size_t,size_t) {
        write(destination,row,source);
    });
    return 1;
    NVK_END(0)
}
int nvk_image_read(NvkImage *image,void *pixels,size_t pitch) {
    return nvk_image_read_rect(image,pixels,pitch,nullptr);
}
int nvk_image_read_rect(NvkImage *image,void *pixels,size_t pitch,const int32_t bounds[4]) {
    const int32_t full[4]={0,0,image?(int32_t)image->width:0,image?(int32_t)image->height:0};
    if(!bounds)bounds=full;
    const NvkImageRead read{image,{bounds[0],bounds[1],bounds[2],bounds[3]},pixels,pitch,nullptr};
    return nvk_images_read(&read,1);
}
int nvk_images_read(const NvkImageRead *reads,unsigned count) {
    NVK_BEGIN
    require(reads && count && count<=NVK_COLOR_READ_COUNT,"Invalid Vulkan color readback count");
    VkBufferImageCopy copies[NVK_COLOR_READ_COUNT];
    size_t bytes=0;
    for(unsigned i=0;i<count;++i) {
        const auto &read=reads[i];
        const NvkImage *image=read.image;
        require(image && read.destination && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT,
                "Invalid Vulkan color readback");
        const int32_t *bounds=read.bounds;
        require(bounds[0]>=0 && bounds[1]>=0 && bounds[2]>bounds[0] && bounds[3]>bounds[1] &&
                (uint32_t)bounds[2]<=image->width && (uint32_t)bounds[3]<=image->height,
                "Invalid Vulkan color readback bounds");
        auto &copy=copies[i];
        copy={};
        copy.bufferOffset=bytes;
        copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,image->layers};
        copy.imageOffset={bounds[0],bounds[1],0};
        copy.imageExtent={(uint32_t)(bounds[2]-bounds[0]),(uint32_t)(bounds[3]-bounds[1]),image->depth};
        const size_t row=(size_t)copy.imageExtent.width*4;
        require(read.publish || read.pitch>=row,"Invalid Vulkan color readback pitch");
        bytes+=row*copy.imageExtent.height*copy.imageExtent.depth*image->layers;
    }
    /* All regions fit the existing synchronous staging buffer; keep the
     * graphics lock until the fence and all guest publications are complete. */
    Buffer &staging=sync_transfer_storage(bytes);
    try {
        for(unsigned i=0;i<count;++i) {
            auto *image=reads[i].image;
            transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(begin().commands,image->image,image->layout,staging.buffer,1,&copies[i]);
        }
        wait_serial(submit());host_memory(staging,true);
        for(unsigned i=0;i<count;++i) {
            const auto &read=reads[i];
            const auto &copy=copies[i];
            const char *pixels=static_cast<const char *>(staging.mapped)+copy.bufferOffset;
            const size_t row=(size_t)copy.imageExtent.width*4;
            if(read.publish) {
                read.publish(read.destination,pixels,row);
            } else {
                const size_t rows=(size_t)copy.imageExtent.height*copy.imageExtent.depth*copy.imageSubresource.layerCount;
                if(read.pitch==row)std::memcpy(read.destination,pixels,row*rows);
                else for(size_t y=0;y<rows;++y)
                    std::memcpy(static_cast<char *>(read.destination)+read.pitch*y,pixels+row*y,row);
            }
        }
        return 1;
    }catch(...){destroy(staging);throw;}
    NVK_END(0)
}
int nvk_image_clear(NvkImage *image,const float rgba[4]) {
    NVK_BEGIN
    require(image && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT,"Invalid Vulkan color clear");
    transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue value;std::memcpy(value.float32,rgba,16);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,image->layers};
    vkCmdClearColorImage(begin().commands,image->image,image->layout,&value,1,&range);return 1;
    NVK_END(0)
}
int nvk_depth_transfer(NvkImage *image,void *pixels,int writing,unsigned read_step) {
    NVK_BEGIN
    require(image && pixels && !image->volume && image->format==VK_FORMAT_D32_SFLOAT_S8_UINT && image->layers==1,"Invalid Vulkan depth transfer");
    require(read_step && image->width%read_step==0 && image->height%read_step==0 && (!writing || read_step==1),"Invalid Vulkan depth sampling step");
    size_t count=(size_t)image->width*image->height;
    Buffer &staging=sync_transfer_storage(count*5);
    try {
        if(writing) {
            for(size_t i=0;i<count;i++) {
                std::memcpy(static_cast<char *>(staging.mapped)+i*4,static_cast<char *>(pixels)+i*8,4);
                static_cast<char *>(staging.mapped)[count*4+i]=static_cast<char *>(pixels)[i*8+4];
            }
            host_memory(staging,false);
        }
        transition(image,writing?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy regions[2]{};
        regions[0].imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        regions[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};regions[1].bufferOffset=count*4;
        regions[0].imageExtent=regions[1].imageExtent={image->width,image->height,1};
        if(writing)vkCmdCopyBufferToImage(begin().commands,staging.buffer,image->image,image->layout,2,regions);
        else vkCmdCopyImageToBuffer(begin().commands,image->image,image->layout,staging.buffer,2,regions);
        wait_serial(submit());
        if(!writing) {
            host_memory(staging,true);
            size_t k=0;
            for(unsigned y=0;y<image->height;y+=read_step)for(unsigned x=0;x<image->width;x+=read_step,++k) {
                size_t i=(size_t)y*image->width+x;
                std::memcpy(static_cast<char *>(pixels)+k*8,static_cast<char *>(staging.mapped)+i*4,4);
                std::memset(static_cast<char *>(pixels)+k*8+4,0,4);
                static_cast<char *>(pixels)[k*8+4]=static_cast<char *>(staging.mapped)[count*4+i];
            }
        }
        return 1;
    }catch(...){destroy(staging);throw;}
    NVK_END(0)
}
int nvk_depth_clear(NvkImage *image,VkImageAspectFlags mask,float depth,unsigned stencil,const int32_t bounds[4]) {
    NVK_BEGIN
    require(image && !image->volume && image->format==VK_FORMAT_D32_SFLOAT_S8_UINT && mask && !(mask&~aspects(image->format)),"Invalid Vulkan depth clear");
    if(!bounds) {
        transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange range{mask,0,1,0,1};VkClearDepthStencilValue value{depth,stencil};
        vkCmdClearDepthStencilImage(begin().commands,image->image,image->layout,&value,1,&range);
    } else {
        require(bounds[0]>=0 && bounds[1]>=0 && bounds[2]>(int)bounds[0] && bounds[3]>(int)bounds[1] && bounds[2]<=(int)image->width && bounds[3]<=(int)image->height,"Invalid Vulkan clear bounds");
        transition(image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView=image->view;attachment.imageLayout=image->layout;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.renderArea.extent={image->width,image->height};rendering.layerCount=1;
        rendering.pDepthAttachment=&attachment;rendering.pStencilAttachment=&attachment;
        vkCmdBeginRendering(begin().commands,&rendering);
        VkClearAttachment clear{};clear.aspectMask=mask;clear.clearValue.depthStencil={depth,stencil};
        VkClearRect rect{{{bounds[0],bounds[1]},{(unsigned)(bounds[2]-bounds[0]),(unsigned)(bounds[3]-bounds[1])}},0,1};
        vkCmdClearAttachments(begin().commands,1,&clear,1,&rect);vkCmdEndRendering(begin().commands);
    }
    return 1;NVK_END(0)
}
int nvk_image_copy(NvkImage *src,NvkImage *dst,const int32_t bounds[4],int linear) {
    return nvk_image_copy_rect(src,dst,nullptr,bounds,linear);
}
int nvk_image_copy_rect(NvkImage *src,NvkImage *dst,const int32_t source_bounds[4],const int32_t bounds[4],int linear) {
    NVK_BEGIN
    require(src && dst && !src->volume && !dst->volume && src!=dst && src->layers==1 && dst->layers==1 && src->format!=VK_FORMAT_D32_SFLOAT_S8_UINT && dst->format!=VK_FORMAT_D32_SFLOAT_S8_UINT,"Invalid Vulkan image copy");
    int32_t full[4]={0,0,(int32_t)dst->width,(int32_t)dst->height};if(!bounds)bounds=full;
    int32_t source_full[4]={0,0,(int32_t)src->width,(int32_t)src->height};if(!source_bounds)source_bounds=source_full;
    require(bounds[0]>=0 && bounds[1]>=0 && bounds[2]>bounds[0] && bounds[3]>bounds[1] && bounds[2]<=(int)dst->width && bounds[3]<=(int)dst->height,"Invalid Vulkan copy bounds");
    require(source_bounds[0]>=0 && source_bounds[1]>=0 && source_bounds[2]>source_bounds[0] && source_bounds[3]>source_bounds[1] &&
        source_bounds[2]<=(int)src->width && source_bounds[3]<=(int)src->height,"Invalid Vulkan source bounds");
    transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);transition(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const int32_t width=bounds[2]-bounds[0],height=bounds[3]-bounds[1];
    if(!linear && src->format==VK_FORMAT_B8G8R8A8_UNORM && dst->format==src->format &&
       width==source_bounds[2]-source_bounds[0] && height==source_bounds[3]-source_bounds[1]) {
        VkImageCopy copy{};
        copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.srcOffset={source_bounds[0],source_bounds[1],0};
        copy.dstOffset={bounds[0],bounds[1],0};
        copy.extent={(uint32_t)width,(uint32_t)height,1};
        vkCmdCopyImage(begin().commands,src->image,src->layout,dst->image,dst->layout,1,&copy);
        return 1;
    }
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[0]={source_bounds[0],source_bounds[1],0};blit.srcOffsets[1]={source_bounds[2],source_bounds[3],1};
    blit.dstOffsets[0]={bounds[0],bounds[1],0};blit.dstOffsets[1]={bounds[2],bounds[3],1};
    vkCmdBlitImage(begin().commands,src->image,src->layout,dst->image,dst->layout,1,&blit,linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    return 1;NVK_END(0)
}

static void reflect_shader(NvkShader *shader);
NvkShader *nvk_shader_create(const char *source,const char *entry,VkShaderStageFlagBits stage) {
    NVK_BEGIN
    require(source && entry,"Missing Vulkan shader source");
    const wchar_t *target=stage==VK_SHADER_STAGE_VERTEX_BIT?L"vs_6_0":stage==VK_SHADER_STAGE_GEOMETRY_BIT?L"gs_6_0":L"ps_6_0";
    const wchar_t *shift=stage==VK_SHADER_STAGE_VERTEX_BIT?L"0":stage==VK_SHADER_STAGE_GEOMETRY_BIT?L"8":L"16";
    std::wstring wide_entry(entry,entry+std::strlen(entry));
    std::vector<LPCWSTR> args={L"-spirv",L"-T",target,L"-E",wide_entry.c_str(),L"-O3",L"-Gis",
        L"-fspv-target-env=vulkan1.3",L"-fvk-use-dx-layout",L"-fvk-stage-io-order=alpha",
        L"-fvk-b-shift",shift,L"0",L"-fvk-t-shift",L"24",L"0",L"-fvk-s-shift",L"28",L"0"};
    if(stage==VK_SHADER_STAGE_FRAGMENT_BIT)args.push_back(L"-fvk-use-dx-position-w");
    DxcBuffer input{source,std::strlen(source),DXC_CP_UTF8};
    ShaderDigest key;
    const std::wstring path=shader_cache_entry(input,args,key);
    std::vector<uint32_t> words;
    const bool cached=read_shader_cache(path,key,words);
    if(!cached) {
        require(context.compiler!=nullptr,"Missing Vulkan shader compiler");
        IDxcResult *result=nullptr;
        require(SUCCEEDED(context.compiler->Compile(&input,args.data(),(UINT32)args.size(),nullptr,IID_PPV_ARGS(&result))),"SPIR-V compiler invocation failed");
        HRESULT status=E_FAIL;result->GetStatus(&status);
        if(FAILED(status)) {
            IDxcBlobUtf8 *diagnostic=nullptr;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&diagnostic),nullptr);
            std::string message=diagnostic?diagnostic->GetStringPointer():"SPIR-V compilation failed";
            if(diagnostic)diagnostic->Release();result->Release();throw Failure(message);
        }
        IDxcBlob *code=nullptr;HRESULT hr=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&code),nullptr);result->Release();
        require(SUCCEEDED(hr) && code,"SPIR-V compiler produced no object");
        words.resize(code->GetBufferSize()/sizeof(uint32_t));
        std::memcpy(words.data(),code->GetBufferPointer(),words.size()*sizeof(uint32_t));
        code->Release();
    }
    auto *shader=new NvkShader{};shader->stage=stage;shader->entry=entry;
    shader->code=std::move(words);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=shader->code.size()*4;info.pCode=shader->code.data();
    VkResult created=vkCreateShaderModule(context.device,&info,nullptr,&shader->module);
    if(created!=VK_SUCCESS){delete shader;check(created,"vkCreateShaderModule");}
    reflect_shader(shader);
    if(!cached)write_shader_cache(path,key,shader->code);
    return shader;NVK_END(nullptr)
}
void nvk_shader_destroy(NvkShader *shader) {
    if(!shader)return;
    try {retire_pipelines(shader->module);
        vkDestroyShaderModule(context.device,shader->module,nullptr);delete shader;
    }catch(const std::exception &e){error=e.what();}
}
static void reflect_shader(NvkShader *shader) {
    const bool vertex=shader->stage==VK_SHADER_STAGE_VERTEX_BIT;
    if(!vertex && shader->stage!=VK_SHADER_STAGE_FRAGMENT_BIT)return;
    std::map<uint32_t,std::string> names;std::map<uint32_t,uint32_t> loc;std::vector<uint32_t> inputs;
    for(size_t i=5;i<shader->code.size();) {
        uint32_t head=shader->code[i],count=head>>16,op=head&65535;
        require(count && count<=shader->code.size()-i,"Invalid compiled SPIR-V instruction");
        const uint32_t *p=shader->code.data()+i;
        if(vertex) {
            if(op==5 && count>2)names[p[1]]=reinterpret_cast<const char *>(p+2);
            if(op==71 && count==4 && p[2]==30)loc[p[1]]=p[3];
            if(op==59 && count>=4 && p[3]==1)inputs.push_back(p[2]);
        } else if(op==71 && count==4 && p[2]==33 && p[3]>=24 && p[3]<32) {
            /* The fragment layout pairs image bindings 24-27 with samplers 28-31. */
            shader->texture_slots|=1u<<((p[3]-24)%4);
        }
        i+=count;
    }
    for(uint32_t id:inputs) {
        auto it=names.find(id);if(it==names.end() || loc.find(id)==loc.end())continue;
        auto start=it->second.find("ATTR");if(start==std::string::npos)continue;
        shader->inputs.emplace_back(loc[id],(unsigned)std::stoul(it->second.substr(start+4)));
    }
}
unsigned nvk_shader_vertex_attributes(NvkShader *shader,uint32_t input_mask,VkVertexInputAttributeDescription attributes[16]) {
    if(!shader)return 0;
    const auto count=shader->inputs.size();
    if(count>shader->packed_attributes.size())return (unsigned)count;
    if(!shader->packed_attributes_valid || shader->packed_input_mask!=input_mask) {
        for(size_t i=0;i<count;++i) {
            const auto &input=shader->inputs[i];
            uint32_t earlier=input_mask&((1u<<input.second)-1);
            unsigned offset=0;
            for(;earlier;earlier&=earlier-1)offset+=16;
            shader->packed_attributes[i]={input.first,0,VK_FORMAT_R32G32B32A32_SFLOAT,offset};
        }
        shader->packed_input_mask=input_mask;
        shader->packed_attributes_valid=true;
    }
    std::copy_n(shader->packed_attributes.data(),count,attributes);
    return (unsigned)count;
}

namespace {
VkSampler sampler(const NvkTexture &texture) {
    VkBorderColor border=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const float black[4]={0,0,0,0},opaque[4]={0,0,0,1},white[4]={1,1,1,1};
    if(!std::memcmp(texture.border,black,16))border=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if(!std::memcmp(texture.border,opaque,16))border=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(!std::memcmp(texture.border,white,16))border=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(texture.u==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || texture.v==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
            texture.w==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
        require(context.custom_border_colors,"Vulkan device lacks custom texture border colors");
        border=VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    }

    SamplerKey key;append(key,texture.filter);append(key,texture.u);
    append(key,texture.v);append(key,texture.w);append(key,border);
    if(border==VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) {append(key,texture.border);append(key,texture.image->format);}
    auto found=samplers.find(key);if(found!=samplers.end())return found->second;

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter=info.minFilter=texture.filter;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=texture.u;info.addressModeV=texture.v;info.addressModeW=texture.w;
    info.borderColor=border;
    VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if(border==VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) {
        std::memcpy(custom.customBorderColor.float32,texture.border,16);custom.format=texture.image->format;
        info.pNext=&custom;
    }
    VkSampler result;CHECK(vkCreateSampler(context.device,&info,nullptr,&result));samplers.emplace(key,result);return result;
}
const PipelineCache::value_type *pipeline(const NvkDraw &draw) {
    PipelineKey key;
    append(key,draw.vertex->module);VkShaderModule gs=draw.geometry?draw.geometry->module:VkShaderModule{};
    append(key,gs);append(key,draw.pixel->module);append(key,draw.stride);append(key,draw.topology);
    append(key,draw.color->format);VkFormat depth_format=draw.depth?draw.depth->format:VK_FORMAT_UNDEFINED;append(key,depth_format);
    append(key,draw.attribute_count);
    for(unsigned i=0;i<draw.attribute_count;i++)append(key,draw.attributes[i]);
    append(key,draw.stencil);if(draw.stencil)append(key,draw.stencil_state);
    append(key,draw.blend);
    append(key,draw.logic);if(draw.logic)append(key,draw.logic_op);
    append(key,draw.wireframe);
    if(draw.blend){append(key,draw.source_blend);append(key,draw.destination_blend);append(key,draw.blend_op);}
    append(key,draw.color_mask);
    const auto *bound=context.bound_pipeline;
    if(bound && bound->first.size==key.size &&
        !std::memcmp(bound->first.data.data(),key.data.data(),key.size))return bound;
    auto found=pipelines.find(key);if(found!=pipelines.end())return &*found;
    if(pipelines.size()>=8192)retire_pipelines();
    VkPipelineShaderStageCreateInfo stages[3]{};unsigned count=0;
    for(NvkShader *shader:{draw.vertex,draw.geometry,draw.pixel})if(shader) {
        auto &stage=stages[count++];stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage=shader->stage;stage.module=shader->module;stage.pName=shader->entry.c_str();
    }
    VkVertexInputBindingDescription binding{0,draw.stride,VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&binding;
    input.vertexAttributeDescriptionCount=draw.attribute_count;input.pVertexAttributeDescriptions=draw.attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=draw.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    require(!draw.wireframe || context.fill_mode_non_solid,"Vulkan device does not support wireframe polygons");
    raster.polygonMode=draw.wireframe?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL;
    raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.stencilTestEnable=draw.stencil;depth.front=depth.back=draw.stencil_state;
    VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=draw.blend;blend.colorWriteMask=draw.color_mask;
    blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=draw.source_blend;
    blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=draw.destination_blend;
    blend.colorBlendOp=blend.alphaBlendOp=draw.blend_op;
    require(!draw.logic || context.logic_op,"Vulkan device has no colour logic operations");
    VkPipelineColorBlendStateCreateInfo blends{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blends.attachmentCount=1;blends.pAttachments=&blend;
    blends.logicOpEnable=draw.logic;blends.logicOp=draw.logic_op;
    const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount=sizeof states/sizeof *states;dynamic.pDynamicStates=states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&draw.color->format;
    rendering.depthAttachmentFormat=rendering.stencilAttachmentFormat=depth_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pNext=&rendering;
    info.stageCount=count;info.pStages=stages;info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;
    info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&samples;
    info.pDepthStencilState=&depth;info.pColorBlendState=&blends;info.pDynamicState=&dynamic;info.layout=context.pipeline_layout;
    VkPipeline result;CHECK(vkCreateGraphicsPipelines(context.device,context.pipeline_cache,1,&info,nullptr,&result));
    return &*pipelines.emplace(std::move(key),result).first;
}
}

int nvk_fill_column_misses_samples(const NvkImage *color,unsigned guest_width,unsigned column_x16) {
    const auto &limits=context.properties.limits;
    if(!color || color->volume || !guest_width || color->width>16384 ||
       color->width%guest_width || column_x16>=guest_width*16 ||
       !limits.standardSampleLocations || limits.subPixelPrecisionBits<8)return 0;
    const unsigned scale=color->width/guest_width;
    const int offset=int((column_x16*scale)&15)-8;
    const double distance=double(offset<0?-offset:offset)/16.0;
    // Include viewport conversion roundoff and a full subpixel step.
    return distance>double(color->width)*0x1p-17+0x1p-8;
}

int nvk_draw(NvkDraw *input) {
    NVK_BEGIN
    require(input && input->color && !input->color->volume && (!input->depth || !input->depth->volume) && input->vertex && input->pixel && input->count && input->vertices &&
        input->attribute_count<=16 && input->uniform_count<=8 && input->draw_count &&
        input->draw_first<=input->count && input->draw_count<=input->count-input->draw_first,"Incomplete Vulkan draw");
    const auto &draw=*input;
    const size_t stream_bytes=draw.vertex_bytes+(draw.indices?draw.count*4:0);
    size_t bytes=32;
    VkDeviceSize align=std::max<VkDeviceSize>(16,context.properties.limits.minUniformBufferOffsetAlignment);
    unsigned shared_uniform[8]{};
    for(unsigned i=0;i<draw.uniform_count;i++) {
        const auto &uniform=draw.uniforms[i];shared_uniform[i]=i;
        for(unsigned j=0;j<i;j++)if(draw.uniforms[j].data==uniform.data && draw.uniforms[j].bytes==uniform.bytes){shared_uniform[i]=j;break;}
        if(shared_uniform[i]==i)bytes+=uniform.bytes+align;
    }
    /* Pipeline retirement may submit and rotate chunks. Select before this
     * draw reserves or captures any upload, descriptor or command ownership. */
    const auto *selected_pipeline=pipeline(draw);
    unsigned stream_chunk=chunk_count;
    if(draw.upload_generation && draw.upload_offset && *draw.upload_generation) {
        for(unsigned i=0;i<chunk_count;i++) {
            const auto &cached=context.chunks[i];
            if(cached.generation==*draw.upload_generation && *draw.upload_offset+stream_bytes<=cached.used) {
                stream_chunk=i;
                break;
            }
        }
    }
    const auto &active=context.chunks[context.active];
    /* Reclaiming a chunk can invalidate a resident stream. Reserve its full
     * size if the constants alone would already require a rotation. */
    if(stream_chunk==chunk_count || active.used+bytes>active.upload.size || active.sets>=2048)
        bytes+=stream_bytes;
    auto &chunk=reserve(bytes);
    VkBuffer vertex_buffer{};VkDeviceSize vertex_offset=0,index_offset=0;
    input->uploaded=input->upload_started=0;
    if(stream_chunk<chunk_count && context.chunks[stream_chunk].generation==*draw.upload_generation) {
        vertex_buffer=context.chunks[stream_chunk].upload.buffer;vertex_offset=*draw.upload_offset;
        index_offset=vertex_offset+draw.vertex_bytes;chunk.read_chunks|=1u<<stream_chunk;
    }
    if(!vertex_buffer) {
        vertex_buffer=chunk.upload.buffer;vertex_offset=upload(chunk,draw.vertices,draw.vertex_bytes);
        if(draw.indices)index_offset=upload(chunk,draw.indices,draw.count*4,4);
        input->uploaded=1;input->upload_started=vertex_offset==0;
        if(draw.upload_generation && draw.upload_offset) {
            *draw.upload_generation=chunk.generation;*draw.upload_offset=(uint32_t)vertex_offset;
        }
    }
    std::array<uint64_t,16> descriptor_key{};
    std::array<uint32_t,8> dynamic_offsets{};
    uint32_t uniform_offsets[8]{};
    for(unsigned i=0;i<8;i++)descriptor_key[i]=16;
    for(unsigned i=0;i<draw.uniform_count;i++) {
        const auto &uniform=draw.uniforms[i];
        require(uniform.data && uniform.bytes && uniform.bytes<=context.properties.limits.maxUniformBufferRange,"Invalid Vulkan uniform range");
        unsigned slot=uniform_slot(uniform.binding),shared=shared_uniform[i];
        uniform_offsets[i]=shared<i?uniform_offsets[shared]:upload_uniform(chunk,slot,uniform.data,uniform.bytes,align);
        dynamic_offsets[slot]=uniform_offsets[i];descriptor_key[slot]=uniform.bytes;
    }
    unsigned texture_slots=0;
    for(unsigned i=0;i<4;i++)if(draw.textures[i].image) {
        const auto &texture=draw.textures[i];require(texture.image!=draw.color,"Vulkan color attachment cannot sample itself");
        if(!(draw.pixel->texture_slots&(1u<<i)))continue;
        texture_slots|=1u<<i;
        transition(texture.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkSampler handle=sampler(texture);
        std::memcpy(&descriptor_key[8+i],&texture.image->view,sizeof(VkImageView));
        std::memcpy(&descriptor_key[12+i],&handle,sizeof handle);
    }
    const auto *descriptor=context.bound_descriptors;
    if(!descriptor || descriptor->first!=descriptor_key) {
        auto cached=chunk.descriptor_sets.find(descriptor_key);
        if(cached!=chunk.descriptor_sets.end())descriptor=&*cached;
        else {
            VkDescriptorSet descriptors;
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=chunk.descriptors;
            alloc.descriptorSetCount=1;alloc.pSetLayouts=&context.descriptor_layout;
            CHECK(vkAllocateDescriptorSets(context.device,&alloc,&descriptors));++chunk.sets;
            if(timing_enabled)++g_nvk_timings[6];
            VkWriteDescriptorSet writes[16]{};VkDescriptorBufferInfo buffers[8]{};unsigned write_count=0;
            for(unsigned i=0;i<8;i++) {
                buffers[i]={chunk.upload.buffer,0,descriptor_key[i]};
                auto &write=writes[write_count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=descriptors;
                write.dstBinding=uniform_bindings[i];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;write.pBufferInfo=&buffers[i];
            }
            VkDescriptorImageInfo images[8];
            for(unsigned i=0;i<4;i++)if(texture_slots&(1u<<i)) {
                const auto *image=draw.textures[i].image;
                VkSampler handle;
                std::memcpy(&handle,&descriptor_key[12+i],sizeof handle);
                images[i*2]={VkSampler{},image->view,image->layout};
                images[i*2+1]={handle,VkImageView{},VK_IMAGE_LAYOUT_UNDEFINED};
                for(unsigned j=0;j<2;j++) {
                    auto &write=writes[write_count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=descriptors;
                    write.dstBinding=24+i+j*4;write.descriptorCount=1;
                    write.descriptorType=j?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&images[i*2+j];
                }
            }
            vkUpdateDescriptorSets(context.device,write_count,writes,0,nullptr);
            descriptor=&*chunk.descriptor_sets.emplace(descriptor_key,descriptors).first;
        }
    }
    if(draw.query)prepare_occlusion_query(chunk,*draw.query);
    if(context.render_color!=draw.color || context.render_depth!=draw.depth) {
        end_rendering();
        transition(draw.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if(draw.depth)transition(draw.depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    if(!context.render_color) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView=draw.color->view;color.imageLayout=draw.color->layout;color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if(draw.depth){depth.imageView=draw.depth->view;depth.imageLayout=draw.depth->layout;depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent={draw.color->width,draw.color->height};
        render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
        if(draw.depth)render.pDepthAttachment=render.pStencilAttachment=&depth;
        vkCmdBeginRendering(chunk.commands,&render);
        context.render_color=draw.color;context.render_depth=draw.depth;
    }
    if(context.bound_pipeline!=selected_pipeline) {
        vkCmdBindPipeline(chunk.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,selected_pipeline->second);
        context.bound_pipeline=selected_pipeline;
    }
    /* All our pipelines use dynamic depth state. Keep write/compare settings
     * while testing is disabled, since disabled depth cannot consume them. */
    const uint32_t previous_depth=context.bound_depth_state;
    const bool first_depth_state=previous_depth==UINT32_MAX;
    uint32_t depth_state=first_depth_state?0u:previous_depth&~depth_test_bit;
    if(draw.depth_test)depth_state=depth_test_bit|(draw.depth_write?depth_write_bit:0u)|
        (static_cast<uint32_t>(draw.depth_compare)<<depth_compare_shift);
    const uint32_t changed_depth=first_depth_state?UINT32_MAX:previous_depth^depth_state;
    if(changed_depth) {
        if(changed_depth&depth_test_bit)vkCmdSetDepthTestEnable(chunk.commands,(depth_state&depth_test_bit)!=0);
        if(changed_depth&depth_write_bit)vkCmdSetDepthWriteEnable(chunk.commands,(depth_state&depth_write_bit)!=0);
        if(changed_depth>>depth_compare_shift)vkCmdSetDepthCompareOp(chunk.commands,
            static_cast<VkCompareOp>(depth_state>>depth_compare_shift));
        context.bound_depth_state=depth_state;
    }
    if(context.viewport_width!=draw.color->width || context.viewport_height!=draw.color->height) {
        VkViewport viewport{0,(float)draw.color->height,(float)draw.color->width,-(float)draw.color->height,0,1};
        VkRect2D scissor{{0,0},{draw.color->width,draw.color->height}};
        vkCmdSetViewport(chunk.commands,0,1,&viewport);vkCmdSetScissor(chunk.commands,0,1,&scissor);
        context.viewport_width=draw.color->width;context.viewport_height=draw.color->height;
    }
    /* Every graphics pipeline uses the same layout. The bindings survive
     * changes of pipeline and render target within this command buffer. */
    if(context.bound_descriptors!=descriptor || context.bound_uniform_offsets!=dynamic_offsets) {
        vkCmdBindDescriptorSets(chunk.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,context.pipeline_layout,0,1,
            &descriptor->second,static_cast<uint32_t>(dynamic_offsets.size()),dynamic_offsets.data());
        context.bound_descriptors=descriptor;
        context.bound_uniform_offsets=dynamic_offsets;
    }
    if(context.bound_vertex!=vertex_buffer || context.bound_vertex_offset!=vertex_offset) {
        vkCmdBindVertexBuffers(chunk.commands,0,1,&vertex_buffer,&vertex_offset);
        context.bound_vertex=vertex_buffer;context.bound_vertex_offset=vertex_offset;
    }
    if(draw.query) {
        vkCmdBeginQuery(chunk.commands,draw.query->pool->handle,draw.query->index,VK_QUERY_CONTROL_PRECISE_BIT);
        draw.query->reset_recorded=false;
    }
    if(draw.indices) {
        if(context.bound_index!=vertex_buffer || context.bound_index_offset!=index_offset) {
            vkCmdBindIndexBuffer(chunk.commands,vertex_buffer,index_offset,VK_INDEX_TYPE_UINT32);
            context.bound_index=vertex_buffer;context.bound_index_offset=index_offset;
        }
        vkCmdDrawIndexed(chunk.commands,draw.draw_count,1,draw.draw_first,0,0);
    } else vkCmdDraw(chunk.commands,draw.draw_count,1,draw.draw_first,0);
    if(draw.query){vkCmdEndQuery(chunk.commands,draw.query->pool->handle,draw.query->index);draw.query->serial=context.submitted+1;}
    return 1;
    NVK_END(0)
}
int nvk_read_upload(uint64_t generation,uint32_t offset,void *output,size_t bytes) {
    NVK_BEGIN
    require(output!=nullptr,"Missing upload inspection destination");
    for(const auto &chunk:context.chunks)if(chunk.generation==generation && offset<=chunk.used && bytes<=chunk.used-offset) {
        std::memcpy(output,static_cast<const char *>(chunk.upload.mapped)+offset,bytes);return 1;
    }
    throw Failure("Vulkan upload range is no longer live");
    NVK_END(0)
}
NvkQuery *nvk_query_create(void) {
    NVK_BEGIN
    if(!free_occlusion_query)add_occlusion_pool();
    NvkQuery *query=free_occlusion_query;
    free_occlusion_query=query->next_free;
    return query;NVK_END(nullptr)
}
int nvk_query_result(NvkQuery *query,uint64_t *samples,int wait) {
    NVK_BEGIN
    require(query && query->serial && samples,"Invalid Vulkan visibility query");
    if(query->serial>context.submitted)submit();
    if(wait)wait_serial(query->serial);
    else {
        int completed=nvk_completed(query->serial,1);
        if(completed<=0)return completed;
    }
    VkResult result=vkGetQueryPoolResults(context.device,query->pool->handle,query->index,1,sizeof *samples,samples,sizeof *samples,VK_QUERY_RESULT_64_BIT);
    if(result==VK_NOT_READY)return 0;check(result,"vkGetQueryPoolResults");return 1;
    NVK_END(-1)
}
void nvk_query_destroy(NvkQuery *query) {
    if(!query)return;
    try {if(query->serial){if(query->serial>context.submitted)submit();wait_serial(query->serial);}
        /* Reuse only after the last submitted reset and draw have completed. */
        query->serial=0;
        query->next_free=free_occlusion_query;
        free_occlusion_query=query;
    }catch(const std::exception &e){error=e.what();}
}

int nvk_window_present(void *window,NvkImage *image,unsigned width,unsigned height,const int32_t bounds[4],int vsync) {
    NVK_BEGIN
    if(!width || !height)return 2;
    require(window && image && !image->volume && bounds,"Invalid Vulkan display image/window");
    if(context.window && context.window!=(HWND)window)throw Failure("Vulkan display window changed without closing");
    const VkPresentModeKHR mode=vsync?VK_PRESENT_MODE_FIFO_KHR:VK_PRESENT_MODE_IMMEDIATE_KHR;
    if(context.swapchain && (context.window_width!=width || context.window_height!=height || context.window_mode!=mode))window_close();
    if(!context.surface) {
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};info.hwnd=(HWND)window;info.hinstance=GetModuleHandleW(nullptr);
        CHECK(vkCreateWin32SurfaceKHR(context.instance,&info,nullptr,&context.surface));context.window=(HWND)window;
        VkBool32 supported;CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(context.physical,context.family,context.surface,&supported));
        require(supported!=0,"Vulkan graphics queue cannot present this window");
    }
    if(!context.swapchain) {
        VkSurfaceCapabilitiesKHR caps;CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.physical,context.surface,&caps));
        require((caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)!=0,"Vulkan surface cannot receive the completed image");
        unsigned count=0;CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(context.physical,context.surface,&count,nullptr));
        std::vector<VkSurfaceFormatKHR> formats(count);CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(context.physical,context.surface,&count,formats.data()));
        auto selected=std::find_if(formats.begin(),formats.end(),[](const auto &f){return f.format==VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        require(selected!=formats.end(),"Vulkan surface has no BGRA8 UNORM presentation format");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface=context.surface;info.minImageCount=std::max(2u,caps.minImageCount);
        if(caps.maxImageCount)info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);
        info.imageFormat=selected->format;info.imageColorSpace=selected->colorSpace;
        info.imageExtent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{width,height}:caps.currentExtent;
        if(!info.imageExtent.width || !info.imageExtent.height)return 2;
        info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.preTransform=caps.currentTransform;info.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        require((caps.supportedCompositeAlpha&info.compositeAlpha)!=0,"Vulkan surface has no opaque composition mode");
        unsigned mode_count=0;CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(context.physical,context.surface,&mode_count,nullptr));
        std::vector<VkPresentModeKHR> modes(mode_count);
        CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(context.physical,context.surface,&mode_count,modes.data()));
        require(std::find(modes.begin(),modes.end(),mode)!=modes.end(),"Vulkan surface does not support the selected VSync setting");
        /* Game pacing remains independent of the requested display mode. */
        info.presentMode=mode;info.clipped=VK_TRUE;
        CHECK(vkCreateSwapchainKHR(context.device,&info,nullptr,&context.swapchain));
        context.window_width=width;context.window_height=height;context.window_mode=mode;
        context.swap_extent=info.imageExtent;
        CHECK(vkGetSwapchainImagesKHR(context.device,context.swapchain,&count,nullptr));context.swap_images.resize(count);
        CHECK(vkGetSwapchainImagesKHR(context.device,context.swapchain,&count,context.swap_images.data()));
        context.ready.resize(count);context.presented.assign(count,false);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(auto &s:context.ready)CHECK(vkCreateSemaphore(context.device,&semaphore,nullptr,&s));
    }
    Chunk &chunk=context.chunks[context.active];
    /* Acquiring only reuses the semaphore. begin() waits for the command buffer. */
    if(!context.recording)wait_serial(chunk.acquire_serial);
    unsigned index;uint64_t acquisition_started=timing_clock();
    VkResult acquired=vkAcquireNextImageKHR(context.device,context.swapchain,0,chunk.acquired,{},&index);
    timing_finish(acquisition_started,3);
    if(acquired==VK_NOT_READY || acquired==VK_TIMEOUT)return 2;
    if(acquired==VK_ERROR_OUT_OF_DATE_KHR){window_close();return 2;}
    require(acquired==VK_SUCCESS || acquired==VK_SUBOPTIMAL_KHR,"Vulkan swapchain acquisition failed");
    begin();
    transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImage target=context.swap_images[index];
    barrier(target,context.presented[index]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,1);
    if(bounds[0]!=0 || bounds[1]!=0 ||
       bounds[2]!=(int32_t)context.swap_extent.width || bounds[3]!=(int32_t)context.swap_extent.height) {
        VkClearColorValue black{{0,0,0,1}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(chunk.commands,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,1);
    }
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[1]={(int32_t)image->width,(int32_t)image->height,1};
    blit.dstOffsets[0]={bounds[0],bounds[1],0};blit.dstOffsets[1]={bounds[2],bounds[3],1};
    VkFilter filter=(unsigned)(bounds[2]-bounds[0])>image->width?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
    vkCmdBlitImage(chunk.commands,image->image,image->layout,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_ASPECT_COLOR_BIT,1);
    chunk.acquire_serial=submit(chunk.acquired,context.ready[index]);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&context.ready[index];
    present.swapchainCount=1;present.pSwapchains=&context.swapchain;present.pImageIndices=&index;
    uint64_t presentation_started=timing_clock();
    VkResult result=vkQueuePresentKHR(context.queue,&present);context.presented[index]=true;
    timing_finish(presentation_started,2);
    if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR || acquired==VK_SUBOPTIMAL_KHR){window_close();return 2;}
    CHECK(result);return 1;NVK_END(0)
}
void nvk_window_close(void) {try{window_close();}catch(const std::exception &e){error=e.what();}}
void nvk_shutdown(void) {
    try {
        if(context.device) {
            window_close();
            context.bound_pipeline=nullptr;
            for(auto &p:pipelines)vkDestroyPipeline(context.device,p.second,nullptr);pipelines.clear();
            for(auto &s:samplers)vkDestroySampler(context.device,s.second,nullptr);samplers.clear();
            for(auto &pool:occlusion_pools)vkDestroyQueryPool(context.device,pool.handle,nullptr);
            occlusion_pools.clear();
            free_occlusion_query=nullptr;
            destroy(context.sync_transfer);
            for(auto &chunk:context.chunks) {
                destroy(chunk.upload);
                if(chunk.descriptors)vkDestroyDescriptorPool(context.device,chunk.descriptors,nullptr);
                if(chunk.acquired)vkDestroySemaphore(context.device,chunk.acquired,nullptr);
            }
            if(context.pipeline_cache)vkDestroyPipelineCache(context.device,context.pipeline_cache,nullptr);
            if(context.pipeline_layout)vkDestroyPipelineLayout(context.device,context.pipeline_layout,nullptr);
            if(context.descriptor_layout)vkDestroyDescriptorSetLayout(context.device,context.descriptor_layout,nullptr);
            if(context.timeline)vkDestroySemaphore(context.device,context.timeline,nullptr);
            if(context.pool)vkDestroyCommandPool(context.device,context.pool,nullptr);
            vkDestroyDevice(context.device,nullptr);
        }
        if(context.instance && vkDestroyInstance)vkDestroyInstance(context.instance,nullptr);
        if(context.compiler)context.compiler->Release();
        /* Driver and compiler modules retain process lifetime, as do the
         * existing graphics runtimes. */
        context=Context{};
    }catch(const std::exception &e){error=e.what();}
}
