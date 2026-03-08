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
/* Descriptors                                                              */
/* ======================================================================== */

/**
 * Create a shader from pre-compiled / pre-transpiled bytecode.
 *
 * The bytecode format is backend-specific:
 * - Vulkan : SPIR-V binary.
 * - Metal  : Precompiled metallib binary.
 *
 * This path has no external dependencies and is intended for release builds.
 */
typedef struct NEShaderDesc {
    NEShaderStage stage;

    /** Pointer to the bytecode blob.  Copied internally. */
    const void *bytecode;

    /** Size of the bytecode blob in bytes. */
    size_t bytecode_size;

    /** Entry-point function name (e.g. "main", "vertexMain"). */
    const char *entry_point;
} NEShaderDesc;

/**
 * Create a shader by compiling source code at runtime.
 *
 * The engine uses the Slang compiler (loaded dynamically) to transpile
 * the source to the appropriate backend target:
 * - Vulkan : Slang -> SPIR-V.
 * - Metal  : Slang -> MSL source -> Metal runtime compilation.
 *
 * If the Slang library is not available, this function returns a null handle.
 * This path is intended for development / rapid iteration.
 */
typedef struct NEShaderSourceDesc {
    NEShaderStage stage;

    /** Null-terminated Slang source code. */
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
 * Create a shader from pre-compiled bytecode.
 *
 * Returns NE_SHADER_HANDLE_NULL on failure.
 */
NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc);

/**
 * Create a shader by compiling Slang source at runtime.
 *
 * Requires the Slang shared library to be loadable at runtime.
 * Returns NE_SHADER_HANDLE_NULL on failure or if Slang is unavailable.
 */
NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc);

/**
 * Destroy a shader.
 *
 * Destruction is deferred until the GPU is no longer using the resource.
 * Null handles are silently ignored.
 */
void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle);

#ifdef __cplusplus
}
#endif

#endif
