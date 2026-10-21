#define VK_NO_PROTOTYPES
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include "nv2a_vulkan.h"
#include "vulkan/vulkan_win32.h"
#include "dxc/dxcapi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define NVK_INSTANCE_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) \
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
 X(CmdDraw) X(CmdDrawIndexed) X(CreateQueryPool) X(DestroyQueryPool) \
 X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults) \
 X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
 X(AcquireNextImageKHR) X(QueuePresentKHR)
#define DECLARE(name) static PFN_vk##name vk##name;
NVK_INSTANCE_FUNCTIONS(DECLARE)
NVK_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

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
struct Chunk {
    VkCommandBuffer commands{};
    VkDescriptorPool descriptors{};
    VkSemaphore acquired{};
    Buffer upload;
    VkDeviceSize used{};
    uint64_t serial{},last_read{},generation{};
    unsigned sets{},read_chunks{};
    std::map<std::array<uint64_t,16>,VkDescriptorSet> descriptor_sets;
    std::array<UniformRange,8> uniforms;
};
constexpr unsigned uniform_bindings[]={0,1,2,3,9,10,16,17};
unsigned uniform_slot(unsigned binding) {
    for(unsigned i=0;i<8;i++)if(uniform_bindings[i]==binding)return i;
    throw Failure("Unknown original shader uniform binding");
}
constexpr unsigned chunk_count=4;
constexpr VkDeviceSize upload_size=16*1024*1024;
struct Context {
    HMODULE loader{},compiler_module{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkBool32 fill_mode_non_solid{};
    VkDevice device{};
    VkQueue queue{};
    unsigned family{};
    VkCommandPool pool{};
    VkSemaphore timeline{};
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout pipeline_layout{};
    VkPipelineCache pipeline_cache{};
    std::array<Chunk,chunk_count> chunks{};
    Buffer readback;
    unsigned active{};
    bool recording{};
    NvkImage *render_color{},*render_depth{};
    VkPipeline bound_pipeline{};
    unsigned viewport_width{},viewport_height{};
    uint64_t submitted{},completed{},generation{};
    IDxcCompiler3 *compiler{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    HWND window{};
    unsigned window_width{},window_height{};
    VkPresentModeKHR window_mode{};
    VkFormat window_format{};
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> ready;
    std::vector<bool> presented;
} context;
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
            (usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT)?VK_MEMORY_PROPERTY_HOST_CACHED_BIT:0);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=index;
        CHECK(vkAllocateMemory(context.device,&alloc,nullptr,&b.memory));
        CHECK(vkBindBufferMemory(context.device,b.buffer,b.memory,0));
        CHECK(vkMapMemory(context.device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped));
        b.coherent=(context.memory.memoryTypes[index].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        return b;
    } catch(...) {destroy(b);throw;}
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
        if(chunk.sets>=2048) {
            CHECK(vkResetDescriptorPool(context.device,chunk.descriptors,0));
            chunk.sets=0;chunk.descriptor_sets.clear();
        }
        chunk.read_chunks=0;
        if(!chunk.generation)chunk.generation=++context.generation;
        context.bound_pipeline={};context.viewport_width=context.viewport_height=0;
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
VkDeviceSize upload(const void *data,size_t bytes,VkDeviceSize alignment=16) {
    Chunk &chunk=begin();
    VkDeviceSize offset=(chunk.used+alignment-1)&~(alignment-1);
    require(offset+bytes<=chunk.upload.size,"Vulkan draw exceeded its reserved upload range");
    std::memcpy(static_cast<char *>(chunk.upload.mapped)+offset,data,bytes);
    if(timing_enabled)g_nvk_timings[7]+=bytes;
    chunk.used=offset+bytes;return offset;
}
uint32_t upload_uniform(unsigned slot,const void *data,size_t bytes,VkDeviceSize alignment) {
    Chunk &chunk=begin();
    auto &range=chunk.uniforms[slot];
    if(range.generation==chunk.generation && range.value.size()==bytes &&
       !std::memcmp(range.value.data(),data,bytes))return range.offset;
    range.offset=(uint32_t)upload(data,bytes,alignment);
    range.value.assign(static_cast<const unsigned char *>(data),static_cast<const unsigned char *>(data)+bytes);
    range.generation=chunk.generation;
    return range.offset;
}
void reserve(size_t bytes) {
    Chunk &current=context.chunks[context.active];
    if(current.used+bytes<=current.upload.size) {
        begin();
        if(current.sets<2048)return;
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
template<size_t Capacity> struct StateKey {
    std::array<unsigned char,Capacity> data{};
    size_t size{};
    bool operator<(const StateKey &other) const {
        int order=std::memcmp(data.data(),other.data.data(),std::min(size,other.size));
        return order<0 || (!order && size<other.size);
    }
};
using PipelineKey=StateKey<512>;
using SamplerKey=StateKey<32>;
std::map<PipelineKey,VkPipeline> pipelines;
std::map<SamplerKey,VkSampler> samplers;
template<size_t Capacity,class T> void append(StateKey<Capacity> &key,const T &value) {
    require(key.size+sizeof value<=Capacity,"Native Vulkan state key capacity exceeded");
    std::memcpy(key.data.data()+key.size,&value,sizeof value);key.size+=sizeof value;
}
}

struct NvkImage {
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};
    VkFormat format{};unsigned width{},height{},layers{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};
struct NvkShader {
    VkShaderModule module{};VkShaderStageFlagBits stage{};std::string entry;
    std::vector<uint32_t> code;
    std::vector<std::pair<unsigned,unsigned>> inputs;
};
struct NvkQuery { VkQueryPool pool{};uint64_t serial{}; };

namespace {
void transition(NvkImage *image,VkImageLayout layout) {
    require(image!=nullptr,"Missing Vulkan image");
    if(image->layout==layout && (layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return;
    barrier(image->image,image->layout,layout,aspects(image->format),image->layers);image->layout=layout;
}
void image_destroy(NvkImage *image) {
    if(!image)return;
    for(auto &chunk:context.chunks)chunk.descriptor_sets.clear();
    if(image->view)vkDestroyImageView(context.device,image->view,nullptr);
    if(image->image)vkDestroyImage(context.device,image->image,nullptr);
    if(image->memory)vkFreeMemory(context.device,image->memory,nullptr);
    delete image;
}
NvkImage *image_create(unsigned width,unsigned height,unsigned layers,VkFormat format,bool cube) {
    require(width && height && (cube?layers==6:layers==1),"Invalid Vulkan image dimensions");
    auto *image=new NvkImage{};image->width=width;image->height=height;image->layers=layers;image->format=format;
    try {
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(context.physical,format,&properties);
        VkFormatFeatureFlags required=aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        require((properties.optimalTilingFeatures&required)==required,"Unsupported Vulkan image format");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;info.imageType=VK_IMAGE_TYPE_2D;
        info.format=format;info.extent={width,height,1};info.mipLevels=1;info.arrayLayers=layers;
        info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(aspects(format)==VK_IMAGE_ASPECT_COLOR_BIT)info.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        else info.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        CHECK(vkCreateImage(context.device,&info,nullptr,&image->image));
        VkMemoryRequirements req;vkGetImageMemoryRequirements(context.device,image->image,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
        alloc.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(context.device,&alloc,nullptr,&image->memory));
        CHECK(vkBindImageMemory(context.device,image->image,image->memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image->image;view.format=format;
        view.viewType=cube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D;
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
            context.fill_mode_non_solid=features.features.fillModeNonSolid;break;
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
    features.fillModeNonSolid=context.fill_mode_non_solid;
    const char *device_extensions[]={"VK_KHR_swapchain"};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.pNext=&f12;info.queueCreateInfoCount=1;info.pQueueCreateInfos=&queue;
    info.pEnabledFeatures=&features;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=device_extensions;
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
    context.compiler_module=LoadLibraryExW(compiler_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(context.compiler_module!=nullptr,"Cannot load the Vulkan SPIR-V compiler");
    auto factory=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(context.compiler_module,"DxcCreateInstance"));
    require(factory!=nullptr,"Missing DxcCreateInstance");
    require(SUCCEEDED(factory(CLSID_DxcCompiler,IID_PPV_ARGS(&context.compiler))),"Cannot create SPIR-V compiler");
    std::fprintf(stderr,"[GPU Vulkan native] %s; Vulkan %u.%u; direct Vulkan queue/resources, no D3D device\n",
        context.properties.deviceName,VK_API_VERSION_MAJOR(context.properties.apiVersion),VK_API_VERSION_MINOR(context.properties.apiVersion));
    return 1;
    NVK_END(0)
}

uint64_t nvk_submit(void) {NVK_BEGIN return submit();NVK_END(0)}
uint64_t nvk_fence(void) {return context.submitted+(context.recording?1:0);}
int nvk_wait(uint64_t serial) {NVK_BEGIN if(serial>context.submitted)submit();wait_serial(serial);return 1;NVK_END(0)}
int nvk_completed(uint64_t serial) {
    NVK_BEGIN
    if(serial<=context.completed)return 1;
    if(serial>context.submitted)submit();
    uint64_t done;CHECK(vkGetSemaphoreCounterValue(context.device,context.timeline,&done));
    context.completed=done;return done>=serial?1:0;
    NVK_END(-1)
}
NvkImage *nvk_image_create(unsigned w,unsigned h,unsigned layers,VkFormat format,int cube) {
    NVK_BEGIN return image_create(w,h,layers,format,cube!=0);NVK_END(nullptr)
}
void nvk_image_destroy(NvkImage *image) {
    try {if(image){wait_serial(submit());image_destroy(image);}}catch(const std::exception &e){error=e.what();}
}
int nvk_image_upload(NvkImage *image,const void *pixels,size_t pitch) {
    NVK_BEGIN
    require(image && pixels && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT && pitch>=image->width*4,"Invalid Vulkan color upload");
    size_t row=image->width*4,bytes=row*image->height*image->layers;
    Buffer staging=buffer_create(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    try {
        for(unsigned y=0;y<image->height*image->layers;y++)
            std::memcpy(static_cast<char *>(staging.mapped)+row*y,static_cast<const char *>(pixels)+pitch*y,row);
        host_memory(staging,false);transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,image->layers};copy.imageExtent={image->width,image->height,1};
        vkCmdCopyBufferToImage(begin().commands,staging.buffer,image->image,image->layout,1,&copy);
        wait_serial(submit());destroy(staging);return 1;
    }catch(...){destroy(staging);throw;}
    NVK_END(0)
}
int nvk_image_read(NvkImage *image,void *pixels,size_t pitch) {
    NVK_BEGIN
    require(image && pixels && image->format!=VK_FORMAT_D32_SFLOAT_S8_UINT && pitch>=image->width*4,"Invalid Vulkan color readback");
    size_t row=image->width*4,bytes=row*image->height*image->layers;
    /* The original graphics lock serializes reads, and each read waits for
     * its GPU copy before returning. Reuse storage, never pixel contents. */
    Buffer &staging=context.readback;
    if(staging.size<bytes) {
        Buffer larger=buffer_create(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        destroy(staging);staging=larger;
    }
    try {
        transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,image->layers};copy.imageExtent={image->width,image->height,1};
        vkCmdCopyImageToBuffer(begin().commands,image->image,image->layout,staging.buffer,1,&copy);
        wait_serial(submit());host_memory(staging,true);
        for(unsigned y=0;y<image->height*image->layers;y++)
            std::memcpy(static_cast<char *>(pixels)+pitch*y,static_cast<char *>(staging.mapped)+row*y,row);
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
int nvk_depth_transfer(NvkImage *image,void *pixels,int writing) {
    NVK_BEGIN
    require(image && pixels && image->format==VK_FORMAT_D32_SFLOAT_S8_UINT && image->layers==1,"Invalid Vulkan depth transfer");
    size_t count=(size_t)image->width*image->height;
    Buffer staging=buffer_create(count*5,writing?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_TRANSFER_DST_BIT);
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
            for(size_t i=0;i<count;i++) {
                std::memcpy(static_cast<char *>(pixels)+i*8,static_cast<char *>(staging.mapped)+i*4,4);
                std::memset(static_cast<char *>(pixels)+i*8+4,0,4);
                static_cast<char *>(pixels)[i*8+4]=static_cast<char *>(staging.mapped)[count*4+i];
            }
        }
        destroy(staging);return 1;
    }catch(...){destroy(staging);throw;}
    NVK_END(0)
}
int nvk_depth_clear(NvkImage *image,VkImageAspectFlags mask,float depth,unsigned stencil,const int32_t bounds[4]) {
    NVK_BEGIN
    require(image && image->format==VK_FORMAT_D32_SFLOAT_S8_UINT && mask && !(mask&~aspects(image->format)),"Invalid Vulkan depth clear");
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
    require(src && dst && src!=dst && src->layers==1 && dst->layers==1 && src->format!=VK_FORMAT_D32_SFLOAT_S8_UINT && dst->format!=VK_FORMAT_D32_SFLOAT_S8_UINT,"Invalid Vulkan image copy");
    int32_t full[4]={0,0,(int32_t)dst->width,(int32_t)dst->height};if(!bounds)bounds=full;
    int32_t source_full[4]={0,0,(int32_t)src->width,(int32_t)src->height};if(!source_bounds)source_bounds=source_full;
    require(bounds[0]>=0 && bounds[1]>=0 && bounds[2]>bounds[0] && bounds[3]>bounds[1] && bounds[2]<=(int)dst->width && bounds[3]<=(int)dst->height,"Invalid Vulkan copy bounds");
    require(source_bounds[0]>=0 && source_bounds[1]>=0 && source_bounds[2]>source_bounds[0] && source_bounds[3]>source_bounds[1] &&
        source_bounds[2]<=(int)src->width && source_bounds[3]<=(int)src->height,"Invalid Vulkan source bounds");
    transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);transition(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[0]={source_bounds[0],source_bounds[1],0};blit.srcOffsets[1]={source_bounds[2],source_bounds[3],1};
    blit.dstOffsets[0]={bounds[0],bounds[1],0};blit.dstOffsets[1]={bounds[2],bounds[3],1};
    vkCmdBlitImage(begin().commands,src->image,src->layout,dst->image,dst->layout,1,&blit,linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    return 1;NVK_END(0)
}

static void reflect_inputs(NvkShader *shader);
NvkShader *nvk_shader_create(const char *source,const char *entry,VkShaderStageFlagBits stage) {
    NVK_BEGIN
    require(source && entry && context.compiler,"Missing Vulkan shader source/compiler");
    const wchar_t *target=stage==VK_SHADER_STAGE_VERTEX_BIT?L"vs_6_0":stage==VK_SHADER_STAGE_GEOMETRY_BIT?L"gs_6_0":L"ps_6_0";
    const wchar_t *shift=stage==VK_SHADER_STAGE_VERTEX_BIT?L"0":stage==VK_SHADER_STAGE_GEOMETRY_BIT?L"8":L"16";
    std::wstring wide_entry(entry,entry+std::strlen(entry));
    std::vector<LPCWSTR> args={L"-spirv",L"-T",target,L"-E",wide_entry.c_str(),L"-O3",L"-Gis",
        L"-fspv-target-env=vulkan1.3",L"-fvk-use-dx-layout",L"-fvk-stage-io-order=alpha",
        L"-fvk-b-shift",shift,L"0",L"-fvk-t-shift",L"24",L"0",L"-fvk-s-shift",L"28",L"0"};
    if(stage==VK_SHADER_STAGE_FRAGMENT_BIT)args.push_back(L"-fvk-use-dx-position-w");
    DxcBuffer input{source,std::strlen(source),DXC_CP_UTF8};IDxcResult *result=nullptr;
    require(SUCCEEDED(context.compiler->Compile(&input,args.data(),(UINT32)args.size(),nullptr,IID_PPV_ARGS(&result))),"SPIR-V compiler invocation failed");
    HRESULT status=E_FAIL;result->GetStatus(&status);
    if(FAILED(status)) {
        IDxcBlobUtf8 *diagnostic=nullptr;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&diagnostic),nullptr);
        std::string message=diagnostic?diagnostic->GetStringPointer():"SPIR-V compilation failed";
        if(diagnostic)diagnostic->Release();result->Release();throw Failure(message);
    }
    IDxcBlob *code=nullptr;HRESULT hr=result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&code),nullptr);result->Release();
    require(SUCCEEDED(hr) && code,"SPIR-V compiler produced no object");
    auto *shader=new NvkShader{};shader->stage=stage;shader->entry=entry;
    shader->code.resize(code->GetBufferSize()/4);std::memcpy(shader->code.data(),code->GetBufferPointer(),code->GetBufferSize());code->Release();
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=shader->code.size()*4;info.pCode=shader->code.data();
    VkResult created=vkCreateShaderModule(context.device,&info,nullptr,&shader->module);
    if(created!=VK_SUCCESS){delete shader;check(created,"vkCreateShaderModule");}
    reflect_inputs(shader);
    return shader;NVK_END(nullptr)
}
void nvk_shader_destroy(NvkShader *shader) {
    if(!shader)return;
    try {wait_serial(submit());for(auto &p:pipelines)vkDestroyPipeline(context.device,p.second,nullptr);pipelines.clear();
        vkDestroyShaderModule(context.device,shader->module,nullptr);delete shader;
    }catch(const std::exception &e){error=e.what();}
}
static void reflect_inputs(NvkShader *shader) {
    if(shader->stage!=VK_SHADER_STAGE_VERTEX_BIT)return;
    std::map<uint32_t,std::string> names;std::map<uint32_t,uint32_t> loc;std::vector<uint32_t> inputs;
    for(size_t i=5;i<shader->code.size();) {
        uint32_t head=shader->code[i],count=head>>16,op=head&65535;
        require(count && count<=shader->code.size()-i,"Invalid compiled SPIR-V instruction");
        const uint32_t *p=shader->code.data()+i;
        if(op==5 && count>2)names[p[1]]=reinterpret_cast<const char *>(p+2);
        if(op==71 && count==4 && p[2]==30)loc[p[1]]=p[3];
        if(op==59 && count>=4 && p[3]==1)inputs.push_back(p[2]);
        i+=count;
    }
    for(uint32_t id:inputs) {
        auto it=names.find(id);if(it==names.end() || loc.find(id)==loc.end())continue;
        auto start=it->second.find("ATTR");if(start==std::string::npos)continue;
        shader->inputs.emplace_back(loc[id],(unsigned)std::stoul(it->second.substr(start+4)));
    }
}
unsigned nvk_shader_inputs(const NvkShader *shader,unsigned *locations,unsigned *attributes,unsigned capacity) {
    if(!shader)return 0;
    for(unsigned i=0;i<capacity && i<shader->inputs.size();i++) {
        locations[i]=shader->inputs[i].first;attributes[i]=shader->inputs[i].second;
    }
    return (unsigned)shader->inputs.size();
}

namespace {
VkSampler sampler(const NvkTexture &texture) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter=info.minFilter=texture.filter;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=texture.u;info.addressModeV=texture.v;info.addressModeW=texture.w;
    const float black[4]={0,0,0,0},opaque[4]={0,0,0,1},white[4]={1,1,1,1};
    if(!std::memcmp(texture.border,black,16))info.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if(!std::memcmp(texture.border,opaque,16))info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(!std::memcmp(texture.border,white,16))info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else require(texture.u!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER && texture.v!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER &&
        texture.w!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,"Custom Vulkan texture border color is not connected");
    SamplerKey key;append(key,info.magFilter);append(key,info.addressModeU);
    append(key,info.addressModeV);append(key,info.addressModeW);append(key,info.borderColor);
    auto found=samplers.find(key);if(found!=samplers.end())return found->second;
    VkSampler result;CHECK(vkCreateSampler(context.device,&info,nullptr,&result));samplers.emplace(key,result);return result;
}
VkPipeline pipeline(const NvkDraw &draw) {
    PipelineKey key;
    append(key,draw.vertex->module);VkShaderModule gs=draw.geometry?draw.geometry->module:VkShaderModule{};
    append(key,gs);append(key,draw.pixel->module);append(key,draw.stride);append(key,draw.topology);
    append(key,draw.color->format);VkFormat depth_format=draw.depth?draw.depth->format:VK_FORMAT_UNDEFINED;append(key,depth_format);
    append(key,draw.attribute_count);
    for(unsigned i=0;i<draw.attribute_count;i++)append(key,draw.attributes[i]);
    append(key,draw.depth_test);append(key,draw.depth_write);append(key,draw.depth_compare);
    append(key,draw.stencil);append(key,draw.stencil_state);append(key,draw.blend);
    append(key,draw.wireframe);
    append(key,draw.source_blend);append(key,draw.destination_blend);append(key,draw.blend_op);append(key,draw.color_mask);
    auto found=pipelines.find(key);if(found!=pipelines.end())return found->second;
    require(pipelines.size()<8192,"Vulkan pipeline cache capacity exceeded");
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
    depth.depthTestEnable=draw.depth_test;depth.depthWriteEnable=draw.depth_write;depth.depthCompareOp=draw.depth_compare;
    depth.stencilTestEnable=draw.stencil;depth.front=depth.back=draw.stencil_state;
    VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=draw.blend;blend.colorWriteMask=draw.color_mask;
    blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=draw.source_blend;
    blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=draw.destination_blend;
    blend.colorBlendOp=blend.alphaBlendOp=draw.blend_op;
    VkPipelineColorBlendStateCreateInfo blends{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blends.attachmentCount=1;blends.pAttachments=&blend;
    VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&draw.color->format;
    rendering.depthAttachmentFormat=rendering.stencilAttachmentFormat=depth_format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pNext=&rendering;
    info.stageCount=count;info.pStages=stages;info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;
    info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&samples;
    info.pDepthStencilState=&depth;info.pColorBlendState=&blends;info.pDynamicState=&dynamic;info.layout=context.pipeline_layout;
    VkPipeline result;CHECK(vkCreateGraphicsPipelines(context.device,context.pipeline_cache,1,&info,nullptr,&result));
    pipelines.emplace(std::move(key),result);return result;
}
}

int nvk_draw(NvkDraw *input) {
    NVK_BEGIN
    require(input && input->color && input->vertex && input->pixel && input->count && input->vertices &&
        input->attribute_count<=16 && input->uniform_count<=8,"Incomplete Vulkan draw");
    const auto &draw=*input;size_t bytes=draw.vertex_bytes+(draw.indices?draw.count*4:0)+32;
    VkDeviceSize align=std::max<VkDeviceSize>(16,context.properties.limits.minUniformBufferOffsetAlignment);
    for(unsigned i=0;i<draw.uniform_count;i++)bytes+=draw.uniforms[i].bytes+align;
    reserve(bytes);
    auto &chunk=begin();
    VkBuffer vertex_buffer{};VkDeviceSize vertex_offset=0,index_offset=0;
    input->uploaded=input->upload_started=0;
    const size_t stream_bytes=draw.vertex_bytes+(draw.indices?draw.count*4:0);
    if(draw.upload_generation && draw.upload_offset && *draw.upload_generation) {
        for(unsigned i=0;i<chunk_count;i++) {
            auto &cached=context.chunks[i];
            if(cached.generation==*draw.upload_generation && *draw.upload_offset+stream_bytes<=cached.used) {
                vertex_buffer=cached.upload.buffer;vertex_offset=*draw.upload_offset;
                index_offset=vertex_offset+draw.vertex_bytes;chunk.read_chunks|=1u<<i;break;
            }
        }
    }
    if(!vertex_buffer) {
        vertex_buffer=chunk.upload.buffer;vertex_offset=upload(draw.vertices,draw.vertex_bytes);
        if(draw.indices)index_offset=upload(draw.indices,draw.count*4,4);
        input->uploaded=1;input->upload_started=vertex_offset==0;
        if(draw.upload_generation && draw.upload_offset) {
            *draw.upload_generation=chunk.generation;*draw.upload_offset=(uint32_t)vertex_offset;
        }
    }
    std::array<uint64_t,16> descriptor_key{};
    uint32_t dynamic_offsets[8]{},uniform_offsets[8]{};
    for(unsigned i=0;i<8;i++)descriptor_key[i]=16;
    for(unsigned i=0;i<draw.uniform_count;i++) {
        const auto &uniform=draw.uniforms[i];
        require(uniform.data && uniform.bytes && uniform.bytes<=context.properties.limits.maxUniformBufferRange,"Invalid Vulkan uniform range");
        unsigned slot=uniform_slot(uniform.binding),shared=i;
        for(unsigned j=0;j<i;j++)if(draw.uniforms[j].data==uniform.data && draw.uniforms[j].bytes==uniform.bytes){shared=j;break;}
        uniform_offsets[i]=shared<i?uniform_offsets[shared]:upload_uniform(slot,uniform.data,uniform.bytes,align);
        dynamic_offsets[slot]=uniform_offsets[i];descriptor_key[slot]=uniform.bytes;
    }
    VkDescriptorImageInfo images[8]{};
    for(unsigned i=0;i<4;i++)if(draw.textures[i].image) {
        const auto &texture=draw.textures[i];require(texture.image!=draw.color,"Vulkan color attachment cannot sample itself");
        transition(texture.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        images[i*2]={VkSampler{},texture.image->view,texture.image->layout};
        images[i*2+1]={sampler(texture),VkImageView{},VK_IMAGE_LAYOUT_UNDEFINED};
        std::memcpy(&descriptor_key[8+i],&images[i*2].imageView,sizeof(VkImageView));
        std::memcpy(&descriptor_key[12+i],&images[i*2+1].sampler,sizeof(VkSampler));
    }
    VkDescriptorSet descriptors;
    auto cached=chunk.descriptor_sets.find(descriptor_key);
    if(cached!=chunk.descriptor_sets.end())descriptors=cached->second;
    else {
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
        for(unsigned i=0;i<4;i++)if(draw.textures[i].image)for(unsigned j=0;j<2;j++) {
            auto &write=writes[write_count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=descriptors;
            write.dstBinding=24+i+j*4;write.descriptorCount=1;
            write.descriptorType=j?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&images[i*2+j];
        }
        vkUpdateDescriptorSets(context.device,write_count,writes,0,nullptr);
        chunk.descriptor_sets.emplace(descriptor_key,descriptors);
    }
    if(draw.query) {
        end_rendering();vkCmdResetQueryPool(chunk.commands,draw.query->pool,0,1);
    }
    if(context.render_color!=draw.color || context.render_depth!=draw.depth) {
        end_rendering();
        transition(draw.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if(draw.depth)transition(draw.depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView=draw.color->view;color.imageLayout=draw.color->layout;color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if(draw.depth){depth.imageView=draw.depth->view;depth.imageLayout=draw.depth->layout;depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent={draw.color->width,draw.color->height};
    render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
    if(draw.depth)render.pDepthAttachment=render.pStencilAttachment=&depth;
    VkPipeline selected_pipeline=pipeline(draw);
    if(!context.render_color) {
        vkCmdBeginRendering(chunk.commands,&render);
        context.render_color=draw.color;context.render_depth=draw.depth;
    }
    if(context.bound_pipeline!=selected_pipeline) {
        vkCmdBindPipeline(chunk.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,selected_pipeline);
        context.bound_pipeline=selected_pipeline;
    }
    if(context.viewport_width!=draw.color->width || context.viewport_height!=draw.color->height) {
        VkViewport viewport{0,(float)draw.color->height,(float)draw.color->width,-(float)draw.color->height,0,1};
        VkRect2D scissor{{0,0},{draw.color->width,draw.color->height}};
        vkCmdSetViewport(chunk.commands,0,1,&viewport);vkCmdSetScissor(chunk.commands,0,1,&scissor);
        context.viewport_width=draw.color->width;context.viewport_height=draw.color->height;
    }
    vkCmdBindDescriptorSets(chunk.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,context.pipeline_layout,0,1,&descriptors,8,dynamic_offsets);
    vkCmdBindVertexBuffers(chunk.commands,0,1,&vertex_buffer,&vertex_offset);
    if(draw.query)vkCmdBeginQuery(chunk.commands,draw.query->pool,0,VK_QUERY_CONTROL_PRECISE_BIT);
    if(draw.indices) {
        vkCmdBindIndexBuffer(chunk.commands,vertex_buffer,index_offset,VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(chunk.commands,draw.count,1,0,0,0);
    } else vkCmdDraw(chunk.commands,draw.count,1,0,0);
    if(draw.query){vkCmdEndQuery(chunk.commands,draw.query->pool,0);draw.query->serial=context.submitted+1;}
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
    auto *query=new NvkQuery{};
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_OCCLUSION;info.queryCount=1;
    VkResult result=vkCreateQueryPool(context.device,&info,nullptr,&query->pool);
    if(result!=VK_SUCCESS){delete query;check(result,"vkCreateQueryPool");}
    return query;NVK_END(nullptr)
}
int nvk_query_result(NvkQuery *query,uint64_t *samples,int wait) {
    NVK_BEGIN
    require(query && query->serial && samples,"Invalid Vulkan visibility query");
    if(query->serial>context.submitted)submit();
    if(wait)wait_serial(query->serial);
    VkResult result=vkGetQueryPoolResults(context.device,query->pool,0,1,sizeof *samples,samples,sizeof *samples,VK_QUERY_RESULT_64_BIT);
    if(result==VK_NOT_READY)return 0;check(result,"vkGetQueryPoolResults");return 1;
    NVK_END(-1)
}
void nvk_query_destroy(NvkQuery *query) {
    if(!query)return;
    try {if(query->serial){if(query->serial>context.submitted)submit();wait_serial(query->serial);}
        vkDestroyQueryPool(context.device,query->pool,nullptr);delete query;
    }catch(const std::exception &e){error=e.what();}
}

int nvk_window_present(void *window,NvkImage *image,unsigned width,unsigned height,const int32_t bounds[4],int vsync) {
    NVK_BEGIN
    if(!width || !height)return 1;
    require(window && image && bounds,"Invalid Vulkan display image/window");
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
        context.window_width=width;context.window_height=height;context.window_format=selected->format;context.window_mode=mode;
        CHECK(vkGetSwapchainImagesKHR(context.device,context.swapchain,&count,nullptr));context.swap_images.resize(count);
        CHECK(vkGetSwapchainImagesKHR(context.device,context.swapchain,&count,context.swap_images.data()));
        context.ready.resize(count);context.presented.assign(count,false);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(auto &s:context.ready)CHECK(vkCreateSemaphore(context.device,&semaphore,nullptr,&s));
    }
    Chunk &chunk=begin();unsigned index;uint64_t acquisition_started=timing_clock();
    VkResult acquired=vkAcquireNextImageKHR(context.device,context.swapchain,0,chunk.acquired,{},&index);
    timing_finish(acquisition_started,3);
    if(acquired==VK_NOT_READY || acquired==VK_TIMEOUT)return 1;
    if(acquired==VK_ERROR_OUT_OF_DATE_KHR){window_close();return 1;}
    require(acquired==VK_SUCCESS || acquired==VK_SUBOPTIMAL_KHR,"Vulkan swapchain acquisition failed");
    transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImage target=context.swap_images[index];
    barrier(target,context.presented[index]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,1);
    VkClearColorValue black{{0,0,0,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(chunk.commands,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,1);
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[1]={(int32_t)image->width,(int32_t)image->height,1};
    blit.dstOffsets[0]={bounds[0],bounds[1],0};blit.dstOffsets[1]={bounds[2],bounds[3],1};
    VkFilter filter=(unsigned)(bounds[2]-bounds[0])>image->width?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
    vkCmdBlitImage(chunk.commands,image->image,image->layout,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_ASPECT_COLOR_BIT,1);
    submit(chunk.acquired,context.ready[index]);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&context.ready[index];
    present.swapchainCount=1;present.pSwapchains=&context.swapchain;present.pImageIndices=&index;
    uint64_t presentation_started=timing_clock();
    VkResult result=vkQueuePresentKHR(context.queue,&present);context.presented[index]=true;
    timing_finish(presentation_started,2);
    if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR || acquired==VK_SUBOPTIMAL_KHR){window_close();return 1;}
    CHECK(result);return 1;NVK_END(0)
}
void nvk_window_close(void) {try{window_close();}catch(const std::exception &e){error=e.what();}}
void nvk_shutdown(void) {
    try {
        if(context.device) {
            window_close();
            for(auto &p:pipelines)vkDestroyPipeline(context.device,p.second,nullptr);pipelines.clear();
            for(auto &s:samplers)vkDestroySampler(context.device,s.second,nullptr);samplers.clear();
            destroy(context.readback);
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
