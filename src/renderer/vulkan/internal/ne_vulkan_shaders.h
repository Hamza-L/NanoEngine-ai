#ifndef INTERNAL_NE_VULKAN_SHADERS_H
#define INTERNAL_NE_VULKAN_SHADERS_H

#include <stdint.h>

#include "internal/ne_vulkan_renderer.h"

/* ── Shader resource pool ───────────────────────────────────────────────── */
typedef struct NEVulkanShaderSlot {
    bool occupied;
    uint32_t stage;             /* NEShaderStage value */
    VkShaderModule module;
    char *entry_point;          /* entry point name */
} NEVulkanShaderSlot;

void ne_shader_destroy_all(NERenderer *renderer);

#endif //INTERNAL_NE_VULKAN_SHADERS_H
