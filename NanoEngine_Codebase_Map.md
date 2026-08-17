# NanoEngine — Codebase Map

A cross-platform C23 rendering engine. Currently ~10K LOC. Runs on Windows (Vulkan),
macOS (Metal), and the web (WebGPU) from one shared public API.

## Project Structure

```
NanoEngine/
├── include/                            # Public API — platform-neutral
│   ├── ne_alloc.h                      # Pool allocator
│   ├── ne_app.h                        # Application lifecycle
│   ├── ne_file.h                       # File I/O
│   ├── ne_frame.h                      # Frame dispatch context
│   ├── ne_handle.h                     # (empty — placeholder)
│   ├── ne_log.h                        # Logging + assertions
│   ├── ne_renderer.h                   # Renderer + surface creation
│   ├── ne_renderer_buffer.h            # GPU buffer API
│   ├── ne_renderer_image.h             # GPU image API
│   ├── ne_renderer_pass.h              # Render/compute pass commands
│   ├── ne_renderer_pipeline.h          # Graphics/compute pipeline API
│   ├── ne_renderer_shader.h            # Shader creation API
│   ├── ne_window.h                     # Window + input events
│   └── test/
│       ├── ne_test.h
│       └── ne_test_buffer.h
│
├── src/
│   ├── main.c                          # Demo (spinning triangle, DXGI transparent)
│   ├── ne_alloc.c                      # Pool allocator impl
│   ├── ne_file.c                       # File I/O impl
│   ├── ne_frame.c                      # ne_render_frame() impl (~20 lines)
│   ├── ne_log.c                        # Logger impl
│   │
│   ├── platform/
│   │   ├── win32/window_win32.c        # Win32: custom borderless, DWM, DPI
│   │   ├── macos/window_macos.m        # Cocoa + CADisplayLink
│   │   └── web/window_web.c            # Emscripten canvas + rAF loop
│   │
│   ├── renderer/
│   │   ├── vulkan/                     # Windows backend
│   │   │   ├── ne_renderer_vulkan.c    # Public entry + frame loop (1274 LOC)
│   │   │   ├── ne_swapchain.h          # Swapchain vtable (NESwapchainOps)
│   │   │   ├── ne_swapchain_vulkan_wsi.c   # Standard vkCreateSwapchainKHR path
│   │   │   ├── ne_swapchain_dxgi.c     # DXGI + DirectComposition (transparency)
│   │   │   └── internal/               # Vulkan backend, split by concern
│   │   │       ├── ne_vulkan_globals.h     # NE_VK_MAX_FRAMES_IN_FLIGHT = 2
│   │   │       ├── ne_vulkan_loader.h      # PFN globals + LoadLibrary loader
│   │   │       ├── ne_vulkan_renderer.{c,h}    # struct NERenderer, shared helpers
│   │   │       ├── ne_vulkan_buffers.{c,h}     # Buffers + image slot union
│   │   │       ├── ne_vulkan_images.{c,h}      # Images (share buffers pool)
│   │   │       ├── ne_vulkan_pipelines.{c,h}   # Deferred pipeline compile
│   │   │       └── ne_vulkan_shaders.{c,h}     # SPIR-V + glslang runtime path
│   │   │
│   │   ├── metal/ne_renderer_metal.m   # macOS backend (~1180 LOC)
│   │   └── wgpu/ne_renderer_wgpu.c     # Web backend (~1175 LOC)
│   │
│   └── test/test_buffer.c              # GPU buffer test suite
│
├── shaders/
│   ├── glsl/basic.{vert,frag}          # Compiled to SPIR-V for Vulkan
│   └── wgsl/basic.wgsl                 # WebGPU source
│
├── external/                           # Auto-fetched deps (see Build)
├── build/                              # Build output
├── Makefile                            # Neutral driver; includes one of ↓
├── MakeFile_win32.mk                   # clang + Vulkan + glslang + FreeType
├── MakeFile_macos.mk                   # clang + Cocoa/Metal/QuartzCore
└── MakeFile_emscripten.mk              # emcc + emdawnwebgpu
```

---

## Build Configuration

- **Compiler**: `clang -std=c23 -Wall -Wextra -Wpedantic -Werror`
- **ASan**: opt-in via `SANITIZE=1` (`Makefile:59`)
- **Backend selection**: **entirely by makefile** — the platform mk file adds
  exactly one renderer `.c/.m` to `SRC_C`/`SRC_M`. There is no `#ifdef` fan-out;
  each backend `#error`s if compiled on the wrong platform.
- **Test mode**: `make test` sets `TESTING_ENABLED=1` and adds `src/test/test_buffer.c`.

### External deps (auto-fetched by the makefiles)

| Platform | Deps |
|----------|------|
| Win32    | Vulkan-Headers 1.3.280, glslang 16.5.0 (built from source), FreeType 2.13.3 (CMake+MSBuild), `stb_image.h`, plus link libs `user32/gdi32/dwmapi/d3d11/dxgi` |
| macOS    | Cocoa, Metal, QuartzCore (system frameworks); `-fobjc-arc` |
| Web      | emsdk 5.0.2 (git+`emsdk install/activate`), `--use-port=emdawnwebgpu`; `basic.wgsl` preloaded |

Shader compile: on Win32, `shaders/glsl/basic.{vert,frag}` are compiled by
`glslang.exe` into `build/shaders/*.spv` at build time. Their paths are
threaded through to the C code via `-DNE_SPIRV_VERT_PATH=…`.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              main.c (Demo App)                          │
│  Creates: App → Window → Renderer → Surface → Pipeline → VBO           │
│  Registers: ne_render_frame as the window's render dispatch            │
│  Calls: ne_app_run(app) to enter the loop                              │
└─────────────────────────┬───────────────────────────────────────────────┘
                          │
     ┌────────────────────┴─────────────────────┐
     ▼                                          ▼
┌──────────────────────┐              ┌──────────────────────┐
│  NEApp (per-plat)    │              │   NEFrameContext     │
│  ─ Win32:  HINSTANCE │              │                      │
│  ─ macOS:  NSApp     │              │  renderer, surface,  │
│  ─ Web:    (single)  │              │  pipeline, VBO,      │
│                      │─── drives ──▶│  vertex_count,       │
│  intrusive list of   │              │  on_update()         │
│  NEWindow            │              │                      │
└──────────────────────┘              └──────────────────────┘
```

`NEApp` owns windows. `NERenderer` (singleton) owns render surfaces plus three
resource pools (buffers, shaders, pipelines). Each `NERenderSurface` binds one
renderer to one window.

---

## Layer 1: Platform / Windowing

Three implementations of `ne_window.h`. Each stores a per-window
`render` callback registered via `ne_set_window_render_dispatch`.

### Win32 — `src/platform/win32/window_win32.c` (863 LOC)

- **Pull model**: `ne_app_run` polls messages then invokes each window's
  `renderFrame` (`window_win32.c:547`). Also fires inline during `WM_SIZE`
  to keep content painted during the OS-blocking resize loop (`:301`).
- **Custom borderless window** (`WS_POPUP`): `WM_NCHITTEST` implements corner
  resize (radius = `BORDER_THICKNESS*4`), edge resize, 28px title-bar drag.
- **DWM integration**: `DWMWA_USE_IMMERSIVE_DARK_MODE`, `DwmExtendFrameIntoClientArea`
  with `{-1,-1,-1,-1}` for the acrylic look.
- **DPI aware**: `GetDpiForWindow` scales content → framebuffer size.
- **Input**: `WM_KEYDOWN/UP`, `WM_CHAR` (full UTF-16 surrogate pair handling),
  mouse deltas, `WM_MOUSEWHEEL` normalized to ±1.0.

### macOS — `src/platform/macos/window_macos.m` (~930 LOC)

- **Push model**: per-view `CADisplayLink` fires `onDisplayLink:` which calls
  the window's `render_frame` at display refresh. `ne_app_run` just pumps `NSApp`.
- Refcounted app singleton (`g_app_state`).

### Web — `src/platform/web/window_web.c` (~520 LOC)

- **Push model**: `emscripten_set_main_loop_arg(ne_web_frame_tick, ...)`
  drives frames; `ne_web_frame_tick` walks the window list and invokes each
  `render_frame`.

---

## Layer 2: Renderer

Three interchangeable backends. **No vtable** — the abstraction is
source-file-per-backend, selected by the makefile (see Build). Each backend
defines its own opaque `struct NERenderer` and implements the whole public API.

**Known cross-backend drift** (worth fixing):
- `ne_renderer_create` is declared `(const NERendererDesc*)` in
  `include/ne_renderer.h:82`, but Metal (`ne_renderer_metal.m:225`) and WGPU
  (`ne_renderer_wgpu.c:211`) implement it as `(NEApp*, const NERendererDesc*)`.
- `NEImageHandle` is `uint64_t` (`ne_renderer_image.h:17`) — every other handle
  is a `struct { uint32_t id; }`.

### Vulkan backend (Windows)

**Entry**: `src/renderer/vulkan/ne_renderer_vulkan.c` (1274 LOC).
**Split**: instead of one monolithic file, the backend is now split under
`internal/` by concern.

#### Dynamic loading — `internal/ne_vulkan_loader.h`

`#define VK_NO_PROTOTYPES` — no link-time Vulkan. All ~70 function pointers
live as inline globals in the loader header (`:11-117`). Boot sequence:

```
LoadLibraryA("vulkan-1.dll")
  → vkGetInstanceProcAddr
    → ne_vk_load_loader()      // global-scope PFNs (vkCreateInstance, …)
    → ne_vk_load_instance_fns()
    → ne_vk_load_device_fns()
```

Each translation unit that uses a PFN re-`extern`s just the ones it needs.

#### struct NERenderer — `internal/ne_vulkan_renderer.h`

```c
struct NERenderer {
    HMODULE vulkan_lib;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;

    NEPool buffers;    // NEVulkanBufferSlot (union: buffer | image)
    NEPool shaders;    // NEVulkanShaderSlot
    NEPool pipelines;  // NEVulkanPipelineSlot

    VkBuffer        staging_buffer;      // reusable CPU→GPU staging
    VkDeviceMemory  staging_memory;
    void           *staging_mapped;

    VkCommandPool   transfer_cmd_pool;
    VkCommandBuffer transfer_cmd;
    VkFence         transfer_fence;

    NEShaderOptimization shader_optimization;
    struct NERenderSurface *surfaces;    // intrusive list
};
```

#### struct NERenderSurface — `ne_renderer_vulkan.c:47`

```c
struct NERenderSurface {
    NERenderer  *renderer;
    NEWindow    *window;
    VkSurfaceKHR surface;
    NESwapchain *swapchain;          // pointer to interface (see below)
    VkRenderPass render_pass;

    VkCommandPool   cmd_pool;
    VkCommandBuffer cmds[2];             // per flight frame
    VkSemaphore     sem_image_available[2];
    VkFence         fences_in_flight[2];
    uint32_t        frame_index;

    bool  vsync;
    float clear_color[4];

    NERenderPass pass;                    // embedded; returned by begin_frame
    NEPresentBackend present_backend;     // chosen per surface
};
```

`needs_recreate` and the swapchain images/views live on the swapchain base, not on the surface (moved).

#### Swapchain — real vtable (`ne_swapchain.h`)

```c
typedef struct NESwapchainOps {
    void (*destroy) (NESwapchain*);
    bool (*recreate)(NESwapchain*, bool vsync);
    bool (*acquire) (NESwapchain*, VkSemaphore image_available);
    bool (*present) (NESwapchain*, VkQueue, VkSemaphore render_finished);
} NESwapchainOps;

struct NESwapchain {
    NESwapchainOps *ops;
    VkFormat  format;
    VkExtent2D extent;
    uint32_t  image_count;
    VkImage    *images;
    VkImageView *image_views;
    uint32_t  acquired_image_index;
    bool needs_recreate;
    // …
};
```

Concrete types place `NESwapchain base;` first (C-style inheritance).
Two implementations, selected **per surface** by `NERenderSurfaceDesc.present_backend`:

| Backend                        | File                            | Notes |
|--------------------------------|---------------------------------|-------|
| `NE_PRESENT_BACKEND_VULKAN_WSI`| `ne_swapchain_vulkan_wsi.c` (440 LOC) | Standard `vkCreateSwapchainKHR` + `VK_KHR_swapchain`. Present-mode preference: `FIFO` if vsync else `MAILBOX > IMMEDIATE > FIFO`. |
| `NE_PRESENT_BACKEND_DXGI`      | `ne_swapchain_dxgi.c` (777 LOC) | DXGI swapchain + DirectComposition + `VK_KHR_external_memory_win32` to import images. Enables per-pixel transparency via `DXGI_ALPHA_MODE_PREMULTIPLIED`. |

The DXGI path bypasses the standard semaphore chain and uses fence +
`vkDeviceWaitIdle` internally (`ne_renderer_vulkan.c:1017`).

#### Frame loop — `ne_renderer_vulkan.c:855-1050`

```
frame = frame_index % 2
├─ vkWaitForFences(fences_in_flight[frame])
├─ swapchain->ops->acquire(sem_image_available[frame])
├─ (WSI only) wait per-image fence to avoid image-reuse race
├─ vkResetFences
├─ Reset + Begin cmd buffer
├─ Begin render pass  (LOAD_OP_CLEAR)
├─ Set Y-flipped viewport:  y = height, height = -height
├─ (return &surface->pass to caller for its draw commands)
├─ vkCmdEndRenderPass
├─ vkQueueSubmit  (wait: image_available; signal: render_finished from swapchain)
└─ swapchain->ops->present
```

`NE_VK_MAX_FRAMES_IN_FLIGHT = 2`; `frame_index` wraps mod 2.

### Metal backend — `src/renderer/metal/ne_renderer_metal.m`

macOS-only. Full implementation of the same public API against `MTLDevice` /
`CAMetalLayer`. `-fobjc-arc`.

### WebGPU backend — `src/renderer/wgpu/ne_renderer_wgpu.c`

Web-only. Uses `emdawnwebgpu`. WGSL shaders under `shaders/wgsl/`.

---

## Layer 3: Resource Management

### Pool allocator (`ne_alloc.c`)

```
+-----------+-----------+-----------+-----------+
| slot 0    | slot 1    | slot 2    | slot 3    |
|[occ|data] |[occ|data] |[occ|data] |[occ|data] |
+-----------+-----------+-----------+-----------+
      <---- slot_size ---->
```

- First byte of every slot is `bool occupied`.
- **Allocate**: scan from `hint` → wrap → grow (double, initial 16).
- **Free**: clear occupied byte, rewind hint if slot < hint.
- **Handles**: `handle.id = slot_index + 1` (so `0` = null).

**Note**: images share the buffers pool via a union in `NEVulkanBufferSlot`.

### GPU Buffers — `ne_vulkan_buffers.c`

`NEBufferDesc.dynamic` selects strategy:

**Static** (device-local, staged upload):
```
CPU ── memcpy ──▶ renderer->staging_buffer ── vkCmdCopyBuffer ──▶ device_local
                  (host-visible, reusable)                        (GPU-only)
```
Submitted via the renderer's `transfer_cmd_pool` / `transfer_cmd` / `transfer_fence`.

**Dynamic** (per-frame host-visible copies, zero-copy update):
```
dyn_buffers[0], dyn_memories[0], dyn_mapped[0]  ←── CPU writes on frame 0
dyn_buffers[1], dyn_memories[1], dyn_mapped[1]  ←── CPU writes on frame 1
```
Persistently mapped. `ne_render_pass_update_buffer` is a pure `memcpy` into
`dyn_mapped[frame_index]`. No staging, no copy command.

**Memory allocation** is not pooled — each buffer/image calls its own
`vkAllocateMemory` (`ne_vk_find_memory_type` in `ne_vulkan_renderer.c:50`).
Destruction is immediate (surface destroy issues `vkDeviceWaitIdle`) even
though `ne_renderer_buffer.h:127` says "deferred" — the doc is wrong.

### Shaders — `ne_vulkan_shaders.c`

Two creation paths, both fully implemented:

1. **`ne_shader_create`** — takes pre-compiled SPIR-V bytecode (production).
2. **`ne_shader_create_from_source`** — takes GLSL text, compiles at runtime
   via glslang (`glslang_shader_create` → `preprocess` → `parse` → `link` →
   `SPIRV_generate`), then feeds the result through path (1).

Shader entry-point strings are strdup'd into the slot.

### Pipelines — deferred compile (`ne_vulkan_pipelines.c`)

`ne_pipeline_create` captures the recipe and returns a handle immediately.
It does **not** call `vkCreateGraphicsPipelines` — that fires from
`set_pipeline` on first use, when the `VkRenderPass` is known.

```
ne_pipeline_create():
  ├─ Resolve shader handles → VkShaderModule (stash on slot)
  ├─ Deep-copy vertex bindings/attributes + entry-point names
  ├─ Create VkPipelineLayout (128-byte push constant range up front)
  ├─ Store: needs_compile = true
  └─ Return handle

set_pipeline() (first use):
  ├─ if (slot->needs_compile)
  │     ne_vk_pipeline_compile(slot, surface->render_pass)
  │       → vkCreateGraphicsPipelines(...)
  │       → needs_compile = false
  └─ vkCmdBindPipeline(...)
```

Fixed state: cull NONE, front-face CCW, polygon FILL, dynamic VIEWPORT+SCISSOR,
no depth/stencil, no MSAA.

### Uniforms

`ne_render_pass_set_uniform_data` → `vkCmdPushConstants`. 128-byte limit
enforced (`ne_renderer_vulkan.c:1170`). No descriptor sets yet — the header
comment in `ne_vulkan_buffers.c:36` calls this out as a "future step".

---

## Layer 4: Frame Dispatch — `src/ne_frame.c`

```c
void ne_render_frame(void *user) {
    NEFrameContext *ctx = (NEFrameContext *)user;
    NERenderPass *pass = ne_renderer_begin_frame(ctx->renderer, ctx->surface);
    if (!pass) return;                            // minimized / not ready
    if (ctx->on_update) ctx->on_update(ctx, pass);
    ne_render_pass_set_pipeline    (pass, ctx->pipeline);
    ne_render_pass_set_vertex_buffer(pass, 0, ctx->vertex_buffer);
    ne_render_pass_draw            (pass, 0, ctx->vertex_count);
    ne_renderer_end_frame(ctx->renderer, ctx->surface);
}
```

Registered on the window via `ne_set_window_render_dispatch`. Called by the
platform:
- **Win32**: once per main-loop iteration; also inline during `WM_SIZE`.
- **macOS**: on `CADisplayLink` fire.
- **Web**: on `emscripten_set_main_loop_arg` tick.

---

## Demo Application — `src/main.c`

```c
int main(void) {
    // 1. App + borderless window
    NEApp *app = ne_app_create();
    NEWindow *window = ne_window_create(app, { .undecorated = true, ... });

    // 2. Vulkan renderer + DXGI-backed transparent surface
    NERenderer *renderer = ne_renderer_create(&(NERendererDesc){ .enable_validation = true });
    NERenderSurface *surface = ne_renderer_create_surface(renderer, window,
        &(NERenderSurfaceDesc){ .vsync = true,
                                .present_backend = NE_PRESENT_BACKEND_DXGI });

    // 3. Load SPIR-V shaders → pipeline
    NEShaderHandle vs = ne_shader_create(renderer, /* SPIR-V */);
    NEShaderHandle fs = ne_shader_create(renderer, /* SPIR-V */);
    NEPipelineHandle pipeline = ne_pipeline_create(renderer, /* … */);

    // 4. Dynamic vertex buffer (rotated per frame)
    NEBufferHandle vbo = ne_buffer_create(renderer, { .dynamic = true, .initial_data = triangle });

    // 5. Frame ctx + register dispatch
    NEFrameContext frame_ctx = { renderer, surface, pipeline, vbo, 3, on_frame_update, &demo_state };
    ne_set_window_render_dispatch(window, ne_render_frame, &frame_ctx);

    // 6. Run
    ne_app_run(app);

    // 7. Cleanup (reverse order)
}
```

`on_frame_update` rotates the triangle vertices and pushes them via
`ne_render_pass_update_buffer` (dynamic buffer path).

---

## Shaders

**GLSL** (Vulkan, `shaders/glsl/`):

```glsl
// basic.vert
#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 frag_color;
void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_color  = in_color;
}
```

**WGSL** (WebGPU, `shaders/wgsl/basic.wgsl`) is the same shader translated for
the web backend. GLSL sources are compiled to SPIR-V at build time by
`glslang.exe`; the resulting paths are baked in via `-DNE_SPIRV_*_PATH`.

---

## Logging — `include/ne_log.h`, `src/ne_log.c`

- Levels: `TRACE < DEBUG < INFO < WARN < ERROR < FATAL < OFF`.
- Default sink: stdout (`< WARN`) / stderr (`>= WARN`).
- Custom sinks and loggers supported (`ne_logger_*`).
- Formatting uses a 1024-byte stack buffer, heap fallback for larger messages.
- `NE_ASSERT(cond)` logs FATAL + `abort()` in debug builds.

---

## Testing

Compile-time `TESTING_ENABLED=1` (`make test`) opens a separate window
"NanoEngine2 — Tests" and runs `test_buffer()` — create/destroy, initial data,
partial updates, usage-flag matrix. Exits 0 (pass) / 1 (fail).

---

## Key Design Patterns

| Pattern                         | Where |
|---------------------------------|-------|
| Opaque types + handle IDs       | All renderer resources (buffers, shaders, pipelines) |
| Pool allocator (index = handle−1) | `NEPool` in `ne_alloc.c` |
| Deferred pipeline compilation   | Fires on first `set_pipeline` (needs render pass) |
| Double-buffered dynamic buffers | `dyn_buffers[MAX_FRAMES_IN_FLIGHT]` |
| Swapchain vtable                | `NESwapchainOps` (WSI vs DXGI) chosen per-surface |
| Pull-based swapchain resize     | Framebuffer size queried in `swapchain->ops->recreate` |
| Platform-agnostic frame dispatch| `void (*)(void*)` callback registered on the window |
| Singleton renderer              | `g_renderer_singleton` (enforced) |
| Intrusive linked lists          | `NEApp::windows`, `NERenderer::surfaces` |
| Runtime shader compilation      | glslang (Vulkan); direct WGSL (WebGPU); MSL (Metal) |
| Deep copy on capture            | Pipeline creation clones vertex layout + entry-point strings |

---

## Compute Pipeline — stub

The API declares compute pipelines and passes, but the Vulkan backend still
stubs them:

```c
NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass) {
    NE_LOG_WARN("compute passes not yet implemented on Vulkan");
    return NULL;
}
```

`ne_compute_pipeline_create` allocates a slot + layout but never compiles.

---

## Known Inconsistencies

Documented here so they don't get lost:

1. **`ne_renderer_create` signature drift** — header takes `(const NERendererDesc*)`; Metal/WGPU take `(NEApp*, const NERendererDesc*)`.
2. **`NEImageHandle` is `uint64_t`**, breaking the `struct { uint32_t id; }` convention used by every other handle.
3. **`ne_vulkan_shaders.c:41` comment** describes `create_from_source` as a "no-op stub" — the function is fully implemented via glslang.
4. **`ne_renderer_buffer.h:127` comment** claims destruction is deferred until GPU idle — it is actually immediate (surface destroy issues `vkDeviceWaitIdle` up front).
5. **`ne_handle.h` is empty** — reserved but unused; per-resource handles are declared inline in the resource headers.
