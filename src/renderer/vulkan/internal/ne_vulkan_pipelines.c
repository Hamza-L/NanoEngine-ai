#include "internal/ne_vulkan_pipelines.h"
#include "internal/ne_vulkan_renderer.h"
#include "internal/ne_vulkan_shaders.h"

#include "ne_renderer_pipeline.h"
#include "ne_log.h"

extern PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
extern PFN_vkResetFences vkResetFences;
extern PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
extern PFN_vkDestroyPipeline vkDestroyPipeline;
extern PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
extern PFN_vkDeviceWaitIdle vkDeviceWaitIdle;

/* ========================================================================
 * Pipeline Management
 * ======================================================================== */

/*
 * PIPELINE ARCHITECTURE — DEFERRED COMPILATION
 *
 * ne_pipeline_create() validates inputs, resolves shader handles, deep-copies
 * vertex layout state, creates the VkPipelineLayout, and stores everything in
 * a pool slot with needs_compile = true.
 *
 * The actual vkCreateGraphicsPipelines call is deferred until
 * ne_render_pass_set_pipeline() (Task #5), where we have access to the
 * surface's VkRenderPass.  This avoids coupling pipeline creation to any
 * specific render surface.
 *
 * ne_vk_pipeline_compile() is the internal helper that performs the deferred
 * compilation.  It is called from ne_render_pass_set_pipeline() on first use.
 */


/* ── Enum converters ─────────────────────────────────────────────────────── */

static VkFormat ne_vertex_format_to_vk(NEVertexFormat fmt) {
    switch (fmt) {
    case NE_VERTEX_FORMAT_FLOAT:    return VK_FORMAT_R32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT2:   return VK_FORMAT_R32G32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT3:   return VK_FORMAT_R32G32B32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NE_VERTEX_FORMAT_UNORM8X4: return VK_FORMAT_R8G8B8A8_UNORM;
    default:                        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor ne_blend_factor_to_vk(NEBlendFactor f) {
    switch (f) {
    case NE_BLEND_FACTOR_ZERO:                return VK_BLEND_FACTOR_ZERO;
    case NE_BLEND_FACTOR_ONE:                 return VK_BLEND_FACTOR_ONE;
    case NE_BLEND_FACTOR_SRC_ALPHA:           return VK_BLEND_FACTOR_SRC_ALPHA;
    case NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NE_BLEND_FACTOR_DST_ALPHA:           return VK_BLEND_FACTOR_DST_ALPHA;
    case NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:                                  return VK_BLEND_FACTOR_ZERO;
    }
}

static VkBlendOp ne_blend_op_to_vk(NEBlendOp op) {
    switch (op) {
    case NE_BLEND_OP_ADD:              return VK_BLEND_OP_ADD;
    case NE_BLEND_OP_SUBTRACT:         return VK_BLEND_OP_SUBTRACT;
    case NE_BLEND_OP_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NE_BLEND_OP_MIN:              return VK_BLEND_OP_MIN;
    case NE_BLEND_OP_MAX:              return VK_BLEND_OP_MAX;
    default:                           return VK_BLEND_OP_ADD;
    }
}

static VkPrimitiveTopology ne_topology_to_vk(NEPrimitiveTopology t) {
    switch (t) {
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NE_PRIMITIVE_TOPOLOGY_LINE_LIST:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NE_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NE_PRIMITIVE_TOPOLOGY_POINT_LIST:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    default:                                   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* ── Deferred pipeline compilation ───────────────────────────────────────── */

/**
 * Compile a pipeline slot into a live VkPipeline.
 *
 * Called from ne_render_pass_set_pipeline() when slot->needs_compile is true.
 * Requires a valid VkRenderPass from the active render surface.
 *
 * On success: clears needs_compile, sets slot->pipeline.
 * On failure: sets compilation_failed, logs error.
 */
bool ne_vk_pipeline_compile(NERenderer *r, NEVulkanPipelineSlot *slot, VkRenderPass render_pass) {
    /* ── Shader stages ───────────────────────────────────────────────── */

    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));

    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = slot->vert_module;
    stages[0].pName  = slot->vert_entry;

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = slot->frag_module;
    stages[1].pName  = slot->frag_entry;

    /* ── Vertex input state ──────────────────────────────────────────── */

    VkPipelineVertexInputStateCreateInfo vertex_input;
    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = slot->binding_count;
    vertex_input.pVertexBindingDescriptions      = slot->bindings;
    vertex_input.vertexAttributeDescriptionCount = slot->attribute_count;
    vertex_input.pVertexAttributeDescriptions    = slot->attributes;

    /* ── Input assembly ──────────────────────────────────────────────── */

    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    memset(&input_assembly, 0, sizeof(input_assembly));
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = slot->topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    /* ── Viewport / scissor (dynamic — values set at draw time) ──────── */

    VkPipelineViewportStateCreateInfo viewport_state;
    memset(&viewport_state, 0, sizeof(viewport_state));
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    /* ── Rasterization ───────────────────────────────────────────────── */

    VkPipelineRasterizationStateCreateInfo rasterization;
    memset(&rasterization, 0, sizeof(rasterization));
    rasterization.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode    = VK_CULL_MODE_NONE;
    rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth   = 1.0f;

    /* ── Multisample (no MSAA) ───────────────────────────────────────── */

    VkPipelineMultisampleStateCreateInfo multisample;
    memset(&multisample, 0, sizeof(multisample));
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* ── Color blend ─────────────────────────────────────────────────── */

    VkPipelineColorBlendStateCreateInfo color_blend;
    memset(&color_blend, 0, sizeof(color_blend));
    color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments    = &slot->blend;

    /* ── Dynamic state ───────────────────────────────────────────────── */

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state;
    memset(&dynamic_state, 0, sizeof(dynamic_state));
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    /* ── Graphics pipeline ───────────────────────────────────────────── */

    VkGraphicsPipelineCreateInfo gpci;
    memset(&gpci, 0, sizeof(gpci));
    gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState      = &viewport_state;
    gpci.pRasterizationState = &rasterization;
    gpci.pMultisampleState   = &multisample;
    gpci.pColorBlendState    = &color_blend;
    gpci.pDynamicState       = &dynamic_state;
    gpci.layout              = slot->layout;
    gpci.renderPass          = render_pass;
    gpci.subpass             = 0;

    VkResult vr = vkCreateGraphicsPipelines(
        r->device, VK_NULL_HANDLE, 1, &gpci, NULL, &slot->pipeline);
    if (vr != VK_SUCCESS || slot->pipeline == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateGraphicsPipelines failed (vr=%d)", (int)vr);
        slot->compilation_failed = true;
        slot->pipeline = VK_NULL_HANDLE;
        return false;
    }

    slot->needs_compile = false;
    return true;
}


/* ── ne_pipeline_create ──────────────────────────────────────────────────── */

NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Validate inputs ─────────────────────────────────────────────── */

    if (!ne_shader_handle_valid(desc->vertex_shader)) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_shader handle is null");
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (!ne_shader_handle_valid(desc->fragment_shader)) {
        NE_LOG_ERROR("ne_pipeline_create: fragment_shader handle is null");
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (!desc->vertex_layouts || desc->vertex_layout_count == 0) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_layouts is null or count is 0");
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Resolve shader handles ──────────────────────────────────────── */

    const uint32_t vs_index = desc->vertex_shader.id - 1;
    const uint32_t fs_index = desc->fragment_shader.id - 1;

    if (vs_index >= renderer->shaders.cap || !((NEVulkanShaderSlot*)renderer->shaders.slots)[vs_index].occupied) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_shader handle (id=%u) is invalid or destroyed",
                     desc->vertex_shader.id);
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (fs_index >= renderer->shaders.cap || !((NEVulkanShaderSlot*)renderer->shaders.slots)[fs_index].occupied) {
        NE_LOG_ERROR("ne_pipeline_create: fragment_shader handle (id=%u) is invalid or destroyed",
                     desc->fragment_shader.id);
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanShaderSlot *vs_slot = &((NEVulkanShaderSlot*)renderer->shaders.slots)[vs_index];
    NEVulkanShaderSlot *fs_slot = &((NEVulkanShaderSlot*)renderer->shaders.slots)[fs_index];

    /* ── Deep-copy vertex layout ─────────────────────────────────────── */

    uint32_t total_attributes = 0;
    for (uint32_t i = 0; i < desc->vertex_layout_count; i++) {
        total_attributes += desc->vertex_layouts[i].attribute_count;
    }

    VkVertexInputBindingDescription *bindings = (VkVertexInputBindingDescription *)calloc(
        desc->vertex_layout_count, sizeof(VkVertexInputBindingDescription));
    VkVertexInputAttributeDescription *attributes = (VkVertexInputAttributeDescription *)calloc(
        total_attributes, sizeof(VkVertexInputAttributeDescription));

    if (!bindings || !attributes) {
        NE_LOG_ERROR("ne_pipeline_create: out of memory for vertex layout");
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    uint32_t attr_index = 0;
    for (uint32_t i = 0; i < desc->vertex_layout_count; i++) {
        const NEVertexBufferLayout *layout = &desc->vertex_layouts[i];

        bindings[i].binding   = i;
        bindings[i].stride    = layout->stride;
        bindings[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        for (uint32_t j = 0; j < layout->attribute_count; j++) {
            const NEVertexAttribute *attr = &layout->attributes[j];
            attributes[attr_index].location = attr->location;
            attributes[attr_index].binding  = i;
            attributes[attr_index].format   = ne_vertex_format_to_vk(attr->format);
            attributes[attr_index].offset   = attr->offset;
            attr_index++;
        }
    }

    /* ── Convert blend state ─────────────────────────────────────────── */

    VkPipelineColorBlendAttachmentState blend_attachment;
    memset(&blend_attachment, 0, sizeof(blend_attachment));
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (desc->blend.enabled) {
        blend_attachment.blendEnable         = VK_TRUE;
        blend_attachment.srcColorBlendFactor = ne_blend_factor_to_vk(desc->blend.src_color);
        blend_attachment.dstColorBlendFactor = ne_blend_factor_to_vk(desc->blend.dst_color);
        blend_attachment.colorBlendOp        = ne_blend_op_to_vk(desc->blend.color_op);
        blend_attachment.srcAlphaBlendFactor = ne_blend_factor_to_vk(desc->blend.src_alpha);
        blend_attachment.dstAlphaBlendFactor = ne_blend_factor_to_vk(desc->blend.dst_alpha);
        blend_attachment.alphaBlendOp        = ne_blend_op_to_vk(desc->blend.alpha_op);
    }

    /* ── Create pipeline layout ──────────────────────────────────────── */

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset     = 0;
    push_range.size       = 128; /* Guaranteed minimum by Vulkan spec. */

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push_range;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult vr = vkCreatePipelineLayout(renderer->device, &plci, NULL, &layout);
    if (vr != VK_SUCCESS || layout == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreatePipelineLayout failed (vr=%d)", (int)vr);
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Allocate pool slot ──────────────────────────────────────────── */

    const uint32_t slot_index = ne_pool_alloc(&renderer->pipelines, sizeof(NEVulkanPipelineSlot));

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_pipeline_create: pipeline pool allocation failed");
        vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanPipelineSlot *slot = &((NEVulkanPipelineSlot*)renderer->pipelines.slots)[slot_index];

    slot->needs_compile      = true;
    slot->compilation_failed = false;

    slot->layout       = layout;
    slot->vert_module  = vs_slot->module;
    slot->frag_module  = fs_slot->module;
    slot->vert_entry   = _strdup(vs_slot->entry_point);
    slot->frag_entry   = _strdup(fs_slot->entry_point);

    if (!slot->vert_entry || !slot->frag_entry) {
        NE_LOG_ERROR("ne_pipeline_create: out of memory copying entry point names");
        vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(slot->vert_entry);
        free(slot->frag_entry);
        free(bindings);
        free(attributes);
        slot->occupied = false;
        renderer->pipelines.count--;
        slot->layout = VK_NULL_HANDLE;
        slot->vert_entry = NULL;
        slot->frag_entry = NULL;
        return NE_PIPELINE_HANDLE_NULL;
    }

    slot->bindings        = bindings;
    slot->binding_count   = desc->vertex_layout_count;
    slot->attributes      = attributes;
    slot->attribute_count = total_attributes;

    slot->topology = ne_topology_to_vk(desc->topology);
    slot->blend    = blend_attachment;
    slot->pipeline = VK_NULL_HANDLE;

    return (NEPipelineHandle){ slot_index + 1 };
}

/* ── ne_pipeline_destroy ─────────────────────────────────────────────────── */

void ne_pipeline_destroy(NERenderer *renderer, NEPipelineHandle handle) {
    if (!renderer || !ne_pipeline_handle_valid(handle)) {
        return;
    }

    const uint32_t index = handle.id - 1;
    if (index >= renderer->pipelines.cap || !((NEVulkanPipelineSlot*)renderer->pipelines.slots)[index].occupied) {
        NE_LOG_WARN("ne_pipeline_destroy: invalid or already-destroyed pipeline handle (id=%u)", handle.id);
        return;
    }

    NEVulkanPipelineSlot *slot = &((NEVulkanPipelineSlot*)renderer->pipelines.slots)[index];

    /* Ensure the GPU is done with any command buffers referencing this pipeline. */
    if (renderer->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(renderer->device);
    }

    if (slot->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(renderer->device, slot->pipeline, NULL);
        slot->pipeline = VK_NULL_HANDLE;
    }

    if (slot->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(renderer->device, slot->layout, NULL);
        slot->layout = VK_NULL_HANDLE;
    }

    free(slot->vert_entry);
    free(slot->frag_entry);
    free(slot->bindings);
    free(slot->attributes);

    slot->vert_entry   = NULL;
    slot->frag_entry   = NULL;
    slot->bindings     = NULL;
    slot->attributes   = NULL;
    slot->vert_module  = VK_NULL_HANDLE;
    slot->frag_module  = VK_NULL_HANDLE;

    ne_pool_free(&renderer->pipelines, index, sizeof(NEVulkanPipelineSlot));
}

/* ── Compute pipeline stubs ──────────────────────────────────────────────── */

NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer, const NEComputePipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Validate inputs ─────────────────────────────────────────────── */

    if (!ne_shader_handle_valid(desc->compute_shader)) {
        NE_LOG_ERROR("ne_compute_pipeline_create: compute_shader handle is null");
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Resolve shader handles ──────────────────────────────────────── */

    const uint32_t cs_index = desc->compute_shader.id - 1;

    if (cs_index >= renderer->shaders.cap || !((NEVulkanShaderSlot*)renderer->shaders.slots)[cs_index].occupied) {
        NE_LOG_ERROR("ne_compute_pipeline_create: compute_shader handle (id=%u) is invalid or destroyed",
                     desc->compute_shader.id);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanShaderSlot *cs_slot = &((NEVulkanShaderSlot*)renderer->shaders.slots)[cs_index];

    /* ── Create pipeline layout ──────────────────────────────────────── */

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset     = 0;
    push_range.size       = 128; /* Guaranteed minimum by Vulkan spec. */

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push_range;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult vr = vkCreatePipelineLayout(renderer->device, &plci, NULL, &layout);
    if (vr != VK_SUCCESS || layout == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreatePipelineLayout failed (vr=%d)", (int)vr);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Allocate pool slot ──────────────────────────────────────────── */
    const uint32_t slot_index = ne_pool_alloc(&renderer->pipelines, sizeof(NEVulkanPipelineSlot));

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_compute_pipeline_create: pipeline pool allocation failed");
        vkDestroyPipelineLayout(renderer->device, layout, NULL);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanPipelineSlot *slot = &((NEVulkanPipelineSlot*)renderer->pipelines.slots)[slot_index];

    slot->needs_compile      = true;
    slot->compilation_failed = false;

    slot->layout       = layout;
    slot->compute_module  = cs_slot->module;
    slot->compute_entry   = _strdup(cs_slot->entry_point);

    if (!slot->compute_entry){
        NE_LOG_ERROR("ne_compute_pipeline_create: out of memory copying entry point names");
        vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(slot->compute_entry);
        slot->occupied = false;
        renderer->pipelines.count--;
        slot->layout = VK_NULL_HANDLE;
        slot->compute_entry = NULL;
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }
    slot->pipeline = VK_NULL_HANDLE;

    return (NEComputePipelineHandle){ slot_index + 1 };
}

void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle) {
    (void)renderer;
    (void)handle;
}

void ne_pipeline_destroy_all(NERenderer *r) {
    /* Destroy all live pipelines. */
    for (uint32_t i = 0; i < r->pipelines.cap; i++) {
        NEVulkanPipelineSlot *pslot = &((NEVulkanPipelineSlot*)r->pipelines.slots)[i];
        if (pslot->occupied) {
            if (pslot->pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(r->device, pslot->pipeline, NULL);
            }
            if (pslot->layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(r->device, pslot->layout, NULL);
            }
            free(pslot->vert_entry);
            free(pslot->frag_entry);
            free(pslot->bindings);
            free(pslot->attributes);
        }
    }
    ne_pool_destroy(&r->pipelines);
}
