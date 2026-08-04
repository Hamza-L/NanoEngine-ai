#include "internal/ne_vulkan_shaders.h"

#include <string.h>

#include "ne_log.h"
#include "ne_file.h"

#include "glslang_c_shader_types.h"
#include "glslang_c_interface.h"
#include "../Public/resource_limits_c.h"

/* Shader management */
extern PFN_vkCreateShaderModule vkCreateShaderModule;
extern PFN_vkDestroyShaderModule vkDestroyShaderModule;


/* ========================================================================
 * Shader Management
 * ======================================================================== */

/*
 * SHADER ARCHITECTURE
 *
 * Each VkShaderModule wraps a single SPIR-V blob.  After creation the module
 * is opaque — the entry point name and stage are NOT stored inside it.  We
 * strdup the entry point here so that pipeline creation can read both back
 * from the slot without requiring the caller to keep the original descriptor
 * alive.
 *
 * SPIR-V alignment contract
 * ─────────────────────────
 * vkCreateShaderModule requires:
 *   - pCode  : pointer to uint32_t-aligned memory
 *   - codeSize : a multiple of 4 bytes
 * We validate the size.  The pointer cast from (const void *) to
 * (const uint32_t *) is safe because ne_file_read() (malloc) guarantees at
 * least sizeof(void *) alignment, which is ≥ 4 on all supported platforms
 * (x86-64 Windows).
 *
 * Runtime source compilation
 * ──────────────────────────
 * The API header documents a future Slang → SPIR-V path.  Until Slang is
 * integrated, ne_shader_create_from_source() is an explicit no-op stub: it
 * logs a clear actionable warning and returns NE_SHADER_HANDLE_NULL.  This
 * mirrors the Metal backend's compute pipeline stubs exactly, making the
 * future integration point obvious.
 */

NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc) {
    if (!renderer || !desc || !desc->bytecode || desc->bytecode_size == 0 || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    if (desc->bytecode_size % 4 != 0) {
        NE_LOG_ERROR("ne_shader_create: SPIR-V bytecode size (%zu) must be a multiple of 4",
                     desc->bytecode_size);
        return NE_SHADER_HANDLE_NULL;
    }

    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = desc->bytecode_size;
    /* Cast is safe — see alignment note above. */
    smci.pCode    = (const uint32_t *)desc->bytecode;

    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult vr = vkCreateShaderModule(renderer->device, &smci, NULL, &module);
    if (vr != VK_SUCCESS || module == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateShaderModule failed (vr=%d)", (int)vr);
        return NE_SHADER_HANDLE_NULL;
    }

    const uint32_t slot_index = ne_pool_alloc(&renderer->shaders, sizeof(NEVulkanShaderSlot));

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_shader_create: shader pool allocation failed");
        vkDestroyShaderModule(renderer->device, module, NULL);
        return NE_SHADER_HANDLE_NULL;
    }

    NEVulkanShaderSlot *slot = &((NEVulkanShaderSlot*)renderer->shaders.slots)[slot_index];
    slot->stage       = (uint32_t)desc->stage;
    slot->module      = module;
    slot->entry_point = _strdup(desc->entry_point);

    if (!slot->entry_point) {
        NE_LOG_ERROR("ne_shader_create: out of memory copying entry point name");
        vkDestroyShaderModule(renderer->device, module, NULL);
        slot->occupied = false;
        renderer->shaders.count--;
        slot->module   = VK_NULL_HANDLE;
        return NE_SHADER_HANDLE_NULL;
    }

    return (NEShaderHandle){ .id = slot_index + 1 };
}

NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc) {
    if (!renderer || !desc || (!desc->source && !desc->filename) || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    const char *filename = desc->filename;

    glslang_stage_t stage = {};
    if(desc->stage == NE_SHADER_STAGE_VERTEX) stage = GLSLANG_STAGE_VERTEX;
    else if(desc->stage == NE_SHADER_STAGE_FRAGMENT) stage = GLSLANG_STAGE_FRAGMENT;
    else if(desc->stage == NE_SHADER_STAGE_COMPUTE) stage = GLSLANG_STAGE_COMPUTE;
    else { return NE_SHADER_HANDLE_NULL;}

    static bool glslang_ready = false;
    if (!glslang_ready) {
        glslang_initialize_process();
        glslang_ready = true;
    }

    size_t size = 0;
    void *src = ne_file_read(filename, &size);

    glslang_input_t input = {
	.language = GLSLANG_SOURCE_GLSL,
    .client = GLSLANG_CLIENT_VULKAN,
    .client_version = GLSLANG_TARGET_VULKAN_1_3,
    .target_language = GLSLANG_TARGET_SPV,
    .target_language_version = GLSLANG_TARGET_SPV_1_6,
    .default_profile = GLSLANG_NO_PROFILE,
	.default_version = 450,
    .stage = stage,
    .code = src,
    .force_default_version_and_profile = false,
    .forward_compatible = false,
    .messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_DEBUG_INFO_BIT,
    .resource = glslang_default_resource(),
};

    glslang_shader_t *shader = glslang_shader_create(&input);
	glslang_shader_set_entry_point(shader, desc->entry_point);

	if (!glslang_shader_preprocess(shader, &input))	{
		NE_LOG_ERROR("GLSL preprocessing failed %s\n", filename);
		NE_LOG_ERROR("%s\n", glslang_shader_get_info_log(shader));
		NE_LOG_ERROR("%s\n", glslang_shader_get_info_debug_log(shader));
		NE_LOG_ERROR("%s\n", input.code);
		glslang_shader_delete(shader);
		return NE_SHADER_HANDLE_NULL;
	}

    if (!glslang_shader_parse(shader, &input)) {
        NE_LOG_ERROR("GLSL parsing failed %s\n", filename);
        NE_LOG_ERROR("%s\n", glslang_shader_get_info_log(shader));
        NE_LOG_ERROR("%s\n", glslang_shader_get_info_debug_log(shader));
        NE_LOG_ERROR("%s\n", glslang_shader_get_preprocessed_code(shader));
        glslang_shader_delete(shader);
        return NE_SHADER_HANDLE_NULL;
    }

	glslang_program_t* program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        NE_LOG_ERROR("GLSL linking failed %s\n", filename);
        NE_LOG_ERROR("%s\n", glslang_program_get_info_log(program));
        NE_LOG_ERROR("%s\n", glslang_program_get_info_debug_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NE_SHADER_HANDLE_NULL;
    }

    glslang_program_SPIRV_generate(program, stage);

    size_t sprv_size = glslang_program_SPIRV_get_size(program) * sizeof(uint32_t);
    void* sprv_bytes = malloc(sprv_size);
    glslang_program_SPIRV_get(program, sprv_bytes);

    const char* spirv_messages = glslang_program_SPIRV_get_messages(program);
    if (spirv_messages) NE_LOG_INFO("(%s) %s\b", filename, spirv_messages);

    const NEShaderHandle handle = ne_shader_create(renderer, &(NEShaderDesc){
        .stage         = desc->stage,
        .bytecode      = sprv_bytes,
        .bytecode_size = sprv_size,
        .entry_point   = desc->entry_point,
    });

    free(sprv_bytes);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    ne_file_free(src);

    return handle;
}

void ne_renderer_set_shader_optimization(NERenderer *renderer, NEShaderOptimization level) {
    if (renderer) {
        renderer->shader_optimization = level;
    }
}

void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle) {
    if (!renderer || !ne_shader_handle_valid(handle)) {
        return;
    }

    const uint32_t index = handle.id - 1;
    if (index >= renderer->shaders.cap || !((NEVulkanShaderSlot*)renderer->shaders.slots)[index].occupied) {
        NE_LOG_WARN("ne_shader_destroy: invalid or already-destroyed shader handle (id=%u)",
                    handle.id);
        return;
    }

    NEVulkanShaderSlot *slot = &((NEVulkanShaderSlot*)renderer->shaders.slots)[index];

    free(slot->entry_point);
    vkDestroyShaderModule(renderer->device, slot->module, NULL);
    slot->entry_point = NULL;
    slot->stage       = 0;

    ne_pool_free(&renderer->shaders, index, sizeof(NEVulkanShaderSlot));
}

void ne_shader_destroy_all(NERenderer *r) {
    /* Destroy all live shaders. */
    for (uint32_t i = 0; i < r->shaders.cap; i++) {
        NEVulkanShaderSlot *sslot = &((NEVulkanShaderSlot*)r->shaders.slots)[i];
        if (sslot->occupied) {
            if (sslot->module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(r->device, sslot->module, NULL);
            }
            free(sslot->entry_point);
            sslot->entry_point = NULL;
            sslot->stage       = 0;
        }
    }
    ne_pool_destroy(&r->shaders);

    glslang_finalize_process();
}
