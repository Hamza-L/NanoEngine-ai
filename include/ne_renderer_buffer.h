#ifndef NE_RENDERER_BUFFER_H
#define NE_RENDERER_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NERenderer NERenderer;

/* ======================================================================== */
/* Handle                                                                   */
/* ======================================================================== */

typedef struct NEBufferHandle { uint32_t id; } NEBufferHandle;

#define NE_BUFFER_HANDLE_NULL ((NEBufferHandle){0})

static inline bool ne_buffer_handle_valid(NEBufferHandle h) { return h.id != 0; }

/* ======================================================================== */
/* Usage flags                                                              */
/* ======================================================================== */

/**
 * Buffer usage flags (bitfield).
 *
 * A buffer may combine multiple usages (e.g. vertex | uniform).
 */
typedef enum NEBufferUsage {
    NE_BUFFER_USAGE_VERTEX  = 1u << 0,
    NE_BUFFER_USAGE_INDEX   = 1u << 1,
    NE_BUFFER_USAGE_UNIFORM = 1u << 2,

    /** Read/write access from compute shaders. */
    NE_BUFFER_USAGE_STORAGE = 1u << 3,
} NEBufferUsage;

/* ======================================================================== */
/* Descriptor                                                               */
/* ======================================================================== */

/**
 * Buffer creation parameters.
 */
typedef struct NEBufferDesc {
    /** Total size in bytes. */
    uint32_t size;

    /** Bitwise OR of `NEBufferUsage` values. */
    uint32_t usage;

    /**
     * Optional initial data to upload.
     * If non-NULL, `size` bytes are copied from this pointer.
     * If NULL, the buffer contents are undefined.
     */
    const void *initial_data;
} NEBufferDesc;

/* ======================================================================== */
/* Index type                                                               */
/* ======================================================================== */

/**
 * Index buffer element type.
 */
typedef enum NEIndexType {
    NE_INDEX_TYPE_UINT16 = 0,
    NE_INDEX_TYPE_UINT32 = 1,
} NEIndexType;

/* ======================================================================== */
/* Functions                                                                */
/* ======================================================================== */

/**
 * Create a GPU buffer.
 *
 * Returns NE_BUFFER_HANDLE_NULL on failure.
 */
NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc);

/**
 * Update a region of an existing buffer.
 *
 * Parameters:
 * - `data`   : Source data to copy.
 * - `size`   : Number of bytes to copy.
 * - `offset` : Byte offset into the buffer.
 *
 * The update is staged and applied before the next draw that uses the buffer.
 */
void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle,
                      const void *data, uint32_t size, uint32_t offset);

/**
 * Destroy a GPU buffer.
 *
 * Destruction is deferred until the GPU is no longer using the resource.
 * Null handles are silently ignored.
 */
void ne_buffer_destroy(NERenderer *renderer, NEBufferHandle handle);

#ifdef __cplusplus
}
#endif

#endif
