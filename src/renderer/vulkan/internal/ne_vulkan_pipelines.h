#ifndef INTERNAL_NE_VULKAN_PIPELINES_H
#define INTERNAL_NE_VULKAN_PIPELINES_H

#include "vulkan/vulkan.h"

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

#endif //INTERNAL_NE_VULKAN_PIPELINES_H
