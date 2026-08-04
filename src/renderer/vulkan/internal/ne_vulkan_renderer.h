#ifndef INTERNAL_NE_VULKAN_RENDERER_H
#define INTERNAL_NE_VULKAN_RENDERER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "ne_alloc.h"
#include "ne_renderer_shader.h"
#include "ne_renderer_image.h"


struct NERenderer {
    HMODULE vulkan_lib;
    VkInstance instance;

    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;

    NEPool buffers;
    NEPool shaders;
    NEPool pipelines;

    /* Staging buffer for data uploads */
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    uint32_t staging_size;

    /* Transfer command pool for staging uploads */
    VkCommandPool transfer_cmd_pool;
    VkCommandBuffer transfer_cmd;
    VkFence transfer_fence;

    /* Runtime GLSL → SPIR-V compilation via shaderc (loaded dynamically). */
    NEShaderOptimization shader_optimization; /* default: NE_SHADER_OPTIMIZATION_NONE (0) */

    struct NERenderSurface *surfaces;
};

uint32_t ne_vk_find_memory_type(NERenderer *r, uint32_t type_filter, VkMemoryPropertyFlags properties);

bool ne_vk_submit_transfer_cmd(NERenderer *r, VkCommandBuffer cmd);

void ne_cmd_transition_image_layout(const NERenderer *renderer, const VkCommandBuffer cmd, const NEImageHandle handle, const VkImageLayout oldLayout, const VkImageLayout newLayout);

#endif //INTERNAL_NE_VULKAN_RENDERER_H
