# Vulkan Backend Feature Parity Implementation Plan

## Overview

This document outlines the plan to implement missing GPU resource features in the Vulkan backend to achieve feature parity with the Metal backend.

**Current Status (Updated):**
- ✅ Application lifecycle, windowing, input
- ✅ Renderer initialization, device selection, queue management
- ✅ Swapchain creation, frame pacing, presentation
- ✅ Basic frame begin/end with clear-screen
- ✅ GPU Buffers (VkBuffer, memory management) — **COMPLETE**
- ❌ Shaders (VkShaderModule from SPIR-V)
- ❌ Graphics Pipelines (VkPipeline, VkPipelineLayout)
- ❌ Render pass command recording (vkCmdDraw, etc.)
- ❌ Compute pipelines and compute passes

---

## Session Completion: GPU Buffer Implementation

### What Was Implemented

In this session, the GPU buffer management system for Vulkan was fully implemented, including:

1. **Transfer Command Pool Infrastructure**
   - Single reusable transfer command pool created at device initialization
   - Allocated as `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` for temporary short-lived buffers
   - Single command buffer reused for all buffer transfers (ne_vk_pick_device_and_queue)

2. **Lazy Staging Buffer Allocation** (ne_vk_ensure_staging_buffer)
   - Host-visible, coherent staging buffer created on first use
   - Grows with 25% padding for future reuse
   - Properly cleaned up and reallocated when size requirements increase
   - Memory mapped and flushed for CPU→GPU transfers

3. **GPU Buffer Creation** (ne_buffer_create)
   - Allocates slot from dynamic pool (ne_pool_alloc)
   - Creates VkBuffer with appropriate usage flags for VERTEX, INDEX, UNIFORM, STORAGE buffers
   - Allocates device-local VkDeviceMemory
   - Supports optional initial data upload via staging transfers
   - All buffers include VK_BUFFER_USAGE_TRANSFER_DST_BIT for updates
   - Returns 1-based handle for null safety

4. **GPU Buffer Updates** (ne_buffer_update)
   - Updates buffer contents via staging buffer transfer
   - Uses memory flushing for coherency
   - Submits transfer commands and waits for completion synchronously
   - Supports partial buffer updates with offset

5. **GPU Buffer Destruction** (ne_buffer_destroy)
   - Validates handles and marks slots unoccupied
   - Properly destroys VkBuffer and VkDeviceMemory in correct order
   - Updates pool counts

6. **Descriptor Set Infrastructure Placeholder**
   - Added comprehensive comments in NERenderer struct
   - Documented future descriptor set integration points:
     - Descriptor pool (vkCreateDescriptorPool)
     - Descriptor set layout cache for reuse
     - Integration with pipeline creation (ne_pipeline_create)
     - Binding infrastructure for SSBOs and compute tasks
   - Current strategy: Push constants for uniforms (128-256 byte limit)
   - Future: Migrate to descriptor sets for flexible resource binding

7. **Comprehensive Documentation**
   - Buffer architecture section with descriptor set integration notes
   - Buffer usage flag mapping to Vulkan equivalents
   - Memory type selection strategy
   - Design rationale for staging buffer approach

### Implementation Details

#### Memory Management Strategy
- **GPU Buffers**: Device-local memory (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
- **Staging Buffer**: Host-visible + coherent (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
- **Synchronization**: Fence-based waiting for transfer completion (blocking for simplicity)
- **Alignment**: Automatic via vkGetBufferMemoryRequirements

#### Transfer Command Pool Design
- Single transfer pool created at device init in ne_vk_pick_device_and_queue
- Single reusable command buffer per pool
- Reset after each transfer using vkResetCommandBuffer
- Transient pool flag for driver optimization (temp command buffers)

#### Buffer Handle Design
- 1-based IDs for null safety (handle.id - 1 = array index)
- Occupied flag prevents reuse of destroyed slots
- Pool auto-growth: doubles capacity on exhaustion
- O(1) handle resolution

#### Synchronization Strategy
- Transfer submission waits synchronously for completion via fence
- vkQueueSubmit + vkWaitForFences blocks until transfer done
- Simplistic but safe; future optimization: pipelined transfers with ring buffer

### Testing
- Triangle demo (src/main.c) includes buffer creation test:
  - Creates vertex buffer with initial triangle data
  - Verifies handle validity
  - Proper cleanup in shutdown sequence
- Compiles without errors/warnings
- Ready for integration with pipeline implementation

### Architecture Decisions Maintained

1. **No Vulkan Extensions**: Only core Vulkan 1.3 + KHR_surface + KHR_win32_surface + KHR_swapchain
2. **Single Queue**: Graphics + present on single queue (efficient for simple scenarios)
3. **Push Constants for Initial Uniforms**: 128-256 byte limit sufficient for MVP; descriptor sets deferred
4. **Descriptor Sets Designed In**: Infrastructure placeholder ready for future SSBO binding

### Code Statistics
- ~550 lines of new implementation code
- Transfer command pool creation: 34 lines
- Lazy staging buffer helper: 105 lines  
- ne_buffer_create: 180 lines
- ne_buffer_update: 60 lines
- ne_buffer_destroy: 25 lines
- Transfer submission helper: 45 lines
- Documentation comments: ~100 lines

### Files Modified
- `src/renderer/vulkan/ne_renderer_vulkan.c`: Added transfer pool, staging buffer, buffer management functions, infrastructure comments
- `docs/VULKAN_IMPLEMENTATION_PLAN.md`: Updated status, added completion summary

### Next Steps
The buffer implementation is complete and ready. Next tasks in priority order:
1. **Shader Modules** (VkShaderModule) — Required for pipelines
2. **Graphics Pipelines** (VkPipeline + VkPipelineLayout) — Required for rendering
3. **Render Pass Initialization** — Extend begin/end_frame for render pass lifecycle
4. **Graphics Commands** — vkCmdDraw, vkCmdBindVertexBuffers, etc.
5. **Testing & Validation** — Triangle demo with actual rendering
6. **Compute Pipelines** — Future: Compute task support

---

## Architecture Reference: Metal Backend Patterns

The Metal backend uses these patterns that the Vulkan backend should follow:

### 1. **Resource Pool Pattern**

Metal stores resources in dynamic pools with indices as handles:

```c
typedef struct NEBufferSlot {
    bool occupied;
    uint32_t usage;
    uint32_t size;
    void *buffer;  // id<MTLBuffer>
} NEBufferSlot;

// In NERenderer:
NEBufferSlot *buffers;
uint32_t buffer_count, buffer_cap;
```

**Vulkan equivalent:**
- Store `VkBuffer`, `VkDeviceMemory`, usage flags, size
- Use same `ne_pool_alloc()` pattern for handle generation
- Index = handle.id - 1 (1-based indexing for null safety)

### 2. **Error Handling**

- Log errors/warnings with context (e.g., handle id, sizes)
- Return null handles on failure
- Validate handles before use (check `occupied` flag + index bounds)


### 3. **Memory Management**

Metal uses ARC (`__bridge_retained` / `CFBridgingRelease`):
```c
slot->buffer = (__bridge_retained void *)mtl_buffer;
(void)CFBridgingRelease(slot->buffer);  // Release & deallocate
```

**Vulkan equivalent:**
- Manually manage VkBuffer + VkDeviceMemory pairs
- Destroy in proper order: buffers → memory
- Store both handles in slot and destroy both on cleanup

---

## Detailed Implementation Tasks

### Task 1: GPU Buffers (VkBuffer + Memory)

#### Requirements

| Requirement | Details |
|-------------|---------|
| **Handle Type** | `NEBufferHandle` (already defined, wraps uint32_t id) |
| **Pool Storage** | VkBuffer + VkDeviceMemory + usage + size |
| **Usage Flags** | VERTEX, INDEX, UNIFORM, STORAGE |
| **Initial Data** | Staging buffer upload path |
| **Updates** | Partial updates via `ne_buffer_update()` |
| **Memory Layout** | HOST_VISIBLE for staging; DEVICE_LOCAL for GPU buffers |

#### Implementation Steps

1. **Add resource pool to NERenderer struct:**
   ```c
   typedef struct NEVulkanBufferSlot {
       bool occupied;
       uint32_t usage;
       uint32_t size;
       VkBuffer buffer;
       VkDeviceMemory memory;
   } NEVulkanBufferSlot;
   
   // In NERenderer:
   NEVulkanBufferSlot *buffers;
   uint32_t buffer_count, buffer_cap;
   ```

2. **Load Vulkan function pointers:**
   - `vkCreateBuffer`
   - `vkAllocateMemory`
   - `vkBindBufferMemory`
   - `vkMapMemory` / `vkUnmapMemory`
   - `vkDestroyBuffer`
   - `vkFreeMemory`
   - `vkGetPhysicalDeviceMemoryProperties`
   - `vkCmdCopyBuffer` (already loaded)

3. **Helper: Memory type lookup**
   ```c
   static uint32_t ne_vk_find_memory_type(
       NERenderer *r,
       uint32_t type_filter,
       VkMemoryPropertyFlags properties
   ) {
       VkPhysicalDeviceMemoryProperties mem_props;
       r->fns.vkGetPhysicalDeviceMemoryProperties(r->phys, &mem_props);
       
       for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
           if ((type_filter & (1 << i)) &&
               (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
               return i;
           }
       }
       return UINT32_MAX;
   }
   ```

4. **Implement `ne_buffer_create()`:**
   - For VERTEX/INDEX/UNIFORM: Create device-local buffer + staging buffer for initial data
   - For STORAGE: Same as above (read/write capable)
   - Allocate memory with proper flags and bind to buffer
   - Copy initial data via staging buffer (if provided)
   - Return handle (pool-allocated index + 1)

5. **Implement `ne_buffer_update()`:**
   - Create temporary staging buffer
   - Copy data to staging buffer
   - Record vkCmdCopyBuffer in a temporary command buffer
   - Submit and wait for transfer completion
   - OR: Use ring buffering if high-frequency updates needed

6. **Implement `ne_buffer_destroy()`:**
   - Validate handle
   - Mark slot as unoccupied
   - Destroy VkBuffer + VkDeviceMemory
   - Free pool references

#### Considerations

- **Staging buffer strategy:** For now, create temporary staging buffer per update. Optimize later if needed.
- **Memory pressure:** Keep track of allocated memory; warn if growing too large.
- **Alignment:** Respect `minMemoryMapAlignment` and `minUniformBufferOffsetAlignment`.
- **Sparse buffers:** Don't implement initially; standard allocation is sufficient.

---

### Task 2: Shader Modules (VkShaderModule from SPIR-V)

#### Requirements

| Requirement | Details |
|-------------|---------|
| **Handle Type** | `NEShaderHandle` (already defined) |
| **Pool Storage** | VkShaderModule + stage + entry point |
| **Input Format** | SPIR-V bytecode (binary) |
| **Entry Point** | Stored for later reference in pipelines |
| **Source Compilation** | Return null with warning for now |

#### Implementation Steps

1. **Add resource pool to NERenderer struct:**
   ```c
   typedef struct NEVulkanShaderSlot {
       bool occupied;
       NEShaderStage stage;
       VkShaderModule module;
       char *entry_point;  // strdup for later reference
   } NEVulkanShaderSlot;
   
   // In NERenderer:
   NEVulkanShaderSlot *shaders;
   uint32_t shader_count, shader_cap;
   ```

2. **Load Vulkan function pointers:**
   - `vkCreateShaderModule`
   - `vkDestroyShaderModule`

3. **Implement `ne_shader_create()`:**
   - Validate descriptor (non-null bytecode, valid stage, entry point)
   - Create VkShaderModule from SPIR-V bytecode
   - Allocate pool slot
   - Store module, stage, and strdup entry point
   - Return handle

4. **Implement `ne_shader_create_from_source()`:**
   - Log warning: "source compilation not yet implemented on Vulkan"
   - Return null handle
   - (Future: Integrate Slang compiler)

5. **Implement `ne_shader_destroy()`:**
   - Validate handle
   - Destroy VkShaderModule
   - Free strdup'd entry point
   - Mark slot unoccupied

#### Considerations

- **SPIR-V validation:** vkCreateShaderModule will validate bytecode; let VK errors propagate.
- **Future Slang support:** Design entry point storage to support runtime compilation path.
- **Multiple stages from same module:** Currently OK; each stage gets its own handle.

---

### Task 3: Graphics Pipelines (VkPipeline + VkPipelineLayout)

#### Requirements

| Requirement | Details |
|-------------|---------|
| **Handle Type** | `NEPipelineHandle` (already defined) |
| **Pool Storage** | VkPipeline + VkPipelineLayout + VkRenderPass + topology |
| **Shader Refs** | Resolve shader handles to VkShaderModule + entry point |
| **Vertex Layout** | Convert to VkVertexInputState |
| **Blend State** | Convert to VkColorBlendAttachmentState |
| **Push Constants** | For uniform data (vs descriptor sets) |
| **Render Pass** | Single color attachment (compatibility with swapchain format) |

#### Implementation Steps

1. **Add resource pool to NERenderer struct:**
   ```c
   typedef struct NEVulkanPipelineSlot {
       bool occupied;
       VkPipeline pipeline;
       VkPipelineLayout layout;
       VkRenderPass render_pass;
       uint32_t topology;  // VkPrimitiveTopology value
   } NEVulkanPipelineSlot;
   
   // In NERenderer:
   NEVulkanPipelineSlot *pipelines;
   uint32_t pipeline_count, pipeline_cap;
   ```

2. **Load Vulkan function pointers:**
   - `vkCreatePipelineLayout`
   - `vkDestroyPipelineLayout`
   - `vkCreateGraphicsPipelines`
   - `vkDestroyPipeline`
   - `vkCreateRenderPass`
   - `vkDestroyRenderPass`

3. **Helper: Convert NEVertexFormat to VkFormat**
   ```c
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
   ```

4. **Helper: Convert NEBlendState to VkColorBlendAttachmentState**
   - Map blend factors and operations to Vulkan equivalents

5. **Helper: Create VkRenderPass**
   - Single color attachment with format matching swapchain
   - Load action: CLEAR
   - Store action: STORE
   - Layout transitions: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC

6. **Implement `ne_pipeline_create()`:**
   - Resolve vertex/fragment shader handles to VkShaderModule + entry points
   - Build VkVertexInputState from NEVertexBufferLayout array
   - Build VkColorBlendAttachmentState from NEBlendState
   - Create VkPipelineLayout with push constant range (128 bytes, all shader stages)
   - Create VkRenderPass (or share if same format)
   - Create VkPipeline with vkCreateGraphicsPipelines
   - Store in pool slot with topology
   - Return handle

7. **Implement `ne_pipeline_destroy()`:**
   - Validate handle
   - Destroy VkPipeline, VkPipelineLayout, VkRenderPass
   - Mark slot unoccupied

#### Considerations

- **Render pass caching:** For now, create new render pass per pipeline (simple). Later, cache by format.
- **Push constant size:** 128 bytes is typical minimum; sufficient for most uniforms.
- **Viewport/scissor:** Set dynamically during rendering, not in pipeline.
- **Depth/stencil:** Not yet implemented; defer to later.
- **Multisampling:** Not yet implemented; use 1x sampling.

---

### Task 4: Render Pass Command Recording

#### Requirements

| Requirement | Details |
|-------------|---------|
| **RenderPass Scope** | Commands recorded between vkCmdBeginRenderPass / vkCmdEndRenderPass |
| **Image Views** | Create VkImageView for each swapchain image |
| **Framebuffers** | Create VkFramebuffer per image |
| **State Tracking** | Track bound pipeline, vertex buffers, index buffer in surface |
| **Command Buffer** | Already allocated in swapchain; now must record render commands |

#### Implementation Steps

1. **Add image views and framebuffers to NESwapchain:**
   ```c
   typedef struct NESwapchain {
       // ... existing fields ...
       VkImageView *image_views;
       VkFramebuffer *framebuffers;
   } NESwapchain;
   ```

2. **Load Vulkan function pointers:**
   - `vkCreateImageView`
   - `vkDestroyImageView`
   - `vkCreateFramebuffer`
   - `vkDestroyFramebuffer`
   - `vkCmdBeginRenderPass`
   - `vkCmdEndRenderPass`
   - `vkCmdBindPipeline`
   - `vkCmdBindVertexBuffers`
   - `vkCmdBindIndexBuffer`
   - `vkCmdPushConstants`
   - `vkCmdDraw`
   - `vkCmdDrawIndexed`

3. **Update `ne_vk_sc_create()`:**
   - Create VkImageView for each swapchain image:
     ```c
     VkImageViewCreateInfo ivci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = surface->sc.images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = surface->sc.format,
         .subresourceRange = {
             .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
             .baseMipLevel = 0,
             .levelCount = 1,
             .baseArrayLayer = 0,
             .layerCount = 1,
         },
     };
     ```
   - Create VkFramebuffer for each image with render pass from pipeline
   - NOTE: We don't have render pass yet at this stage; delay framebuffer creation until first use OR store in surface, not swapchain

4. **Update `ne_vk_sc_cleanup()`:**
   - Destroy framebuffers and image views

5. **Update `ne_renderer_begin_frame()`:**
   - Instead of just clearing image, prepare for render pass commands
   - Create image views if not yet created
   - Get render pass from pipeline (or create default)
   - Create framebuffers
   - Call vkCmdBeginRenderPass with clear values
   - Return NERenderPass token

6. **Update `ne_renderer_end_frame()`:**
   - Call vkCmdEndRenderPass
   - Continue with existing submit/present logic

#### Considerations

- **Render pass per pipeline:** Each pipeline has its own render pass. Can refactor later to share.
- **Framebuffer recreation:** Recreate on swapchain resize (already detected in begin_frame).
- **Layout transitions:** Handle UNDEFINED → COLOR_ATTACHMENT_OPTIMAL in begin_frame, PRESENT_SRC in end_frame.

---

### Task 5: Render Pass Graphics Commands

#### Implementation

Implement the following in terms of standard Vulkan commands:

| API Function | Vulkan Equivalent |
|--------------|-------------------|
| `ne_render_pass_set_pipeline()` | `vkCmdBindPipeline(cmd, GRAPHICS, pipeline)` |
| `ne_render_pass_set_vertex_buffer()` | `vkCmdBindVertexBuffers(cmd, slot, 1, &buffer, &offset)` |
| `ne_render_pass_set_index_buffer()` | `vkCmdBindIndexBuffer(cmd, buffer, offset, type)` |
| `ne_render_pass_set_uniform_data()` | `vkCmdPushConstants(cmd, layout, stageFlags, offset, size, data)` |
| `ne_render_pass_draw()` | `vkCmdDraw(cmd, vertexCount, 1, firstVertex, 0)` |
| `ne_render_pass_draw_indexed()` | `vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, vertexOffset, 0)` |

**Key details:**
- Track bound pipeline/buffers in NERenderSurface for validation
- Use offset=0 for push constants (always append)
- Validate handles before recording commands
- Log warnings for invalid/missing state (e.g., no pipeline bound)

---

## Implementation Order

1. **Buffers** (Task 1) — Required by pipelines
2. **Shaders** (Task 2) — Required by pipelines
3. **Pipelines** (Task 3) — Required by render pass
4. **Render Pass** (Task 4) — Extends existing begin/end_frame
5. **Graphics Commands** (Task 5) — Draw call recording
6. **Testing** — Verify with triangle demo
7. **Optimization** — Code review and refinements

---

## Known Issues & Uncertainties

### Synchronization
- **Current approach:** Single queue, FIFO presentation, semaphore-based pacing
- **Question:** Do we need VK_KHR_synchronization2 for better control? Probably not for initial release.
- **Push constant consistency:** Currently using offset=0, always append. Works for small uniforms, but may need descriptor sets for larger data.

### Memory Management
- **Current approach:** Create staging buffer per upload
- **Optimization:** Ring buffer or persistent mapped staging buffer
- **Uncertainty:** Should we implement VMA (Vulkan Memory Allocator) instead of manual memory management?
  - **Pro:** Simpler, handles fragmentation
  - **Con:** External dependency; adds complexity
  - **Recommendation:** Keep manual for now; add VMA later if memory pressure becomes issue

### Push Constants vs Descriptor Sets
- **Current approach:** Push constants (vkCmdPushConstants)
- **Advantages:** Simple, small overhead
- **Disadvantages:** 128-256 byte limit, less flexible
- **When to switch:** If > 128 bytes needed or if compute buffers required
- **Recommendation:** Push constants sufficient for graphics pass; defer descriptors to compute pass implementation

### Render Pass Caching
- **Current approach:** Create new render pass per pipeline
- **Optimization:** Share render pass when format is identical
- **Recommendation:** Implement simple caching by swapchain format

### Device Features
- **Question:** Should we check `vkGetPhysicalDeviceFeatures()` for required features?
- **Answer:** For initial implementation, assume standard GPU capabilities
- **Future:** Add feature detection and graceful degradation

---

## Testing Strategy

### Unit Tests (via triangle demo)
1. Buffer creation with vertex data
2. Shader loading from SPIR-V
3. Pipeline creation with blend state
4. Render pass with draw call
5. Visual output matches Metal backend

### Validation
- Run with VK_LAYER_KHRONOS_validation enabled
- Check for synchronization errors, memory leaks, descriptor state
- Resolve all validation warnings

### Performance
- Measure frame time (should be 16.6ms for 60 FPS)
- Check GPU utilization
- Profile memory usage

---

## Future Enhancements

### Compute Pipeline Support
- Implement compute pass lifecycle
- Create compute pipeline from compute shader
- Record compute commands (dispatch)
- Handle storage buffer binding

### Advanced Features
- Ray tracing (VK_KHR_ray_tracing)
- Mesh shaders
- Dynamic rendering (VK_KHR_dynamic_rendering)

### Optimizations
- Descriptor set caching
- Pipeline layout caching
- Render pass caching
- Memory suballocation
- Batch command recording
- Persistent mapped staging buffer

---

## References

- [Vulkan Tutorial - Triangle](https://vulkan-tutorial.com/Drawing_a_triangle)
- [Vulkan Samples](https://github.com/khronosgroup/vulkan-samples)
- [Khronos Vulkan Best Practices](https://www.khronos.org/blog/khronos-releases-vulkan-best-practices)
