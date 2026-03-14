#ifndef NE_RENDERER_PASS_H
#define NE_RENDERER_PASS_H

#include <stdint.h>

#include "ne_renderer_buffer.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Opaque pass types                                                        */
/*                                                                          */
/* An NERenderPass is obtained from `ne_renderer_begin_frame` and is valid  */
/* until the corresponding `ne_renderer_end_frame` call.                    */
/*                                                                          */
/* An NEComputePass is obtained from `ne_render_pass_begin_compute` within  */
/* an active frame and must be ended before issuing draw commands or ending  */
/* the frame.                                                               */
/* ======================================================================== */

typedef struct NERenderPass NERenderPass;
typedef struct NEComputePass NEComputePass;

/* ======================================================================== */
/* Compute pass lifecycle                                                   */
/* ======================================================================== */

/**
 * Begin a compute pass within the current frame.
 *
 * Compute work is recorded before the graphics render pass and shares the
 * same command buffer / synchronization scope.
 *
 * The returned NEComputePass must be ended with `ne_render_pass_end_compute`
 * before any draw commands are issued or the frame is ended.
 *
 * Returns NULL on failure.
 */
NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass);

/**
 * End a compute pass.
 *
 * After this call, the NEComputePass pointer is invalid.
 */
void ne_render_pass_end_compute(NERenderPass *pass, NEComputePass *compute);

/* ======================================================================== */
/* Render pass commands (graphics)                                          */
/* ======================================================================== */

/**
 * Bind a graphics pipeline for subsequent draw calls.
 */
void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline);

/**
 * Bind a vertex buffer to a slot.
 *
 * Parameters:
 * - `slot`   : Vertex buffer binding index (corresponds to the index in
 *              `NEPipelineDesc::vertex_layouts`).
 * - `buffer` : Buffer handle (must have NE_BUFFER_USAGE_VERTEX).
 */
void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint64_t slot,
                                      NEBufferHandle buffer);

/**
 * Bind an index buffer.
 *
 * Parameters:
 * - `buffer` : Buffer handle (must have NE_BUFFER_USAGE_INDEX).
 * - `type`   : Element type (uint16 or uint32).
 */
void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer,
                                     NEIndexType type);

/**
 * Set uniform / push-constant data for a shader stage.
 *
 * This is a lightweight path for small, frequently-changing data such as
 * MVP matrices.  The data is copied internally and does not need to persist
 * after the call returns.
 *
 * Backend mapping:
 * - Metal  : `setVertexBytes` / `setFragmentBytes` at the given slot index.
 * - Vulkan : push constants (offset derived from slot).
 *
 * Parameters:
 * - `stage` : Which shader stage receives the data.
 * - `slot`  : Binding slot / index.
 * - `data`  : Pointer to the data.
 * - `size`  : Size in bytes (must not exceed backend limit; typically 128-256 B).
 */
void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage,
                                     uint64_t slot, const void *data, size_t size);

/**
 * Issue a non-indexed draw call.
 *
 * Parameters:
 * - `first_vertex` : Offset of the first vertex.
 * - `vertex_count` : Number of vertices to draw.
 */
void ne_render_pass_draw(NERenderPass *pass, uint64_t first_vertex,
                         uint64_t vertex_count);

/**
 * Issue an indexed draw call.
 *
 * Parameters:
 * - `index_count`   : Number of indices to draw.
 * - `first_index`   : Offset into the index buffer (in elements, not bytes).
 * - `vertex_offset` : Value added to each index before fetching the vertex.
 */
void ne_render_pass_draw_indexed(NERenderPass *pass, uint64_t index_count,
                                 uint64_t first_index, int64_t vertex_offset);

/* ======================================================================== */
/* Compute pass commands                                                    */
/* ======================================================================== */

/**
 * Bind a compute pipeline.
 */
void ne_compute_pass_set_pipeline(NEComputePass *pass, NEComputePipelineHandle pipeline);

/**
 * Bind a storage buffer to a slot (read/write access from the compute shader).
 *
 * Parameters:
 * - `slot`   : Binding slot / index.
 * - `buffer` : Buffer handle (must have NE_BUFFER_USAGE_STORAGE).
 */
void ne_compute_pass_set_storage_buffer(NEComputePass *pass, uint64_t slot,
                                        NEBufferHandle buffer);

/**
 * Set uniform / push-constant data for the compute shader.
 *
 * Same semantics as `ne_render_pass_set_uniform_data` but targets the
 * compute stage.
 */
void ne_compute_pass_set_uniform_data(NEComputePass *pass, uint64_t slot,
                                      const void *data, uint32_t size);

/**
 * Dispatch compute work.
 *
 * Parameters:
 * - `group_count_x/y/z` : Number of workgroups in each dimension.
 */
void ne_compute_pass_dispatch(NEComputePass *pass, uint64_t group_count_x,
                              uint64_t group_count_y, uint64_t group_count_z);

#ifdef __cplusplus
}
#endif

#endif
