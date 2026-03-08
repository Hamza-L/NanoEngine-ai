#ifndef NE_RENDERER_PIPELINE_H
#define NE_RENDERER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "ne_renderer_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NERenderer NERenderer;

/* ======================================================================== */
/* Handles                                                                  */
/* ======================================================================== */

/** Handle to a graphics (render) pipeline. */
typedef struct NEPipelineHandle { uint32_t id; } NEPipelineHandle;

/** Handle to a compute pipeline. */
typedef struct NEComputePipelineHandle { uint32_t id; } NEComputePipelineHandle;

#define NE_PIPELINE_HANDLE_NULL         ((NEPipelineHandle){0})
#define NE_COMPUTE_PIPELINE_HANDLE_NULL ((NEComputePipelineHandle){0})

static inline bool ne_pipeline_handle_valid(NEPipelineHandle h) { return h.id != 0; }
static inline bool ne_compute_pipeline_handle_valid(NEComputePipelineHandle h) { return h.id != 0; }

/* ======================================================================== */
/* Vertex layout                                                            */
/* ======================================================================== */

/**
 * Vertex attribute data format.
 */
typedef enum NEVertexFormat {
    NE_VERTEX_FORMAT_FLOAT    = 0,
    NE_VERTEX_FORMAT_FLOAT2   = 1,
    NE_VERTEX_FORMAT_FLOAT3   = 2,
    NE_VERTEX_FORMAT_FLOAT4   = 3,

    /** Four unsigned bytes, normalized to [0..1].  Suitable for RGBA colors. */
    NE_VERTEX_FORMAT_UNORM8X4 = 4,
} NEVertexFormat;

/**
 * A single vertex attribute within a vertex buffer binding.
 */
typedef struct NEVertexAttribute {
    /** Shader attribute location / index. */
    uint32_t location;

    /** Data format of this attribute. */
    NEVertexFormat format;

    /** Byte offset of this attribute within a vertex. */
    uint32_t offset;
} NEVertexAttribute;

/**
 * Describes the layout of vertices in a single vertex buffer binding.
 *
 * One `NEVertexBufferLayout` corresponds to one vertex buffer slot.
 * A pipeline may reference multiple slots (e.g. separate position / color
 * buffers).
 */
typedef struct NEVertexBufferLayout {
    /** Byte stride between consecutive vertices. */
    uint32_t stride;

    /** Array of attributes in this buffer binding. */
    const NEVertexAttribute *attributes;

    /** Number of attributes. */
    uint32_t attribute_count;
} NEVertexBufferLayout;

/* ======================================================================== */
/* Primitive topology                                                       */
/* ======================================================================== */

typedef enum NEPrimitiveTopology {
    NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST  = 0,
    NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 1,
    NE_PRIMITIVE_TOPOLOGY_LINE_LIST      = 2,
    NE_PRIMITIVE_TOPOLOGY_LINE_STRIP     = 3,
    NE_PRIMITIVE_TOPOLOGY_POINT_LIST     = 4,
} NEPrimitiveTopology;

/* ======================================================================== */
/* Blend state                                                              */
/* ======================================================================== */

typedef enum NEBlendFactor {
    NE_BLEND_FACTOR_ZERO                = 0,
    NE_BLEND_FACTOR_ONE                 = 1,
    NE_BLEND_FACTOR_SRC_ALPHA           = 2,
    NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 3,
    NE_BLEND_FACTOR_DST_ALPHA           = 4,
    NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 5,
} NEBlendFactor;

typedef enum NEBlendOp {
    NE_BLEND_OP_ADD              = 0,
    NE_BLEND_OP_SUBTRACT         = 1,
    NE_BLEND_OP_REVERSE_SUBTRACT = 2,
    NE_BLEND_OP_MIN              = 3,
    NE_BLEND_OP_MAX              = 4,
} NEBlendOp;

/**
 * Blend state for a single color attachment.
 *
 * When `enabled` is false, the remaining fields are ignored and blending
 * is disabled (source fragment overwrites the destination).
 */
typedef struct NEBlendState {
    bool enabled;

    NEBlendFactor src_color;
    NEBlendFactor dst_color;
    NEBlendOp     color_op;

    NEBlendFactor src_alpha;
    NEBlendFactor dst_alpha;
    NEBlendOp     alpha_op;
} NEBlendState;

/* ======================================================================== */
/* Graphics pipeline descriptor                                             */
/* ======================================================================== */

/**
 * Render (graphics) pipeline description.
 *
 * A graphics pipeline encapsulates the full GPU state needed for a draw call:
 * shaders, vertex layout, topology, and blend state.
 */
typedef struct NEPipelineDesc {
    /** Vertex shader (required). */
    NEShaderHandle vertex_shader;

    /** Fragment shader (required). */
    NEShaderHandle fragment_shader;

    /**
     * Array of vertex buffer layouts, one per vertex buffer slot.
     * Must contain at least one layout.
     */
    const NEVertexBufferLayout *vertex_layouts;
    uint32_t vertex_layout_count;

    /** Primitive topology (default: NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST). */
    NEPrimitiveTopology topology;

    /** Blend state (default: blending disabled). */
    NEBlendState blend;
} NEPipelineDesc;

/* ======================================================================== */
/* Compute pipeline descriptor                                              */
/* ======================================================================== */

/**
 * Compute pipeline description.
 *
 * A compute pipeline wraps a single compute shader.
 */
typedef struct NEComputePipelineDesc {
    /** Compute shader (required). */
    NEShaderHandle compute_shader;
} NEComputePipelineDesc;

/* ======================================================================== */
/* Functions                                                                */
/* ======================================================================== */

/**
 * Create a graphics (render) pipeline.
 *
 * Returns NE_PIPELINE_HANDLE_NULL on failure.
 */
NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc);

/**
 * Destroy a graphics pipeline.
 *
 * Destruction is deferred until the GPU is no longer using the resource.
 * Null handles are silently ignored.
 */
void ne_pipeline_destroy(NERenderer *renderer, NEPipelineHandle handle);

/**
 * Create a compute pipeline.
 *
 * Returns NE_COMPUTE_PIPELINE_HANDLE_NULL on failure.
 */
NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer, const NEComputePipelineDesc *desc);

/**
 * Destroy a compute pipeline.
 *
 * Destruction is deferred until the GPU is no longer using the resource.
 * Null handles are silently ignored.
 */
void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle);

#ifdef __cplusplus
}
#endif

#endif
