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
    uint32_t width;
    uint32_t height;
    NEImageFormat format;

    /** Bitwise OR of `NEBufferUsage` values. */
    uint32_t usage;

    /**
     * Optional initial data to upload.
     * If non-NULL, width * height * format tightly packed bytes are copied.
     * If NULL, upload pixels before sampling the image.
     */
    const void *initial_data;
    /** Interpret sampled color as sRGB. Leave false for linear numeric data.
     * sRGB storage images are unsupported. */
    bool srgb;
} NEImageDesc;

/* ======================================================================== */
/* Functions                                                                */
/* ======================================================================== */

/**
 * Create a GPU image buffer.
 *
 * Returns NE_IMAGE_HANDLE_NULL on failure.
 */
NEImageHandle ne_image_create(NERenderer *renderer, const NEImageDesc *desc);

/**
 * Load a file as a sampled, sRGB RGBA image (currently Vulkan only).
 *
 * Returns NE_IMAGE_HANDLE_NULL on failure.
 */
NEImageHandle ne_image_load(NERenderer *renderer, const char* filename);

/**
 * Replace the complete image with tightly packed pixels.
 *
 * Parameters:
 * - `data`   : Source data to copy.
 * - `size`   : Must equal width * height * format.
 *
 * The Vulkan update is staged synchronously. It preserves the image's tracked
 * layout and returns it to shader-readable (or storage) use.
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
