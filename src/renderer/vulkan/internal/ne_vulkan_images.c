#include "internal/ne_vulkan_images.h"
#include "internal/ne_vulkan_renderer.h"
#include "internal/ne_vulkan_buffers.h"

#include "ne_log.h"
#include "ne_file.h"
#include "ne_renderer_image.h"
#include "ne_renderer_buffer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

extern PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
extern PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
extern PFN_vkCreateImage vkCreateImage;
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkDestroyImage vkDestroyImage;
extern PFN_vkFreeMemory vkFreeMemory;
extern PFN_vkAllocateMemory vkAllocateMemory;
extern PFN_vkBindImageMemory vkBindImageMemory;
extern PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
extern PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
extern PFN_vkResetCommandBuffer vkResetCommandBuffer;
extern PFN_vkDestroyImage vkDestroyImage;
extern PFN_vkDestroyImageView vkDestroyImageView;

void ne_cmd_copy_buffer_to_image(const NERenderer *renderer, const VkCommandBuffer cmd, const VkBuffer buffer, const NEImageHandle handle) {
    if (!ne_image_handle_valid(handle)) {
        NE_LOG_ERROR("ne_cmd_copy_buffer_to_image: INVALID IMAGE HANDLE/n");
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */
    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

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

    vkCmdCopyBufferToImage(cmd, buffer, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
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
    uint32_t slot_index = ne_pool_alloc(&renderer->buffers, sizeof(NEVulkanBufferSlot));

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("failed to allocate buffer slot from pool");
        return NE_IMAGE_HANDLE_NULL;
    }

    NEImageHandle handle = (NEImageHandle)(slot_index + 1);
    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

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

    VkResult vr = vkCreateImage(renderer->device, &img_info, NULL, &slot->image);
    if (vr != VK_SUCCESS || slot->image == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateImage failed (vr=%d, size=%u)", (int)vr, size);
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Get memory requirements for the buffer */
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(renderer->device, slot->image, &mem_req);

    /* Find device-local memory for the GPU buffer */
    uint32_t mem_type = ne_vk_find_memory_type(
        renderer,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find device-local memory type for buffer");
        vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Allocate memory for the buffer */
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vr = vkAllocateMemory(renderer->device, &alloc_info, NULL, &slot->memory);
    if (vr != VK_SUCCESS || slot->memory == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateMemory failed (vr=%d)", (int)vr);
        vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* Bind memory to buffer */
    vr = vkBindImageMemory(renderer->device, slot->image, slot->memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory failed (vr=%d)", (int)vr);
        vkFreeMemory(renderer->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
        vkDestroyImage(renderer->device, slot->image, NULL);
        slot->image = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_IMAGE_HANDLE_NULL;
    }

    /* If initial data is provided, stage it to the buffer */
    if (desc->initial_data) {
        if (!ne_vk_ensure_staging_buffer(renderer, size)) {
            NE_LOG_ERROR("failed to ensure staging buffer for initial data");
            vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            vkDestroyImage(renderer->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_IMAGE_HANDLE_NULL;
        }

        /* Copy initial data into the staging buffer */
        memcpy(renderer->staging_mapped, desc->initial_data, size);

        /* Record and submit transfer command */
        VkCommandBufferBeginInfo begin_info;
        memset(&begin_info, 0, sizeof(begin_info));
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vr = vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkBeginCommandBuffer (transfer) failed (vr=%d)", (int)vr);
            vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            vkDestroyImage(renderer->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_IMAGE_HANDLE_NULL;
        }

        ne_cmd_transition_image_layout(renderer, renderer->transfer_cmd, handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ne_cmd_copy_buffer_to_image(renderer, renderer->transfer_cmd, renderer->staging_buffer, handle);

        /* Submit and wait for completion */
        if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
            NE_LOG_ERROR("failed to submit transfer command for initial data");
            vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            vkDestroyImage(renderer->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_IMAGE_HANDLE_NULL;
        }

        /* Reset the command buffer for reuse */
        vr = vkResetCommandBuffer(renderer->transfer_cmd, 0);
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

        vr = vkCreateImageView(renderer->device, &viewInfo, nullptr, &slot->imageView);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkCreateImageView failed (vr=%d), continuing anyway", (int)vr);
        }
    }

    /* Store metadata (slot already marked occupied by ne_pool_alloc) */
    slot->usage = desc->usage;
    slot->width = desc->width;
    slot->height = desc->height;
    slot->format = desc->format;

    /* Return handle (1-based ID for null safety) */
    return handle;
}

NEImageHandle ne_image_load(NERenderer *renderer, const char *filename) {
    if (!renderer || !filename) {
        return NE_IMAGE_HANDLE_NULL;
    }

    return (NEImageHandle){};
}

void ne_image_update(NERenderer *renderer, NEImageHandle handle, const void *data, uint32_t size) {
    if (!renderer || !ne_image_handle_valid(handle) || !data || size == 0) {
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffers.cap || !((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index].occupied) {
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

    /* Record the copy command */
    VkCommandBufferBeginInfo begin_info;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult vr = vkBeginCommandBuffer(renderer->transfer_cmd, &begin_info);
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
    vr = vkResetCommandBuffer(renderer->transfer_cmd, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
    }
}

void ne_image_destroy(NERenderer *renderer, NEImageHandle handle) {
    if (!renderer || !ne_image_handle_valid(handle)) {
        return;
    }

    uint32_t slot_index = handle - 1;

    if (slot_index >= renderer->buffers.cap) {
        return;
    }

    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

    if (!slot->occupied) {
        return;
    }

    ne_vk_buffer_slot_free(renderer, slot);
    ne_pool_free(&renderer->buffers, slot_index, sizeof(NEVulkanBufferSlot));
}
