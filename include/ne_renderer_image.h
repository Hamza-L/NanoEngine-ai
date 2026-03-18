#ifndef NE_RENDERER_IMAGE_H
#define NE_RENDERER_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NERenderer NERenderer;

/* ======================================================================== */
/* Handle                                                                   */
/* ======================================================================== */

typedef uint64_t NEImageHandle;

#define NE_IMAGE_HANDLE_NULL ((NEImageHandle){0})

static inline bool ne_image_handle_valid(NEImageHandle h) { return h != 0; }

typedef enum NEImageFormat{
    NE_IMAGE_FORMAT_GRAY = 1,
    NE_IMAGE_FORMAT_RGB = 3,
    NE_IMAGE_FORMAT_RGBA = 4
} NEImageFormat;

typedef struct NEImageDesc {
    /* info */
    float width;
    float height;
    NEImageFormat format;

    /** Bitwise OR of `NEBufferUsage` values. */
    uint32_t usage;

    /**
     * Optional initial data to upload.
     * If non-NULL, `size` bytes are copied from this pointer.
     * If NULL, the buffer contents are undefined.
     */
    const void *initial_data;
} NEImageDesc;

/* ======================================================================== */
/* Functions                                                                */
/* ======================================================================== */

/**
 * Create a GPU image buffer.
 *
 * Returns NE_BUFFER_HANDLE_NULL on failure.
 */
NEImageHandle ne_image_create(NERenderer *renderer, const NEImageDesc *desc);

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
void ne_image_update(NERenderer *renderer, NEImageHandle handle, const void *data, uint32_t size);

/**
 * Destroy a GPU image buffer.
 *
 * Destruction is deferred until the GPU is no longer using the resource.
 * Null handles are silently ignored.
 */
void ne_image_destroy(NERenderer *renderer, NEImageHandle handle);


#ifdef __cplusplus
}
#endif

#endif
