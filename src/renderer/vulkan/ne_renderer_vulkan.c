#include "glslang_c_shader_types.h"
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
#include "ne_file.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_image.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"
#include "ne_alloc.h"

#include "glslang_c_interface.h"
#include "../Public/resource_limits_c.h"

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

    /* Buffer management */
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
    PFN_vkCmdCopyImage vkCmdCopyImage;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;

    /* Shader management */
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;

    /* Render pass */
    PFN_vkCreateRenderPass vkCreateRenderPass;
    PFN_vkDestroyRenderPass vkDestroyRenderPass;

    /* Image views */
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;

    /* Framebuffers */
    PFN_vkCreateFramebuffer vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer;

    /* Pipeline */
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;

    /* Render pass commands */
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer;
    PFN_vkCmdSetViewport vkCmdSetViewport;
    PFN_vkCmdSetScissor vkCmdSetScissor;
    PFN_vkCmdDraw vkCmdDraw;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed;
    PFN_vkCmdPushConstants vkCmdPushConstants;
} NEVulkanFunctions;

enum {
    NE_VK_MAX_FRAMES_IN_FLIGHT = 2,
};

/* ── Buffer resource pool ───────────────────────────────────────────────── */

typedef struct NEVulkanBufferSlot {
    bool occupied;
    bool dynamic;          /* true: per-frame host-visible copies (no stall, no race) */
    uint32_t usage;
    union {
        uint32_t size;
        struct {
            uint32_t width;
            uint32_t height;
            NEImageFormat format;
        };
    };
    union {
        VkBuffer buffer;
        struct {
            VkImage image;
            VkImageView imageView;
        };
    };
    VkDeviceMemory memory;

    /*
     * Dynamic buffers only: one host-visible + coherent, persistently-mapped
     * copy per in-flight frame. The CPU writes mapped[frame] directly (no
     * staging buffer, no transfer, no stall) while the GPU reads another copy.
     * Static buffers leave these zeroed and use `buffer`/`memory` above
     * (device-local + staged upload).
     */
    VkBuffer dyn_buffers[NE_VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory dyn_memories[NE_VK_MAX_FRAMES_IN_FLIGHT];
    void *dyn_mapped[NE_VK_MAX_FRAMES_IN_FLIGHT];
} NEVulkanBufferSlot;

/* Select the VkBuffer a slot exposes for a given frame: the per-frame copy for
 * dynamic buffers, or the single device-local buffer for static ones. */
static VkBuffer ne_vk_buffer_for_frame(const NEVulkanBufferSlot *slot, uint32_t frame_index) {
    if (slot->dynamic) {
        return slot->dyn_buffers[frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT];
    }
    return slot->buffer;
}

/* ── Shader resource pool ───────────────────────────────────────────────── */

typedef struct NEVulkanShaderSlot {
    bool occupied;
    uint32_t stage;             /* NEShaderStage value */
    VkShaderModule module;
    char *entry_point;          /* strdup'd entry point name */
} NEVulkanShaderSlot;

/* ── Pipeline resource pool ─────────────────────────────────────────────── */

typedef struct NEVulkanPipelineSlot {
    bool occupied;
    bool needs_compile;         /* true until first successful vkCreateGraphicsPipelines */
    bool compilation_failed;    /* true if deferred compilation failed */

    /* Eagerly created at ne_pipeline_create time. */
    VkPipelineLayout layout;

    /* Captured state for deferred vkCreateGraphicsPipelines. */
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    VkShaderModule compute_module;
    char *vert_entry;           /* strdup'd entry point name */
    char *frag_entry;           /* strdup'd entry point name */
    char *compute_entry;        /* strdup'd entry point name */

    VkVertexInputBindingDescription *bindings;
    uint32_t binding_count;
    VkVertexInputAttributeDescription *attributes;
    uint32_t attribute_count;

    VkPrimitiveTopology topology;
    VkPipelineColorBlendAttachmentState blend;

    /* Created during deferred compilation (VK_NULL_HANDLE until then). */
    VkPipeline pipeline;
} NEVulkanPipelineSlot;

typedef struct NESwapchain {
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;

    VkImage *images;
    VkImageLayout *image_layouts;
    VkImageView *image_views;
    VkFramebuffer *framebuffers;
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

    /* Buffer resource pool */
    NEVulkanBufferSlot *buffers;
    uint32_t buffer_count;
    uint32_t buffer_cap;

    /* Staging buffer for data uploads */
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    uint32_t staging_size;

    /* Transfer command pool for staging uploads */
    VkCommandPool transfer_cmd_pool;
    VkCommandBuffer transfer_cmd;


    /* ─── Descriptor Set Infrastructure (foundation for future GPU binding) ─── */
    /* 
     * Descriptor sets enable binding SSBOs, UBOs, and samplers to pipelines.
     * This infrastructure is prepared for future implementation.
     *
     * Design:
     * - Descriptor pool: centralized allocation point for descriptor sets
     * - Descriptor set layout cache: avoids redundant layout creation
     * - Binding structure: associates buffers/images with pipeline resources
     *
     * Current strategy: use push constants for uniforms (simpler, ~128-256 byte limit)
     * Future: migrate to descriptor set-based binding for flexible resource management
     *
     * Integration points:
     * - ne_pipeline_create(): will accept descriptor set layout requirements
     * - ne_render_pass_bind_buffer(): future function to bind SSBOs to pipelines
     * - Pipeline layout creation: will include descriptor set layouts
     */
    /* VkDescriptorPool descriptor_pool;         Placeholder: not yet created  */
    /* uint32_t descriptor_pool_size;            Placeholder: pool capacity  */
    /* VkDescriptorSetLayout *layout_cache;     Placeholder: reuse layouts  */
    /* uint32_t layout_cache_count;             Placeholder: active layouts */

    /* Shader resource pool */
    NEVulkanShaderSlot *shaders;
    uint32_t shader_count;
    uint32_t shader_cap;

    /* Pipeline resource pool */
    NEVulkanPipelineSlot *pipelines;
    uint32_t pipeline_count;
    uint32_t pipeline_cap;

    /* Runtime GLSL → SPIR-V compilation via shaderc (loaded dynamically). */
    NEShaderOptimization shader_optimization; /* default: NE_SHADER_OPTIMIZATION_NONE (0) */

    struct NERenderSurface *surfaces;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    VkSurfaceKHR surface;
    NESwapchain sc;
    VkRenderPass render_pass;

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
    VkCommandBuffer cmd;              /* live command buffer for this frame */
    VkPipelineLayout bound_layout;    /* layout of currently bound pipeline (for push constants) */
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

static void ne_vk_swapchain_cleanup(NERenderer *r, NESwapchain *sc) {
    if (!r || !sc) {
        return;
    }

    if (r->device != VK_NULL_HANDLE && r->fns.vkDeviceWaitIdle) {
        (void)r->fns.vkDeviceWaitIdle(r->device);
    }

    /* Destroy framebuffers before image views (framebuffers reference views). */
    if (sc->framebuffers && r->fns.vkDestroyFramebuffer) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->framebuffers[i] != VK_NULL_HANDLE) {
                r->fns.vkDestroyFramebuffer(r->device, sc->framebuffers[i], NULL);
                sc->framebuffers[i] = VK_NULL_HANDLE;
            }
        }
        free(sc->framebuffers);
        sc->framebuffers = NULL;
    }

    if (sc->image_views && r->fns.vkDestroyImageView) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->image_views[i] != VK_NULL_HANDLE) {
                r->fns.vkDestroyImageView(r->device, sc->image_views[i], NULL);
                sc->image_views[i] = VK_NULL_HANDLE;
            }
        }
        free(sc->image_views);
        sc->image_views = NULL;
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
    f->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceMemoryProperties");

    f->vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)ne_vk_get_instance(f, r->instance, "vkCreateWin32SurfaceKHR");
    f->vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)ne_vk_get_instance(f, r->instance, "vkDestroySurfaceKHR");

    f->vkGetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    f->vkGetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    f->vkGetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    f->vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)ne_vk_get_instance(f, r->instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    return f->vkDestroyInstance && f->vkEnumeratePhysicalDevices && f->vkGetPhysicalDeviceQueueFamilyProperties && f->vkCreateDevice &&
           f->vkCreateWin32SurfaceKHR && f->vkDestroySurfaceKHR && f->vkGetPhysicalDeviceSurfaceSupportKHR &&
           f->vkGetPhysicalDeviceSurfaceCapabilitiesKHR && f->vkGetPhysicalDeviceSurfaceFormatsKHR && f->vkGetPhysicalDeviceSurfacePresentModesKHR &&
           f->vkGetPhysicalDeviceMemoryProperties;
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

    /* Buffer management functions */
    f->vkCreateBuffer = (PFN_vkCreateBuffer)ne_vk_get_device(f, r->device, "vkCreateBuffer");
    f->vkDestroyBuffer = (PFN_vkDestroyBuffer)ne_vk_get_device(f, r->device, "vkDestroyBuffer");
    f->vkCreateImage = (PFN_vkCreateImage)ne_vk_get_device(f, r->device, "vkCreateImage");
    f->vkDestroyImage = (PFN_vkDestroyImage)ne_vk_get_device(f, r->device, "vkDestroyImage");
    f->vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)ne_vk_get_device(f, r->device, "vkGetBufferMemoryRequirements");
    f->vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)ne_vk_get_device(f, r->device, "vkGetImageMemoryRequirements");
    f->vkAllocateMemory = (PFN_vkAllocateMemory)ne_vk_get_device(f, r->device, "vkAllocateMemory");
    f->vkFreeMemory = (PFN_vkFreeMemory)ne_vk_get_device(f, r->device, "vkFreeMemory");
    f->vkBindBufferMemory = (PFN_vkBindBufferMemory)ne_vk_get_device(f, r->device, "vkBindBufferMemory");
    f->vkBindImageMemory = (PFN_vkBindImageMemory)ne_vk_get_device(f, r->device, "vkBindImageMemory");
    f->vkMapMemory = (PFN_vkMapMemory)ne_vk_get_device(f, r->device, "vkMapMemory");
    f->vkUnmapMemory = (PFN_vkUnmapMemory)ne_vk_get_device(f, r->device, "vkUnmapMemory");
    f->vkFlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)ne_vk_get_device(f, r->device, "vkFlushMappedMemoryRanges");
    f->vkCmdCopyBuffer = (PFN_vkCmdCopyBuffer)ne_vk_get_device(f, r->device, "vkCmdCopyBuffer");
    f->vkCmdCopyImage = (PFN_vkCmdCopyImage)ne_vk_get_device(f, r->device, "vkCmdCopyImage");
    f->vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)ne_vk_get_device(f, r->device, "vkCmdCopyBufferToImage");

    /* Shader management functions */
    f->vkCreateShaderModule = (PFN_vkCreateShaderModule)ne_vk_get_device(f, r->device, "vkCreateShaderModule");
    f->vkDestroyShaderModule = (PFN_vkDestroyShaderModule)ne_vk_get_device(f, r->device, "vkDestroyShaderModule");

    /* Render pass */
    f->vkCreateRenderPass = (PFN_vkCreateRenderPass)ne_vk_get_device(f, r->device, "vkCreateRenderPass");
    f->vkDestroyRenderPass = (PFN_vkDestroyRenderPass)ne_vk_get_device(f, r->device, "vkDestroyRenderPass");

    /* Image views */
    f->vkCreateImageView = (PFN_vkCreateImageView)ne_vk_get_device(f, r->device, "vkCreateImageView");
    f->vkDestroyImageView = (PFN_vkDestroyImageView)ne_vk_get_device(f, r->device, "vkDestroyImageView");

    /* Framebuffers */
    f->vkCreateFramebuffer = (PFN_vkCreateFramebuffer)ne_vk_get_device(f, r->device, "vkCreateFramebuffer");
    f->vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)ne_vk_get_device(f, r->device, "vkDestroyFramebuffer");

    /* Pipeline */
    f->vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)ne_vk_get_device(f, r->device, "vkCreatePipelineLayout");
    f->vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)ne_vk_get_device(f, r->device, "vkDestroyPipelineLayout");
    f->vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)ne_vk_get_device(f, r->device, "vkCreateGraphicsPipelines");
    f->vkDestroyPipeline = (PFN_vkDestroyPipeline)ne_vk_get_device(f, r->device, "vkDestroyPipeline");

    /* Render pass commands */
    f->vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)ne_vk_get_device(f, r->device, "vkCmdBeginRenderPass");
    f->vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)ne_vk_get_device(f, r->device, "vkCmdEndRenderPass");
    f->vkCmdBindPipeline = (PFN_vkCmdBindPipeline)ne_vk_get_device(f, r->device, "vkCmdBindPipeline");
    f->vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)ne_vk_get_device(f, r->device, "vkCmdBindVertexBuffers");
    f->vkCmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)ne_vk_get_device(f, r->device, "vkCmdBindIndexBuffer");
    f->vkCmdSetViewport = (PFN_vkCmdSetViewport)ne_vk_get_device(f, r->device, "vkCmdSetViewport");
    f->vkCmdSetScissor = (PFN_vkCmdSetScissor)ne_vk_get_device(f, r->device, "vkCmdSetScissor");
    f->vkCmdDraw = (PFN_vkCmdDraw)ne_vk_get_device(f, r->device, "vkCmdDraw");
    f->vkCmdDrawIndexed = (PFN_vkCmdDrawIndexed)ne_vk_get_device(f, r->device, "vkCmdDrawIndexed");
    f->vkCmdPushConstants = (PFN_vkCmdPushConstants)ne_vk_get_device(f, r->device, "vkCmdPushConstants");

    return f->vkDestroyDevice && f->vkGetDeviceQueue && f->vkCreateSwapchainKHR && f->vkGetSwapchainImagesKHR &&
           f->vkAcquireNextImageKHR && f->vkQueueSubmit && f->vkQueuePresentKHR && f->vkCreateSemaphore && f->vkCreateFence &&
           f->vkWaitForFences && f->vkResetFences && f->vkCreateCommandPool && f->vkResetCommandBuffer && f->vkAllocateCommandBuffers &&
           f->vkBeginCommandBuffer && f->vkEndCommandBuffer && f->vkCmdPipelineBarrier && f->vkCmdClearColorImage &&
           f->vkCreateBuffer && f->vkDestroyBuffer && f->vkGetBufferMemoryRequirements && f->vkAllocateMemory && f->vkFreeMemory && f->vkBindBufferMemory &&
           f->vkMapMemory && f->vkUnmapMemory && f->vkFlushMappedMemoryRanges && f->vkCmdCopyBuffer &&
           f->vkCreateShaderModule && f->vkDestroyShaderModule &&
           f->vkCreateRenderPass && f->vkDestroyRenderPass &&
           f->vkCreateImageView && f->vkDestroyImageView &&
           f->vkCreateFramebuffer && f->vkDestroyFramebuffer &&
           f->vkCreatePipelineLayout && f->vkDestroyPipelineLayout &&
           f->vkCreateGraphicsPipelines && f->vkDestroyPipeline &&
           f->vkCmdBeginRenderPass && f->vkCmdEndRenderPass &&
           f->vkCmdBindPipeline && f->vkCmdBindVertexBuffers && f->vkCmdBindIndexBuffer &&
           f->vkCmdSetViewport && f->vkCmdSetScissor &&
           f->vkCmdDraw && f->vkCmdDrawIndexed && f->vkCmdPushConstants;
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

    /* Create a single transfer command pool for buffer staging operations.
     * Uses TRANSIENT_BIT for optimization and RESET_COMMAND_BUFFER_BIT to allow reuse.
     */
    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = r->queue_family_index;

    vr = r->fns.vkCreateCommandPool(r->device, &pool_info, NULL, &r->transfer_cmd_pool);
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

    vr = r->fns.vkAllocateCommandBuffers(r->device, &alloc_info, &r->transfer_cmd);
    if (vr != VK_SUCCESS || r->transfer_cmd == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateCommandBuffers (transfer) failed (vr=%d)", (int)vr);
        r->fns.vkDestroyCommandPool(r->device, r->transfer_cmd_pool, NULL);
        r->transfer_cmd_pool = VK_NULL_HANDLE;
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
    /*
     * FIFO is the only present mode the spec guarantees is supported, and it is
     * vsync-paced (presents are released on vblank). It is therefore both the
     * vsync choice and the universal fallback.
     *
     * When vsync is disabled we prefer MAILBOX (low-latency, no tearing) then
     * IMMEDIATE (uncapped, may tear) — but only if the driver actually offers
     * them; otherwise we fall back to FIFO.
     */
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

    return VK_PRESENT_MODE_FIFO_KHR;
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

/* ── Buffer helpers ────────────────────────────────────────────────────── */

/**
 * Find a suitable memory type index for the given requirements.
 * Returns UINT32_MAX if no suitable type found.
 */
static uint32_t ne_vk_find_memory_type(NERenderer *r, uint32_t type_filter,
                                        VkMemoryPropertyFlags properties) {
    if (!r || !r->fns.vkGetPhysicalDeviceMemoryProperties) {
        return UINT32_MAX;
    }

    VkPhysicalDeviceMemoryProperties mem_props;
    r->fns.vkGetPhysicalDeviceMemoryProperties(r->phys, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

/* Free all Vulkan resources a slot owns. Handles the three slot shapes that
 * share this pool/union: images, dynamic buffers (N mapped copies), and static
 * buffers. Does not touch occupied/usage/size bookkeeping. */
static void ne_vk_buffer_slot_free(NERenderer *r, NEVulkanBufferSlot *slot) {
    /* Image slots alias `buffer` with `image` in the union; free them as images
     * (this also releases the imageView, which the buffer path would leak). */
    if (slot->usage & (NE_BUFFER_USAGE_IMAGE_STORAGE | NE_BUFFER_USAGE_IMAGE_SAMPLED)) {
        if (slot->image != VK_NULL_HANDLE && r->fns.vkDestroyImage) {
            r->fns.vkDestroyImage(r->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
        }
        if (slot->imageView != VK_NULL_HANDLE && r->fns.vkDestroyImageView) {
            r->fns.vkDestroyImageView(r->device, slot->imageView, NULL);
            slot->imageView = VK_NULL_HANDLE;
        }
        if (slot->memory != VK_NULL_HANDLE && r->fns.vkFreeMemory) {
            r->fns.vkFreeMemory(r->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
        }
        return;
    }

    if (slot->dynamic) {
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (slot->dyn_mapped[i]) {
                r->fns.vkUnmapMemory(r->device, slot->dyn_memories[i]);
                slot->dyn_mapped[i] = NULL;
            }
            if (slot->dyn_buffers[i] != VK_NULL_HANDLE && r->fns.vkDestroyBuffer) {
                r->fns.vkDestroyBuffer(r->device, slot->dyn_buffers[i], NULL);
                slot->dyn_buffers[i] = VK_NULL_HANDLE;
            }
            if (slot->dyn_memories[i] != VK_NULL_HANDLE && r->fns.vkFreeMemory) {
                r->fns.vkFreeMemory(r->device, slot->dyn_memories[i], NULL);
                slot->dyn_memories[i] = VK_NULL_HANDLE;
            }
        }
        return;
    }

    if (slot->buffer != VK_NULL_HANDLE && r->fns.vkDestroyBuffer) {
        r->fns.vkDestroyBuffer(r->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
    }
    if (slot->memory != VK_NULL_HANDLE && r->fns.vkFreeMemory) {
        r->fns.vkFreeMemory(r->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
    }
}

/**
 * Ensure the staging buffer exists with at least the requested size.
 * Creates or resizes the staging buffer as needed.
 * Returns true on success, false on failure.
 *
 * The staging buffer is host-visible and coherent, suitable for CPU->GPU transfers.
 */
static bool ne_vk_ensure_staging_buffer(NERenderer *r, uint32_t required_size) {
    if (!r) {
        return false;
    }

    /* If we already have a staging buffer with sufficient capacity, reuse it */
    if (r->staging_buffer != VK_NULL_HANDLE && r->staging_size >= required_size) {
        return true;
    }

    /* Clean up old staging buffer if it exists */
    if (r->staging_mapped) {
        r->fns.vkUnmapMemory(r->device, r->staging_memory);
        r->staging_mapped = NULL;
    }
    if (r->staging_buffer != VK_NULL_HANDLE && r->fns.vkDestroyBuffer) {
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
    }
    if (r->staging_memory != VK_NULL_HANDLE && r->fns.vkFreeMemory) {
        r->fns.vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
    }
    r->staging_size = 0;

    /* Create new staging buffer with some extra padding for future reuse */
    uint32_t alloc_size = required_size + (required_size / 4); /* 25% padding */

    VkBufferCreateInfo buf_info;
    memset(&buf_info, 0, sizeof(buf_info));
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = alloc_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult vr = r->fns.vkCreateBuffer(r->device, &buf_info, NULL, &r->staging_buffer);
    if (vr != VK_SUCCESS || r->staging_buffer == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateBuffer (staging) failed (vr=%d)", (int)vr);
        return false;
    }

    /* Get memory requirements for the staging buffer */
    VkMemoryRequirements mem_req;
    r->fns.vkGetBufferMemoryRequirements(r->device, r->staging_buffer, &mem_req);

    /* Find a suitable memory type: host-visible and coherent for CPU mapping */
    uint32_t mem_type = ne_vk_find_memory_type(
        r,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find suitable memory type for staging buffer");
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Allocate memory for the staging buffer */
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vr = r->fns.vkAllocateMemory(r->device, &alloc_info, NULL, &r->staging_memory);
    if (vr != VK_SUCCESS || r->staging_memory == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateMemory (staging) failed (vr=%d)", (int)vr);
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Bind the memory to the buffer */
    vr = r->fns.vkBindBufferMemory(r->device, r->staging_buffer, r->staging_memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory (staging) failed (vr=%d)", (int)vr);
        r->fns.vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Map the staging memory for CPU access (use actual allocated size) */
    void *mapped_ptr = NULL;
    vr = r->fns.vkMapMemory(r->device, r->staging_memory, 0, mem_req.size, 0, &mapped_ptr);
    if (vr != VK_SUCCESS || !mapped_ptr) {
        NE_LOG_ERROR("vkMapMemory (staging) failed (vr=%d)", (int)vr);
        r->fns.vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    r->staging_mapped = mapped_ptr;
    r->staging_size = mem_req.size;

    return true;
}

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
        r->fns.vkDestroyRenderPass(r->device, surface->render_pass, NULL);
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

    VkResult vr = r->fns.vkCreateRenderPass(r->device, &rpci, NULL, &surface->render_pass);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkCreateRenderPass failed (vr=%d)", (int)vr);
        return false;
    }

    return true;
}

static bool ne_vk_swapchain_create(NERenderSurface *surface, bool vsync) {
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

    ne_vk_swapchain_cleanup(r, &surface->sc);

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
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
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

    /* ── Render pass (created once per surface, or recreated on format change) ── */

    if (surface->render_pass == VK_NULL_HANDLE) {
        if (!ne_vk_create_render_pass(surface, surface->sc.format)) {
            return false;
        }
    }

    /* ── Image views ──────────────────────────────────────────────────────── */

    surface->sc.image_views = (VkImageView *)calloc(img_count, sizeof(VkImageView));
    if (!surface->sc.image_views) {
        return false;
    }

    for (uint32_t i = 0; i < img_count; i++) {
        VkImageViewCreateInfo ivci;
        memset(&ivci, 0, sizeof(ivci));
        ivci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image    = surface->sc.images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format   = surface->sc.format;
        ivci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel   = 0;
        ivci.subresourceRange.levelCount     = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount     = 1;

        vr = r->fns.vkCreateImageView(r->device, &ivci, NULL, &surface->sc.image_views[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateImageView[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
    }

    /* ── Framebuffers ─────────────────────────────────────────────────────── */

    surface->sc.framebuffers = (VkFramebuffer *)calloc(img_count, sizeof(VkFramebuffer));
    if (!surface->sc.framebuffers) {
        return false;
    }

    for (uint32_t i = 0; i < img_count; i++) {
        VkFramebufferCreateInfo fbci;
        memset(&fbci, 0, sizeof(fbci));
        fbci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass      = surface->render_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments    = &surface->sc.image_views[i];
        fbci.width           = surface->sc.extent.width;
        fbci.height          = surface->sc.extent.height;
        fbci.layers          = 1;

        vr = r->fns.vkCreateFramebuffer(r->device, &fbci, NULL, &surface->sc.framebuffers[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateFramebuffer[%u] failed (vr=%d)", (unsigned)i, (int)vr);
            return false;
        }
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

    glslang_initialize_process();

    g_renderer_singleton = r;
    return r;
}

void ne_renderer_destroy(NERenderer *r) {
    if (!r) {
        return;
    }

    /* Drain the GPU before tearing down any resources. */
    if (r->device != VK_NULL_HANDLE && r->fns.vkDeviceWaitIdle) {
        (void)r->fns.vkDeviceWaitIdle(r->device);
    }

    /* Destroy surfaces first (this cleans up swapchains). */
    struct NERenderSurface *s = r->surfaces;
    while (s) {
        struct NERenderSurface *next = s->next;

        ne_vk_swapchain_cleanup(r, &s->sc);

        if (s->render_pass != VK_NULL_HANDLE && r->fns.vkDestroyRenderPass) {
            r->fns.vkDestroyRenderPass(r->device, s->render_pass, NULL);
            s->render_pass = VK_NULL_HANDLE;
        }

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

    /* Destroy all live buffers. */
    for (uint32_t i = 0; i < r->buffer_cap; i++) {
        if (r->buffers[i].occupied) {
            ne_vk_buffer_slot_free(r, &r->buffers[i]);
        }
    }
    free(r->buffers);
    r->buffers = NULL;
    r->buffer_count = 0;
    r->buffer_cap = 0;

    /* Destroy all live shaders. */
    for (uint32_t i = 0; i < r->shader_cap; i++) {
        if (r->shaders[i].occupied) {
            if (r->shaders[i].module != VK_NULL_HANDLE && r->fns.vkDestroyShaderModule) {
                r->fns.vkDestroyShaderModule(r->device, r->shaders[i].module, NULL);
            }
            free(r->shaders[i].entry_point);
        }
    }
    free(r->shaders);
    r->shaders = NULL;
    r->shader_count = 0;
    r->shader_cap = 0;

    /* Destroy all live pipelines. */
    for (uint32_t i = 0; i < r->pipeline_cap; i++) {
        if (r->pipelines[i].occupied) {
            if (r->pipelines[i].pipeline != VK_NULL_HANDLE && r->fns.vkDestroyPipeline) {
                r->fns.vkDestroyPipeline(r->device, r->pipelines[i].pipeline, NULL);
            }
            if (r->pipelines[i].layout != VK_NULL_HANDLE && r->fns.vkDestroyPipelineLayout) {
                r->fns.vkDestroyPipelineLayout(r->device, r->pipelines[i].layout, NULL);
            }
            free(r->pipelines[i].vert_entry);
            free(r->pipelines[i].frag_entry);
            free(r->pipelines[i].bindings);
            free(r->pipelines[i].attributes);
        }
    }
    free(r->pipelines);
    r->pipelines = NULL;
    r->pipeline_count = 0;
    r->pipeline_cap = 0;

    /* Destroy staging buffer and transfer command pool. */
    if (r->staging_buffer != VK_NULL_HANDLE && r->fns.vkDestroyBuffer) {
        r->fns.vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
    }
    if (r->staging_memory != VK_NULL_HANDLE && r->fns.vkFreeMemory) {
        r->fns.vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
    }
    r->staging_mapped = NULL;
    r->staging_size = 0;

    if (r->transfer_cmd_pool != VK_NULL_HANDLE && r->fns.vkDestroyCommandPool) {
        r->fns.vkDestroyCommandPool(r->device, r->transfer_cmd_pool, NULL);
        r->transfer_cmd_pool = VK_NULL_HANDLE;
    }
    r->transfer_cmd = VK_NULL_HANDLE;

    if (r->device != VK_NULL_HANDLE && r->fns.vkDestroyDevice) {
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
        glslang_finalize_process();
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

    ne_vk_swapchain_cleanup(r, &surface->sc);

    if (surface->render_pass != VK_NULL_HANDLE) {
        r->fns.vkDestroyRenderPass(r->device, surface->render_pass, NULL);
        surface->render_pass = VK_NULL_HANDLE;
    }

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
        if (!ne_vk_swapchain_create(surface, vsync)) {
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
    bi.flags = 0;

    vr = r->fns.vkBeginCommandBuffer(cmd, &bi);
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
    rpbi.framebuffer       = surface->sc.framebuffers[image_index];
    rpbi.renderArea.offset = (VkOffset2D){0, 0};
    rpbi.renderArea.extent = surface->sc.extent;
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear_value;

    r->fns.vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    /* Set default viewport and scissor to match the surface extent. */
    VkViewport viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.x        = 0.0f;
    viewport.y        = (float)surface->sc.extent.height;
    viewport.width    = (float)surface->sc.extent.width;
    viewport.height   = -(float)surface->sc.extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    r->fns.vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor;
    memset(&scissor, 0, sizeof(scissor));
    scissor.offset = (VkOffset2D){0, 0};
    scissor.extent = surface->sc.extent;

    r->fns.vkCmdSetScissor(cmd, 0, 1, &scissor);

    surface->sc.image_layouts[image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    g_active_pass.surface      = surface;
    g_active_pass.cmd          = cmd;
    g_active_pass.bound_layout = VK_NULL_HANDLE;
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


    /* ── Close the render pass and command buffer ────────────────────── */

    VkCommandBuffer cmd = pass->cmd;
    r->fns.vkCmdEndRenderPass(cmd);

    VkResult vr = r->fns.vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkEndCommandBuffer failed (vr=%d)", (int)vr);
        pass->surface      = NULL;
        pass->cmd          = VK_NULL_HANDLE;
        pass->bound_layout = VK_NULL_HANDLE;
        surface->wants_swapchain_recreate = true;
        return;
    }

    /* ── Submit and present ──────────────────────────────────────────── */

    const uint32_t image_index = surface->sc.acquired_image_index;
    const uint32_t frame_index = surface->sc.frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &surface->sc.sem_image_available[frame_index];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &surface->sc.cmds[frame_index];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &surface->sc.sem_render_finished[image_index];

    vr = r->fns.vkQueueSubmit(r->queue, 1, &si, surface->sc.fences_in_flight[frame_index]);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueueSubmit failed (vr=%d)", (int)vr);
        surface->wants_swapchain_recreate = true;
        return;
    }

    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &surface->sc.sem_render_finished[image_index];
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

    pass->surface      = NULL;
    pass->cmd          = VK_NULL_HANDLE;
    pass->bound_layout = VK_NULL_HANDLE;
}


/* ========================================================================
 * GPU Buffer Management
 * ======================================================================== */

/**
 * BUFFER ARCHITECTURE & DESCRIPTOR SET INTEGRATION
 *
 * Current Implementation:
 * - Device-local GPU buffers for all resource types (vertex, index, uniform, storage)
 * - Lazy staging buffer allocation on first data upload
 * - Single reusable transfer command pool/buffer for all CPU->GPU transfers
 * - Memory mapping via staging transfers (no direct host mapping)
 *
 * Descriptor Set Integration (Future):
 * - Uniform buffers (UBO) will use descriptor set binding (currently use push constants)
 * - Storage buffers (SSBO) will enable compute task operations
 * - Flexible binding via descriptor sets instead of per-buffer mechanisms
 * - Descriptor pool manages allocation; layout cache avoids redundant layouts
 * 
 * Current Buffer Usage Mapping:
 * - NE_BUFFER_USAGE_VERTEX   → VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
 * - NE_BUFFER_USAGE_INDEX    → VK_BUFFER_USAGE_INDEX_BUFFER_BIT
 * - NE_BUFFER_USAGE_UNIFORM  → VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT (future: descriptor binding)
 * - NE_BUFFER_USAGE_STORAGE  → VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (future: compute binding)
 *
 * All buffers also get VK_BUFFER_USAGE_TRANSFER_DST_BIT for staging updates.
 */

static bool ne_vk_submit_transfer_cmd(NERenderer *r, VkCommandBuffer cmd) {
    if (!r || !r->transfer_cmd || cmd == VK_NULL_HANDLE) {
        return false;
    }

    VkResult vr = r->fns.vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkEndCommandBuffer (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    /* Create a fence to wait for the transfer to complete */
    VkFenceCreateInfo fence_info;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    vr = r->fns.vkCreateFence(r->device, &fence_info, NULL, &fence);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkCreateFence (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    /* Submit the transfer command buffer */
    VkSubmitInfo submit_info;
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vr = r->fns.vkQueueSubmit(r->queue, 1, &submit_info, fence);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueueSubmit (transfer) failed (vr=%d)", (int)vr);
        r->fns.vkDestroyFence(r->device, fence, NULL);
        return false;
    }

    /* Wait for the transfer to complete */
    vr = r->fns.vkWaitForFences(r->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkWaitForFences (transfer) failed (vr=%d)", (int)vr);
        r->fns.vkDestroyFence(r->device, fence, NULL);
        return false;
    }

    r->fns.vkDestroyFence(r->device, fence, NULL);
    return true;
}

NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc) {
    if (!renderer || !desc || desc->size == 0) {
        return NE_BUFFER_HANDLE_NULL;
    }

    if (!desc->usage) {
        NE_LOG_ERROR("buffer creation requires at least one usage flag");
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Allocate a slot from the buffer pool */
    uint32_t slot_index = ne_pool_alloc(
        (void **)&renderer->buffers,
        &renderer->buffer_count,
        &renderer->buffer_cap,
        sizeof(NEVulkanBufferSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("failed to allocate buffer slot from pool");
        return NE_BUFFER_HANDLE_NULL;
    }

    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    /* Convert NEBufferUsage flags to VkBufferUsageFlags */
    VkBufferUsageFlags vk_usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; /* All buffers can receive transfers */

    if (desc->usage & NE_BUFFER_USAGE_VERTEX) {
        vk_usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (desc->usage & NE_BUFFER_USAGE_INDEX) {
        vk_usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (desc->usage & NE_BUFFER_USAGE_UNIFORM) {
        vk_usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (desc->usage & NE_BUFFER_USAGE_STORAGE) {
        vk_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }

    /*
     * Dynamic path: one host-visible + coherent, persistently-mapped buffer per
     * in-flight frame. The CPU writes mapped[frame] directly each frame (no
     * staging, no transfer, no stall) while the GPU reads another frame's copy.
     * This mirrors the Metal backend's per-frame dynamic buffering and is the
     * idiomatic equivalent of Metal's StorageModeShared for dynamic data.
     */
    if (desc->dynamic) {
        slot->dynamic = true;
        slot->usage = desc->usage;
        slot->size = desc->size;

        /* Host-visible dynamic buffers are written directly, not transfer
         * targets; drop TRANSFER_DST (it was added unconditionally above). */
        const VkBufferUsageFlags dyn_usage = vk_usage & ~(VkBufferUsageFlags)VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo dbi;
            memset(&dbi, 0, sizeof(dbi));
            dbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            dbi.size = desc->size;
            dbi.usage = dyn_usage;
            dbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkResult dvr = renderer->fns.vkCreateBuffer(renderer->device, &dbi, NULL, &slot->dyn_buffers[i]);
            if (dvr != VK_SUCCESS || slot->dyn_buffers[i] == VK_NULL_HANDLE) {
                NE_LOG_ERROR("vkCreateBuffer (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            VkMemoryRequirements dmr;
            renderer->fns.vkGetBufferMemoryRequirements(renderer->device, slot->dyn_buffers[i], &dmr);

            uint32_t dmt = ne_vk_find_memory_type(
                renderer, dmr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (dmt == UINT32_MAX) {
                NE_LOG_ERROR("no host-visible memory type for dynamic buffer");
                goto dynamic_fail;
            }

            VkMemoryAllocateInfo dai;
            memset(&dai, 0, sizeof(dai));
            dai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            dai.allocationSize = dmr.size;
            dai.memoryTypeIndex = dmt;

            dvr = renderer->fns.vkAllocateMemory(renderer->device, &dai, NULL, &slot->dyn_memories[i]);
            if (dvr != VK_SUCCESS || slot->dyn_memories[i] == VK_NULL_HANDLE) {
                NE_LOG_ERROR("vkAllocateMemory (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            dvr = renderer->fns.vkBindBufferMemory(renderer->device, slot->dyn_buffers[i], slot->dyn_memories[i], 0);
            if (dvr != VK_SUCCESS) {
                NE_LOG_ERROR("vkBindBufferMemory (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            dvr = renderer->fns.vkMapMemory(renderer->device, slot->dyn_memories[i], 0, desc->size, 0, &slot->dyn_mapped[i]);
            if (dvr != VK_SUCCESS || !slot->dyn_mapped[i]) {
                NE_LOG_ERROR("vkMapMemory (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            /* Seed every copy with initial data so the buffer is valid before
             * the first per-frame update. */
            if (desc->initial_data) {
                memcpy(slot->dyn_mapped[i], desc->initial_data, desc->size);
            }
        }

        slot->occupied = true;
        return (NEBufferHandle){ slot_index + 1 };

    dynamic_fail:
        /* Release any dynamic resources created before the failure. */
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (slot->dyn_mapped[i]) {
                renderer->fns.vkUnmapMemory(renderer->device, slot->dyn_memories[i]);
                slot->dyn_mapped[i] = NULL;
            }
            if (slot->dyn_memories[i] != VK_NULL_HANDLE) {
                renderer->fns.vkFreeMemory(renderer->device, slot->dyn_memories[i], NULL);
                slot->dyn_memories[i] = VK_NULL_HANDLE;
            }
            if (slot->dyn_buffers[i] != VK_NULL_HANDLE) {
                renderer->fns.vkDestroyBuffer(renderer->device, slot->dyn_buffers[i], NULL);
                slot->dyn_buffers[i] = VK_NULL_HANDLE;
            }
        }
        slot->occupied = false;
        slot->dynamic = false;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Create the GPU buffer */
    VkBufferCreateInfo buf_info;
    memset(&buf_info, 0, sizeof(buf_info));
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = desc->size;
    buf_info.usage = vk_usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult vr = renderer->fns.vkCreateBuffer(renderer->device, &buf_info, NULL, &slot->buffer);
    if (vr != VK_SUCCESS || slot->buffer == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateBuffer failed (vr=%d, size=%u)", (int)vr, desc->size);
        slot->occupied = false;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Get memory requirements for the buffer */
    VkMemoryRequirements mem_req;
    renderer->fns.vkGetBufferMemoryRequirements(renderer->device, slot->buffer, &mem_req);

    /* Find device-local memory for the GPU buffer */
    uint32_t mem_type = ne_vk_find_memory_type(
        renderer,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find device-local memory type for buffer");
        renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Allocate memory for the buffer */
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vr = renderer->fns.vkAllocateMemory(renderer->device, &alloc_info, NULL, &slot->memory);
    if (vr != VK_SUCCESS || slot->memory == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateMemory failed (vr=%d)", (int)vr);
        renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Bind memory to buffer */
    vr = renderer->fns.vkBindBufferMemory(renderer->device, slot->buffer, slot->memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory failed (vr=%d)", (int)vr);
        renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
        renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* If initial data is provided, stage it to the buffer */
    if (desc->initial_data) {
        if (!ne_vk_ensure_staging_buffer(renderer, desc->size)) {
            NE_LOG_ERROR("failed to ensure staging buffer for initial data");
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Copy initial data into the staging buffer */
        memcpy(renderer->staging_mapped, desc->initial_data, desc->size);

        /* Flush the entire staged memory (VK_WHOLE_SIZE avoids alignment issues) */
        VkMappedMemoryRange flush_range;
        memset(&flush_range, 0, sizeof(flush_range));
        flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flush_range.memory = renderer->staging_memory;
        flush_range.offset = 0;
        flush_range.size = VK_WHOLE_SIZE;

        vr = renderer->fns.vkFlushMappedMemoryRanges(renderer->device, 1, &flush_range);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkFlushMappedMemoryRanges failed (vr=%d), continuing anyway", (int)vr);
        }

        /* Record and submit transfer command */
        VkCommandBufferBeginInfo begin_info;
        memset(&begin_info, 0, sizeof(begin_info));
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vr = renderer->fns.vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkBeginCommandBuffer (transfer) failed (vr=%d)", (int)vr);
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Record the copy command */
        VkBufferCopy copy_region;
        memset(&copy_region, 0, sizeof(copy_region));
        copy_region.srcOffset = 0;
        copy_region.dstOffset = 0;
        copy_region.size = desc->size;

        renderer->fns.vkCmdCopyBuffer(renderer->transfer_cmd, renderer->staging_buffer, slot->buffer, 1, &copy_region);

        /* Submit and wait for completion */
        if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
            NE_LOG_ERROR("failed to submit transfer command for initial data");
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Reset the command buffer for reuse */
        vr = renderer->fns.vkResetCommandBuffer(renderer->transfer_cmd, 0);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
        }
    }

    /* Mark the slot as occupied and store metadata */
    slot->occupied = true;
    slot->usage = desc->usage;
    slot->size = desc->size;

    /* Return handle (1-based ID for null safety) */
    NEBufferHandle handle = {slot_index + 1};
    return handle;
}

static void ne_cmd_transition_image_layout(const NERenderer *renderer, const VkCommandBuffer cmd, const NEImageHandle handle, const VkImageLayout oldLayout, const VkImageLayout newLayout) {
    if (!ne_image_handle_valid(handle)) {
        NE_LOG_ERROR("ne_cmd_transition_image_layout: INVALID IMAGE HANDLE/n");
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */
    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    VkImageMemoryBarrier barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = slot->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = 0;
    VkPipelineStageFlags destinationStage = 0;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        NE_LOG_ERROR("UNSUPPORTED LAYOUT TRANSITION\n");
        return;
    }

    renderer->fns.vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

static void ne_cmd_copy_buffer_to_image(const NERenderer *renderer, const VkCommandBuffer cmd, const VkBuffer buffer, const NEImageHandle handle) {
    if (!ne_image_handle_valid(handle)) {
        NE_LOG_ERROR("ne_cmd_copy_buffer_to_image: INVALID IMAGE HANDLE/n");
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */
    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    // copy the staging buffer to the GPU image buffer
    VkBufferImageCopy copy_region;
    memset(&copy_region, 0, sizeof(copy_region));
    copy_region.bufferOffset = 0;
    copy_region.bufferRowLength = 0;
    copy_region.bufferImageHeight = 0;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = 0;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageOffset = (VkOffset3D){0, 0, 0};
    copy_region.imageExtent = (VkExtent3D){slot->width, slot->height, 1};

    renderer->fns.vkCmdCopyBufferToImage(cmd, buffer, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
}

NEImageHandle ne_image_create(NERenderer *renderer, const NEImageDesc *desc) {
    if (!renderer || !desc || desc->width == 0 || desc->height == 0) {
        return NE_IMAGE_HANDLE_NULL;
    }

    if (!desc->usage) {
        NE_LOG_ERROR("buffer creation requires at least one usage flag");
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Allocate a slot from the buffer pool */
    uint32_t slot_index = ne_pool_alloc(
        (void **)&renderer->buffers,
        &renderer->buffer_count,
        &renderer->buffer_cap,
        sizeof(NEVulkanBufferSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("failed to allocate buffer slot from pool");
        return NE_IMAGE_HANDLE_NULL;
    }

    NEImageHandle handle = (NEImageHandle)(slot_index + 1);
    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    /* Convert NEBufferUsage flags to VkBufferUsageFlags */
    VkBufferUsageFlags vk_usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; /* All buffers can receive transfers */
    if (desc->usage & NE_BUFFER_USAGE_IMAGE_STORAGE) {
        vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (desc->usage & NE_BUFFER_USAGE_IMAGE_SAMPLED) {
        vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    VkFormat vk_format = VK_FORMAT_R8G8B8A8_SRGB; // default
    if (desc->format == NE_IMAGE_FORMAT_GRAY) {
        vk_format = VK_FORMAT_R8_SRGB;
    } else if (desc->format == NE_IMAGE_FORMAT_RGB) {
        vk_format = VK_FORMAT_R8G8B8_SRGB;
    } else if (desc->format == NE_IMAGE_FORMAT_RGBA) {
        vk_format = VK_FORMAT_R8G8B8A8_SRGB;
    }

    const uint32_t size = desc->width * desc->height * desc->format;

    /* Create the GPU buffer */
    VkImageCreateInfo img_info;
    memset(&img_info, 0, sizeof(img_info));
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.extent.width = desc->width;
    img_info.extent.height = desc->height;
    img_info.extent.depth = 1;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.format = vk_format;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.usage = vk_usage;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;

    VkResult vr = renderer->fns.vkCreateImage(renderer->device, &img_info, NULL, &slot->image);
    if (vr != VK_SUCCESS || slot->image == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateImage failed (vr=%d, size=%u)", (int)vr, size);
        slot->occupied = false;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Get memory requirements for the buffer */
    VkMemoryRequirements mem_req;
    renderer->fns.vkGetImageMemoryRequirements(renderer->device, slot->image, &mem_req);

    /* Find device-local memory for the GPU buffer */
    uint32_t mem_type = ne_vk_find_memory_type(
        renderer,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find device-local memory type for buffer");
        renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Allocate memory for the buffer */
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vr = renderer->fns.vkAllocateMemory(renderer->device, &alloc_info, NULL, &slot->memory);
    if (vr != VK_SUCCESS || slot->memory == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateMemory failed (vr=%d)", (int)vr);
        renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Bind memory to buffer */
    vr = renderer->fns.vkBindImageMemory(renderer->device, slot->image, slot->memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory failed (vr=%d)", (int)vr);
        renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
        renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* If initial data is provided, stage it to the buffer */
    if (desc->initial_data) {
        if (!ne_vk_ensure_staging_buffer(renderer, size)) {
            NE_LOG_ERROR("failed to ensure staging buffer for initial data");
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_IMAGE_HANDLE_NULL;
        }

        /* Copy initial data into the staging buffer */
        memcpy(renderer->staging_mapped, desc->initial_data, size);

        /* Flush the entire staged memory (VK_WHOLE_SIZE avoids alignment issues) */
        VkMappedMemoryRange flush_range;
        memset(&flush_range, 0, sizeof(flush_range));
        flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flush_range.memory = renderer->staging_memory;
        flush_range.offset = 0;
        flush_range.size = VK_WHOLE_SIZE;

        vr = renderer->fns.vkFlushMappedMemoryRanges(renderer->device, 1, &flush_range);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkFlushMappedMemoryRanges failed (vr=%d), continuing anyway", (int)vr);
        }

        /* Record and submit transfer command */
        VkCommandBufferBeginInfo begin_info;
        memset(&begin_info, 0, sizeof(begin_info));
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vr = renderer->fns.vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkBeginCommandBuffer (transfer) failed (vr=%d)", (int)vr);
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
            slot->image= VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_IMAGE_HANDLE_NULL;
        }

        ne_cmd_transition_image_layout(renderer, renderer->transfer_cmd, handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ne_cmd_copy_buffer_to_image(renderer, renderer->transfer_cmd, renderer->staging_buffer, handle);

        /* Submit and wait for completion */
        if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
            NE_LOG_ERROR("failed to submit transfer command for initial data");
            renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            renderer->fns.vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            return NE_IMAGE_HANDLE_NULL;
        }

        /* Reset the command buffer for reuse */
        vr = renderer->fns.vkResetCommandBuffer(renderer->transfer_cmd, 0);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
        }
    }

    // create image view
    {
        VkImageViewCreateInfo viewInfo;
        memset(&viewInfo, 0, sizeof(viewInfo));
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = slot->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vk_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vr = renderer->fns.vkCreateImageView(renderer->device, &viewInfo, nullptr, &slot->imageView);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkCreateImageView failed (vr=%d), continuing anyway", (int)vr);
        }
    }

    /* Mark the slot as occupied and store metadata */
    slot->occupied = true;
    slot->usage = desc->usage;
    slot->width = desc->width;
    slot->height = desc->height;
    slot->format = desc->format;

    /* Return handle (1-based ID for null safety) */
    return handle;
}

void ne_image_update(NERenderer *renderer, NEImageHandle handle, const void *data, uint32_t size) {
    if (!renderer || !ne_image_handle_valid(handle) || !data || size == 0) {
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffer_cap || !renderer->buffers[slot_index].occupied) {
        NE_LOG_WARN("buffer_update called on invalid or destroyed buffer handle");
        return;
    }

    /* Ensure we have a staging buffer for the update */
    if (!ne_vk_ensure_staging_buffer(renderer, size)) {
        NE_LOG_ERROR("failed to ensure staging buffer for buffer update");
        return;
    }

    /* Copy the updated data into the staging buffer */
    memcpy((uint8_t *)renderer->staging_mapped, data, size);

    /* Flush the entire staged memory (VK_WHOLE_SIZE avoids alignment issues) */
    VkMappedMemoryRange flush_range;
    memset(&flush_range, 0, sizeof(flush_range));
    flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    flush_range.memory = renderer->staging_memory;
    flush_range.offset = 0;
    flush_range.size = VK_WHOLE_SIZE;

    VkResult vr = renderer->fns.vkFlushMappedMemoryRanges(renderer->device, 1, &flush_range);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkFlushMappedMemoryRanges failed (vr=%d), continuing anyway", (int)vr);
    }

    /* Record the copy command */
    VkCommandBufferBeginInfo begin_info;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vr = renderer->fns.vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBeginCommandBuffer (transfer) failed (vr=%d)", (int)vr);
        return;
    }

    ne_cmd_transition_image_layout(renderer, renderer->transfer_cmd, handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ne_cmd_copy_buffer_to_image(renderer, renderer->transfer_cmd, renderer->staging_buffer, handle);

    /* Submit and wait for completion */
    if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
        NE_LOG_ERROR("failed to submit transfer command for buffer update");
        return;
    }

    /* Reset the command buffer for reuse */
    vr = renderer->fns.vkResetCommandBuffer(renderer->transfer_cmd, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
    }
}

void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle,
                      const void *data, uint32_t size, uint32_t offset) {
    if (!renderer || !ne_buffer_handle_valid(handle) || !data || size == 0) {
        return;
    }

    uint32_t slot_index = handle.id - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffer_cap || !renderer->buffers[slot_index].occupied) {
        NE_LOG_WARN("buffer_update called on invalid or destroyed buffer handle");
        return;
    }

    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    /* Overflow-safe bounds check: `offset + size` could wrap around uint32_t. */
    if (size > slot->size || offset > slot->size - size) {
        NE_LOG_ERROR("buffer update out of bounds (offset=%u + size=%u > buffer_size=%u)",
                     offset, size, slot->size);
        return;
    }

    /*
     * Renderer-scoped update: intended for static/one-time setup (no frame
     * context). For a dynamic buffer we have no surface frame_index here, so we
     * write ALL mapped copies to keep them consistent. This is only safe
     * outside an in-flight frame. Per-frame dynamic updates must use
     * ne_render_pass_update_buffer instead.
     */
    if (slot->dynamic) {
        NE_LOG_WARN("ne_buffer_update on a dynamic buffer writes all copies; "
                    "use ne_render_pass_update_buffer for per-frame updates");
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (slot->dyn_mapped[i]) {
                memcpy((uint8_t *)slot->dyn_mapped[i] + offset, data, size);
            }
        }
        return;
    }

    /* Ensure we have a staging buffer for the update */
    if (!ne_vk_ensure_staging_buffer(renderer, offset + size)) {
        NE_LOG_ERROR("failed to ensure staging buffer for buffer update");
        return;
    }

    /* Copy the updated data into the staging buffer */
    memcpy((uint8_t *)renderer->staging_mapped + offset, data, size);

    /* Flush the entire staged memory (VK_WHOLE_SIZE avoids alignment issues) */
    VkMappedMemoryRange flush_range;
    memset(&flush_range, 0, sizeof(flush_range));
    flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    flush_range.memory = renderer->staging_memory;
    flush_range.offset = 0;
    flush_range.size = VK_WHOLE_SIZE;

    VkResult vr = renderer->fns.vkFlushMappedMemoryRanges(renderer->device, 1, &flush_range);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkFlushMappedMemoryRanges failed (vr=%d), continuing anyway", (int)vr);
    }

    /* Record the copy command */
    VkCommandBufferBeginInfo begin_info;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vr = renderer->fns.vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBeginCommandBuffer (transfer) failed (vr=%d)", (int)vr);
        return;
    }

    VkBufferCopy copy_region;
    memset(&copy_region, 0, sizeof(copy_region));
    copy_region.srcOffset = offset;
    copy_region.dstOffset = offset;
    copy_region.size = size;

    renderer->fns.vkCmdCopyBuffer(renderer->transfer_cmd, renderer->staging_buffer, slot->buffer, 1, &copy_region);

    /* Submit and wait for completion */
    if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
        NE_LOG_ERROR("failed to submit transfer command for buffer update");
        return;
    }

    /* Reset the command buffer for reuse */
    vr = renderer->fns.vkResetCommandBuffer(renderer->transfer_cmd, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
    }
}

void ne_image_destroy(NERenderer *renderer, NEImageHandle handle) {
    if (!renderer || !ne_image_handle_valid(handle)) {
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffer_cap) {
        return;
    }

    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    if (!slot->occupied) {
        return; /* Already destroyed or never existed */
    }

    /* Destroy the buffer and free its memory */
    if (slot->image != VK_NULL_HANDLE && renderer->fns.vkDestroyImage) {
        renderer->fns.vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
    }

    if (slot->imageView != VK_NULL_HANDLE && renderer->fns.vkDestroyImageView) {
        renderer->fns.vkDestroyImageView(renderer->device, slot->imageView, NULL);
        slot->imageView = VK_NULL_HANDLE;
    }

    if (slot->memory != VK_NULL_HANDLE && renderer->fns.vkFreeMemory) {
        renderer->fns.vkFreeMemory(renderer->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
    }

    /* Mark the slot as unoccupied */
    slot->occupied = false;
    slot->usage = 0;
    slot->size = 0;

    /* Decrement the count */
    if (renderer->buffer_count > 0) {
        renderer->buffer_count--;
    }
}

void ne_buffer_destroy(NERenderer *renderer, NEBufferHandle handle) {
    if (!renderer || !ne_buffer_handle_valid(handle)) {
        return;
    }

    uint32_t slot_index = handle.id - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffer_cap) {
        return;
    }

    NEVulkanBufferSlot *slot = &renderer->buffers[slot_index];

    if (!slot->occupied) {
        return; /* Already destroyed or never existed */
    }

    ne_vk_buffer_slot_free(renderer, slot);

    /* Mark the slot as unoccupied */
    slot->occupied = false;
    slot->usage = 0;
    slot->size = 0;

    /* Decrement the count */
    if (renderer->buffer_count > 0) {
        renderer->buffer_count--;
    }
}

/* ========================================================================
 * Shader Management
 * ======================================================================== */

/*
 * SHADER ARCHITECTURE
 *
 * Each VkShaderModule wraps a single SPIR-V blob.  After creation the module
 * is opaque — the entry point name and stage are NOT stored inside it.  We
 * strdup the entry point here so that pipeline creation can read both back
 * from the slot without requiring the caller to keep the original descriptor
 * alive.
 *
 * SPIR-V alignment contract
 * ─────────────────────────
 * vkCreateShaderModule requires:
 *   - pCode  : pointer to uint32_t-aligned memory
 *   - codeSize : a multiple of 4 bytes
 * We validate the size.  The pointer cast from (const void *) to
 * (const uint32_t *) is safe because ne_file_read() (malloc) guarantees at
 * least sizeof(void *) alignment, which is ≥ 4 on all supported platforms
 * (x86-64 Windows).
 *
 * Runtime source compilation
 * ──────────────────────────
 * The API header documents a future Slang → SPIR-V path.  Until Slang is
 * integrated, ne_shader_create_from_source() is an explicit no-op stub: it
 * logs a clear actionable warning and returns NE_SHADER_HANDLE_NULL.  This
 * mirrors the Metal backend's compute pipeline stubs exactly, making the
 * future integration point obvious.
 */

NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc) {
    if (!renderer || !desc || !desc->bytecode || desc->bytecode_size == 0 || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    if (desc->bytecode_size % 4 != 0) {
        NE_LOG_ERROR("ne_shader_create: SPIR-V bytecode size (%zu) must be a multiple of 4",
                     desc->bytecode_size);
        return NE_SHADER_HANDLE_NULL;
    }

    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = desc->bytecode_size;
    /* Cast is safe — see alignment note above. */
    smci.pCode    = (const uint32_t *)desc->bytecode;

    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult vr = renderer->fns.vkCreateShaderModule(renderer->device, &smci, NULL, &module);
    if (vr != VK_SUCCESS || module == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateShaderModule failed (vr=%d)", (int)vr);
        return NE_SHADER_HANDLE_NULL;
    }

    const uint32_t slot_index = ne_pool_alloc(
        (void **)&renderer->shaders,
        &renderer->shader_count,
        &renderer->shader_cap,
        sizeof(NEVulkanShaderSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_shader_create: shader pool allocation failed");
        renderer->fns.vkDestroyShaderModule(renderer->device, module, NULL);
        return NE_SHADER_HANDLE_NULL;
    }

    NEVulkanShaderSlot *slot = &renderer->shaders[slot_index];
    slot->occupied    = true;
    slot->stage       = (uint32_t)desc->stage;
    slot->module      = module;
    slot->entry_point = _strdup(desc->entry_point);

    if (!slot->entry_point) {
        NE_LOG_ERROR("ne_shader_create: out of memory copying entry point name");
        renderer->fns.vkDestroyShaderModule(renderer->device, module, NULL);
        slot->occupied = false;
        slot->module   = VK_NULL_HANDLE;
        return NE_SHADER_HANDLE_NULL;
    }

    return (NEShaderHandle){ .id = slot_index + 1 };
}

NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc) {
    if (!renderer || !desc || (!desc->source && !desc->filename) || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    const char *filename = desc->filename;

    glslang_stage_t stage = {};
    const char *ptr = filename;
    while(*ptr) {ptr++;};
    while(*ptr != '.') {ptr--;};
    if(desc->stage == NE_SHADER_STAGE_VERTEX) stage = GLSLANG_STAGE_VERTEX;
    else if(desc->stage == NE_SHADER_STAGE_FRAGMENT) stage = GLSLANG_STAGE_FRAGMENT;
    else if(desc->stage == NE_SHADER_STAGE_COMPUTE) stage = GLSLANG_STAGE_COMPUTE;
    else { return NE_SHADER_HANDLE_NULL;}

    size_t size = 0;
    void *src = ne_file_read(filename, &size);

    glslang_input_t input = {
	.language = GLSLANG_SOURCE_GLSL,
    .client = GLSLANG_CLIENT_VULKAN,
    .client_version = GLSLANG_TARGET_VULKAN_1_3,
    .target_language = GLSLANG_TARGET_SPV,
    .target_language_version = GLSLANG_TARGET_SPV_1_6,
    .default_profile = GLSLANG_NO_PROFILE,
	.default_version = 450,
    .stage = stage,
    .code = src,
    .force_default_version_and_profile = false,
    .forward_compatible = false,
    .messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_DEBUG_INFO_BIT,
    .resource = glslang_default_resource(),
};

    glslang_shader_t *shader = glslang_shader_create(&input);
	glslang_shader_set_entry_point(shader, desc->entry_point);

	if (!glslang_shader_preprocess(shader, &input))	{
		NE_LOG_ERROR("GLSL preprocessing failed %s\n", filename);
		NE_LOG_ERROR("%s\n", glslang_shader_get_info_log(shader));
		NE_LOG_ERROR("%s\n", glslang_shader_get_info_debug_log(shader));
		NE_LOG_ERROR("%s\n", input.code);
		glslang_shader_delete(shader);
		return NE_SHADER_HANDLE_NULL;
	}

    if (!glslang_shader_parse(shader, &input)) {
        NE_LOG_ERROR("GLSL parsing failed %s\n", filename);
        NE_LOG_ERROR("%s\n", glslang_shader_get_info_log(shader));
        NE_LOG_ERROR("%s\n", glslang_shader_get_info_debug_log(shader));
        NE_LOG_ERROR("%s\n", glslang_shader_get_preprocessed_code(shader));
        glslang_shader_delete(shader);
        return NE_SHADER_HANDLE_NULL;
    }

	glslang_program_t* program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        NE_LOG_ERROR("GLSL linking failed %s\n", filename);
        NE_LOG_ERROR("%s\n", glslang_program_get_info_log(program));
        NE_LOG_ERROR("%s\n", glslang_program_get_info_debug_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NE_SHADER_HANDLE_NULL;
    }

    glslang_program_SPIRV_generate(program, stage);

    size_t sprv_size = glslang_program_SPIRV_get_size(program) * sizeof(uint32_t);
    void* sprv_bytes = malloc(sprv_size);
    glslang_program_SPIRV_get(program, sprv_bytes);

    const char* spirv_messages = glslang_program_SPIRV_get_messages(program);
    if (spirv_messages) NE_LOG_INFO("(%s) %s\b", filename, spirv_messages);

    const NEShaderHandle handle = ne_shader_create(renderer, &(NEShaderDesc){
        .stage         = desc->stage,
        .bytecode      = sprv_bytes,
        .bytecode_size = sprv_size,
        .entry_point   = desc->entry_point,
    });

    free(sprv_bytes);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    ne_file_free(src);

    return handle;
}

void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle) {
    if (!renderer || !ne_shader_handle_valid(handle)) {
        return;
    }

    const uint32_t index = handle.id - 1;
    if (index >= renderer->shader_cap || !renderer->shaders[index].occupied) {
        NE_LOG_WARN("ne_shader_destroy: invalid or already-destroyed shader handle (id=%u)",
                    handle.id);
        return;
    }

    NEVulkanShaderSlot *slot = &renderer->shaders[index];

    free(slot->entry_point);
    slot->entry_point = NULL;
    slot->stage       = 0;
    slot->occupied    = false;

    if (renderer->shader_count > 0) {
        renderer->shader_count--;
    }
}

/* ========================================================================
 * Pipeline Management
 * ======================================================================== */

/*
 * PIPELINE ARCHITECTURE — DEFERRED COMPILATION
 *
 * ne_pipeline_create() validates inputs, resolves shader handles, deep-copies
 * vertex layout state, creates the VkPipelineLayout, and stores everything in
 * a pool slot with needs_compile = true.
 *
 * The actual vkCreateGraphicsPipelines call is deferred until
 * ne_render_pass_set_pipeline() (Task #5), where we have access to the
 * surface's VkRenderPass.  This avoids coupling pipeline creation to any
 * specific render surface.
 *
 * ne_vk_pipeline_compile() is the internal helper that performs the deferred
 * compilation.  It is called from ne_render_pass_set_pipeline() on first use.
 */

/* ── Enum converters ─────────────────────────────────────────────────────── */

static VkFormat ne_vertex_format_to_vk(NEVertexFormat fmt) {
    switch (fmt) {
    case NE_VERTEX_FORMAT_FLOAT:    return VK_FORMAT_R32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT2:   return VK_FORMAT_R32G32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT3:   return VK_FORMAT_R32G32B32_SFLOAT;
    case NE_VERTEX_FORMAT_FLOAT4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NE_VERTEX_FORMAT_UNORM8X4: return VK_FORMAT_R8G8B8A8_UNORM;
    default:                        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor ne_blend_factor_to_vk(NEBlendFactor f) {
    switch (f) {
    case NE_BLEND_FACTOR_ZERO:                return VK_BLEND_FACTOR_ZERO;
    case NE_BLEND_FACTOR_ONE:                 return VK_BLEND_FACTOR_ONE;
    case NE_BLEND_FACTOR_SRC_ALPHA:           return VK_BLEND_FACTOR_SRC_ALPHA;
    case NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NE_BLEND_FACTOR_DST_ALPHA:           return VK_BLEND_FACTOR_DST_ALPHA;
    case NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:                                  return VK_BLEND_FACTOR_ZERO;
    }
}

static VkBlendOp ne_blend_op_to_vk(NEBlendOp op) {
    switch (op) {
    case NE_BLEND_OP_ADD:              return VK_BLEND_OP_ADD;
    case NE_BLEND_OP_SUBTRACT:         return VK_BLEND_OP_SUBTRACT;
    case NE_BLEND_OP_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NE_BLEND_OP_MIN:              return VK_BLEND_OP_MIN;
    case NE_BLEND_OP_MAX:              return VK_BLEND_OP_MAX;
    default:                           return VK_BLEND_OP_ADD;
    }
}

static VkPrimitiveTopology ne_topology_to_vk(NEPrimitiveTopology t) {
    switch (t) {
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NE_PRIMITIVE_TOPOLOGY_LINE_LIST:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NE_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NE_PRIMITIVE_TOPOLOGY_POINT_LIST:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    default:                                   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* ── Deferred pipeline compilation ───────────────────────────────────────── */

/**
 * Compile a pipeline slot into a live VkPipeline.
 *
 * Called from ne_render_pass_set_pipeline() when slot->needs_compile is true.
 * Requires a valid VkRenderPass from the active render surface.
 *
 * On success: clears needs_compile, sets slot->pipeline.
 * On failure: sets compilation_failed, logs error.
 */
static bool ne_vk_pipeline_compile(NERenderer *r, NEVulkanPipelineSlot *slot,
                                   VkRenderPass render_pass) {
    /* ── Shader stages ───────────────────────────────────────────────── */

    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));

    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = slot->vert_module;
    stages[0].pName  = slot->vert_entry;

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = slot->frag_module;
    stages[1].pName  = slot->frag_entry;

    /* ── Vertex input state ──────────────────────────────────────────── */

    VkPipelineVertexInputStateCreateInfo vertex_input;
    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = slot->binding_count;
    vertex_input.pVertexBindingDescriptions      = slot->bindings;
    vertex_input.vertexAttributeDescriptionCount = slot->attribute_count;
    vertex_input.pVertexAttributeDescriptions    = slot->attributes;

    /* ── Input assembly ──────────────────────────────────────────────── */

    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    memset(&input_assembly, 0, sizeof(input_assembly));
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = slot->topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    /* ── Viewport / scissor (dynamic — values set at draw time) ──────── */

    VkPipelineViewportStateCreateInfo viewport_state;
    memset(&viewport_state, 0, sizeof(viewport_state));
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    /* ── Rasterization ───────────────────────────────────────────────── */

    VkPipelineRasterizationStateCreateInfo rasterization;
    memset(&rasterization, 0, sizeof(rasterization));
    rasterization.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode    = VK_CULL_MODE_NONE;
    rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth   = 1.0f;

    /* ── Multisample (no MSAA) ───────────────────────────────────────── */

    VkPipelineMultisampleStateCreateInfo multisample;
    memset(&multisample, 0, sizeof(multisample));
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* ── Color blend ─────────────────────────────────────────────────── */

    VkPipelineColorBlendStateCreateInfo color_blend;
    memset(&color_blend, 0, sizeof(color_blend));
    color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments    = &slot->blend;

    /* ── Dynamic state ───────────────────────────────────────────────── */

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state;
    memset(&dynamic_state, 0, sizeof(dynamic_state));
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    /* ── Graphics pipeline ───────────────────────────────────────────── */

    VkGraphicsPipelineCreateInfo gpci;
    memset(&gpci, 0, sizeof(gpci));
    gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState      = &viewport_state;
    gpci.pRasterizationState = &rasterization;
    gpci.pMultisampleState   = &multisample;
    gpci.pColorBlendState    = &color_blend;
    gpci.pDynamicState       = &dynamic_state;
    gpci.layout              = slot->layout;
    gpci.renderPass          = render_pass;
    gpci.subpass             = 0;

    VkResult vr = r->fns.vkCreateGraphicsPipelines(
        r->device, VK_NULL_HANDLE, 1, &gpci, NULL, &slot->pipeline);
    if (vr != VK_SUCCESS || slot->pipeline == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateGraphicsPipelines failed (vr=%d)", (int)vr);
        slot->compilation_failed = true;
        slot->pipeline = VK_NULL_HANDLE;
        return false;
    }

    slot->needs_compile = false;
    return true;
}

/* ── ne_pipeline_create ──────────────────────────────────────────────────── */

NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Validate inputs ─────────────────────────────────────────────── */

    if (!ne_shader_handle_valid(desc->vertex_shader)) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_shader handle is null");
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (!ne_shader_handle_valid(desc->fragment_shader)) {
        NE_LOG_ERROR("ne_pipeline_create: fragment_shader handle is null");
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (!desc->vertex_layouts || desc->vertex_layout_count == 0) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_layouts is null or count is 0");
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Resolve shader handles ──────────────────────────────────────── */

    const uint32_t vs_index = desc->vertex_shader.id - 1;
    const uint32_t fs_index = desc->fragment_shader.id - 1;

    if (vs_index >= renderer->shader_cap || !renderer->shaders[vs_index].occupied) {
        NE_LOG_ERROR("ne_pipeline_create: vertex_shader handle (id=%u) is invalid or destroyed",
                     desc->vertex_shader.id);
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (fs_index >= renderer->shader_cap || !renderer->shaders[fs_index].occupied) {
        NE_LOG_ERROR("ne_pipeline_create: fragment_shader handle (id=%u) is invalid or destroyed",
                     desc->fragment_shader.id);
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanShaderSlot *vs_slot = &renderer->shaders[vs_index];
    NEVulkanShaderSlot *fs_slot = &renderer->shaders[fs_index];

    /* ── Deep-copy vertex layout ─────────────────────────────────────── */

    uint32_t total_attributes = 0;
    for (uint32_t i = 0; i < desc->vertex_layout_count; i++) {
        total_attributes += desc->vertex_layouts[i].attribute_count;
    }

    VkVertexInputBindingDescription *bindings = (VkVertexInputBindingDescription *)calloc(
        desc->vertex_layout_count, sizeof(VkVertexInputBindingDescription));
    VkVertexInputAttributeDescription *attributes = (VkVertexInputAttributeDescription *)calloc(
        total_attributes, sizeof(VkVertexInputAttributeDescription));

    if (!bindings || !attributes) {
        NE_LOG_ERROR("ne_pipeline_create: out of memory for vertex layout");
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    uint32_t attr_index = 0;
    for (uint32_t i = 0; i < desc->vertex_layout_count; i++) {
        const NEVertexBufferLayout *layout = &desc->vertex_layouts[i];

        bindings[i].binding   = i;
        bindings[i].stride    = layout->stride;
        bindings[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        for (uint32_t j = 0; j < layout->attribute_count; j++) {
            const NEVertexAttribute *attr = &layout->attributes[j];
            attributes[attr_index].location = attr->location;
            attributes[attr_index].binding  = i;
            attributes[attr_index].format   = ne_vertex_format_to_vk(attr->format);
            attributes[attr_index].offset   = attr->offset;
            attr_index++;
        }
    }

    /* ── Convert blend state ─────────────────────────────────────────── */

    VkPipelineColorBlendAttachmentState blend_attachment;
    memset(&blend_attachment, 0, sizeof(blend_attachment));
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (desc->blend.enabled) {
        blend_attachment.blendEnable         = VK_TRUE;
        blend_attachment.srcColorBlendFactor = ne_blend_factor_to_vk(desc->blend.src_color);
        blend_attachment.dstColorBlendFactor = ne_blend_factor_to_vk(desc->blend.dst_color);
        blend_attachment.colorBlendOp        = ne_blend_op_to_vk(desc->blend.color_op);
        blend_attachment.srcAlphaBlendFactor = ne_blend_factor_to_vk(desc->blend.src_alpha);
        blend_attachment.dstAlphaBlendFactor = ne_blend_factor_to_vk(desc->blend.dst_alpha);
        blend_attachment.alphaBlendOp        = ne_blend_op_to_vk(desc->blend.alpha_op);
    }

    /* ── Create pipeline layout ──────────────────────────────────────── */

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset     = 0;
    push_range.size       = 128; /* Guaranteed minimum by Vulkan spec. */

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push_range;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult vr = renderer->fns.vkCreatePipelineLayout(renderer->device, &plci, NULL, &layout);
    if (vr != VK_SUCCESS || layout == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreatePipelineLayout failed (vr=%d)", (int)vr);
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Allocate pool slot ──────────────────────────────────────────── */

    const uint32_t slot_index = ne_pool_alloc(
        (void **)&renderer->pipelines,
        &renderer->pipeline_count,
        &renderer->pipeline_cap,
        sizeof(NEVulkanPipelineSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_pipeline_create: pipeline pool allocation failed");
        renderer->fns.vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(bindings);
        free(attributes);
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanPipelineSlot *slot = &renderer->pipelines[slot_index];

    slot->occupied           = true;
    slot->needs_compile      = true;
    slot->compilation_failed = false;

    slot->layout       = layout;
    slot->vert_module  = vs_slot->module;
    slot->frag_module  = fs_slot->module;
    slot->vert_entry   = _strdup(vs_slot->entry_point);
    slot->frag_entry   = _strdup(fs_slot->entry_point);

    if (!slot->vert_entry || !slot->frag_entry) {
        NE_LOG_ERROR("ne_pipeline_create: out of memory copying entry point names");
        renderer->fns.vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(slot->vert_entry);
        free(slot->frag_entry);
        free(bindings);
        free(attributes);
        slot->occupied = false;
        slot->layout = VK_NULL_HANDLE;
        slot->vert_entry = NULL;
        slot->frag_entry = NULL;
        return NE_PIPELINE_HANDLE_NULL;
    }

    slot->bindings        = bindings;
    slot->binding_count   = desc->vertex_layout_count;
    slot->attributes      = attributes;
    slot->attribute_count = total_attributes;

    slot->topology = ne_topology_to_vk(desc->topology);
    slot->blend    = blend_attachment;
    slot->pipeline = VK_NULL_HANDLE;

    return (NEPipelineHandle){ slot_index + 1 };
}

/* ── ne_pipeline_destroy ─────────────────────────────────────────────────── */

void ne_pipeline_destroy(NERenderer *renderer, NEPipelineHandle handle) {
    if (!renderer || !ne_pipeline_handle_valid(handle)) {
        return;
    }

    const uint32_t index = handle.id - 1;
    if (index >= renderer->pipeline_cap || !renderer->pipelines[index].occupied) {
        NE_LOG_WARN("ne_pipeline_destroy: invalid or already-destroyed pipeline handle (id=%u)", handle.id);
        return;
    }

    NEVulkanPipelineSlot *slot = &renderer->pipelines[index];

    /* Ensure the GPU is done with any command buffers referencing this pipeline. */
    if (renderer->device != VK_NULL_HANDLE && renderer->fns.vkDeviceWaitIdle) {
        (void)renderer->fns.vkDeviceWaitIdle(renderer->device);
    }

    if (slot->pipeline != VK_NULL_HANDLE && renderer->fns.vkDestroyPipeline) {
        renderer->fns.vkDestroyPipeline(renderer->device, slot->pipeline, NULL);
        slot->pipeline = VK_NULL_HANDLE;
    }

    if (slot->layout != VK_NULL_HANDLE && renderer->fns.vkDestroyPipelineLayout) {
        renderer->fns.vkDestroyPipelineLayout(renderer->device, slot->layout, NULL);
        slot->layout = VK_NULL_HANDLE;
    }

    if (slot->vert_module != VK_NULL_HANDLE && renderer->fns.vkDestroyShaderModule) {
        renderer->fns.vkDestroyShaderModule(renderer->device, slot->vert_module, NULL);
        slot->vert_module = VK_NULL_HANDLE;
    }
    if (slot->frag_module != VK_NULL_HANDLE && renderer->fns.vkDestroyShaderModule) {
        renderer->fns.vkDestroyShaderModule(renderer->device, slot->frag_module, NULL);
        slot->frag_module = VK_NULL_HANDLE;
    }

    free(slot->vert_entry);
    free(slot->frag_entry);
    free(slot->bindings);
    free(slot->attributes);

    slot->vert_entry   = NULL;
    slot->frag_entry   = NULL;
    slot->bindings     = NULL;
    slot->attributes   = NULL;
    slot->vert_module  = VK_NULL_HANDLE;
    slot->frag_module  = VK_NULL_HANDLE;
    slot->occupied     = false;

    if (renderer->pipeline_count > 0) {
        renderer->pipeline_count--;
    }
}

/* ── Compute pipeline stubs ──────────────────────────────────────────────── */

NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer,
                                                   const NEComputePipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Validate inputs ─────────────────────────────────────────────── */

    if (!ne_shader_handle_valid(desc->compute_shader)) {
        NE_LOG_ERROR("ne_compute_pipeline_create: compute_shader handle is null");
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Resolve shader handles ──────────────────────────────────────── */

    const uint32_t cs_index = desc->compute_shader.id - 1;

    if (cs_index >= renderer->shader_cap || !renderer->shaders[cs_index].occupied) {
        NE_LOG_ERROR("ne_compute_pipeline_create: compute_shader handle (id=%u) is invalid or destroyed",
                     desc->compute_shader.id);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanShaderSlot *cs_slot = &renderer->shaders[cs_index];

    /* ── Create pipeline layout ──────────────────────────────────────── */

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset     = 0;
    push_range.size       = 128; /* Guaranteed minimum by Vulkan spec. */

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push_range;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult vr = renderer->fns.vkCreatePipelineLayout(renderer->device, &plci, NULL, &layout);
    if (vr != VK_SUCCESS || layout == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreatePipelineLayout failed (vr=%d)", (int)vr);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    /* ── Allocate pool slot ──────────────────────────────────────────── */
    const uint32_t slot_index = ne_pool_alloc(
        (void **)&renderer->pipelines,
        &renderer->pipeline_count,
        &renderer->pipeline_cap,
        sizeof(NEVulkanPipelineSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("ne_compute_pipeline_create: pipeline pool allocation failed");
        renderer->fns.vkDestroyPipelineLayout(renderer->device, layout, NULL);
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }

    NEVulkanPipelineSlot *slot = &renderer->pipelines[slot_index];

    slot->occupied           = true;
    slot->needs_compile      = true;
    slot->compilation_failed = false;

    slot->layout       = layout;
    slot->compute_module  = cs_slot->module;
    slot->compute_entry   = _strdup(cs_slot->entry_point);

    if (!slot->compute_entry){
        NE_LOG_ERROR("ne_compute_pipeline_create: out of memory copying entry point names");
        renderer->fns.vkDestroyPipelineLayout(renderer->device, layout, NULL);
        free(slot->compute_entry);
        slot->occupied = false;
        slot->layout = VK_NULL_HANDLE;
        slot->compute_entry = NULL;
        return NE_COMPUTE_PIPELINE_HANDLE_NULL;
    }
    slot->pipeline = VK_NULL_HANDLE;

    return (NEComputePipelineHandle){ slot_index + 1 };
}

void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle) {
    (void)renderer;
    (void)handle;
}

/* ========================================================================
 * Render Pass Commands (Graphics)
 * ======================================================================== */

void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline) {
    if (!pass || !pass->surface || !pass->cmd) {
        return;
    }

    NERenderer *r = pass->surface->renderer;
    if (!r || !ne_pipeline_handle_valid(pipeline)) {
        NE_LOG_WARN("ne_render_pass_set_pipeline: invalid pass or pipeline handle");
        return;
    }

    const uint32_t index = pipeline.id - 1;
    if (index >= r->pipeline_cap || !r->pipelines[index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_pipeline: pipeline handle (id=%u) is invalid or destroyed", pipeline.id);
        return;
    }

    NEVulkanPipelineSlot *slot = &r->pipelines[index];

    /* Deferred compilation: build the VkPipeline on first use. */
    if (slot->needs_compile) {
        if (!ne_vk_pipeline_compile(r, slot, pass->surface->render_pass)) {
            /* compilation_failed is already set inside ne_vk_pipeline_compile. */
            return;
        }
    }

    if (slot->compilation_failed || slot->pipeline == VK_NULL_HANDLE) {
        return;
    }

    r->fns.vkCmdBindPipeline(pass->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, slot->pipeline);
    pass->bound_layout = slot->layout;
}

void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint64_t slot,
                                      NEBufferHandle buffer) {
    if (!pass || !pass->surface || !pass->cmd) {
        return;
    }


    NERenderer *r = pass->surface->renderer;
    if (!r || !ne_buffer_handle_valid(buffer)) {
        return;
    }

    const uint32_t buf_index = buffer.id - 1;
    if (buf_index >= r->buffer_cap || !r->buffers[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_vertex_buffer: buffer handle (id=%u) is invalid or destroyed", buffer.id);
        return;
    }

    VkBuffer vk_buffer = ne_vk_buffer_for_frame(&r->buffers[buf_index], pass->surface->sc.frame_index);
    VkDeviceSize offset = 0;

    r->fns.vkCmdBindVertexBuffers(pass->cmd, slot, 1, &vk_buffer, &offset);
}

void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer,
                                     NEIndexType type) {
    if (!pass || !pass->surface || !pass->cmd) {
        return;
    }

    NERenderer *r = pass->surface->renderer;
    if (!r || !ne_buffer_handle_valid(buffer)) {
        return;
    }

    const uint32_t buf_index = buffer.id - 1;
    if (buf_index >= r->buffer_cap || !r->buffers[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_set_index_buffer: buffer handle (id=%u) is invalid or destroyed", buffer.id);
        return;
    }

    VkBuffer vk_buffer = ne_vk_buffer_for_frame(&r->buffers[buf_index], pass->surface->sc.frame_index);
    VkIndexType vk_type = (type == NE_INDEX_TYPE_UINT32) ? VK_INDEX_TYPE_UINT32
                                                         : VK_INDEX_TYPE_UINT16;

    r->fns.vkCmdBindIndexBuffer(pass->cmd, vk_buffer, 0, vk_type);
}

void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage, uint64_t slot, const void *data, size_t size) {
    if (!pass || !pass->surface || !pass->cmd || !data || size == 0) {
        return;
    }

    if (pass->bound_layout == VK_NULL_HANDLE) {
        NE_LOG_WARN("ne_render_pass_set_uniform_data: no pipeline bound (call set_pipeline first)");
        return;
    }

    NERenderer *r = pass->surface->renderer;
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

    r->fns.vkCmdPushConstants(pass->cmd, pass->bound_layout, stage_flags, 0, size, data);
}

void ne_render_pass_update_buffer(NERenderPass *pass, NEBufferHandle handle,
                                  const void *data, uint32_t size, uint32_t offset) {
    if (!pass || !pass->surface || !data || size == 0 || !ne_buffer_handle_valid(handle)) {
        return;
    }

    NERenderer *r = pass->surface->renderer;
    const uint32_t buf_index = handle.id - 1;
    if (buf_index >= r->buffer_cap || !r->buffers[buf_index].occupied) {
        NE_LOG_WARN("ne_render_pass_update_buffer: invalid buffer handle (id=%u)", handle.id);
        return;
    }

    NEVulkanBufferSlot *slot = &r->buffers[buf_index];

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
    const uint32_t frame = pass->surface->sc.frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT;
    if (slot->dyn_mapped[frame]) {
        memcpy((uint8_t *)slot->dyn_mapped[frame] + offset, data, size);
    }
}

void ne_render_pass_draw(NERenderPass *pass, uint64_t first_vertex, uint64_t vertex_count) {
    if (!pass || !pass->surface || !pass->cmd || vertex_count == 0) {
        return;
    }

    NERenderer *r = pass->surface->renderer;
    r->fns.vkCmdDraw(pass->cmd, vertex_count, 1, first_vertex, 0);
}

void ne_render_pass_draw_indexed(NERenderPass *pass, uint64_t index_count,
                                 uint64_t first_index, int64_t vertex_offset) {
    if (!pass || !pass->surface || !pass->cmd || index_count == 0) {
        return;
    }

    NERenderer *r = pass->surface->renderer;
    r->fns.vkCmdDrawIndexed(pass->cmd, index_count, 1, first_index, vertex_offset, 0);
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
