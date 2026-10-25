#ifndef NINE_NV2A_VULKAN_H
#define NINE_NV2A_VULKAN_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include "vulkan/vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvkImage NvkImage;
typedef struct NvkShader NvkShader;
typedef struct NvkQuery NvkQuery;

/* Callers serialize this device with the original graphics-context lock.
 * These handles contain Vulkan resources only; no D3D device is created. */
int nvk_initialize(const wchar_t *compiler_path);
const char *nvk_error(void);
void nvk_shutdown(void);
uint64_t nvk_submit(void);
uint64_t nvk_fence(void);
int nvk_wait(uint64_t serial);
int nvk_completed(uint64_t serial);

NvkImage *nvk_image_create(unsigned width,unsigned height,unsigned layers,
                          VkFormat format,int cube);
/* Volume slices are z-major, each containing height rows at the upload/read pitch. */
NvkImage *nvk_image_create_volume(unsigned width,unsigned height,unsigned depth,VkFormat format);
void nvk_image_destroy(NvkImage *image);
int nvk_image_upload(NvkImage *image,const void *pixels,size_t pitch);
int nvk_image_read(NvkImage *image,void *pixels,size_t pitch);
/* Reads return tightly packed float/stencil tuples at (x*read_step,y*read_step).
 * Image dimensions must be divisible by read_step; uploads require step 1. */
int nvk_depth_transfer(NvkImage *image,void *pixels,int upload,unsigned read_step);
int nvk_image_clear(NvkImage *image,const float rgba[4]);
int nvk_depth_clear(NvkImage *image,VkImageAspectFlags aspects,float depth,
                    unsigned stencil,const int32_t bounds[4]);
int nvk_image_copy(NvkImage *source,NvkImage *destination,
                   const int32_t bounds[4],int linear);
int nvk_image_copy_rect(NvkImage *source,NvkImage *destination,
                        const int32_t source_bounds[4],const int32_t destination_bounds[4],int linear);

/* Source is emitted by the existing NV2A translators. Binding namespaces are
 * VS b0..3, GS b9..10, PS b16..17, textures t24..27, samplers s28..31. */
NvkShader *nvk_shader_create(const char *source,const char *entry,
                             VkShaderStageFlagBits stage);
void nvk_shader_destroy(NvkShader *shader);
unsigned nvk_shader_inputs(const NvkShader *shader,unsigned *locations,
                            unsigned *attributes,unsigned capacity);

typedef struct NvkUniform {
    const void *data;
    size_t bytes;
    unsigned binding;
} NvkUniform;
typedef struct NvkTexture {
    NvkImage *image;
    VkSamplerAddressMode u,v,w;
    VkFilter filter;
    float border[4];
} NvkTexture;
typedef struct NvkDraw {
    NvkImage *color,*depth;
    NvkShader *vertex,*geometry,*pixel;
    const void *vertices;
    size_t vertex_bytes;
    unsigned stride,count;
    unsigned draw_first,draw_count;
    const uint32_t *indices;
    /* Existing converted-vertex cache owns these range identities. */
    uint64_t *upload_generation;
    uint32_t *upload_offset;
    unsigned uploaded,upload_started;
    VkPrimitiveTopology topology;
    unsigned attribute_count;
    VkVertexInputAttributeDescription attributes[16];
    unsigned uniform_count;
    NvkUniform uniforms[12];
    NvkTexture textures[4];
    VkBool32 depth_test,depth_write,blend,stencil,wireframe;
    VkCompareOp depth_compare;
    VkStencilOpState stencil_state;
    VkBlendFactor source_blend,destination_blend;
    VkBlendOp blend_op;
    VkBool32 logic;
    VkLogicOp logic_op;
    VkColorComponentFlags color_mask;
    NvkQuery *query;
} NvkDraw;
int nvk_draw(NvkDraw *draw);
/* Read the actual host-visible upload selected by its existing range identity. */
int nvk_read_upload(uint64_t generation,uint32_t offset,void *output,size_t bytes);
NvkQuery *nvk_query_create(void);
int nvk_query_result(NvkQuery *query,uint64_t *samples,int wait);
void nvk_query_destroy(NvkQuery *query);
/* 0 fails, 1 presents, 2 retains the request for the existing retry. */
int nvk_window_present(void *window,NvkImage *image,unsigned width,
                        unsigned height,const int32_t bounds[4],int vsync);
void nvk_window_close(void);

#ifdef __cplusplus
}
#endif
#endif
