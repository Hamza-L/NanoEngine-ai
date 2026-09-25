/* GPU-independent regression tests for Vulkan retirement ordering.
 * Only completion and submission are mocked; the retirement code is real. */
#include "internal/ne_vulkan_renderer.h"

#include <assert.h>
#include <stdio.h>

static bool recording;
static bool fail_submit;
static bool signaled[32];
static uintptr_t next_fence = 1;
static VkFence last_fence;
static unsigned submissions;
static unsigned releases;
static unsigned released_sum;

bool ne_vk_has_active_frames(const NERenderer *r) {
    (void)r;
    return recording;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_fence(
    VkDevice device, const VkFenceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device; (void)info; (void)allocator;
    assert(next_fence < 32);
    *fence = (VkFence)next_fence++;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_fence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    signaled[(uintptr_t)fence] = false;
}

static VKAPI_ATTR VkResult VKAPI_CALL wait_fences(
    VkDevice device, uint32_t count, const VkFence *fences,
    VkBool32 all, uint64_t timeout) {
    (void)device;
    assert(count == 1 && all && timeout == 0); /* collection never blocks */
    return signaled[(uintptr_t)*fences] ? VK_SUCCESS : VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL submit(
    VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    (void)queue;
    assert(count == 0 && info == NULL && !recording);
    if (fail_submit) return VK_ERROR_OUT_OF_HOST_MEMORY;
    submissions++;
    last_fence = fence;
    return VK_SUCCESS;
}

PFN_vkCreateFence vkCreateFence = create_fence;
PFN_vkDestroyFence vkDestroyFence = destroy_fence;
PFN_vkWaitForFences vkWaitForFences = wait_fences;
PFN_vkQueueSubmit vkQueueSubmit = submit;
/* Other helpers in the same source file are not called by this test. */
PFN_vkEndCommandBuffer vkEndCommandBuffer;
PFN_vkResetFences vkResetFences;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;

static void release_resource(NERenderer *r, void *data) {
    (void)r;
    releases++;
    released_sum += *(unsigned *)data;
}

int main(void) {
    NERenderer renderer = {0};
    unsigned resource = 7;
    recording = true;
    assert(ne_vk_retire(&renderer, release_resource, &resource, sizeof(resource)));
    resource = 99; /* retirement owns a snapshot, not a pointer to the old slot */
    ne_vk_collect_retired(&renderer, false);
    assert(submissions == 0 && releases == 0);

    recording = false;
    ne_vk_collect_retired(&renderer, false);
    assert(submissions == 1 && releases == 0);
    ne_vk_collect_retired(&renderer, false);
    assert(releases == 0);
    signaled[(uintptr_t)last_fence] = true;
    ne_vk_collect_retired(&renderer, false);
    assert(releases == 1 && released_sum == 7);

    resource = 2;
    assert(ne_vk_retire(&renderer, release_resource, &resource, sizeof(resource)));
    assert(ne_vk_retire(&renderer, release_resource, &resource, sizeof(resource)));
    ne_vk_collect_retired(&renderer, false);
    assert(submissions == 2 && releases == 1); /* one marker for both */
    VkFence earlier = last_fence;
    recording = true;
    resource = 3;
    assert(ne_vk_retire(&renderer, release_resource, &resource, sizeof(resource)));
    signaled[(uintptr_t)earlier] = true;
    ne_vk_collect_retired(&renderer, false);
    assert(releases == 3 && submissions == 2); /* earlier batch can retire */

    recording = false;
    fail_submit = true;
    ne_vk_collect_retired(&renderer, false);
    assert(releases == 3 && renderer.retired_pending != NULL);
    fail_submit = false;
    ne_vk_collect_retired(&renderer, false);
    assert(submissions == 3 && releases == 3);
    signaled[(uintptr_t)last_fence] = true;
    ne_vk_collect_retired(&renderer, false);
    assert(releases == 4 && released_sum == 14);

    resource = 5;
    assert(ne_vk_retire(&renderer, release_resource, &resource, sizeof(resource)));
    ne_vk_collect_retired(&renderer, true); /* caller has already drained GPU */
    assert(releases == 5 && released_sum == 19);
    assert(!renderer.retired_pending && !renderer.retired_batches);
    puts("Vulkan retirement ordering tests passed");
    return 0;
}
