#ifndef NE_RENDERER_SHADER_H
#define NE_RENDERER_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NERenderer NERenderer;

/* ======================================================================== */
/* Handle                                                                   */
/* ======================================================================== */

typedef struct NEShaderHandle { uint32_t id; } NEShaderHandle;

#define NE_SHADER_HANDLE_NULL ((NEShaderHandle){0})

static inline bool ne_shader_handle_valid(NEShaderHandle h) { return h.id != 0; }

/* ======================================================================== */
/* Shader stage                                                             */
/* ======================================================================== */

typedef enum NEShaderStage {
    NE_SHADER_STAGE_VERTEX   = 0,
    NE_SHADER_STAGE_FRAGMENT = 1,
    NE_SHADER_STAGE_COMPUTE  = 2,
} NEShaderStage;

/* ======================================================================== */
/* Shader compilation optimization level                                    */
/* ======================================================================== */

/**
 * Controls the optimization level applied by ne_shader_create_from_source().
 *
 * Set once per renderer via ne_renderer_set_shader_optimization().
 * Defaults to NE_SHADER_OPTIMIZATION_NONE if the function is never called,
 * which prioritises fast compile times and accurate error reporting.
 *
 * Backend mapping:
 *   Vulkan (shaderc) :  NONE        → -O0 (no optimisation)
 *                       SIZE        → -Os (optimise for binary size)
 *                       PERFORMANCE → -O  (optimise for GPU throughput)
 *   Metal            :  NONE / SIZE → fastMathEnabled = NO
 *                       PERFORMANCE → fastMathEnabled = YES
 */
typedef enum NEShaderOptimization {
    NE_SHADER_OPTIMIZATION_NONE        = 0, /* fastest compile, best error messages */
    NE_SHADER_OPTIMIZATION_SIZE        = 1, /* optimise for binary size             */
    NE_SHADER_OPTIMIZATION_PERFORMANCE = 2, /* optimise for GPU throughput          */
} NEShaderOptimization;

/* ======================================================================== */
/* Descriptors                                                              */
/* ======================================================================== */

/**
 * Create a shader from pre-compiled bytecode.
 *
 * The bytecode format is backend-specific:
 *   Vulkan : SPIR-V binary.
 *   Metal  : Precompiled metallib binary.
 *
 * This path has no external runtime dependencies and is intended for
 * release / production builds.
 */
typedef struct NEShaderDesc {
    NEShaderStage stage;

    /** Pointer to the bytecode blob. Copied internally by vkCreateShaderModule /
     *  newLibraryWithData, so the caller may free it after this call returns. */
    const void *bytecode;

    /** Size of the bytecode blob in bytes. */
    size_t bytecode_size;

    /** Entry-point function name (e.g. "main", "vertexMain"). */
    const char *entry_point;
} NEShaderDesc;

/**
 * Create a shader by compiling source code at runtime.
 *
 * Source language is backend-specific:
 *   Vulkan : GLSL, compiled to SPIR-V via shaderc (loaded dynamically).
 *   Metal  : MSL, compiled at runtime by the Metal framework.
 *
 * The optimization level applied during compilation is controlled globally
 * via ne_renderer_set_shader_optimization() (default: NONE).
 *
 * This path is intended for development and hot-reload workflows.
 * For release builds prefer ne_shader_create() with pre-compiled bytecode.
 */
typedef struct NEShaderSourceDesc {
    NEShaderStage stage;

    /** Null-terminated shader source (GLSL on Vulkan, MSL on Metal). */
    const char *source;

    /** Entry-point function name within the source. */
    const char *entry_point;

    /**
     * Optional filename used in diagnostics / error messages.
     * May be NULL.
     */
    const char *filename;
} NEShaderSourceDesc;

/* ======================================================================== */
/* Functions                                                                */
/* ======================================================================== */

/**
 * Set the optimization level used by all subsequent ne_shader_create_from_source() calls.
 *
 * Defaults to NE_SHADER_OPTIMIZATION_NONE (fastest compile, best diagnostics).
 * Has no effect on shaders already compiled.
 */
void ne_renderer_set_shader_optimization(NERenderer *renderer, NEShaderOptimization level);

/**
 * Create a shader from pre-compiled bytecode.
 *
 * Returns NE_SHADER_HANDLE_NULL on failure.
 */
NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc);

/**
 * Create a shader by compiling source code at runtime.
 *
 * Returns NE_SHADER_HANDLE_NULL on failure.
 * On Vulkan, requires shaderc_shared.dll to be present next to the executable.
 */
NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc);

/**
 * Destroy a shader.
 *
 * Null handles are silently ignored.
 */
void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle);

#ifdef __cplusplus
}
#endif

#endif
