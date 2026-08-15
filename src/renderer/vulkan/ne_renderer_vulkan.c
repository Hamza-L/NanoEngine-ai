#if !defined(_WIN32)
#error "Vulkan renderer backend is currently implemented for Win32 only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_image.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"
#include "ne_alloc.h"
#include "ne_swapchain.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal/ne_vulkan_globals.h"
#include "internal/ne_vulkan_loader.h"
#include "internal/ne_vulkan_buffers.h"

#include "internal/ne_vulkan_shaders.h"
#include "internal/ne_vulkan_pipelines.h"
#include "internal/ne_vulkan_renderer.h"

/* Forward declare accessors from ne_swapchain_vulkan_wsi.c */
VkFence *ne_swapchain_vulkan_wsi_get_images_in_flight(NESwapchain *iface);
VkSemaphore ne_swapchain_vulkan_wsi_get_render_finished_sem(NESwapchain *iface, uint32_t image_index);

NERenderer *g_renderer_singleton = NULL;

struct NERenderPass {
    VkCommandBuffer cmd;
    VkPipelineLayout bound_layout;
    VkRenderPass render_pass;
    uint32_t frame_index;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    VkSurfaceKHR surface;
    NESwapchain *swapchain;
    NEPresentBackend present_backend;
    VkRenderPass render_pass;
    VkFramebuffer *framebuffers;
    uint32_t framebuffer_count;

    VkCommandPool cmd_pool;
    VkCommandBuffer cmds[NE_VK_MAX_FRAMES_IN_FLIGHT];
    VkSemaphore sem_image_available[NE_VK_MAX_FRAMES_IN_FLIGHT];
    VkFence fences_in_flight[NE_VK_MAX_FRAMES_IN_FLIGHT];
    uint32_t frame_index;

    bool vsync;

    float clear_color[4];

    struct NERenderSurface *next;

    NERenderPass pass;
};

static bool ne_vk_pick_device_and_queue(NERenderer *r, VkSurfaceKHR surface) {
    if (r->phys != VK_NULL_HANDLE && r->device != VK_NULL_HANDLE) {
        return true;
    }

    uint32_t phys_count = 0;
    VkResult vr = vkEnumeratePhysicalDevices(r->instance, &phys_count, NULL);
    if (vr != VK_SUCCESS || phys_count == 0) {
        NE_LOG_ERROR("vkEnumeratePhysicalDevices failed or no devices (vr=%d)", (int)vr);
        return false;
    }

    VkPhysicalDevice *devs = (VkPhysicalDevice *)calloc(phys_count, sizeof(VkPhysicalDevice));
    if (!devs) {
        return false;
    }

    vr = vkEnumeratePhysicalDevices(r->instance, &phys_count, devs);
    if (vr != VK_SUCCESS) {
        free(devs);
        NE_LOG_ERROR("vkEnumeratePhysicalDevices failed (vr=%d)", (int)vr);
        return false;
    }

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_qfi = UINT32_MAX;

    for (uint32_t d = 0; d < phys_count; d++) {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qf_count, NULL);
        if (qf_count == 0) {
            continue;
        }

        VkQueueFamilyProperties *qfs = (VkQueueFamilyProperties *)calloc(qf_count, sizeof(VkQueueFamilyProperties));
        if (!qfs) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qf_count, qfs);

        for (uint32_t i = 0; i < qf_count; i++) {
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }

            VkBool32 present_support = VK_FALSE;
            vr = vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], i, surface, &present_support);
            if (vr != VK_SUCCESS || !present_support) {
                continue;
            }

            chosen = devs[d];
            chosen_qfi = i;
            break;
        }

        free(qfs);

        if (chosen != VK_NULL_HANDLE) {
            break;
        }
    }

    free(devs);

    if (chosen == VK_NULL_HANDLE || chosen_qfi == UINT32_MAX) {
        NE_LOG_ERROR("no suitable Vulkan physical device/queue family found (need graphics+present)");
        return false;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = chosen_qfi;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char *device_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = sizeof(device_exts) / sizeof(device_exts[0]);
    dci.ppEnabledExtensionNames = device_exts;

    VkDevice device = VK_NULL_HANDLE;
    vr = vkCreateDevice(chosen, &dci, NULL, &device);
    if (vr != VK_SUCCESS || device == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateDevice failed (vr=%d)", (int)vr);
        return false;
    }

    r->phys = chosen;
    vkGetPhysicalDeviceMemoryProperties(r->phys, &r->mem_props);
    r->device = device;
    r->queue_family_index = chosen_qfi;

    if (!ne_vk_load_device_fns(r->device)) {
        NE_LOG_ERROR("failed to load Vulkan device functions");
        return false;
    }

    vkGetDeviceQueue(r->device, r->queue_family_index, 0, &r->queue);
    if (r->queue == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkGetDeviceQueue returned NULL queue");
        return false;
    }

    /* Create a single transfer command pool for buffer staging operations.
     * Uses TRANSIENT_BIT for optimization and RESET_COMMAND_BUFFER_BIT to allow reuse.
     */
    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = r->queue_family_index;

    vr = vkCreateCommandPool(r->device, &pool_info, NULL, &r->transfer_cmd_pool);
    if (vr != VK_SUCCESS || r->transfer_cmd_pool == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateCommandPool (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    /* Allocate a single reusable command buffer from the transfer pool. */
    VkCommandBufferAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = r->transfer_cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    vr = vkAllocateCommandBuffers(r->device, &alloc_info, &r->transfer_cmd);
    if (vr != VK_SUCCESS || r->transfer_cmd == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateCommandBuffers (transfer) failed (vr=%d)", (int)vr);
        vkDestroyCommandPool(r->device, r->transfer_cmd_pool, NULL);
        r->transfer_cmd_pool = VK_NULL_HANDLE;
        return false;
    }

    VkFenceCreateInfo fence_ci;
    memset(&fence_ci, 0, sizeof(fence_ci));
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    vr = vkCreateFence(r->device, &fence_ci, NULL, &r->transfer_fence);
    if (vr != VK_SUCCESS || r->transfer_fence == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateFence (transfer) failed (vr=%d)", (int)vr);
        vkDestroyCommandPool(r->device, r->transfer_cmd_pool, NULL);
        r->transfer_cmd_pool = VK_NULL_HANDLE;
        r->transfer_cmd = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

/* ── Framebuffer helpers ───────────────────────────────────────────────── */
static void ne_vk_destroy_framebuffers(NERenderSurface *surface) {
    NERenderer *r = surface->renderer;
    if (surface->framebuffers) {
        for (uint32_t i = 0; i < surface->framebuffer_count; i++) {
            if (surface->framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(r->device, surface->framebuffers[i], NULL);
            }
        }
        free(surface->framebuffers);
        surface->framebuffers = NULL;
        surface->framebuffer_count = 0;
    }
}

static bool ne_vk_build_framebuffers(NERenderSurface *surface) {
    NERenderer *r = surface->renderer;
    NESwapchain *sc = surface->swapchain;

    ne_vk_destroy_framebuffers(surface);

    surface->framebuffers = (VkFramebuffer *)calloc(sc->image_count, sizeof(VkFramebuffer));
    if (!surface->framebuffers) return false;
    surface->framebuffer_count = sc->image_count;

    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkFramebufferCreateInfo fbci;
        memset(&fbci, 0, sizeof(fbci));
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = surface->render_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &sc->image_views[i];
        fbci.width = sc->extent.width;
        fbci.height = sc->extent.height;
        fbci.layers = 1;

        VkResult vr = vkCreateFramebuffer(r->device, &fbci, NULL, &surface->framebuffers[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateFramebuffer[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    return true;
}

static bool ne_vk_surface_init(NERenderSurface *surface, NERenderer *r,
                               NEWindow *window, VkSurfaceKHR vk_surface,
                               const NERenderSurfaceDesc *desc) {
    surface->renderer = r;
    surface->window = window;
    surface->surface = vk_surface;
    surface->swapchain = NULL;
    surface->vsync = true;

    surface->present_backend = NE_PRESENT_BACKEND_DEFAULT;
    surface->clear_color[0] = 0.1f;
    surface->clear_color[1] = 0.1f;
    surface->clear_color[2] = 0.2f;
    surface->clear_color[3] = 1.0f;
    if (desc) {
        surface->vsync = desc->vsync;
        surface->present_backend = desc->present_backend;
        memcpy(surface->clear_color, desc->clear_color_rgba, sizeof(surface->clear_color));
    }

    if (!ne_vk_pick_device_and_queue(r, vk_surface)) {
        return false;
    }

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = r->queue_family_index;

    VkResult vr = vkCreateCommandPool(r->device, &cpci, NULL, &surface->cmd_pool);
    if (vr != VK_SUCCESS || surface->cmd_pool == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateCommandPool failed (vr=%d)", (int)vr);
        return false;
    }

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = surface->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = NE_VK_MAX_FRAMES_IN_FLIGHT;

    vr = vkAllocateCommandBuffers(r->device, &cbai, surface->cmds);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkAllocateCommandBuffers failed (vr=%d)", (int)vr);
        return false;
    }

    VkSemaphoreCreateInfo sci_sem;
    memset(&sci_sem, 0, sizeof(sci_sem));
    sci_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vr = vkCreateSemaphore(r->device, &sci_sem, NULL, &surface->sem_image_available[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateSemaphore(image_available[%u]) failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vr = vkCreateFence(r->device, &fci, NULL, &surface->fences_in_flight[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateFence[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    surface->frame_index = 0;
    return true;
}

/* (Surface format / present mode / clamp helpers moved to ne_swapchain_vulkan_wsi.c) */

/**
 * Create (or recreate) the surface's VkRenderPass.
 *
 * The render pass is a single-subpass, single color attachment configuration:
 *   - loadOp  = CLEAR  (clears to the surface's clear_color each frame)
 *   - storeOp = STORE  (presents the result)
 *   - layout  = UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
 *
 * Only recreated when the swapchain format changes.
 */
static bool ne_vk_create_render_pass(NERenderSurface *surface, VkFormat format) {
    NERenderer *r = surface->renderer;

    /* Destroy previous render pass if format changed. */
    if (surface->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(r->device, surface->render_pass, NULL);
        surface->render_pass = VK_NULL_HANDLE;
    }

    VkAttachmentDescription color_attachment;
    memset(&color_attachment, 0, sizeof(color_attachment));
    color_attachment.format         = format;
    color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref;
    memset(&color_ref, 0, sizeof(color_ref));
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    VkSubpassDependency dependency;
    memset(&dependency, 0, sizeof(dependency));
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci;
    memset(&rpci, 0, sizeof(rpci));
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &color_attachment;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dependency;

    VkResult vr = vkCreateRenderPass(r->device, &rpci, NULL, &surface->render_pass);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkCreateRenderPass failed (vr=%d)", (int)vr);
        return false;
    }

    return true;
}

/**
 * Create (or recreate) the swapchain via the abstraction layer and rebuild
 * framebuffers. Handles both initial creation and resize.
 */
static bool ne_vk_surface_ensure_swapchain(NERenderSurface *surface) {
    NERenderer *r = surface->renderer;

    if (surface->swapchain) {
        ne_vk_destroy_framebuffers(surface);

        if (!surface->swapchain->ops->recreate(surface->swapchain)) {
            return false;
        }
    } else {
        /*
         * First-time creation: pick backend based on surface desc.
         * For Vulkan WSI we need the device/queue selected first (needs a
         * VkSurfaceKHR). For DXGI we still need the device (for importing
         * shared buffers) but don't use VkSurfaceKHR for presentation.
         */
        if (surface->present_backend == NE_PRESENT_BACKEND_DXGI) {
            if (!ne_vk_pick_device_and_queue(r, surface->surface)) {
                return false;
            }
            surface->swapchain = ne_swapchain_dxgi_create(&(NESwapchainDXGIDesc){
                .device = r->device,
                .phys = r->phys,
                .instance = r->instance,
                .window = surface->window,
                .vsync = surface->vsync,
            });
        } else {
            if (!ne_vk_pick_device_and_queue(r, surface->surface)) {
                return false;
            }
            surface->swapchain = ne_swapchain_vulkan_wsi_create(&(NESwapchainVulkanWSIDesc){
                .device = r->device,
                .phys = r->phys,
                .surface = surface->surface,
                .window = surface->window,
                .vsync = surface->vsync,
            });
        }

        if (!surface->swapchain) {
            return false;
        }
    }

    /* Create render pass if needed (first time, or format changed). */
    if (surface->render_pass == VK_NULL_HANDLE) {
        if (!ne_vk_create_render_pass(surface, surface->swapchain->format)) {
            return false;
        }
    }

    /* Build framebuffers from the swapchain's image views. */
    if (!ne_vk_build_framebuffers(surface)) {
        return false;
    }

    return true;
}

NERenderer *ne_renderer_create(const NERendererDesc *desc) {
    NERenderer *r = NULL;
    VkExtensionProperties *avail_exts = NULL;
    VkLayerProperties *avail_layers = NULL;

    if (g_renderer_singleton) {
        NE_LOG_ERROR("ne_renderer_create: renderer already created (only one renderer is supported)");
        goto err_return;
    }

    r = (NERenderer *)calloc(1, sizeof(NERenderer));
    if (!r) {
        NE_LOG_ERROR("ne_renderer_create: alloc failed");
        goto err_return;
    }

    if (!ne_vk_load_loader(&r->vulkan_lib)) {
        NE_LOG_ERROR("ne_renderer_create: loading vulkan library failed");
        goto err_return;
    }

    /* Enumerate extensions and layers once, then check against the cached lists. */
    uint32_t avail_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, NULL);
    if (avail_ext_count > 0) {
        avail_exts = (VkExtensionProperties *)calloc(avail_ext_count, sizeof(VkExtensionProperties));
        if (avail_exts) {
            vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, avail_exts);
        }
    }

    uint32_t avail_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&avail_layer_count, NULL);
    if (avail_layer_count > 0) {
        avail_layers = (VkLayerProperties *)calloc(avail_layer_count, sizeof(VkLayerProperties));
        if (avail_layers) {
            vkEnumerateInstanceLayerProperties(&avail_layer_count, avail_layers);
        }
    }

    /* Instance extensions required for Win32 surfaces. */
    const char *extensions[8];
    uint32_t ext_count = 0;

    const char *required_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    for (uint32_t i = 0; i < (uint32_t)(sizeof(required_exts) / sizeof(required_exts[0])); i++) {
        bool found = false;
        for (uint32_t j = 0; j < avail_ext_count && avail_exts; j++) {
            if (strcmp(avail_exts[j].extensionName, required_exts[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            NE_LOG_ERROR("ne_renderer_create: required Vulkan instance extension not available: %s", required_exts[i]);
            goto err_return;
        }
        extensions[ext_count++] = required_exts[i];
    }

    /* Best-effort validation support. */
    const char *layers[4];
    uint32_t layer_count = 0;

    if (!desc || desc->enable_validation) {
        const char *val_layer = "VK_LAYER_KHRONOS_validation";
        bool has_val_layer = false;
        for (uint32_t i = 0; i < avail_layer_count && avail_layers; i++) {
            if (strcmp(avail_layers[i].layerName, val_layer) == 0) {
                has_val_layer = true;
                break;
            }
        }
        if (has_val_layer) {
            layers[layer_count++] = val_layer;

            /* Debug utils is optional; we wire it up later. */
            for (uint32_t i = 0; i < avail_ext_count && avail_exts; i++) {
                if (strcmp(avail_exts[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                    break;
                }
            }
        } else {
            NE_LOG_WARN("validation requested but VK_LAYER_KHRONOS_validation not available");
        }
    }

    VkApplicationInfo app_info;
    memset(&app_info, 0, sizeof(app_info));
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "NanoEngine";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.pEngineName = "NanoEngine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = extensions;
    ici.enabledLayerCount = layer_count;
    ici.ppEnabledLayerNames = layer_count ? layers : NULL;

    VkResult vr = vkCreateInstance(&ici, NULL, &r->instance);
    if (vr != VK_SUCCESS || r->instance == VK_NULL_HANDLE) {
        NE_LOG_ERROR("ne_renderer_create: vkCreateInstance failed: %d", (int)vr);
        goto err_return;
    }

    if (!ne_vk_load_instance_fns(r->instance)) {
        NE_LOG_ERROR("ne_renderer_create: failed to load vulkan functions ");
        goto err_return;
    }

    g_renderer_singleton = r;
    return r;

 err_return:
    if(avail_exts) free(avail_exts);
    if(avail_layers) free(avail_layers);
    ne_renderer_destroy(r);
    return r;
}

void ne_renderer_destroy(NERenderer *r) {
    if (!r) {
        return;
    }

    /* Drain the GPU before tearing down any resources. */
    if (r->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(r->device);
    }

    /* Destroy surfaces first (this cleans up swapchains). */
    struct NERenderSurface *s = r->surfaces;
    while (s) {
        struct NERenderSurface *next = s->next;

        ne_vk_destroy_framebuffers(s);
        if (s->swapchain) {
            s->swapchain->ops->destroy(s->swapchain);
            s->swapchain = NULL;
        }

        /* Destroy surface-lifetime sync resources. */
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (s->fences_in_flight[i] != VK_NULL_HANDLE) {
                vkDestroyFence(r->device, s->fences_in_flight[i], NULL);
                s->fences_in_flight[i] = VK_NULL_HANDLE;
            }
        }
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (s->sem_image_available[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(r->device, s->sem_image_available[i], NULL);
                s->sem_image_available[i] = VK_NULL_HANDLE;
            }
        }
        if (s->cmd_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(r->device, s->cmd_pool, NULL);
            s->cmd_pool = VK_NULL_HANDLE;
        }

        if (s->render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(r->device, s->render_pass, NULL);
            s->render_pass = VK_NULL_HANDLE;
        }

        if (s->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(r->instance, s->surface, NULL);
            s->surface = VK_NULL_HANDLE;
        }

        s->window = NULL;
        s->renderer = NULL;
        s->next = NULL;
        free(s);
        s = next;
    }
    r->surfaces = NULL;

    ne_buffer_destroy_all(r); //also destroys all images
    ne_shader_destroy_all(r);
    ne_pipeline_destroy_all(r);


    /* Destroy staging buffer and transfer command pool. */
    if (r->staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
    }
    if (r->staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
    }
    r->staging_mapped = NULL;
    r->staging_size = 0;

    if (r->transfer_fence != VK_NULL_HANDLE) {
        vkDestroyFence(r->device, r->transfer_fence, NULL);
        r->transfer_fence = VK_NULL_HANDLE;
    }
    if (r->transfer_cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(r->device, r->transfer_cmd_pool, NULL);
        r->transfer_cmd_pool = VK_NULL_HANDLE;
    }
    r->transfer_cmd = VK_NULL_HANDLE;

    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
        r->queue = VK_NULL_HANDLE;
        r->phys = VK_NULL_HANDLE;
        r->queue_family_index = 0;
    }

    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }

    if (r->vulkan_lib) {
        FreeLibrary(r->vulkan_lib);
        r->vulkan_lib = NULL;
    }

    if (r == g_renderer_singleton) {
        g_renderer_singleton = NULL;
    }

    free(r);
    r = NULL;
}

NERenderSurface *ne_renderer_create_surface(NERenderer *r, NEWindow *window, const NERenderSurfaceDesc *desc) {
    NERenderSurface *surface = NULL;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (!r || !window) {
        NE_LOG_ERROR("ne_renderer_create_surface: bad param");
        goto err_return;
    }

    if (!ne_window_is_open(window)) {
        NE_LOG_ERROR("ne_renderer_create_surface: window closed");
        goto err_return;
    }

    for (NERenderSurface *it = r->surfaces; it; it = it->next) {
        if (it->window == window) {
            NE_LOG_INFO("ne_renderer_create_surface: window already has a surface");
            return it;
        }
    }

    HWND hwnd = (HWND)ne_window_get_native_handle(window, NE_NATIVE_HANDLE_WIN32_HWND);
    if (!hwnd) {
        NE_LOG_ERROR("ne_renderer_create_surface: cannot retrieve window hwnd");
        goto err_return;
    }

    VkWin32SurfaceCreateInfoKHR sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandleW(NULL);
    sci.hwnd = hwnd;

    const VkResult vr = vkCreateWin32SurfaceKHR(r->instance, &sci, NULL, &vk_surface);
    if (vr != VK_SUCCESS || vk_surface == VK_NULL_HANDLE) {
        NE_LOG_ERROR("ne_renderer_create_surface: failed to create surface: %d", (int)vr);
        goto err_return;
    }

    surface = (NERenderSurface *)calloc(1, sizeof(NERenderSurface));
    if (!surface) {
        NE_LOG_ERROR("ne_renderer_create_surface: alloc failed: %d", (int)vr);
        goto err_return;
    }

    if (!ne_vk_surface_init(surface, r, window, vk_surface, desc)) {
        NE_LOG_ERROR("ne_renderer_create_surface: failed to create surface: %d", (int)vr);
        goto err_return;
    }

    surface->next = r->surfaces;
    r->surfaces = surface;
    return surface;

 err_return:
    if(vk_surface) vkDestroySurfaceKHR(r->instance, vk_surface, NULL);
    if(surface) ne_renderer_destroy_surface(r, surface);
    return surface;
}

void ne_renderer_destroy_surface(NERenderer *r, NERenderSurface *surface) {
    if (!r || !surface) {
        return;
    }

    (void)vkDeviceWaitIdle(r->device);

    NERenderSurface **pp = &r->surfaces;
    while (*pp) {
        if (*pp == surface) {
            *pp = surface->next;
            break;
        }
        pp = &(*pp)->next;
    }

    ne_vk_destroy_framebuffers(surface);
    if (surface->swapchain) {
        surface->swapchain->ops->destroy(surface->swapchain);
        surface->swapchain = NULL;
    }

    /* Destroy surface-lifetime sync resources. */
    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (surface->fences_in_flight[i] != VK_NULL_HANDLE) {
            vkDestroyFence(r->device, surface->fences_in_flight[i], NULL);
            surface->fences_in_flight[i] = VK_NULL_HANDLE;
        }
    }
    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (surface->sem_image_available[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(r->device, surface->sem_image_available[i], NULL);
            surface->sem_image_available[i] = VK_NULL_HANDLE;
        }
    }
    if (surface->cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(r->device, surface->cmd_pool, NULL);
        surface->cmd_pool = VK_NULL_HANDLE;
    }

    if (surface->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(r->device, surface->render_pass, NULL);
        surface->render_pass = VK_NULL_HANDLE;
    }

    if (surface->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(r->instance, surface->surface, NULL);
        surface->surface = VK_NULL_HANDLE;
    }

    surface->window = NULL;
    surface->renderer = NULL;
    surface->next = NULL;

    free(surface);
    surface = NULL;
}

void ne_renderer_surface_set_clear_color(NERenderSurface *surface, float r, float g, float b, float a) {
    if (!surface) {
        return;
    }
    surface->clear_color[0] = r;
    surface->clear_color[1] = g;
    surface->clear_color[2] = b;
    surface->clear_color[3] = a;
}

NERenderPass *ne_renderer_begin_frame(NERenderer *r, NERenderSurface *surface) {
    if (!r || !surface || surface->renderer != r) {
        return NULL;
    }

    if (!surface->window || !ne_window_is_open(surface->window)) {
        return NULL;
    }

    NESwapchain *sc = surface->swapchain;

    if (!sc || sc->needs_recreate) {
        if (!ne_vk_surface_ensure_swapchain(surface)) {
            return NULL;
        }
        sc = surface->swapchain;
    }

    const uint32_t frame = surface->frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;

    const VkResult vr_wait = vkWaitForFences(r->device, 1, &surface->fences_in_flight[frame], VK_TRUE, UINT64_MAX);
    if (vr_wait != VK_SUCCESS) {
        return NULL;
    }

    NESwapchainAcquireResult acq = sc->ops->acquire(sc, surface->sem_image_available[frame]);

    if (acq == NE_SWAPCHAIN_ACQUIRE_OUT_OF_DATE) {
        return NULL;
    }
    if (acq != NE_SWAPCHAIN_ACQUIRE_SUCCESS) {
        return NULL;
    }

    uint32_t image_index = sc->acquired_image_index;

    if (image_index >= sc->image_count) {
        return NULL;
    }

    /*
     * If a previous frame is still using this image, wait for it.
     * Important: do this BEFORE resetting the current frame fence, because
     * images_in_flight[image] may point to fences_in_flight[frame].
     *
     * This tracking only applies to the Vulkan WSI path (where multiple flight
     * frames may target different swapchain images). The DXGI path uses
     * vkDeviceWaitIdle at present time, so this is unnecessary.
     */
    if (surface->present_backend != NE_PRESENT_BACKEND_DXGI) {
        VkFence *images_in_flight = ne_swapchain_vulkan_wsi_get_images_in_flight(sc);
        if (images_in_flight && images_in_flight[image_index] != VK_NULL_HANDLE &&
            images_in_flight[image_index] != surface->fences_in_flight[frame]) {
            (void)vkWaitForFences(r->device, 1, &images_in_flight[image_index], VK_TRUE, UINT64_MAX);
        }
        if (images_in_flight) {
            images_in_flight[image_index] = surface->fences_in_flight[frame];
        }
    }

    (void)vkResetFences(r->device, 1, &surface->fences_in_flight[frame]);
    if (vkResetCommandBuffer) {
        (void)vkResetCommandBuffer(surface->cmds[frame], 0);
    }

    VkCommandBuffer cmd = surface->cmds[frame];

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = 0;

    VkResult vr = vkBeginCommandBuffer(cmd, &bi);
    if (vr != VK_SUCCESS) {
        return NULL;
    }

    /* ── Begin render pass (clear via loadOp, layout transitions automatic) ── */

    VkClearValue clear_value;
    memset(&clear_value, 0, sizeof(clear_value));
    clear_value.color.float32[0] = surface->clear_color[0];
    clear_value.color.float32[1] = surface->clear_color[1];
    clear_value.color.float32[2] = surface->clear_color[2];
    clear_value.color.float32[3] = surface->clear_color[3];

    VkRenderPassBeginInfo rpbi;
    memset(&rpbi, 0, sizeof(rpbi));
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = surface->render_pass;
    rpbi.framebuffer       = surface->framebuffers[image_index];
    rpbi.renderArea.offset = (VkOffset2D){0, 0};
    rpbi.renderArea.extent = sc->extent;
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear_value;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    /* Set default viewport and scissor to match the surface extent. */
    VkViewport viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.x        = 0.0f;
    viewport.y        = (float)sc->extent.height;
    viewport.width    = (float)sc->extent.width;
    viewport.height   = -(float)sc->extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor;
    memset(&scissor, 0, sizeof(scissor));
    scissor.offset = (VkOffset2D){0, 0};
    scissor.extent = sc->extent;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    surface->pass.cmd          = cmd;
    surface->pass.bound_layout = VK_NULL_HANDLE;
    surface->pass.render_pass  = surface->render_pass;
    surface->pass.frame_index  = surface->frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;
    return &surface->pass;
}

void ne_renderer_end_frame(NERenderer *r, NERenderSurface *surface) {
    if (!r || !surface || surface->renderer != r) {
        return;
    }

    NERenderPass *pass = &surface->pass;
    if (!pass->cmd) {
        return;
    }

    NESwapchain *sc = surface->swapchain;

    /* ── Close the render pass and command buffer ────────────────────── */

    vkCmdEndRenderPass(pass->cmd);

    VkResult vr = vkEndCommandBuffer(pass->cmd);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkEndCommandBuffer failed (vr=%d)", (int)vr);
        *pass = (NERenderPass){0};
        sc->needs_recreate = true;
        return;
    }

    /* ── Submit and present ──────────────────────────────────────────── */

    const uint32_t image_index = sc->acquired_image_index;
    const uint32_t frame_index = surface->frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_finished_sem = VK_NULL_HANDLE;

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &surface->cmds[frame_index];

    if (surface->present_backend == NE_PRESENT_BACKEND_DXGI) {
        /*
         * DXGI path: no GPU semaphore from acquire (DXGI doesn't produce one),
         * and no signal semaphore needed (present uses vkDeviceWaitIdle).
         * We still use the flight fence for frame pacing.
         */
    } else {
        /* Vulkan WSI: wait on image_available, signal render_finished. */
        render_finished_sem = ne_swapchain_vulkan_wsi_get_render_finished_sem(sc, image_index);
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &surface->sem_image_available[frame_index];
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_finished_sem;
    }

    vr = vkQueueSubmit(r->queue, 1, &si, surface->fences_in_flight[frame_index]);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueueSubmit failed (vr=%d)", (int)vr);
        sc->needs_recreate = true;
        return;
    }

    NESwapchainPresentResult pres = sc->ops->present(sc, r->queue, render_finished_sem);
    if (pres != NE_SWAPCHAIN_PRESENT_SUCCESS) {
        /* needs_recreate is set inside the present call on OUT_OF_DATE. */
    }

    ne_window_show_at_least_once(surface->window);

    surface->frame_index = (surface->frame_index + 1u) % NE_VK_MAX_FRAMES_IN_FLIGHT;

    *pass = (NERenderPass){0};
}

/* ========================================================================
 * Render Pass Commands (Graphics)
 * ======================================================================== */

void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline) {
    if (!pass || !pass->cmd) {
        return;
    }

    NERenderer *r = g_renderer_singleton;
    if (!r || !ne_pipeline_handle_valid(pipeline)) {
        NE_LOG_WARN("ne_render_pass_set_pipeline: invalid pass or pipeline handle");
        return;
    }

    const uint32_t index = pipeline.id - 1;
    if (index >= r->pipelines.cap || !((NEVulkanPipelineSlot*)r->pipelines.slots)[index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_pipeline: pipeline handle (id=%u) is invalid or destroyed", pipeline.id);
        return;
    }

    NEVulkanPipelineSlot *slot = &((NEVulkanPipelineSlot*)r->pipelines.slots)[index];

    /* Deferred compilation: build the VkPipeline on first use. */
    if (slot->needs_compile) {
        if (!ne_vk_pipeline_compile(r, slot, pass->render_pass)) {
            /* compilation_failed is already set inside ne_vk_pipeline_compile. */
            return;
        }
    }

    if (slot->compilation_failed || slot->pipeline == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, slot->pipeline);
    pass->bound_layout = slot->layout;
}

void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint64_t slot, NEBufferHandle buffer) {
    if (!pass || !pass->cmd) {
        return;
    }


    NERenderer *r = g_renderer_singleton;
    if (!r || !ne_buffer_handle_valid(buffer)) {
        return;
    }

    const uint32_t buf_index = buffer.id - 1;
    if (buf_index >= r->buffers.cap || !((NEVulkanBufferSlot*)r->buffers.slots)[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_vertex_buffer: buffer handle (id=%u) is invalid or destroyed", buffer.id);
        return;
    }

    VkBuffer vk_buffer = ne_vk_buffer_for_frame(&((NEVulkanBufferSlot*)r->buffers.slots)[buf_index], pass->frame_index);
    VkDeviceSize offset = 0;

    vkCmdBindVertexBuffers(pass->cmd, slot, 1, &vk_buffer, &offset);
}

void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer,
                                     NEIndexType type) {
    if (!pass || !pass->cmd) {
        return;
    }

    NERenderer *r = g_renderer_singleton;
    if (!r || !ne_buffer_handle_valid(buffer)) {
        return;
    }

    const uint32_t buf_index = buffer.id - 1;
    if (buf_index >= r->buffers.cap || !((NEVulkanBufferSlot*)r->buffers.slots)[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_index_buffer: buffer handle (id=%u) is invalid or destroyed", buffer.id);
        return;
    }

    VkBuffer vk_buffer = ne_vk_buffer_for_frame(&((NEVulkanBufferSlot*)r->buffers.slots)[buf_index], pass->frame_index);
    VkIndexType vk_type = (type == NE_INDEX_TYPE_UINT32) ? VK_INDEX_TYPE_UINT32
                                                         : VK_INDEX_TYPE_UINT16;

    vkCmdBindIndexBuffer(pass->cmd, vk_buffer, 0, vk_type);
}

void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage, uint64_t slot, const void *data, size_t size) {
    if (!pass || !pass->cmd || !data || size == 0) {
        return;
    }

    if (pass->bound_layout == VK_NULL_HANDLE) {
        NE_LOG_WARN("ne_render_pass_set_uniform_data: no pipeline bound (call set_pipeline first)");
        return;
    }

    NERenderer *r = g_renderer_singleton;
    if (!r) {
        return;
    }

    VkShaderStageFlags stage_flags;
    switch (stage) {
    case NE_SHADER_STAGE_VERTEX:   stage_flags = VK_SHADER_STAGE_VERTEX_BIT;   break;
    case NE_SHADER_STAGE_FRAGMENT: stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT; break;
    default:
        NE_LOG_WARN("ne_render_pass_set_uniform_data: unsupported shader stage (%d)", (int)stage);
        return;
    }

    /*
     * Push constant offset = 0.  The 'slot' parameter is a Metal-ism (buffer
     * index for setVertexBytes/setFragmentBytes).  Vulkan push constants are a
     * flat byte range; we always write from offset 0.
     */
    (void)slot;

    if (size > 128) {
        NE_LOG_WARN("ne_render_pass_set_uniform_data: size (%u) exceeds guaranteed push constant "
                    "minimum (128 bytes)", (unsigned)size);
    }

    vkCmdPushConstants(pass->cmd, pass->bound_layout, stage_flags, 0, size, data);
}

void ne_render_pass_update_buffer(NERenderPass *pass, NEBufferHandle handle,
                                  const void *data, uint32_t size, uint32_t offset) {
    if (!pass || !pass->cmd || !data || size == 0 || !ne_buffer_handle_valid(handle)) {
        return;
    }

    NERenderer *r = g_renderer_singleton;
    const uint32_t buf_index = handle.id - 1;
    if (buf_index >= r->buffers.cap || !((NEVulkanBufferSlot*)r->buffers.slots)[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_update_buffer: invalid buffer handle (id=%u)", handle.id);
        return;
    }

    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)r->buffers.slots)[buf_index];

    /* Overflow-safe bounds check: `offset + size` could wrap around uint32_t. */
    if (size > slot->size || offset > slot->size - size) {
        NE_LOG_ERROR("buffer update out of bounds (offset=%u + size=%u > buffer_size=%u)",
                     offset, size, slot->size);
        return;
    }

    if (!slot->dynamic) {
        NE_LOG_WARN("ne_render_pass_update_buffer: buffer (id=%u) is not dynamic; "
                    "create it with NEBufferDesc.dynamic = true", handle);
        return;
    }

    /*
     * Write only this frame's copy. The fence wait in begin_frame guarantees the
     * GPU has finished the copy at this frame index, so the write neither stalls
     * nor races. Memory is host-coherent, so no explicit flush is needed.
     */
    const uint32_t frame = pass->frame_index;
    if (slot->dyn_mapped[frame]) {
        memcpy((uint8_t *)slot->dyn_mapped[frame] + offset, data, size);
    }
}

void ne_render_pass_draw(NERenderPass *pass, uint64_t first_vertex, uint64_t vertex_count) {
    if (!pass || !pass->cmd || vertex_count == 0) {
        return;
    }

    vkCmdDraw(pass->cmd, vertex_count, 1, first_vertex, 0);
}

void ne_render_pass_draw_indexed(NERenderPass *pass, uint64_t index_count,
                                 uint64_t first_index, int64_t vertex_offset) {
    if (!pass || !pass->cmd || index_count == 0) {
        return;
    }

    vkCmdDrawIndexed(pass->cmd, index_count, 1, first_index, vertex_offset, 0);
}

/* ========================================================================
 * Compute Pass Stubs
 * ======================================================================== */

NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass) {
    (void)pass;
    NE_LOG_WARN("ne_render_pass_begin_compute: compute passes not yet implemented on Vulkan");
    return NULL;
}

void ne_render_pass_end_compute(NERenderPass *pass, NEComputePass *compute) {
    (void)pass;
    (void)compute;
}

void ne_compute_pass_set_pipeline(NEComputePass *pass, NEComputePipelineHandle pipeline) {
    (void)pass;
    (void)pipeline;
}

void ne_compute_pass_set_storage_buffer(NEComputePass *pass, uint64_t slot,
                                        NEBufferHandle buffer) {
    (void)pass;
    (void)slot;
    (void)buffer;
}

void ne_compute_pass_set_uniform_data(NEComputePass *pass, uint64_t slot,
                                      const void *data, uint32_t size) {
    (void)pass;
    (void)slot;
    (void)data;
    (void)size;
}

void ne_compute_pass_dispatch(NEComputePass *pass, uint64_t group_count_x,
                              uint64_t group_count_y, uint64_t group_count_z) {
    (void)pass;
    (void)group_count_x;
    (void)group_count_y;
    (void)group_count_z;
}
