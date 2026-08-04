#ifndef INTERNAL_NE_VULKAN_LOADER_H
#define INTERNAL_NE_VULKAN_LOADER_H

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "ne_log.h"

/* Global */
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

/* Instance */
PFN_vkCreateInstance vkCreateInstance;
PFN_vkDestroyInstance vkDestroyInstance;
PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
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

inline void *ne_vk_get_global(const char *name) {
    if (!vkGetInstanceProcAddr) {
        return NULL;
    }
    return (void *)vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
}

inline void *ne_vk_get_instance(VkInstance instance, const char *name) {
    if (!vkGetInstanceProcAddr) {
        return NULL;
    }
    return (void *)vkGetInstanceProcAddr(instance, name);
}

inline void *ne_vk_get_device(VkDevice device, const char *name) {
    if (!vkGetDeviceProcAddr) {
        return NULL;
    }
    return (void *)vkGetDeviceProcAddr(device, name);
}

inline bool ne_vk_load_instance_fns(VkInstance instance) {
    vkDestroyInstance = (PFN_vkDestroyInstance)ne_vk_get_instance(instance, "vkDestroyInstance");
    vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)ne_vk_get_instance(instance, "vkEnumeratePhysicalDevices");
    vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)ne_vk_get_instance(instance, "vkGetPhysicalDeviceProperties");
    vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)ne_vk_get_instance(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    vkCreateDevice = (PFN_vkCreateDevice)ne_vk_get_instance(instance, "vkCreateDevice");
    vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)ne_vk_get_instance(instance, "vkGetPhysicalDeviceMemoryProperties");

    vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)ne_vk_get_instance(instance, "vkCreateWin32SurfaceKHR");
    vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)ne_vk_get_instance(instance, "vkDestroySurfaceKHR");

    vkGetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)ne_vk_get_instance(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)ne_vk_get_instance(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    vkGetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)ne_vk_get_instance(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)ne_vk_get_instance(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    return vkDestroyInstance && vkEnumeratePhysicalDevices && vkGetPhysicalDeviceQueueFamilyProperties && vkCreateDevice &&
           vkCreateWin32SurfaceKHR && vkDestroySurfaceKHR && vkGetPhysicalDeviceSurfaceSupportKHR &&
           vkGetPhysicalDeviceSurfaceCapabilitiesKHR && vkGetPhysicalDeviceSurfaceFormatsKHR && vkGetPhysicalDeviceSurfacePresentModesKHR &&
           vkGetPhysicalDeviceMemoryProperties;
}

inline bool ne_vk_load_device_fns(VkDevice device) {
    vkDestroyDevice = (PFN_vkDestroyDevice)ne_vk_get_device(device, "vkDestroyDevice");
    vkGetDeviceQueue = (PFN_vkGetDeviceQueue)ne_vk_get_device(device, "vkGetDeviceQueue");
    vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)ne_vk_get_device(device, "vkDeviceWaitIdle");

    vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)ne_vk_get_device(device, "vkCreateSwapchainKHR");
    vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)ne_vk_get_device(device, "vkDestroySwapchainKHR");
    vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)ne_vk_get_device(device, "vkGetSwapchainImagesKHR");
    vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)ne_vk_get_device(device, "vkAcquireNextImageKHR");

    vkQueueSubmit = (PFN_vkQueueSubmit)ne_vk_get_device(device, "vkQueueSubmit");
    vkQueuePresentKHR = (PFN_vkQueuePresentKHR)ne_vk_get_device(device, "vkQueuePresentKHR");
    vkQueueWaitIdle = (PFN_vkQueueWaitIdle)ne_vk_get_device(device, "vkQueueWaitIdle");

    vkCreateSemaphore = (PFN_vkCreateSemaphore)ne_vk_get_device(device, "vkCreateSemaphore");
    vkDestroySemaphore = (PFN_vkDestroySemaphore)ne_vk_get_device(device, "vkDestroySemaphore");
    vkCreateFence = (PFN_vkCreateFence)ne_vk_get_device(device, "vkCreateFence");
    vkDestroyFence = (PFN_vkDestroyFence)ne_vk_get_device(device, "vkDestroyFence");
    vkWaitForFences = (PFN_vkWaitForFences)ne_vk_get_device(device, "vkWaitForFences");
    vkResetFences = (PFN_vkResetFences)ne_vk_get_device(device, "vkResetFences");

    vkCreateCommandPool = (PFN_vkCreateCommandPool)ne_vk_get_device(device, "vkCreateCommandPool");
    vkDestroyCommandPool = (PFN_vkDestroyCommandPool)ne_vk_get_device(device, "vkDestroyCommandPool");
    vkResetCommandPool = (PFN_vkResetCommandPool)ne_vk_get_device(device, "vkResetCommandPool");
    vkResetCommandBuffer = (PFN_vkResetCommandBuffer)ne_vk_get_device(device, "vkResetCommandBuffer");
    vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)ne_vk_get_device(device, "vkAllocateCommandBuffers");
    vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)ne_vk_get_device(device, "vkFreeCommandBuffers");

    vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)ne_vk_get_device(device, "vkBeginCommandBuffer");
    vkEndCommandBuffer = (PFN_vkEndCommandBuffer)ne_vk_get_device(device, "vkEndCommandBuffer");
    vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)ne_vk_get_device(device, "vkCmdPipelineBarrier");
    vkCmdClearColorImage = (PFN_vkCmdClearColorImage)ne_vk_get_device(device, "vkCmdClearColorImage");

    /* Buffer management functions */
    vkCreateBuffer = (PFN_vkCreateBuffer)ne_vk_get_device(device, "vkCreateBuffer");
    vkDestroyBuffer = (PFN_vkDestroyBuffer)ne_vk_get_device(device, "vkDestroyBuffer");
    vkCreateImage = (PFN_vkCreateImage)ne_vk_get_device(device, "vkCreateImage");
    vkDestroyImage = (PFN_vkDestroyImage)ne_vk_get_device(device, "vkDestroyImage");
    vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)ne_vk_get_device(device, "vkGetBufferMemoryRequirements");
    vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)ne_vk_get_device(device, "vkGetImageMemoryRequirements");
    vkAllocateMemory = (PFN_vkAllocateMemory)ne_vk_get_device(device, "vkAllocateMemory");
    vkFreeMemory = (PFN_vkFreeMemory)ne_vk_get_device(device, "vkFreeMemory");
    vkBindBufferMemory = (PFN_vkBindBufferMemory)ne_vk_get_device(device, "vkBindBufferMemory");
    vkBindImageMemory = (PFN_vkBindImageMemory)ne_vk_get_device(device, "vkBindImageMemory");
    vkMapMemory = (PFN_vkMapMemory)ne_vk_get_device(device, "vkMapMemory");
    vkUnmapMemory = (PFN_vkUnmapMemory)ne_vk_get_device(device, "vkUnmapMemory");
    vkFlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)ne_vk_get_device(device, "vkFlushMappedMemoryRanges");
    vkCmdCopyBuffer = (PFN_vkCmdCopyBuffer)ne_vk_get_device(device, "vkCmdCopyBuffer");
    vkCmdCopyImage = (PFN_vkCmdCopyImage)ne_vk_get_device(device, "vkCmdCopyImage");
    vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)ne_vk_get_device(device, "vkCmdCopyBufferToImage");

    /* Shader management functions */
    vkCreateShaderModule = (PFN_vkCreateShaderModule)ne_vk_get_device(device, "vkCreateShaderModule");
    vkDestroyShaderModule = (PFN_vkDestroyShaderModule)ne_vk_get_device(device, "vkDestroyShaderModule");

    /* Render pass */
    vkCreateRenderPass = (PFN_vkCreateRenderPass)ne_vk_get_device(device, "vkCreateRenderPass");
    vkDestroyRenderPass = (PFN_vkDestroyRenderPass)ne_vk_get_device(device, "vkDestroyRenderPass");

    /* Image views */
    vkCreateImageView = (PFN_vkCreateImageView)ne_vk_get_device(device, "vkCreateImageView");
    vkDestroyImageView = (PFN_vkDestroyImageView)ne_vk_get_device(device, "vkDestroyImageView");

    /* Framebuffers */
    vkCreateFramebuffer = (PFN_vkCreateFramebuffer)ne_vk_get_device(device, "vkCreateFramebuffer");
    vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)ne_vk_get_device(device, "vkDestroyFramebuffer");

    /* Pipeline */
    vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)ne_vk_get_device(device, "vkCreatePipelineLayout");
    vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)ne_vk_get_device(device, "vkDestroyPipelineLayout");
    vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)ne_vk_get_device(device, "vkCreateGraphicsPipelines");
    vkDestroyPipeline = (PFN_vkDestroyPipeline)ne_vk_get_device(device, "vkDestroyPipeline");

    /* Render pass commands */
    vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)ne_vk_get_device(device, "vkCmdBeginRenderPass");
    vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)ne_vk_get_device(device, "vkCmdEndRenderPass");
    vkCmdBindPipeline = (PFN_vkCmdBindPipeline)ne_vk_get_device(device, "vkCmdBindPipeline");
    vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)ne_vk_get_device(device, "vkCmdBindVertexBuffers");
    vkCmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)ne_vk_get_device(device, "vkCmdBindIndexBuffer");
    vkCmdSetViewport = (PFN_vkCmdSetViewport)ne_vk_get_device(device, "vkCmdSetViewport");
    vkCmdSetScissor = (PFN_vkCmdSetScissor)ne_vk_get_device(device, "vkCmdSetScissor");
    vkCmdDraw = (PFN_vkCmdDraw)ne_vk_get_device(device, "vkCmdDraw");
    vkCmdDrawIndexed = (PFN_vkCmdDrawIndexed)ne_vk_get_device(device, "vkCmdDrawIndexed");
    vkCmdPushConstants = (PFN_vkCmdPushConstants)ne_vk_get_device(device, "vkCmdPushConstants");

    return vkDestroyDevice && vkGetDeviceQueue && vkCreateSwapchainKHR && vkGetSwapchainImagesKHR &&
           vkAcquireNextImageKHR && vkQueueSubmit && vkQueuePresentKHR && vkCreateSemaphore && vkCreateFence &&
           vkWaitForFences && vkResetFences && vkCreateCommandPool && vkResetCommandBuffer && vkAllocateCommandBuffers &&
           vkBeginCommandBuffer && vkEndCommandBuffer && vkCmdPipelineBarrier && vkCmdClearColorImage &&
           vkCreateBuffer && vkDestroyBuffer && vkGetBufferMemoryRequirements && vkAllocateMemory && vkFreeMemory && vkBindBufferMemory &&
           vkMapMemory && vkUnmapMemory && vkFlushMappedMemoryRanges && vkCmdCopyBuffer &&
           vkCreateShaderModule && vkDestroyShaderModule &&
           vkCreateRenderPass && vkDestroyRenderPass &&
           vkCreateImageView && vkDestroyImageView &&
           vkCreateFramebuffer && vkDestroyFramebuffer &&
           vkCreatePipelineLayout && vkDestroyPipelineLayout &&
           vkCreateGraphicsPipelines && vkDestroyPipeline &&
           vkCmdBeginRenderPass && vkCmdEndRenderPass &&
           vkCmdBindPipeline && vkCmdBindVertexBuffers && vkCmdBindIndexBuffer &&
           vkCmdSetViewport && vkCmdSetScissor &&
           vkCmdDraw && vkCmdDrawIndexed && vkCmdPushConstants;
}

inline bool ne_vk_load_loader(HMODULE* vulkan_lib_handle) {
    *vulkan_lib_handle = LoadLibraryA("vulkan-1.dll");
    if (!vulkan_lib_handle) {
        NE_LOG_ERROR("failed to load vulkan-1.dll (Vulkan runtime not installed?)");
        return false;
    }

    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(*vulkan_lib_handle, "vkGetInstanceProcAddr");
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(*vulkan_lib_handle, "vkGetDeviceProcAddr");

    if (!vkGetInstanceProcAddr || !vkGetDeviceProcAddr) {
        NE_LOG_ERROR("failed to get Vulkan proc address functions");
        return false;
    }

    vkCreateInstance = (PFN_vkCreateInstance)ne_vk_get_global("vkCreateInstance");
    vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)ne_vk_get_global("vkEnumerateInstanceExtensionProperties");
    vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)ne_vk_get_global("vkEnumerateInstanceLayerProperties");

    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) {
        NE_LOG_ERROR("failed to load required Vulkan loader entry points");
        return false;
    }

    return true;
}

#endif //INTERNAL_NE_VULKAN_LOADER_H
