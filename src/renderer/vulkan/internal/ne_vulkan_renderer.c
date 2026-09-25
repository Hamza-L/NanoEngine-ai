#include "internal/ne_vulkan_renderer.h"
#include "internal/ne_vulkan_buffers.h"

#include "ne_log.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern PFN_vkEndCommandBuffer vkEndCommandBuffer;
extern PFN_vkResetFences vkResetFences;
extern PFN_vkQueueSubmit vkQueueSubmit;
extern PFN_vkWaitForFences vkWaitForFences;
extern PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
extern PFN_vkCreateFence vkCreateFence;
extern PFN_vkDestroyFence vkDestroyFence;

typedef struct NEVulkanRetired {
    struct NEVulkanRetired *next;
    void (*destroy)(NERenderer *, void *);
    max_align_t data[];
} NEVulkanRetired;

typedef struct NEVulkanRetiredBatch {
    struct NEVulkanRetiredBatch *next;
    VkFence fence;
    NEVulkanRetired *items;
} NEVulkanRetiredBatch;

bool ne_vk_retire(NERenderer *r, void (*destroy)(NERenderer *, void *),
                  const void *data, size_t size) {
    if (size > SIZE_MAX - sizeof(NEVulkanRetired)) {
        return false;
    }
    NEVulkanRetired *item = malloc(sizeof(*item) + size);
    if (!item) {
        NE_LOG_ERROR("resource retirement allocation failed; resource remains live");
        return false;
    }
    item->destroy = destroy;
    memcpy(item->data, data, size);
    item->next = r->retired_pending;
    r->retired_pending = item;
    return true;
}

static void ne_vk_release_retired(NERenderer *r, NEVulkanRetired *item) {
    while (item) {
        NEVulkanRetired *next = item->next;
        item->destroy(r, item->data);
        free(item);
        item = next;
    }
}

void ne_vk_collect_retired(NERenderer *r, bool force) {
    NEVulkanRetiredBatch **link = &r->retired_batches;
    while (*link) {
        NEVulkanRetiredBatch *batch = *link;
        if (!force && vkWaitForFences(r->device, 1, &batch->fence, VK_TRUE, 0) != VK_SUCCESS) {
            link = &batch->next;
            continue;
        }
        *link = batch->next;
        ne_vk_release_retired(r, batch->items);
        vkDestroyFence(r->device, batch->fence, NULL);
        free(batch);
    }
    if (force) {
        ne_vk_release_retired(r, r->retired_pending);
        r->retired_pending = NULL;
        return;
    }
    if (!r->retired_pending || ne_vk_has_active_frames(r)) {
        return;
    }

    /* One completion marker for the batch. It follows every prior submission
     * on our single queue. Do not insert it while a frame is still recording:
     * that frame may already contain references to the retired resources. */
    NEVulkanRetiredBatch *batch = calloc(1, sizeof(*batch));
    if (!batch) {
        return; /* retain pending resources and retry at the next boundary */
    }
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult result = vkCreateFence(r->device, &info, NULL, &batch->fence);
    if (result == VK_SUCCESS) {
        result = vkQueueSubmit(r->queue, 0, NULL, batch->fence);
    }
    if (result != VK_SUCCESS) {
        if (batch->fence) vkDestroyFence(r->device, batch->fence, NULL);
        free(batch);
        NE_LOG_ERROR("could not submit resource retirement fence (vr=%d)", (int)result);
        return;
    }
    batch->items = r->retired_pending;
    r->retired_pending = NULL;
    batch->next = r->retired_batches;
    r->retired_batches = batch;
}

bool ne_vk_submit_transfer_cmd(NERenderer *r, VkCommandBuffer cmd) {
    if (!r || !r->transfer_cmd || cmd == VK_NULL_HANDLE) {
        return false;
    }

    VkResult vr = vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkEndCommandBuffer (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    vr = vkResetFences(r->device, 1, &r->transfer_fence);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkResetFences (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    VkSubmitInfo submit_info;
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vr = vkQueueSubmit(r->queue, 1, &submit_info, r->transfer_fence);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkQueueSubmit (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    vr = vkWaitForFences(r->device, 1, &r->transfer_fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkWaitForFences (transfer) failed (vr=%d)", (int)vr);
        return false;
    }

    return true;
}

uint32_t ne_vk_find_memory_type(NERenderer *r, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    if (!r) {
        return UINT32_MAX;
    }

    for (uint32_t i = 0; i < r->mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (r->mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

void ne_cmd_transition_image_layout(const NERenderer *renderer, const VkCommandBuffer cmd, const NEImageHandle handle, const VkImageLayout oldLayout, const VkImageLayout newLayout) {
    if (!ne_image_handle_valid(handle)) {
        NE_LOG_ERROR("ne_cmd_transition_image_layout: INVALID IMAGE HANDLE/n");
        return;
    }

    uint32_t slot_index = handle - 1; /* Convert handle to index */
    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

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

    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}
