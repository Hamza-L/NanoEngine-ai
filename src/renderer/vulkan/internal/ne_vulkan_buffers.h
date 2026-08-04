#ifndef INTERNAL_NE_VULKAN_BUFFERS_H
#define INTERNAL_NE_VULKAN_BUFFERS_H

#include "stdint.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan/vulkan.h"

#include "ne_vulkan_globals.h"

#include "ne_renderer_image.h"

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

    VkBuffer dyn_buffers[NE_VK_MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory dyn_memories[NE_VK_MAX_FRAMES_IN_FLIGHT];
    void *dyn_mapped[NE_VK_MAX_FRAMES_IN_FLIGHT];
} NEVulkanBufferSlot;

bool ne_vk_ensure_staging_buffer(NERenderer *r, uint32_t required_size);

VkBuffer ne_vk_buffer_for_frame(const NEVulkanBufferSlot *slot, uint32_t frame_index);

void ne_vk_buffer_slot_free(NERenderer *r, NEVulkanBufferSlot *slot);

void ne_buffer_destroy_all(NERenderer *renderer);

#endif //INTERNAL_NE_VULKAN_BUFFERS_H
