#include "internal/ne_vulkan_buffers.h"
#include "internal/ne_vulkan_renderer.h"

#include "ne_renderer_buffer.h"

#include "ne_alloc.h"
#include "ne_log.h"

extern PFN_vkDestroyImage vkDestroyImage;
extern PFN_vkDestroyImageView vkDestroyImageView;
extern PFN_vkFreeMemory vkFreeMemory;
extern PFN_vkMapMemory vkMapMemory;
extern PFN_vkUnmapMemory vkUnmapMemory;
extern PFN_vkCreateBuffer vkCreateBuffer;
extern PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
extern PFN_vkBindBufferMemory vkBindBufferMemory;
extern PFN_vkDestroyBuffer vkDestroyBuffer;
extern PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
extern PFN_vkAllocateMemory vkAllocateMemory;
extern PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
extern PFN_vkResetCommandBuffer vkResetCommandBuffer;

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

/* Free all Vulkan resources a slot owns. Handles the three slot shapes that
 * share this pool/union: images, dynamic buffers (N mapped copies), and static
 * buffers. Does not touch occupied/usage/size bookkeeping. */
void ne_vk_buffer_slot_free(NERenderer *r, NEVulkanBufferSlot *slot) {
    /* Image slots alias `buffer` with `image` in the union; free them as images
     * (this also releases the imageView, which the buffer path would leak). */
    if (slot->usage & (NE_BUFFER_USAGE_IMAGE_STORAGE | NE_BUFFER_USAGE_IMAGE_SAMPLED)) {
        if (slot->image != VK_NULL_HANDLE) {
            vkDestroyImage(r->device, slot->image, NULL);
            slot->image = VK_NULL_HANDLE;
        }
        if (slot->imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(r->device, slot->imageView, NULL);
            slot->imageView = VK_NULL_HANDLE;
        }
        if (slot->memory != VK_NULL_HANDLE) {
            vkFreeMemory(r->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
        }
        return;
    }

    if (slot->dynamic) {
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (slot->dyn_mapped[i]) {
                vkUnmapMemory(r->device, slot->dyn_memories[i]);
                slot->dyn_mapped[i] = NULL;
            }
            if (slot->dyn_buffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(r->device, slot->dyn_buffers[i], NULL);
                slot->dyn_buffers[i] = VK_NULL_HANDLE;
            }
            if (slot->dyn_memories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(r->device, slot->dyn_memories[i], NULL);
                slot->dyn_memories[i] = VK_NULL_HANDLE;
            }
        }
        return;
    }

    if (slot->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
    }
    if (slot->memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
    }
}

bool ne_vk_ensure_staging_buffer(NERenderer *r, uint32_t required_size) {
    if (!r) {
        return false;
    }

    /* If we already have a staging buffer with sufficient capacity, reuse it */
    if (r->staging_buffer != VK_NULL_HANDLE && r->staging_size >= required_size) {
        return true;
    }

    /* Clean up old staging buffer if it exists */
    if (r->staging_mapped) {
        vkUnmapMemory(r->device, r->staging_memory);
        r->staging_mapped = NULL;
    }
    if (r->staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
    }
    if (r->staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(r->device, r->staging_memory, NULL);
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

    VkResult vr = vkCreateBuffer(r->device, &buf_info, NULL, &r->staging_buffer);
    if (vr != VK_SUCCESS || r->staging_buffer == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateBuffer (staging) failed (vr=%d)", (int)vr);
        return false;
    }

    /* Get memory requirements for the staging buffer */
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(r->device, r->staging_buffer, &mem_req);

    /* Find a suitable memory type: host-visible and coherent for CPU mapping */
    uint32_t mem_type = ne_vk_find_memory_type(
        r,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find suitable memory type for staging buffer");
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Allocate memory for the staging buffer */
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vr = vkAllocateMemory(r->device, &alloc_info, NULL, &r->staging_memory);
    if (vr != VK_SUCCESS || r->staging_memory == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkAllocateMemory (staging) failed (vr=%d)", (int)vr);
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Bind the memory to the buffer */
    vr = vkBindBufferMemory(r->device, r->staging_buffer, r->staging_memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory (staging) failed (vr=%d)", (int)vr);
        vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    /* Map the staging memory for CPU access (use actual allocated size) */
    void *mapped_ptr = NULL;
    vr = vkMapMemory(r->device, r->staging_memory, 0, mem_req.size, 0, &mapped_ptr);
    if (vr != VK_SUCCESS || !mapped_ptr) {
        NE_LOG_ERROR("vkMapMemory (staging) failed (vr=%d)", (int)vr);
        vkFreeMemory(r->device, r->staging_memory, NULL);
        r->staging_memory = VK_NULL_HANDLE;
        vkDestroyBuffer(r->device, r->staging_buffer, NULL);
        r->staging_buffer = VK_NULL_HANDLE;
        return false;
    }

    r->staging_mapped = mapped_ptr;
    r->staging_size = mem_req.size;

    return true;
}

NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc) {
    if (!renderer || !desc || desc->size == 0) {
        NE_LOG_ERROR("ne_buffer_create: bad param");
        return NE_BUFFER_HANDLE_NULL;
    }

    if (!desc->usage) {
        NE_LOG_ERROR("buffer creation requires at least one usage flag");
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Allocate a slot from the buffer pool */
    uint32_t slot_index = ne_pool_alloc(&renderer->buffers, sizeof(NEVulkanBufferSlot));

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("failed to allocate buffer slot from pool");
        return NE_BUFFER_HANDLE_NULL;
    }

    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

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

            VkResult dvr = vkCreateBuffer(renderer->device, &dbi, NULL, &slot->dyn_buffers[i]);
            if (dvr != VK_SUCCESS || slot->dyn_buffers[i] == VK_NULL_HANDLE) {
                NE_LOG_ERROR("vkCreateBuffer (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            VkMemoryRequirements dmr;
            vkGetBufferMemoryRequirements(renderer->device, slot->dyn_buffers[i], &dmr);

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

            dvr = vkAllocateMemory(renderer->device, &dai, NULL, &slot->dyn_memories[i]);
            if (dvr != VK_SUCCESS || slot->dyn_memories[i] == VK_NULL_HANDLE) {
                NE_LOG_ERROR("vkAllocateMemory (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            dvr = vkBindBufferMemory(renderer->device, slot->dyn_buffers[i], slot->dyn_memories[i], 0);
            if (dvr != VK_SUCCESS) {
                NE_LOG_ERROR("vkBindBufferMemory (dynamic copy %u) failed (vr=%d)", i, (int)dvr);
                goto dynamic_fail;
            }

            dvr = vkMapMemory(renderer->device, slot->dyn_memories[i], 0, desc->size, 0, &slot->dyn_mapped[i]);
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

        return (NEBufferHandle){ slot_index + 1 };

    dynamic_fail:
        /* Release any dynamic resources created before the failure. */
        for (uint32_t i = 0; i < NE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
            if (slot->dyn_mapped[i]) {
                vkUnmapMemory(renderer->device, slot->dyn_memories[i]);
                slot->dyn_mapped[i] = NULL;
            }
            if (slot->dyn_memories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(renderer->device, slot->dyn_memories[i], NULL);
                slot->dyn_memories[i] = VK_NULL_HANDLE;
            }
            if (slot->dyn_buffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(renderer->device, slot->dyn_buffers[i], NULL);
                slot->dyn_buffers[i] = VK_NULL_HANDLE;
            }
        }
        slot->occupied = false;
        renderer->buffers.count--;
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

    VkResult vr = vkCreateBuffer(renderer->device, &buf_info, NULL, &slot->buffer);
    if (vr != VK_SUCCESS || slot->buffer == VK_NULL_HANDLE) {
        NE_LOG_ERROR("vkCreateBuffer failed (vr=%d, size=%u)", (int)vr, desc->size);
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Get memory requirements for the buffer */
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(renderer->device, slot->buffer, &mem_req);

    /* Find device-local memory for the GPU buffer */
    uint32_t mem_type = ne_vk_find_memory_type(
        renderer,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if (mem_type == UINT32_MAX) {
        NE_LOG_ERROR("failed to find device-local memory type for buffer");
        vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_BUFFER_HANDLE_NULL;
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
        vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* Bind memory to buffer */
    vr = vkBindBufferMemory(renderer->device, slot->buffer, slot->memory, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_ERROR("vkBindBufferMemory failed (vr=%d)", (int)vr);
        vkFreeMemory(renderer->device, slot->memory, NULL);
        slot->memory = VK_NULL_HANDLE;
        vkDestroyBuffer(renderer->device, slot->buffer, NULL);
        slot->buffer = VK_NULL_HANDLE;
        slot->occupied = false;
        renderer->buffers.count--;
        return NE_BUFFER_HANDLE_NULL;
    }

    /* If initial data is provided, stage it to the buffer */
    if (desc->initial_data) {
        if (!ne_vk_ensure_staging_buffer(renderer, desc->size)) {
            NE_LOG_ERROR("failed to ensure staging buffer for initial data");
            vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Copy initial data into the staging buffer */
        memcpy(renderer->staging_mapped, desc->initial_data, desc->size);

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
            vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Record the copy command */
        VkBufferCopy copy_region;
        memset(&copy_region, 0, sizeof(copy_region));
        copy_region.srcOffset = 0;
        copy_region.dstOffset = 0;
        copy_region.size = desc->size;

        vkCmdCopyBuffer(renderer->transfer_cmd, renderer->staging_buffer, slot->buffer, 1, &copy_region);

        /* Submit and wait for completion */
        if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
            NE_LOG_ERROR("failed to submit transfer command for initial data");
            vkFreeMemory(renderer->device, slot->memory, NULL);
            slot->memory = VK_NULL_HANDLE;
            vkDestroyBuffer(renderer->device, slot->buffer, NULL);
            slot->buffer = VK_NULL_HANDLE;
            slot->occupied = false;
            renderer->buffers.count--;
            return NE_BUFFER_HANDLE_NULL;
        }

        /* Reset the command buffer for reuse */
        vr = vkResetCommandBuffer(renderer->transfer_cmd, 0);
        if (vr != VK_SUCCESS) {
            NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
        }
    }

    /* Store metadata (slot already marked occupied by ne_pool_alloc) */
    slot->usage = desc->usage;
    slot->size = desc->size;

    /* Return handle (1-based ID for null safety) */
    NEBufferHandle handle = {slot_index + 1};
    return handle;
}

void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle,
                      const void *data, uint32_t size, uint32_t offset) {
    if (!renderer || !ne_buffer_handle_valid(handle) || !data || size == 0) {
        return;
    }

    uint32_t slot_index = handle.id - 1; /* Convert handle to index */

    if (slot_index >= renderer->buffers.cap || !((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index].occupied) {
        NE_LOG_WARN("buffer_update called on invalid or destroyed buffer handle");
        return;
    }

    NEVulkanBufferSlot *slot = &((NEVulkanBufferSlot*)renderer->buffers.slots)[slot_index];

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

    VkBufferCopy copy_region;
    memset(&copy_region, 0, sizeof(copy_region));
    copy_region.srcOffset = offset;
    copy_region.dstOffset = offset;
    copy_region.size = size;

    vkCmdCopyBuffer(renderer->transfer_cmd, renderer->staging_buffer, slot->buffer, 1, &copy_region);

    if (!ne_vk_submit_transfer_cmd(renderer, renderer->transfer_cmd)) {
        NE_LOG_ERROR("failed to submit transfer command for buffer update");
        return;
    }

    vr = vkResetCommandBuffer(renderer->transfer_cmd, 0);
    if (vr != VK_SUCCESS) {
        NE_LOG_WARN("vkResetCommandBuffer (transfer) failed (vr=%d), continuing anyway", (int)vr);
    }
}

void ne_buffer_destroy(NERenderer *renderer, NEBufferHandle handle) {
    if (!renderer || !ne_buffer_handle_valid(handle)) {
        return;
    }

    uint32_t slot_index = handle.id - 1;

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

void ne_buffer_destroy_all(NERenderer *r) {
    /* Destroy all live buffers. */
    for (uint32_t i = 0; i < r->buffers.cap; i++) {
        NEVulkanBufferSlot *bslot = &((NEVulkanBufferSlot*)r->buffers.slots)[i];
        if (bslot->occupied) {
            ne_vk_buffer_slot_free(r, bslot);
        }
    }
    ne_pool_destroy(&r->buffers);
}

VkBuffer ne_vk_buffer_for_frame(const NEVulkanBufferSlot *slot, uint32_t frame_index) {
    if (slot->dynamic) {
        return slot->dyn_buffers[frame_index % NE_VK_MAX_FRAMES_IN_FLIGHT];
    }
    return slot->buffer;
}
