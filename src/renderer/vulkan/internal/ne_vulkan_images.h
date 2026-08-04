#ifndef INTERNAL_NE_VULKAN_IMAGES_H
#define INTERNAL_NE_VULKAN_IMAGES_H

#include "internal/ne_vulkan_renderer.h"

void ne_cmd_copy_buffer_to_image(const NERenderer *renderer, const VkCommandBuffer cmd, const VkBuffer buffer, const NEImageHandle handle);

#endif //INTERNAL_NE_VULKAN_IMAGES_H
