#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "ne_swapchain.h"
#include "ne_log.h"
#include "ne_window.h"

#include <stdlib.h>
#include <string.h>

/* ── Vulkan function pointers (resolved by the renderer before we're created) */

extern PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
extern PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
extern PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
extern PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
extern PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
extern PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
extern PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
extern PFN_vkQueuePresentKHR vkQueuePresentKHR;
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkDestroyImageView vkDestroyImageView;
extern PFN_vkCreateSemaphore vkCreateSemaphore;
extern PFN_vkDestroySemaphore vkDestroySemaphore;
extern PFN_vkDeviceWaitIdle vkDeviceWaitIdle;

/* ── Backend-specific data ─────────────────────────────────────────────── */

typedef struct NESwapchainVulkanWSI {
    NESwapchainI base;

    VkDevice device;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    NEWindow *window;
    bool vsync;

    VkSwapchainKHR swapchain;
    VkColorSpaceKHR color_space;

    /* Per-image semaphores: signaled when rendering to this image is done.
     * Used as wait semaphores in vkQueuePresentKHR. */
    VkSemaphore *sem_render_finished;

    /* Per-image fence tracking: which in-flight fence is using this image.
     * This is not owned by the swapchain — the surface stores it. We track
     * the pointers here so present can be self-contained. */
    VkFence *images_in_flight;
} NESwapchainVulkanWSI;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t ne_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static VkSurfaceFormatKHR ne_choose_surface_format(const VkSurfaceFormatKHR *formats, uint32_t count) {
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR out = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        return out;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            return formats[i];
        }
    }

    return formats[0];
}

static VkPresentModeKHR ne_choose_present_mode(const VkPresentModeKHR *modes, uint32_t count, bool vsync) {
    if (!vsync) {
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

/* Free image views and per-image sync objects (not the VkSwapchainKHR itself). */
static void ne_swapchain_wsi_cleanup_images(NESwapchainVulkanWSI *sc) {
    NESwapchainI *base = &sc->base;

    if (base->image_views) {
        for (uint32_t i = 0; i < base->image_count; i++) {
            if (base->image_views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(sc->device, base->image_views[i], NULL);
            }
        }
        free(base->image_views);
        base->image_views = NULL;
    }

    if (sc->sem_render_finished) {
        for (uint32_t i = 0; i < base->image_count; i++) {
            if (sc->sem_render_finished[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(sc->device, sc->sem_render_finished[i], NULL);
            }
        }
        free(sc->sem_render_finished);
        sc->sem_render_finished = NULL;
    }

    free(sc->images_in_flight);
    sc->images_in_flight = NULL;

    free(base->images);
    base->images = NULL;
    base->image_count = 0;
}

/* Core swapchain creation logic. If old_swapchain is provided, it's passed to
 * vkCreateSwapchainKHR for resource recycling and then destroyed. */
static bool ne_swapchain_wsi_build(NESwapchainVulkanWSI *sc, VkSwapchainKHR old_swapchain) {
    NESwapchainI *base = &sc->base;

    int32_t fb_w = 0, fb_h = 0;
    if (!ne_window_get_framebuffer_size(sc->window, &fb_w, &fb_h)) return false;
    if (fb_w <= 0 || fb_h <= 0) return false;

    /* ── Surface capabilities ───────────────────────────────────────── */

    VkSurfaceCapabilitiesKHR caps;
    VkResult vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(sc->phys, sc->surface, &caps);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (vr=%d)", (int)vr);
        if (old_swapchain) vkDestroySwapchainKHR(sc->device, old_swapchain, NULL);
        return false;
    }

    /* ── Surface format ─────────────────────────────────────────────── */

    uint32_t format_count = 0;
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR(sc->phys, sc->surface, &format_count, NULL);
    if (vr != VK_SUCCESS || format_count == 0) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfaceFormatsKHR failed (vr=%d)", (int)vr);
        if (old_swapchain) vkDestroySwapchainKHR(sc->device, old_swapchain, NULL);
        return false;
    }

    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)calloc(format_count, sizeof(VkSurfaceFormatKHR));
    if (!formats) { if (old_swapchain) vkDestroySwapchainKHR(sc->device, old_swapchain, NULL); return false; }

    vkGetPhysicalDeviceSurfaceFormatsKHR(sc->phys, sc->surface, &format_count, formats);
    VkSurfaceFormatKHR chosen_format = ne_choose_surface_format(formats, format_count);
    free(formats);

    /* ── Present mode ───────────────────────────────────────────────── */

    uint32_t mode_count = 0;
    vr = vkGetPhysicalDeviceSurfacePresentModesKHR(sc->phys, sc->surface, &mode_count, NULL);
    if (vr != VK_SUCCESS || mode_count == 0) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfacePresentModesKHR failed (vr=%d)", (int)vr);
        if (old_swapchain) vkDestroySwapchainKHR(sc->device, old_swapchain, NULL);
        return false;
    }

    VkPresentModeKHR *modes = (VkPresentModeKHR *)calloc(mode_count, sizeof(VkPresentModeKHR));
    if (!modes) { if (old_swapchain) vkDestroySwapchainKHR(sc->device, old_swapchain, NULL); return false; }

    vkGetPhysicalDeviceSurfacePresentModesKHR(sc->phys, sc->surface, &mode_count, modes);
    VkPresentModeKHR chosen_mode = ne_choose_present_mode(modes, mode_count, sc->vsync);
    free(modes);

    /* ── Extent ─────────────────────────────────────────────────────── */

    VkExtent2D extent = {0, 0};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = ne_clamp_u32((uint32_t)fb_w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = ne_clamp_u32((uint32_t)fb_h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    /* ── Image count ────────────────────────────────────────────────── */

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    /* ── Composite alpha ────────────────────────────────────────────── */

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    if ((caps.supportedCompositeAlpha & composite_alpha) == 0) {
        const VkCompositeAlphaFlagBitsKHR candidates[] = {
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        };
        for (uint32_t i = 0; i < 3; i++) {
            if (caps.supportedCompositeAlpha & candidates[i]) {
                composite_alpha = candidates[i];
                break;
            }
        }
    }

    /* ── Create swapchain ───────────────────────────────────────────── */

    VkSwapchainCreateInfoKHR sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = sc->surface;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_format.format;
    sci.imageColorSpace = chosen_format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = composite_alpha;
    sci.presentMode = chosen_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old_swapchain;

    vr = vkCreateSwapchainKHR(sc->device, &sci, NULL, &sc->swapchain);

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(sc->device, old_swapchain, NULL);
    }

    if (vr != VK_SUCCESS || sc->swapchain == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateSwapchainKHR failed (vr=%d)", (int)vr);
        sc->swapchain = VK_NULL_HANDLE;
        return false;
    }

    base->format = chosen_format.format;
    sc->color_space = chosen_format.colorSpace;
    base->extent = extent;

    /* ── Get swapchain images ───────────────────────────────────────── */

    uint32_t img_count = 0;
    vr = vkGetSwapchainImagesKHR(sc->device, sc->swapchain, &img_count, NULL);
    if (vr != VK_SUCCESS || img_count == 0) {
        NE_LOG_ERROR("vkGetSwapchainImagesKHR failed (vr=%d)", (int)vr);
        return false;
    }

    base->images = (VkImage *)calloc(img_count, sizeof(VkImage));
    if (!base->images) return false;

    vr = vkGetSwapchainImagesKHR(sc->device, sc->swapchain, &img_count, base->images);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkGetSwapchainImagesKHR failed (vr=%d)", (int)vr);
        return false;
    }

    base->image_count = img_count;

    /* ── Image views ────────────────────────────────────────────────── */

    base->image_views = (VkImageView *)calloc(img_count, sizeof(VkImageView));
    if (!base->image_views) return false;

    for (uint32_t i = 0; i < img_count; i++) {
        VkImageViewCreateInfo ivci;
        memset(&ivci, 0, sizeof(ivci));
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = base->images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = base->format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;

        vr = vkCreateImageView(sc->device, &ivci, NULL, &base->image_views[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateImageView[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    /* ── Per-image semaphores ───────────────────────────────────────── */

    VkSemaphoreCreateInfo sem_ci;
    memset(&sem_ci, 0, sizeof(sem_ci));
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    sc->sem_render_finished = (VkSemaphore *)calloc(img_count, sizeof(VkSemaphore));
    if (!sc->sem_render_finished) return false;

    for (uint32_t i = 0; i < img_count; i++) {
        vr = vkCreateSemaphore(sc->device, &sem_ci, NULL, &sc->sem_render_finished[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateSemaphore(render_finished[%u]) failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    /* ── Per-image fence tracking ───────────────────────────────────── */

    sc->images_in_flight = (VkFence *)calloc(img_count, sizeof(VkFence));
    if (!sc->images_in_flight) return false;

    base->needs_recreate = false;
    return true;
}

/* ── Interface implementation ──────────────────────────────────────────── */

static void ne_swapchain_wsi_destroy(NESwapchainI *iface) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;

    vkDeviceWaitIdle(sc->device);

    ne_swapchain_wsi_cleanup_images(sc);

    if (sc->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(sc->device, sc->swapchain, NULL);
        sc->swapchain = VK_NULL_HANDLE;
    }

    free(sc);
}

static bool ne_swapchain_wsi_recreate(NESwapchainI *iface) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;

    vkDeviceWaitIdle(sc->device);

    ne_swapchain_wsi_cleanup_images(sc);

    VkSwapchainKHR old = sc->swapchain;
    sc->swapchain = VK_NULL_HANDLE;

    return ne_swapchain_wsi_build(sc, old);
}

static NESwapchainAcquireResult ne_swapchain_wsi_acquire(NESwapchainI *iface, VkSemaphore signal_sem) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;

    uint32_t image_index = 0;
    VkResult vr = vkAcquireNextImageKHR(sc->device, sc->swapchain, UINT64_MAX,
                                        signal_sem, VK_NULL_HANDLE, &image_index);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
        iface->needs_recreate = true;
        return NE_SWAPCHAIN_ACQUIRE_OUT_OF_DATE;
    }

    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkAcquireNextImageKHR failed (vr=%d)", (int)vr);
        return NE_SWAPCHAIN_ACQUIRE_FAILED;
    }

    iface->acquired_image_index = image_index;
    return NE_SWAPCHAIN_ACQUIRE_SUCCESS;
}

static NESwapchainPresentResult ne_swapchain_wsi_present(NESwapchainI *iface, VkQueue queue, VkSemaphore wait_sem) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;

    uint32_t image_index = iface->acquired_image_index;

    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait_sem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc->swapchain;
    pi.pImageIndices = &image_index;

    VkResult vr = vkQueuePresentKHR(queue, &pi);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
        iface->needs_recreate = true;
        return NE_SWAPCHAIN_PRESENT_OUT_OF_DATE;
    }

    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueuePresentKHR failed (vr=%d)", (int)vr);
        return NE_SWAPCHAIN_PRESENT_FAILED;
    }

    return NE_SWAPCHAIN_PRESENT_SUCCESS;
}

/* ── Vtable ────────────────────────────────────────────────────────────── */

static const NESwapchainOps g_swapchain_vulkan_wsi_ops = {
    .destroy  = ne_swapchain_wsi_destroy,
    .recreate = ne_swapchain_wsi_recreate,
    .acquire  = ne_swapchain_wsi_acquire,
    .present  = ne_swapchain_wsi_present,
};

/* ── Public constructor ────────────────────────────────────────────────── */

NESwapchainI *ne_swapchain_vulkan_wsi_create(const NESwapchainVulkanWSIDesc *desc) {
    if (!desc || !desc->device || !desc->phys || !desc->surface || !desc->window) {
        return NULL;
    }

    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)calloc(1, sizeof(NESwapchainVulkanWSI));
    if (!sc) return NULL;

    sc->base.ops = &g_swapchain_vulkan_wsi_ops;
    sc->device = desc->device;
    sc->phys = desc->phys;
    sc->surface = desc->surface;
    sc->window = desc->window;
    sc->vsync = desc->vsync;

    if (!ne_swapchain_wsi_build(sc, VK_NULL_HANDLE)) {
        ne_swapchain_wsi_destroy(&sc->base);
        return NULL;
    }

    return &sc->base;
}

/* ── Accessors for renderer compatibility ──────────────────────────────── */

VkFence *ne_swapchain_vulkan_wsi_get_images_in_flight(NESwapchainI *iface) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;
    return sc->images_in_flight;
}

VkSemaphore ne_swapchain_vulkan_wsi_get_render_finished_sem(NESwapchainI *iface, uint32_t image_index) {
    NESwapchainVulkanWSI *sc = (NESwapchainVulkanWSI *)iface;
    if (image_index >= iface->image_count) return VK_NULL_HANDLE;
    return sc->sem_render_finished[image_index];
}
