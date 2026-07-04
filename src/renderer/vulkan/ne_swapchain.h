#ifndef NE_SWAPCHAIN_H
#define NE_SWAPCHAIN_H

/*
 * Internal swapchain interface.
 *
 * The renderer talks to this interface — it never knows which backend is
 * running behind it. Two implementations exist:
 *
 *   1. Vulkan WSI  — wraps vkCreateSwapchainKHR (current behavior)
 *   2. DXGI        — our own DXGI swapchain + DirectComposition (future)
 *
 * The interface exposes exactly what the renderer needs per frame:
 *   acquire → render → present
 */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct NEWindow NEWindow;

/* ======================================================================== */
/* Result codes                                                             */
/* ======================================================================== */

typedef enum NESwapchainAcquireResult {
    NE_SWAPCHAIN_ACQUIRE_SUCCESS = 0,
    NE_SWAPCHAIN_ACQUIRE_OUT_OF_DATE,
    NE_SWAPCHAIN_ACQUIRE_FAILED,
} NESwapchainAcquireResult;

typedef enum NESwapchainPresentResult {
    NE_SWAPCHAIN_PRESENT_SUCCESS = 0,
    NE_SWAPCHAIN_PRESENT_OUT_OF_DATE,
    NE_SWAPCHAIN_PRESENT_FAILED,
} NESwapchainPresentResult;

/* ======================================================================== */
/* Interface                                                                */
/* ======================================================================== */

typedef struct NESwapchainI NESwapchainI;

typedef struct NESwapchainOps {
    /*
     * Destroy the swapchain and free all resources.
     * After this call, the pointer is invalid.
     */
    void (*destroy)(NESwapchainI *sc);

    /*
     * Recreate the swapchain (typically after a resize or format change).
     * The implementation queries the current window size internally.
     * Returns true on success. On failure, the swapchain is in an
     * indeterminate state and should be destroyed.
     */
    bool (*recreate)(NESwapchainI *sc);

    /*
     * Acquire the next presentable image.
     *
     * signal_sem: semaphore to signal when the image is ready for rendering.
     *             The renderer will wait on this semaphore in its submit.
     *
     * On success, sc->acquired_image_index is set and the function returns
     * NE_SWAPCHAIN_ACQUIRE_SUCCESS.
     *
     * Returns OUT_OF_DATE if the swapchain needs recreation.
     * Returns FAILED on unrecoverable error.
     */
    NESwapchainAcquireResult (*acquire)(NESwapchainI *sc, VkSemaphore signal_sem);

    /*
     * Present the previously acquired image.
     *
     * queue:    the graphics/present queue
     * wait_sem: semaphore signaled by the render submit (rendering is done)
     *
     * The implementation presents sc->acquired_image_index.
     *
     * Returns OUT_OF_DATE if the swapchain needs recreation.
     * Returns FAILED on unrecoverable error.
     */
    NESwapchainPresentResult (*present)(NESwapchainI *sc, VkQueue queue, VkSemaphore wait_sem);
} NESwapchainOps;

/*
 * Base swapchain structure.
 *
 * All backends "inherit" from this by placing it as the first field.
 * The renderer reads these fields directly — they are the contract.
 */
struct NESwapchainI {
    const NESwapchainOps *ops;

    /* Current swapchain state (updated on create/recreate). */
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;

    /* Per-image resources. Arrays of image_count elements. */
    VkImage *images;
    VkImageView *image_views;

    /* Index returned by the last successful acquire. */
    uint32_t acquired_image_index;

    /* Set by acquire on OUT_OF_DATE, cleared by recreate. */
    bool needs_recreate;
};

/* ======================================================================== */
/* Vulkan WSI backend                                                       */
/* ======================================================================== */

typedef struct NESwapchainVulkanWSIDesc {
    VkDevice device;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    NEWindow *window;
    bool vsync;
} NESwapchainVulkanWSIDesc;

/*
 * Create a swapchain backed by Vulkan WSI (vkCreateSwapchainKHR).
 * This is the default presentation path.
 * Returns NULL on failure.
 */
NESwapchainI *ne_swapchain_vulkan_wsi_create(const NESwapchainVulkanWSIDesc *desc);

/* ======================================================================== */
/* DXGI + DirectComposition backend                                         */
/* ======================================================================== */

typedef struct NESwapchainDXGIDesc {
    VkDevice device;
    VkPhysicalDevice phys;
    VkInstance instance;
    NEWindow *window;
    bool vsync;
} NESwapchainDXGIDesc;

/*
 * Create a swapchain backed by a DXGI SwapChain + DirectComposition.
 *
 * This path gives full control over alpha compositing (transparency) via
 * DXGI_ALPHA_MODE_PREMULTIPLIED. Back buffers are imported into Vulkan
 * via VK_KHR_external_memory_win32.
 *
 * Returns NULL on failure.
 */
NESwapchainI *ne_swapchain_dxgi_create(const NESwapchainDXGIDesc *desc);

#endif /* NE_SWAPCHAIN_H */
