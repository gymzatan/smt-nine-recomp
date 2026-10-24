# NV2A AY8 resource format

NV097 SZ_AY8 (0x01) and LU_IMAGE_AY8 (0x1B), exposed as AL8 and LIN_AL8, use the complete unsigned byte for both luminance and alpha. The original shared resource converter expands that byte into the existing A8L8 upload representation. The software texture consumer reuses the same converter; 2D and volume upload/update paths retain their original callers.

For raw A3 the RGBA channels are all A3 (packed A3A3A3A3). A8L8 remains two independent bytes: raw 7F4C produces 7F4C4C4C. This is independent of specialized color-key masks, which are not inferred from the format channel mapping.

Source: xemu NV2A Vulkan constants, SZ_AY8/LU_IMAGE_AY8 R8_UNORM with RRRR component mapping: https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/vk/constants.h
