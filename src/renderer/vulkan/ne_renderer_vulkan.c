#if !defined(_WIN32)
#error "Vulkan renderer backend is currently implemented for Win32 only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* We dynamically load Vulkan; don't rely on link-time prototypes. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_window.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Vulkan backend bring-up plan (incremental):
 * 1) Create VkInstance (done)
 * 2) Create VkSurfaceKHR (done)
 * 3) Pick physical device + create VkDevice + queue (done)
 * 4) Create swapchain
 * 5) Clear + present (no pipelines, no render pass yet)
 */

typedef struct NEVulkanFns {
    /* Global */
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;

    /* Instance */
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;

    PFN_vkCreateDevice vkCreateDevice;

    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;

    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;

    /* Device */
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;

    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;

    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;

    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkDestroySemaphore vkDestroySemaphore;
    PFN_vkCreateFence vkCreateFence;
    PFN_vkDestroyFence vkDestroyFence;
    PFN_vkWaitForFences vkWaitForFences;
    PFN_vkResetFences vkResetFences;

    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkResetCommandPool vkResetCommandPool;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;

    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdClearColorImage vkCmdClearColorImage;
} NEVulkanFunctions;

enum {
    NE_VK_MAX_FRAMES_IN_FLIGHT = 2,
};

typedef struct NESwapchain {
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;

    VkImage *images;
    VkImageLayout *image_layouts;
    uint32_t image_count;

    /* One command pool, one command buffer per frame-in-flight. */
    VkCommandPool cmd_pool;
    VkCommandBuffer cmds[NE_VK_MAX_FRAMES_IN_FLIGHT];

    VkSemaphore sem_image_available[NE_VK_MAX_FRAMES_IN_FLIGHT];
    VkFence fences_in_flight[NE_VK_MAX_FRAMES_IN_FLIGHT];

    /* Track which fence currently owns each swapchain image. */
    VkFence *images_in_flight;

    /*
     * Use a separate render-finished semaphore per swapchain image.
     * Rationale: a semaphore waited on by presentation may still be in use
     * after vkQueuePresentKHR returns; indexing by image avoids re-signaling a
     * semaphore that the swapchain is still using.
     */
    VkSemaphore *sem_render_finished;

    uint32_t frame_index;
    uint32_t acquired_image_index;
} NESwapchain;

struct NERenderer {
    HMODULE vulkan_lib;
    VkInstance instance;

    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;

    NEVulkanFunctions fns;

    struct NERenderSurface *surfaces;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    VkSurfaceKHR surface;
    NESwapchain sc;

    bool wants_swapchain_recreate;

    float clear_color[4];

    struct NERenderSurface *next;
};

/**
 * NERenderPass is currently backed by the surface itself.
 * This may become a separate allocation when multiple render passes per frame
 * are supported.
 */
struct NERenderPass {
    NERenderSurface *surface;
};

/* Per-frame render pass instance (valid between begin_frame / end_frame). */
static NERenderPass g_active_pass = {0};

static NERenderer *g_renderer_singleton = NULL;

static void *ne_vk_get_global(const NEVulkanFunctions *fns, const char *name) {
    if (!fns || !fns->vkGetInstanceProcAddr) {
        return NULL;
    }
    return (void *)fns->vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
}

static void *ne_vk_get_instance(const NEVulkanFunctions *fns, VkInstance instance, const char *name) {
    if (!fns || !fns->vkGetInstanceProcAddr) {
        return NULL;
    }
    return (void *)fns->vkGetInstanceProcAddr(instance, name);
}

static void *ne_vk_get_device(const NEVulkanFunctions *fns, VkDevice device, const char *name) {
    if (!fns || !fns->vkGetDeviceProcAddr) {
        return NULL;
    }
    return (void *)fns->vkGetDeviceProcAddr(device, name);
}

static bool ne_vk_has_extension(const NEVulkanFunctions *fns, const char *ext_name) {
    if (!fns || !fns->vkEnumerateInstanceExtensionProperties || !ext_name) {
        return false;
    }

    uint32_t count = 0;
    VkResult r = fns->vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (r != VK_SUCCESS || count == 0) {
        return false;
    }

    VkExtensionProperties *props = (VkExtensionProperties *)calloc(count, sizeof(VkExtensionProperties));
    if (!props) {
        return false;
    }

    r = fns->vkEnumerateInstanceExtensionProperties(NULL, &count, props);
    bool found = false;
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].extensionName, ext_name) == 0) {
                found = true;
                break;
            }
        }
    }

    free(props);
    return found;
}

static bool ne_vk_has_layer(const NEVulkanFunctions *fns, const char *layer_name) {
    if (!fns || !fns->vkEnumerateInstanceLayerProperties || !layer_name) {
        return false;
    }

    uint32_t count = 0;
    VkResult r = fns->vkEnumerateInstanceLayerProperties(&count, NULL);
    if (r != VK_SUCCESS || count == 0) {
        return false;
    }

    VkLayerProperties *props = (VkLayerProperties *)calloc(count, sizeof(VkLayerProperties));
    if (!props) {
        return false;
    }

    r = fns->vkEnumerateInstanceLayerProperties(&count, props);
    bool found = false;
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].layerName, layer_name) == 0) {
                found = true;
                break;
            }
        }
    }

    free(props);
    return found;
}

static void ne_vk_sc_cleanup(NERenderer *r, NESwapchain *sc) {
    if (!r || !sc) {
        return;
    }

    if (r->device != VK_NULL_HANDLE && r->fns.vkDeviceWaitIdle) {
        (void)r->fns.vkDeviceWaitIdle(r->device);
    }

    if (sc->cmd_pool != VK_NULL_HANDLE && r->fns.vkDestroyCommandPool) {
        r->fns.vkDestroyCommandPool(r->device, sc->cmd_pool, NULL);
        sc->cmd_pool = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        sc->cmds[i] = VK_NULL_HANDLE;
    }

    if (sc->sem_render_finished && r->fns.vkDestroySemaphore) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->sem_render_finished[i] != VK_NULL_HANDLE) {
                r->fns.vkDestroySemaphore(r->device, sc->sem_render_finished[i], NULL);
                sc->sem_render_finished[i] = VK_NULL_HANDLE;
            }
        }
        free(sc->sem_render_finished);
        sc->sem_render_finished = NULL;
    }

    if (r->fns.vkDestroySemaphore) {
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (sc->sem_image_available[i] != VK_NULL_HANDLE) {
                r->fns.vkDestroySemaphore(r->device, sc->sem_image_available[i], NULL);
                sc->sem_image_available[i] = VK_NULL_HANDLE;
            }
        }
    }

    if (r->fns.vkDestroyFence) {
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (sc->fences_in_flight[i] != VK_NULL_HANDLE) {
                r->fns.vkDestroyFence(r->device, sc->fences_in_flight[i], NULL);
                sc->fences_in_flight[i] = VK_NULL_HANDLE;
            }
        }
    }

    free(sc->images_in_flight);
    sc->images_in_flight = NULL;

    if (sc->swapchain != VK_NULL_HANDLE && r->fns.vkDestroySwapchainKHR) {
        r->fns.vkDestroySwapchainKHR(r->device, sc->swapchain, NULL);
        sc->swapchain = VK_NULL_HANDLE;
    }

    free(sc->images);
    sc->images = NULL;
    free(sc->image_layouts);
    sc->image_layouts = NULL;
    sc->image_count = 0;

    /* sem_render_finished is freed above (needs image_count). */

    sc->format = VK_FORMAT_UNDEFINED;
    sc->color_space = (VkColorSpaceKHR)0;
    sc->extent.width = 0;
    sc->extent.height = 0;
    sc->frame_index = 0;
    sc->acquired_image_index = 0;
}

static bool ne_vk_load_loader(NERenderer *renderer) {
    renderer->vulkan_lib = LoadLibraryA("vulkan-1.dll");
    if (!renderer->vulkan_lib) {
        NE_LOG_ERROR("failed to load vulkan-1.dll (Vulkan runtime not installed?)");
        return false;
    }

    renderer->fns.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(renderer->vulkan_lib, "vkGetInstanceProcAddr");
    renderer->fns.vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(renderer->vulkan_lib, "vkGetDeviceProcAddr");

    if (!renderer->fns.vkGetInstanceProcAddr || !renderer->fns.vkGetDeviceProcAddr) {
        NE_LOG_ERROR("failed to get Vulkan proc address functions");
        return false;
    }

    renderer->fns.vkCreateInstance = (PFN_vkCreateInstance)ne_vk_get_global(&renderer->fns, "vkCreateInstance");
    renderer->fns.vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)ne_vk_get_global(&renderer->fns, "vkEnumerateInstanceExtensionProperties");
    renderer->fns.vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)ne_vk_get_global(&renderer->fns, "vkEnumerateInstanceLayerProperties");

    if (!renderer->fns.vkCreateInstance || !renderer->fns.vkEnumerateInstanceExtensionProperties) {
        NE_LOG_ERROR("failed to load required Vulkan loader entry points");
        return false;
    }

    return true;
}

static bool ne_vk_load_instance_fns(NERenderer *r) {
    NEVulkanFunctions *f = &r->fns;

    f->vkDestroyInstance = (PFN_vkDestroyInstance)ne_vk_get_instance(f, r->instance, "vkDestroyInstance");
    f->vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)ne_vk_get_instance(f, r->instance, "vkEnumeratePhysicalDevices");
    f->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceProperties");
    f->vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    f->vkCreateDevice = (PFN_vkCreateDevice)ne_vk_get_instance(f, r->instance, "vkCreateDevice");

    f->vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)ne_vk_get_instance(f, r->instance, "vkCreateWin32SurfaceKHR");
    f->vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)ne_vk_get_instance(f, r->instance, "vkDestroySurfaceKHR");

    f->vkGetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    f->vkGetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    f->vkGetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    f->vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    return f->vkDestroyInstance && f->vkEnumeratePhysicalDevices && f->vkGetPhysicalDeviceQueueFamilyProperties && f->vkCreateDevice &&
           f->vkCreateWin32SurfaceKHR && f->vkDestroySurfaceKHR && f->vkGetPhysicalDeviceSurfaceSupportKHR &&
           f->vkGetPhysicalDeviceSurfaceCapabilitiesKHR && f->vkGetPhysicalDeviceSurfaceFormatsKHR && f->vkGetPhysicalDeviceSurfacePresentModesKHR;
}

static bool ne_vk_load_device_fns(NERenderer *r) {
    NEVulkanFunctions *f = &r->fns;

    f->vkDestroyDevice = (PFN_vkDestroyDevice)ne_vk_get_device(f, r->device, "vkDestroyDevice");
    f->vkGetDeviceQueue = (PFN_vkGetDeviceQueue)ne_vk_get_device(f, r->device, "vkGetDeviceQueue");
    f->vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)ne_vk_get_device(f, r->device, "vkDeviceWaitIdle");

    f->vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)ne_vk_get_device(f, r->device, "vkCreateSwapchainKHR");
    f->vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)ne_vk_get_device(f, r->device, "vkDestroySwapchainKHR");
    f->vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)ne_vk_get_device(f, r->device, "vkGetSwapchainImagesKHR");
    f->vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)ne_vk_get_device(f, r->device, "vkAcquireNextImageKHR");

    f->vkQueueSubmit = (PFN_vkQueueSubmit)ne_vk_get_device(f, r->device, "vkQueueSubmit");
    f->vkQueuePresentKHR = (PFN_vkQueuePresentKHR)ne_vk_get_device(f, r->device, "vkQueuePresentKHR");
    f->vkQueueWaitIdle = (PFN_vkQueueWaitIdle)ne_vk_get_device(f, r->device, "vkQueueWaitIdle");

    f->vkCreateSemaphore = (PFN_vkCreateSemaphore)ne_vk_get_device(f, r->device, "vkCreateSemaphore");
    f->vkDestroySemaphore = (PFN_vkDestroySemaphore)ne_vk_get_device(f, r->device, "vkDestroySemaphore");
    f->vkCreateFence = (PFN_vkCreateFence)ne_vk_get_device(f, r->device, "vkCreateFence");
    f->vkDestroyFence = (PFN_vkDestroyFence)ne_vk_get_device(f, r->device, "vkDestroyFence");
    f->vkWaitForFences = (PFN_vkWaitForFences)ne_vk_get_device(f, r->device, "vkWaitForFences");
    f->vkResetFences = (PFN_vkResetFences)ne_vk_get_device(f, r->device, "vkResetFences");

    f->vkCreateCommandPool = (PFN_vkCreateCommandPool)ne_vk_get_device(f, r->device, "vkCreateCommandPool");
    f->vkDestroyCommandPool = (PFN_vkDestroyCommandPool)ne_vk_get_device(f, r->device, "vkDestroyCommandPool");
    f->vkResetCommandPool = (PFN_vkResetCommandPool)ne_vk_get_device(f, r->device, "vkResetCommandPool");
    f->vkResetCommandBuffer = (PFN_vkResetCommandBuffer)ne_vk_get_device(f, r->device, "vkResetCommandBuffer");
    f->vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)ne_vk_get_device(f, r->device, "vkAllocateCommandBuffers");
    f->vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)ne_vk_get_device(f, r->device, "vkFreeCommandBuffers");

    f->vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)ne_vk_get_device(f, r->device, "vkBeginCommandBuffer");
    f->vkEndCommandBuffer = (PFN_vkEndCommandBuffer)ne_vk_get_device(f, r->device, "vkEndCommandBuffer");
    f->vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)ne_vk_get_device(f, r->device, "vkCmdPipelineBarrier");
    f->vkCmdClearColorImage = (PFN_vkCmdClearColorImage)ne_vk_get_device(f, r->device, "vkCmdClearColorImage");

    return f->vkDestroyDevice && f->vkGetDeviceQueue && f->vkCreateSwapchainKHR && f->vkGetSwapchainImagesKHR &&
           f->vkAcquireNextImageKHR && f->vkQueueSubmit && f->vkQueuePresentKHR && f->vkCreateSemaphore && f->vkCreateFence &&
           f->vkWaitForFences && f->vkResetFences && f->vkCreateCommandPool && f->vkResetCommandBuffer && f->vkAllocateCommandBuffers &&
           f->vkBeginCommandBuffer && f->vkEndCommandBuffer && f->vkCmdPipelineBarrier && f->vkCmdClearColorImage;
}

static bool ne_vk_pick_device_and_queue(NERenderer *r, VkSurfaceKHR surface) {
    if (r->phys != VK_NULL_HANDLE && r->device != VK_NULL_HANDLE) {
        return true;
    }

    uint32_t phys_count = 0;
    VkResult vr = r->fns.vkEnumeratePhysicalDevices(r->instance, &phys_count, NULL);
    if (vr != VK_SUCCESS || phys_count == 0) {
        NE_LOG_ERROR("vkEnumeratePhysicalDevices failed or no devices (vr=%d)", (int)vr);
        return false;
    }

    VkPhysicalDevice *devs = (VkPhysicalDevice *)calloc(phys_count, sizeof(VkPhysicalDevice));
    if (!devs) {
        return false;
    }

    vr = r->fns.vkEnumeratePhysicalDevices(r->instance, &phys_count, devs);
    if (vr != VK_SUCCESS) {
        free(devs);
        NE_LOG_ERROR("vkEnumeratePhysicalDevices failed (vr=%d)", (int)vr);
        return false;
    }

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_qfi = UINT32_MAX;

    for (uint32_t d = 0; d < phys_count; d++) {
        uint32_t qf_count = 0;
        r->fns.vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qf_count, NULL);
        if (qf_count == 0) {
            continue;
        }

        VkQueueFamilyProperties *qfs = (VkQueueFamilyProperties *)calloc(qf_count, sizeof(VkQueueFamilyProperties));
        if (!qfs) {
            continue;
        }
        r->fns.vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qf_count, qfs);

        for (uint32_t i = 0; i < qf_count; i++) {
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }

            VkBool32 present_support = VK_FALSE;
            vr = r->fns.vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], i, surface, &present_support);
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

    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_exts;

    VkDevice device = VK_NULL_HANDLE;
    vr = r->fns.vkCreateDevice(chosen, &dci, NULL, &device);
    if (vr != VK_SUCCESS || device == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateDevice failed (vr=%d)", (int)vr);
        return false;
    }

    r->phys = chosen;
    r->device = device;
    r->queue_family_index = chosen_qfi;

    if (!ne_vk_load_device_fns(r)) {
        NE_LOG_ERROR("failed to load Vulkan device functions");
        return false;
    }

    r->fns.vkGetDeviceQueue(r->device, r->queue_family_index, 0, &r->queue);
    if (r->queue == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkGetDeviceQueue returned NULL queue");
        return false;
    }

    return true;
}

static VkSurfaceFormatKHR ne_vk_choose_surface_format(const VkSurfaceFormatKHR *formats, uint32_t count) {
    /* If the surface has no preferred format, pick a reasonable default. */
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR out = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        return out;
    }

    /* Prefer BGRA8 UNORM if available (common on Windows). */
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            return formats[i];
        }
    }

    return formats[0];
}

static VkPresentModeKHR ne_vk_choose_present_mode(const VkPresentModeKHR *modes, uint32_t count, bool vsync) {
    /* FIFO is guaranteed. */
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;

    if (!vsync) {
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                return VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
    }

    return chosen;
}

static uint32_t ne_vk_clamp_u32(uint32_t v, uint32_t minv, uint32_t maxv) {
    if (v < minv) {
        return minv;
    }
    if (v > maxv) {
        return maxv;
    }
    return v;
}

static bool ne_vk_sc_create(NERenderSurface *surface, bool vsync) {
    if (!surface || !surface->renderer) {
        return false;
    }

    NERenderer *r = surface->renderer;

    int32_t fb_w = 0;
    int32_t fb_h = 0;
    if (!ne_window_get_framebuffer_size(surface->window, &fb_w, &fb_h)) {
        return false;
    }
    if (fb_w <= 0 || fb_h <= 0) {
        return false;
    }

    if (!ne_vk_pick_device_and_queue(r, surface->surface)) {
        return false;
    }

    ne_vk_sc_cleanup(r, &surface->sc);

    VkSurfaceCapabilitiesKHR caps;
    VkResult vr = r->fns.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, surface->surface, &caps);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (vr=%d)", (int)vr);
        return false;
    }

    uint32_t format_count = 0;
    vr = r->fns.vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, surface->surface, &format_count, NULL);
    if (vr != VK_SUCCESS || format_count == 0) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfaceFormatsKHR failed/no formats (vr=%d)", (int)vr);
        return false;
    }

    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)calloc(format_count, sizeof(VkSurfaceFormatKHR));
    if (!formats) {
        return false;
    }

    vr = r->fns.vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, surface->surface, &format_count, formats);
    if (vr != VK_SUCCESS) {
        free(formats);
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfaceFormatsKHR failed (vr=%d)", (int)vr);
        return false;
    }

    VkSurfaceFormatKHR chosen_format = ne_vk_choose_surface_format(formats, format_count);
    free(formats);

    uint32_t mode_count = 0;
    vr = r->fns.vkGetPhysicalDeviceSurfacePresentModesKHR(r->phys, surface->surface, &mode_count, NULL);
    if (vr != VK_SUCCESS || mode_count == 0) {
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfacePresentModesKHR failed/no modes (vr=%d)", (int)vr);
        return false;
    }

    VkPresentModeKHR *modes = (VkPresentModeKHR *)calloc(mode_count, sizeof(VkPresentModeKHR));
    if (!modes) {
        return false;
    }

    vr = r->fns.vkGetPhysicalDeviceSurfacePresentModesKHR(r->phys, surface->surface, &mode_count, modes);
    if (vr != VK_SUCCESS) {
        free(modes);
        NE_LOG_ERROR("vkGetPhysicalDeviceSurfacePresentModesKHR failed (vr=%d)", (int)vr);
        return false;
    }

    const VkPresentModeKHR chosen_mode = ne_vk_choose_present_mode(modes, mode_count, vsync);
    free(modes);

    VkExtent2D extent = {0, 0};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = ne_vk_clamp_u32((uint32_t)fb_w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = ne_vk_clamp_u32((uint32_t)fb_h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & composite_alpha) == 0) {
        /* Pick the first supported bit. */
        const VkCompositeAlphaFlagBitsKHR candidates[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (uint32_t i = 0; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
            if (caps.supportedCompositeAlpha & candidates[i]) {
                composite_alpha = candidates[i];
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface->surface;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_format.format;
    sci.imageColorSpace = chosen_format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                          : caps.currentTransform;
    sci.compositeAlpha = composite_alpha;
    sci.presentMode = chosen_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    vr = r->fns.vkCreateSwapchainKHR(r->device, &sci, NULL, &surface->sc.swapchain);
    if (vr != VK_SUCCESS || surface->sc.swapchain == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateSwapchainKHR failed (vr=%d)", (int)vr);
        surface->sc.swapchain = VK_NULL_HANDLE;
        return false;
    }

    surface->sc.format = chosen_format.format;
    surface->sc.color_space = chosen_format.colorSpace;
    surface->sc.extent = extent;

    uint32_t img_count = 0;
    vr = r->fns.vkGetSwapchainImagesKHR(r->device, surface->sc.swapchain, &img_count, NULL);
    if (vr != VK_SUCCESS || img_count == 0) {
        NE_LOG_ERROR("vkGetSwapchainImagesKHR failed/no images (vr=%d)", (int)vr);
        return false;
    }

    surface->sc.images = (VkImage *)calloc(img_count, sizeof(VkImage));
    surface->sc.image_layouts = (VkImageLayout *)calloc(img_count, sizeof(VkImageLayout));
    if (!surface->sc.images || !surface->sc.image_layouts) {
        return false;
    }

    vr = r->fns.vkGetSwapchainImagesKHR(r->device, surface->sc.swapchain, &img_count, surface->sc.images);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkGetSwapchainImagesKHR failed (vr=%d)", (int)vr);
        return false;
    }

    surface->sc.image_count = img_count;
    for (uint32_t i = 0; i < img_count; i++) {
        surface->sc.image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    surface->sc.images_in_flight = (VkFence *)calloc(surface->sc.image_count, sizeof(VkFence));
    if (!surface->sc.images_in_flight) {
        return false;
    }
    for (uint32_t i = 0; i < surface->sc.image_count; i++) {
        surface->sc.images_in_flight[i] = VK_NULL_HANDLE;
    }

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = r->queue_family_index;

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = NE_VK_MAX_FRAMES_IN_FLIGHT;

    vr = r->fns.vkCreateCommandPool(r->device, &cpci, NULL, &surface->sc.cmd_pool);
    if (vr != VK_SUCCESS || surface->sc.cmd_pool == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateCommandPool failed (vr=%d)", (int)vr);
        return false;
    }

    cbai.commandPool = surface->sc.cmd_pool;
    vr = r->fns.vkAllocateCommandBuffers(r->device, &cbai, surface->sc.cmds);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkAllocateCommandBuffers failed (vr=%d)", (int)vr);
        return false;
    }

    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (surface->sc.cmds[i] == VK_NULL_HANDLE) {
            NE_LOG_ERROR("vkAllocateCommandBuffers returned NULL cmd[%u]", (unsigned)i);
            return false;
        }
    }

    VkSemaphoreCreateInfo sci_sem;
    memset(&sci_sem, 0, sizeof(sci_sem));
    sci_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vr = r->fns.vkCreateSemaphore(r->device, &sci_sem, NULL, &surface->sc.sem_image_available[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateSemaphore(image_available[%u]) failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    surface->sc.sem_render_finished = (VkSemaphore *)calloc(surface->sc.image_count, sizeof(VkSemaphore));
    if (!surface->sc.sem_render_finished) {
        return false;
    }

    for (uint32_t i = 0; i < surface->sc.image_count; i++) {
        vr = r->fns.vkCreateSemaphore(r->device, &sci_sem, NULL, &surface->sc.sem_render_finished[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateSemaphore(render_finished[%u]) failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vr = r->fns.vkCreateFence(r->device, &fci, NULL, &surface->sc.fences_in_flight[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateFence[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    surface->sc.frame_index = 0;
    surface->wants_swapchain_recreate = false;
    return true;
}

static void ne_vk_transition_image(NERenderer *r, VkCommandBuffer cmd, VkImage img, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.dstAccessMask = 0;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    r->fns.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc) {
    (void)app;

    if (g_renderer_singleton) {
        NE_LOG_ERROR("renderer already created (only one renderer is supported)");
        return NULL;
    }

    NERenderer *r = (NERenderer *)calloc(1, sizeof(NERenderer));
    if (!r) {
        return NULL;
    }

    if (!ne_vk_load_loader(r)) {
        ne_renderer_destroy(r);
        return NULL;
    }

    /* Instance extensions required for Win32 surfaces. */
    const char *extensions[8];
    uint32_t ext_count = 0;

    const char *required_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    for (uint32_t i = 0; i < (uint32_t)(sizeof(required_exts) / sizeof(required_exts[0])); i++) {
        if (!ne_vk_has_extension(&r->fns, required_exts[i])) {
            NE_LOG_ERROR("required Vulkan instance extension not available: %s", required_exts[i]);
            ne_renderer_destroy(r);
            return NULL;
        }
        extensions[ext_count++] = required_exts[i];
    }

    /* Best-effort validation support. */
    const char *layers[4];
    uint32_t layer_count = 0;

    if (!desc || desc->enable_validation) {
        const char *val_layer = "VK_LAYER_KHRONOS_validation";
        if (ne_vk_has_layer(&r->fns, val_layer)) {
            layers[layer_count++] = val_layer;

            /* Debug utils is optional; we wire it up later. */
            if (ne_vk_has_extension(&r->fns, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
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

    VkResult vr = r->fns.vkCreateInstance(&ici, NULL, &r->instance);
    if (vr != VK_SUCCESS || r->instance == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateInstance failed: %d", (int)vr);
        ne_renderer_destroy(r);
        return NULL;
    }

    if (!ne_vk_load_instance_fns(r)) {
        NE_LOG_ERROR("failed to load Vulkan instance functions");
        ne_renderer_destroy(r);
        return NULL;
    }

    g_renderer_singleton = r;
    return r;
}

void ne_renderer_destroy(NERenderer *r) {
    if (!r) {
        return;
    }

    /* Destroy surfaces first (this cleans up swapchains). */
    struct NERenderSurface *s = r->surfaces;
    while (s) {
        struct NERenderSurface *next = s->next;

        ne_vk_sc_cleanup(r, &s->sc);

        if (s->surface != VK_NULL_HANDLE && r->fns.vkDestroySurfaceKHR) {
            r->fns.vkDestroySurfaceKHR(r->instance, s->surface, NULL);
            s->surface = VK_NULL_HANDLE;
        }

        s->window = NULL;
        s->renderer = NULL;
        s->next = NULL;
        free(s);
        s = next;
    }
    r->surfaces = NULL;

    if (r->device != VK_NULL_HANDLE && r->fns.vkDestroyDevice) {
        (void)r->fns.vkDeviceWaitIdle(r->device);
        r->fns.vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
        r->queue = VK_NULL_HANDLE;
        r->phys = VK_NULL_HANDLE;
        r->queue_family_index = 0;
    }

    if (r->instance != VK_NULL_HANDLE && r->fns.vkDestroyInstance) {
        r->fns.vkDestroyInstance(r->instance, NULL);
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
}

NERenderSurface *ne_renderer_create_surface(NERenderer *r, NEWindow *window, const NERenderSurfaceDesc *desc) {
    if (!r || !window) {
        return NULL;
    }

    if (!ne_window_is_open(window)) {
        return NULL;
    }

    for (NERenderSurface *it = r->surfaces; it; it = it->next) {
        if (it->window == window) {
            NE_LOG_ERROR("window already has a render surface");
            return NULL;
        }
    }

    HWND hwnd = (HWND)ne_window_get_native_handle(window, NE_NATIVE_HANDLE_WIN32_HWND);
    if (!hwnd) {
        return NULL;
    }

    VkWin32SurfaceCreateInfoKHR sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandleW(NULL);
    sci.hwnd = hwnd;

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    const VkResult vr = r->fns.vkCreateWin32SurfaceKHR(r->instance, &sci, NULL, &vk_surface);
    if (vr != VK_SUCCESS || vk_surface == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateWin32SurfaceKHR failed: %d", (int)vr);
        return NULL;
    }

    NERenderSurface *surface = (NERenderSurface *)calloc(1, sizeof(NERenderSurface));
    if (!surface) {
        r->fns.vkDestroySurfaceKHR(r->instance, vk_surface, NULL);
        return NULL;
    }

    surface->renderer = r;
    surface->window = window;
    surface->surface = vk_surface;
    surface->wants_swapchain_recreate = true;

    surface->clear_color[0] = 0.1f;
    surface->clear_color[1] = 0.1f;
    surface->clear_color[2] = 0.2f;
    surface->clear_color[3] = 1.0f;
    if (desc) {
        memcpy(surface->clear_color, desc->clear_color_rgba, sizeof(surface->clear_color));
    }

    /* Device creation is delayed until we have a surface (for present support checks). */
    if (!ne_vk_pick_device_and_queue(r, surface->surface)) {
        ne_renderer_destroy_surface(r, surface);
        return NULL;
    }

    surface->next = r->surfaces;
    r->surfaces = surface;

    return surface;
}

void ne_renderer_destroy_surface(NERenderer *r, NERenderSurface *surface) {
    if (!r || !surface) {
        return;
    }

    (void)r->fns.vkDeviceWaitIdle(r->device);

    NERenderSurface **pp = &r->surfaces;
    while (*pp) {
        if (*pp == surface) {
            *pp = surface->next;
            break;
        }
        pp = &(*pp)->next;
    }

    ne_vk_sc_cleanup(r, &surface->sc);

    if (surface->surface != VK_NULL_HANDLE) {
        r->fns.vkDestroySurfaceKHR(r->instance, surface->surface, NULL);
        surface->surface = VK_NULL_HANDLE;
    }

    surface->window = NULL;
    surface->renderer = NULL;
    surface->next = NULL;

    free(surface);
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

    const bool vsync = true; /* surface desc vsync is not stored yet; treat as true for now. */

    if (surface->wants_swapchain_recreate || surface->sc.swapchain == VK_NULL_HANDLE) {
        if (!ne_vk_sc_create(surface, vsync)) {
            return NULL;
        }
    }

    const uint32_t frame = surface->sc.frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;

    const VkResult vr_wait = r->fns.vkWaitForFences(r->device, 1, &surface->sc.fences_in_flight[frame], VK_TRUE, UINT64_MAX);
    if (vr_wait != VK_SUCCESS) {
        return NULL;
    }

    uint32_t image_index = 0;
    VkResult vr = r->fns.vkAcquireNextImageKHR(r->device, surface->sc.swapchain, UINT64_MAX,
                                              surface->sc.sem_image_available[frame], VK_NULL_HANDLE, &image_index);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
        surface->wants_swapchain_recreate = true;
        return NULL;
    }

    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkAcquireNextImageKHR failed (vr=%d)", (int)vr);
        return NULL;
    }

    if (image_index >= surface->sc.image_count) {
        return NULL;
    }

    /*
     * If a previous frame is still using this image, wait for it.
     * Important: do this BEFORE resetting the current frame fence, because
     * `images_in_flight[image]` may point to `fences_in_flight[frame]`.
     */
    if (surface->sc.images_in_flight && surface->sc.images_in_flight[image_index] != VK_NULL_HANDLE &&
        surface->sc.images_in_flight[image_index] != surface->sc.fences_in_flight[frame]) {
        (void)r->fns.vkWaitForFences(r->device, 1, &surface->sc.images_in_flight[image_index], VK_TRUE, UINT64_MAX);
    }

    (void)r->fns.vkResetFences(r->device, 1, &surface->sc.fences_in_flight[frame]);
    if (r->fns.vkResetCommandBuffer) {
        (void)r->fns.vkResetCommandBuffer(surface->sc.cmds[frame], 0);
    }

    if (surface->sc.images_in_flight) {
        surface->sc.images_in_flight[image_index] = surface->sc.fences_in_flight[frame];
    }

    surface->sc.acquired_image_index = image_index;

    VkCommandBuffer cmd = surface->sc.cmds[frame];

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vr = r->fns.vkBeginCommandBuffer(cmd, &bi);
    if (vr != VK_SUCCESS) {
        return NULL;
    }

    VkImage img = surface->sc.images[image_index];

    /* Transition to TRANSFER_DST_OPTIMAL, clear, then transition back to PRESENT. */
    const VkImageLayout old_layout = surface->sc.image_layouts[image_index];
    ne_vk_transition_image(r, cmd, img, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkClearColorValue cc = {{surface->clear_color[0], surface->clear_color[1], surface->clear_color[2], surface->clear_color[3]}};
    VkImageSubresourceRange range;
    memset(&range, 0, sizeof(range));
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    r->fns.vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

    ne_vk_transition_image(r, cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vr = r->fns.vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        return NULL;
    }

    surface->sc.image_layouts[image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    g_active_pass.surface = surface;
    return &g_active_pass;
}

void ne_renderer_end_frame(NERenderer *r, NERenderPass *pass) {
    if (!r || !pass || !pass->surface) {
        return;
    }

    NERenderSurface *surface = pass->surface;
    if (surface->renderer != r) {
        return;
    }

    const uint32_t image_index = surface->sc.acquired_image_index;

    const uint32_t frame = surface->sc.frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &surface->sc.sem_image_available[frame];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &surface->sc.cmds[frame];

    VkSemaphore sem_render_finished = VK_NULL_HANDLE;
    if (surface->sc.sem_render_finished && image_index < surface->sc.image_count) {
        sem_render_finished = surface->sc.sem_render_finished[image_index];
    }
    if (sem_render_finished == VK_NULL_HANDLE) {
        surface->wants_swapchain_recreate = true;
        return;
    }

    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem_render_finished;

    VkResult vr = r->fns.vkQueueSubmit(r->queue, 1, &si, surface->sc.fences_in_flight[frame]);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueueSubmit failed (vr=%d)", (int)vr);
        surface->wants_swapchain_recreate = true;
        return;
    }

    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sem_render_finished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &surface->sc.swapchain;
    pi.pImageIndices = &image_index;

    vr = r->fns.vkQueuePresentKHR(r->queue, &pi);
    if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
        surface->wants_swapchain_recreate = true;
    } else if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueuePresentKHR failed (vr=%d)", (int)vr);
        surface->wants_swapchain_recreate = true;
    }

    surface->sc.frame_index = (surface->sc.frame_index + 1u) % NE_VK_MAX_FRAMES_IN_FLIGHT;

    pass->surface = NULL;
}
