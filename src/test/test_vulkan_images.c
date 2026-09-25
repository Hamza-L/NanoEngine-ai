/* Exercise image creation/update with Vulkan calls replaced by recording
 * stubs. This catches bad copy extents, layout order, and cleanup decisions. */
#include "internal/ne_vulkan_images.h"
#include "internal/ne_vulkan_buffers.h"
#include "ne_renderer_buffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned creates, copies, transitions, retired;
static bool fail_view;
static VkImageLayout old_layouts[8], new_layouts[8];
static unsigned char staging[64];

static VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo *i,
    const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a;
    assert(i->extent.width == 2 && i->extent.height == 2);
    assert(i->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    creates++;
    *out = (VkImage)(uintptr_t)1;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo *i,
    const VkAllocationCallbacks *a, VkImageView *out) {
    (void)d; (void)i; (void)a;
    if (fail_view) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkImageView)(uintptr_t)2;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL requirements(VkDevice d, VkImage i, VkMemoryRequirements *out) {
    (void)d; (void)i;
    *out = (VkMemoryRequirements){.size = 16, .alignment = 4, .memoryTypeBits = 1};
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d, const VkMemoryAllocateInfo *i,
    const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)d; (void)i; (void)a;
    *out = (VkDeviceMemory)(uintptr_t)3;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL bind_image(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) {
    (void)d; (void)i; (void)m; (void)o;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer c, const VkCommandBufferBeginInfo *i) {
    (void)c; (void)i;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer c, VkCommandBufferResetFlags f) {
    (void)c; (void)f;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL copy(VkCommandBuffer c, VkBuffer b, VkImage i,
    VkImageLayout l, uint32_t n, const VkBufferImageCopy *regions) {
    (void)c; (void)b; (void)i;
    assert(l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n == 1);
    assert(regions->imageExtent.width == 2 && regions->imageExtent.height == 2);
    assert(regions->imageExtent.depth == 1);
    copies++;
}
PFN_vkCreateImage vkCreateImage = create_image;
PFN_vkCreateImageView vkCreateImageView = create_view;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = requirements;
PFN_vkAllocateMemory vkAllocateMemory = allocate;
PFN_vkBindImageMemory vkBindImageMemory = bind_image;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = begin;
PFN_vkResetCommandBuffer vkResetCommandBuffer = reset;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy;

uint32_t ne_vk_find_memory_type(NERenderer *r, uint32_t filter, VkMemoryPropertyFlags flags) {
    (void)r; (void)filter; (void)flags;
    return 0;
}
bool ne_vk_ensure_staging_buffer(NERenderer *r, uint32_t size) {
    assert(size <= sizeof(staging));
    r->staging_mapped = staging;
    return true;
}
bool ne_vk_submit_transfer_cmd(NERenderer *r, VkCommandBuffer c) {
    (void)r; (void)c;
    return true;
}
void ne_cmd_transition_image_layout(const NERenderer *r, VkCommandBuffer c, NEImageHandle h,
    VkImageLayout old, VkImageLayout next) {
    (void)r; (void)c; (void)h;
    assert(transitions < 8);
    old_layouts[transitions] = old;
    new_layouts[transitions++] = next;
}
void ne_vk_buffer_slot_retire(NERenderer *r, uint32_t index) {
    retired++;
    ne_pool_free(&r->buffers, index, sizeof(NEVulkanBufferSlot));
}

int main(void) {
    NERenderer renderer = {0};
    unsigned char pixels[16] = {42};
    NEImageDesc desc = {.width = 2, .height = 2, .format = NE_IMAGE_FORMAT_RGBA,
        .usage = NE_BUFFER_USAGE_IMAGE_SAMPLED, .initial_data = pixels};
    NEImageHandle image = ne_image_create(&renderer, &desc);
    assert(image && copies == 1 && memcmp(staging, pixels, 16) == 0);
    assert(old_layouts[0] == VK_IMAGE_LAYOUT_UNDEFINED);
    assert(new_layouts[1] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pixels[0] = 24;
    ne_image_update(&renderer, image, pixels, 16);
    assert(copies == 2 && staging[0] == 24);
    assert(old_layouts[2] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(new_layouts[3] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ne_image_update(&renderer, image, pixels, 1);
    ne_image_update(&renderer, UINT64_MAX, pixels, 16);
    assert(copies == 2);
    ne_image_destroy(&renderer, image);
    assert(retired == 1);
    fail_view = true;
    assert(!ne_image_create(&renderer, &desc));
    assert(retired == 2 && renderer.buffers.count == 0 && copies == 2);
    unsigned before = creates;
    desc.width = UINT32_MAX;
    assert(!ne_image_create(&renderer, &desc) && creates == before);
    desc.width = 2;
    desc.usage = NE_BUFFER_USAGE_IMAGE_STORAGE;
    desc.srgb = true;
    assert(!ne_image_create(&renderer, &desc) && creates == before);
    desc.srgb = false;
    fail_view = false;
    image = ne_image_create(&renderer, &desc);
    assert(image && new_layouts[5] == VK_IMAGE_LAYOUT_GENERAL);
    ne_image_update(&renderer, image, pixels, 16);
    assert(old_layouts[6] == VK_IMAGE_LAYOUT_GENERAL);
    assert(new_layouts[7] == VK_IMAGE_LAYOUT_GENERAL);
    ne_image_destroy(&renderer, image);
    assert(renderer.buffers.count == 0);
    ne_pool_destroy(&renderer.buffers);
    puts("Vulkan image upload tests passed");
    return 0;
}
