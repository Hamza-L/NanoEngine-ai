#include "internal/ne_vulkan_images.h"
#include "internal/ne_vulkan_buffers.h"
#include "ne_log.h"
#include "ne_renderer_buffer.h"

#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

extern PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
extern PFN_vkCreateImage vkCreateImage;
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkAllocateMemory vkAllocateMemory;
extern PFN_vkBindImageMemory vkBindImageMemory;
extern PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
extern PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
extern PFN_vkResetCommandBuffer vkResetCommandBuffer;

static NEVulkanBufferSlot *ne_image_slot(NERenderer *r, NEImageHandle handle) {
    if (!r || handle == 0 || handle - 1 >= r->buffers.cap) return NULL;
    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot *)r->buffers.slots)[handle - 1];
    if (!slot->occupied || !(slot->usage & (NE_BUFFER_USAGE_IMAGE_STORAGE | NE_BUFFER_USAGE_IMAGE_SAMPLED))) return NULL;
    return slot;
}

void ne_cmd_copy_buffer_to_image(const NERenderer *r, VkCommandBuffer cmd, VkBuffer buffer, NEImageHandle handle) {
    const NEVulkanBufferSlot *slot = &((const NEVulkanBufferSlot *)r->buffers.slots)[handle - 1];
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {slot->width, slot->height, 1},
    };
    vkCmdCopyBufferToImage(cmd, buffer, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

static bool ne_image_upload(NERenderer *r, NEImageHandle handle, const void *data, uint32_t size) {
    NEVulkanBufferSlot *slot = ne_image_slot(r, handle);
    if (!slot || !data || (uint64_t)slot->width * slot->height * slot->format != size) {
        NE_LOG_ERROR("image update requires exactly one complete, tightly packed image");
        return false;
    }
    if (!ne_vk_ensure_staging_buffer(r, size)) return false;
    memcpy(r->staging_mapped, data, size);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(r->transfer_cmd, &begin) != VK_SUCCESS) return false;
    VkImageLayout final_layout = (slot->usage & NE_BUFFER_USAGE_IMAGE_STORAGE)
        ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ne_cmd_transition_image_layout(r, r->transfer_cmd, handle, slot->image_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ne_cmd_copy_buffer_to_image(r, r->transfer_cmd, r->staging_buffer, handle);
    ne_cmd_transition_image_layout(r, r->transfer_cmd, handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout);
    bool ok = ne_vk_submit_transfer_cmd(r, r->transfer_cmd);
    if (!ok) return false; /* never reset a command buffer of uncertain completion */
    slot->image_layout = final_layout;
    if (vkResetCommandBuffer(r->transfer_cmd, 0) != VK_SUCCESS) {
        NE_LOG_ERROR("failed to reset image upload command buffer");
        return false;
    }
    return ok;
}

NEImageHandle ne_image_create(NERenderer *r, const NEImageDesc *desc) {
    const uint32_t image_usage = NE_BUFFER_USAGE_IMAGE_STORAGE | NE_BUFFER_USAGE_IMAGE_SAMPLED;
    if (!r || !desc || !desc->width || !desc->height ||
        !(desc->usage & image_usage) || (desc->usage & ~image_usage)) return NE_IMAGE_HANDLE_NULL;
    if (desc->format != NE_IMAGE_FORMAT_GRAY && desc->format != NE_IMAGE_FORMAT_RGB &&
        desc->format != NE_IMAGE_FORMAT_RGBA) return NE_IMAGE_HANDLE_NULL;
    uint64_t pixels = (uint64_t)desc->width * desc->height;
    if (pixels > UINT32_MAX / (uint32_t)desc->format) return NE_IMAGE_HANDLE_NULL;
    if (desc->srgb && (desc->usage & NE_BUFFER_USAGE_IMAGE_STORAGE)) {
        NE_LOG_ERROR("sRGB images cannot be storage images in the current format set");
        return NE_IMAGE_HANDLE_NULL;
    }
    const uint32_t size = (uint32_t)pixels * (uint32_t)desc->format;
    VkFormat format;
    switch (desc->format) {
        case NE_IMAGE_FORMAT_GRAY: format = desc->srgb ? VK_FORMAT_R8_SRGB : VK_FORMAT_R8_UNORM; break;
        case NE_IMAGE_FORMAT_RGB: format = desc->srgb ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8_UNORM; break;
        case NE_IMAGE_FORMAT_RGBA: format = desc->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; break;
        default: return NE_IMAGE_HANDLE_NULL;
    }
    uint32_t index = ne_pool_alloc(&r->buffers, sizeof(NEVulkanBufferSlot));
    if (index == UINT32_MAX) return NE_IMAGE_HANDLE_NULL;
    NEImageHandle handle = (NEImageHandle)index + 1;
    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot *)r->buffers.slots)[index];
    /* Establish metadata before any helper records a copy or handles failure. */
    *slot = (NEVulkanBufferSlot){
        .occupied = true, .usage = desc->usage,
        .width = desc->width, .height = desc->height, .format = desc->format,
        .image_layout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->usage & NE_BUFFER_USAGE_IMAGE_SAMPLED) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->usage & NE_BUFFER_USAGE_IMAGE_STORAGE) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {desc->width, desc->height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(r->device, &info, NULL, &slot->image) != VK_SUCCESS) goto fail;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(r->device, slot->image, &requirements);
    uint32_t memory_type = ne_vk_find_memory_type(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = memory_type,
    };
    if (vkAllocateMemory(r->device, &allocation, NULL, &slot->memory) != VK_SUCCESS) goto fail;
    if (vkBindImageMemory(r->device, slot->image, slot->memory, 0) != VK_SUCCESS) goto fail;
    /* Create the view before uploading, so view failure has no submitted work. */
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = slot->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(r->device, &view, NULL, &slot->imageView) != VK_SUCCESS) goto fail;
    if (desc->initial_data && !ne_image_upload(r, handle, desc->initial_data, size)) goto fail;
    return handle;
fail:
    NE_LOG_ERROR("image creation/upload failed");
    /* The transfer helper waits for completion before returning success. Use
     * retirement on all failures as well, preserving any submitted references. */
    ne_vk_buffer_slot_retire(r, index);
    return NE_IMAGE_HANDLE_NULL;
}

NEImageHandle ne_image_load(NERenderer *renderer, const char *filename) {
    if (!renderer || !filename) return NE_IMAGE_HANDLE_NULL;
    int width, height, channels;
    stbi_uc *pixels = stbi_load(filename, &width, &height, &channels, 4);
    if (!pixels) return NE_IMAGE_HANDLE_NULL;
    NEImageHandle image = ne_image_create(renderer, &(NEImageDesc){
        .width = (uint32_t)width, .height = (uint32_t)height,
        .format = NE_IMAGE_FORMAT_RGBA, .usage = NE_BUFFER_USAGE_IMAGE_SAMPLED,
        .initial_data = pixels, .srgb = true,
    });
    stbi_image_free(pixels);
    return image;
}

void ne_image_update(NERenderer *renderer, NEImageHandle handle, const void *data, uint32_t size) {
    (void)ne_image_upload(renderer, handle, data, size);
}

void ne_image_destroy(NERenderer *renderer, NEImageHandle handle) {
    if (ne_image_slot(renderer, handle)) ne_vk_buffer_slot_retire(renderer, (uint32_t)(handle - 1));
}
