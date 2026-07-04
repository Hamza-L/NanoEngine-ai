# Swapchain Abstraction — Design

## Problem

The Vulkan WSI (`vkCreateSwapchainKHR`) delegates presentation to the NVIDIA driver. When the driver uses its "DXGI Layered" path internally, it creates a DXGI swapchain with `DXGI_ALPHA_MODE_IGNORE`, which kills transparency. We have no way to configure it.

## Goal

Own the presentation path ourselves. Create a swapchain abstraction that can be backed by:
1. **Vulkan WSI** — current behavior, wraps `vkCreateSwapchainKHR`
2. **DXGI + DirectComposition** — we create the DXGI swapchain, import buffers into Vulkan

From the renderer's perspective, both paths expose the same interface: acquire an image, render to it, present it.

## Where It Fits

```
Before (current):
─────────────────

NERenderSurface
 ├── VkSurfaceKHR
 ├── NESwapchain (struct, embedded)
 │    ├── VkSwapchainKHR         ◀── Vulkan owns this
 │    ├── VkImage[]
 │    ├── VkImageView[]
 │    ├── VkFramebuffer[]
 │    └── sync primitives
 ├── VkRenderPass
 ├── cmd_pool / cmds[]
 └── per-frame sync (fences, semaphores)


After (proposed):
─────────────────

NERenderSurface
 ├── NESwapchain *swapchain       ◀── pointer to backend-specific impl
 │    │
 │    ├── [Vulkan WSI backend]
 │    │    ├── VkSurfaceKHR
 │    │    ├── VkSwapchainKHR
 │    │    ├── VkImage[]
 │    │    ├── VkImageView[]
 │    │    └── per-image semaphores
 │    │
 │    └── [DXGI backend]
 │         ├── IDXGISwapChain1
 │         ├── IDCompositionDevice / Target / Visual
 │         ├── Shared VkImage[] (imported from DXGI buffers)
 │         ├── VkImageView[]
 │         └── Shared fence (keyed mutex or D3D fence)
 │
 ├── VkRenderPass
 ├── VkFramebuffer[]              ◀── built from swapchain's image views
 ├── cmd_pool / cmds[]
 └── per-frame sync (fences, semaphores)
```

## The Interface

The swapchain exposes exactly what the renderer needs — nothing more:

```
┌─────────────────────────────────────────────────────────────────────┐
│  NESwapchain  (opaque, backend-specific)                            │
│                                                                     │
│  Lifecycle:                                                         │
│    create(renderer, window, desc)  → NESwapchain*                   │
│    destroy(swapchain)                                               │
│    recreate(swapchain)             → bool  (on resize)              │
│                                                                     │
│  Per-frame:                                                         │
│    acquire(swapchain, signal_sem)  → image_index or FAILED          │
│    present(swapchain, wait_sem)    → OK, OUT_OF_DATE, or FAILED     │
│                                                                     │
│  Query:                                                             │
│    get_image(swapchain, index)     → VkImage                        │
│    get_image_view(swapchain, index)→ VkImageView                    │
│    get_format(swapchain)           → VkFormat                       │
│    get_extent(swapchain)           → VkExtent2D                     │
│    get_image_count(swapchain)      → uint32_t                       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## What Each Backend Does

### Vulkan WSI Backend (extract of current code)

```
create:
  vkCreateSwapchainKHR(...)
  vkGetSwapchainImagesKHR(...)
  create image views

acquire:
  vkAcquireNextImageKHR(swapchain, signal_semaphore)
  → returns image index
  → semaphore signals when image is ready for rendering

present:
  vkQueuePresentKHR(queue, wait_semaphore, swapchain, image_index)
  → waits for rendering to finish, then flips

recreate:
  destroy old → create new (with oldSwapchain for recycling)
```

### DXGI + DirectComposition Backend (new)

```
create:
  1. Create D3D11 device (lightweight, only for DXGI ownership)
  2. Create DXGI swapchain via CreateSwapChainForComposition:
       - AlphaMode = PREMULTIPLIED    ◀── transparency works
       - SwapEffect = FLIP_SEQUENTIAL
       - Format = B8G8R8A8_UNORM
  3. Set up DirectComposition:
       - DCompositionCreateDevice()
       - CreateTargetForHwnd(hwnd)
       - CreateVisual() → SetContent(swapchain) → SetRoot(visual) → Commit()
  4. For each back buffer:
       - QueryInterface(IDXGIResource1)
       - CreateSharedHandle(DXGI_SHARED_RESOURCE_READ | WRITE)
       - Import into Vulkan:
           VkImportMemoryWin32HandleInfoKHR → VkAllocateMemory
           VkBindImageMemory → now Vulkan can render to it
       - Create VkImageView

acquire:
  IDXGISwapChain3::GetCurrentBackBufferIndex()
  → returns image index (no GPU semaphore — DXGI tracks this internally)
  → we signal our own semaphore/fence to match the Vulkan WSI interface

present:
  1. Ensure Vulkan rendering is complete (fence wait or shared sync)
  2. IDXGISwapChain1::Present1(syncInterval, flags)
  → DXGI flips the buffer, DWM composites with alpha

recreate:
  IDXGISwapChain::ResizeBuffers(...)
  Re-import shared handles for new back buffers
```

## Synchronization — The Hard Part

The two backends have fundamentally different sync models:

```
Vulkan WSI sync (automatic):
─────────────────────────────
  AcquireNextImage ──signals──▶ sem_image_available
                                     │
                                     ▼ (GPU waits)
                               render commands
                                     │
                                     ▼ (GPU signals)
  QueuePresent ◀──waits────── sem_render_finished

  The driver handles all GPU-GPU sync internally.


DXGI sync (we must manage):
───────────────────────────
  GetCurrentBackBufferIndex() ← CPU call, no GPU sync

  Problem: how does the GPU know the buffer is safe to render into?
  Answer: DXGI flip model guarantees the buffer returned by
          GetCurrentBackBufferIndex is not being read by DWM
          (it's already been presented and released).

  Problem: how does DXGI know Vulkan is done rendering?
  Answer: we need explicit sync before calling Present1().

  Two options:

  Option A — CPU fence wait (simple, adds ~0.1ms latency):
    vkQueueSubmit(cmd, fence)
    vkWaitForFences(fence)        ← CPU blocks until GPU done
    IDXGISwapChain::Present1()    ← safe, GPU is done

  Option B — Shared fence (zero extra latency, more complex):
    Create ID3D11Fence (shared)
    Import as VkSemaphore via VK_KHR_external_semaphore_win32
    vkQueueSubmit(cmd, signal: shared_semaphore)
    ID3D11DeviceContext4::Wait(fence, value)
    IDXGISwapChain::Present1()

  We start with Option A. It works and is easy to verify.
  Option B is an optimization for later.
```

## File Layout

```
src/renderer/vulkan/
├── ne_renderer_vulkan.c              # Main renderer (uses swapchain via interface)
├── ne_swapchain.h                    # Internal swapchain interface (function table)
├── ne_swapchain_vulkan_wsi.c         # Backend: Vulkan WSI (extracted from current code)
└── ne_swapchain_dxgi.c               # Backend: DXGI + DComp + external memory import
```

## How the Renderer Changes

The renderer currently has swapchain logic scattered through:
- `ne_vk_swapchain_create()` (~300 lines)
- `ne_vk_swapchain_cleanup()`
- `ne_renderer_begin_frame()` (acquire + recreation)
- `ne_renderer_end_frame()` (present)

After refactoring, these become thin wrappers around the swapchain interface:

```c
// ne_renderer_begin_frame (simplified)
NERenderPass *ne_renderer_begin_frame(NERenderer *r, NERenderSurface *surface) {
    // ...
    if (surface->swapchain->needs_recreate) {
        surface->swapchain->ops->recreate(surface->swapchain);
        rebuild_framebuffers(surface);
    }

    uint32_t image_index = surface->swapchain->ops->acquire(
        surface->swapchain,
        surface->sem_image_available[frame]);

    if (image_index == NE_SWAPCHAIN_ACQUIRE_FAILED) {
        surface->swapchain->needs_recreate = true;
        return NULL;
    }
    // ... begin command buffer, begin render pass using framebuffer[image_index]
}

// ne_renderer_end_frame (simplified)
void ne_renderer_end_frame(NERenderer *r, NERenderSurface *surface) {
    // ... end render pass, end command buffer, queue submit ...

    NESwapchainPresentResult result = surface->swapchain->ops->present(
        surface->swapchain,
        surface->sc_sem_render_finished[image_index]);

    if (result == NE_SWAPCHAIN_OUT_OF_DATE) {
        surface->swapchain->needs_recreate = true;
    }
}
```

## User-Facing API Change

One new field in `NERenderSurfaceDesc`:

```c
typedef enum NEPresentBackend {
    NE_PRESENT_BACKEND_DEFAULT = 0,   // let the engine choose (Vulkan WSI)
    NE_PRESENT_BACKEND_VULKAN_WSI,    // explicit: use vkCreateSwapchainKHR
    NE_PRESENT_BACKEND_DXGI,          // explicit: our DXGI + DComp swapchain
} NEPresentBackend;

typedef struct NERenderSurfaceDesc {
    bool vsync;
    float clear_color_rgba[4];
    NEPresentBackend present_backend;  // ◀── NEW (default = Vulkan WSI)
} NERenderSurfaceDesc;
```

Usage stays identical for the common case. Opting into DXGI is one field:

```c
NERenderSurface *surface = ne_renderer_create_surface(renderer, window,
    &(NERenderSurfaceDesc){
        .vsync = true,
        .clear_color_rgba = {0.0f, 0.0f, 0.0f, 0.0f},  // transparent!
        .present_backend = NE_PRESENT_BACKEND_DXGI,
    });
```

## Required Vulkan Extensions (DXGI backend)

```
VK_KHR_external_memory              (core in 1.1)
VK_KHR_external_memory_win32        (import DXGI shared handles)
VK_KHR_external_semaphore           (core in 1.1, for Option B sync)
VK_KHR_external_semaphore_win32     (for Option B sync)
```

These are universally supported on NVIDIA and AMD on Windows.
Note: when using the DXGI backend, we do NOT need `VK_KHR_surface` or
`VK_KHR_swapchain` — we're bypassing Vulkan's WSI entirely for that surface.

## Implementation Order

```
Phase 1: Extract
  └── Pull current swapchain code into ne_swapchain_vulkan_wsi.c
      behind the NESwapchain interface. Renderer uses the interface.
      Behavior is identical to today. ← verify nothing broke

Phase 2: DXGI skeleton
  └── Implement ne_swapchain_dxgi.c with D3D11 device creation,
      DXGI swapchain creation, DirectComposition setup.
      Present a solid color to verify the window composites correctly.

Phase 3: Vulkan import
  └── Import DXGI back buffers into Vulkan via shared handles.
      Render the spinning triangle through the DXGI path.
      Use CPU fence wait for sync (Option A).

Phase 4: Polish
  └── Handle resize (ResizeBuffers + re-import)
      Error handling, fallback to Vulkan WSI if DXGI creation fails
      Optional: shared fence sync (Option B) for lower latency
```
