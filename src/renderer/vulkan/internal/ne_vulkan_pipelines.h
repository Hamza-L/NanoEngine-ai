#ifndef INTERNAL_NE_VULKAN_PIPELINES_H
#define INTERNAL_NE_VULKAN_PIPELINES_H

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan/vulkan.h"

#include "ne_renderer_pipeline.h"

/* ── Pipeline resource pool ─────────────────────────────────────────────── */
typedef struct NEVulkanPipelineSlot {
    bool occupied;
    bool needs_compile;         /* true until first successful vkCreateGraphicsPipelines */
    bool compilation_failed;    /* true if deferred compilation failed */

    /* Eagerly created at ne_pipeline_create time. */
    VkPipelineLayout layout;

    /* Captured state for deferred vkCreateGraphicsPipelines. */
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    VkShaderModule compute_module;
    char *vert_entry;           /* strdup'd entry point name */
    char *frag_entry;           /* strdup'd entry point name */
    char *compute_entry;        /* strdup'd entry point name */

    VkVertexInputBindingDescription *bindings;
    uint32_t binding_count;
    VkVertexInputAttributeDescription *attributes;
    uint32_t attribute_count;

    VkPrimitiveTopology topology;
    VkPipelineColorBlendAttachmentState blend;

    /* Created during deferred compilation (VK_NULL_HANDLE until then). */
    VkPipeline pipeline;
} NEVulkanPipelineSlot;

bool ne_vk_pipeline_compile(NERenderer *r, NEVulkanPipelineSlot *slot, VkRenderPass render_pass);

void ne_pipeline_destroy_all(NERenderer *r);

#endif //INTERNAL_NE_VULKAN_PIPELINES_H
