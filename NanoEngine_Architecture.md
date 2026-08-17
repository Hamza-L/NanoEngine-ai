# NanoEngine — Architecture

## The Big Picture

NanoEngine is a small, cross-platform C23 rendering engine. It runs on Windows
(Vulkan), macOS (Metal), and the web (WebGPU) from one shared public API. It is
built as a stack of four layers. Each layer only talks to the one directly
below it.

```
┌──────────────────────────────────────────────────────────────┐
│                        YOUR GAME / APP                       │
│                                                              │
│  "I want a window. I want to draw a triangle. Spin it."      │
└──────────────────────────────┬───────────────────────────────┘
                               │
┌──────────────────────────────▼───────────────────────────────┐
│                      FRAME DISPATCH                          │
│                                                              │
│  "Here's one frame: begin, update, draw, present."           │
└──────────────────────────────┬───────────────────────────────┘
                               │
┌──────────────────────────────▼───────────────────────────────┐
│                        RENDERER                              │
│                                                              │
│  "I manage the GPU. I own buffers, shaders, pipelines."      │
│  (Vulkan on Win32 · Metal on macOS · WebGPU on Web)          │
└──────────────────────────────┬───────────────────────────────┘
                               │
┌──────────────────────────────▼───────────────────────────────┐
│                        PLATFORM                              │
│                                                              │
│  "I talk to the OS. I make windows. I pump messages."        │
└──────────────────────────────────────────────────────────────┘
```

---

## Portability at a Glance

The renderer abstraction is **source-file-per-backend**, not a runtime vtable.
The platform's makefile chooses which single file gets compiled in.

```
      include/ne_renderer*.h                     ─── one public API
                    ▲
                    │  each backend implements it
      ┌─────────────┼─────────────┐
      │             │             │
   Win32          macOS          Web
   Vulkan          Metal        WebGPU
      │             │             │
   src/renderer/  src/renderer/  src/renderer/
    vulkan/        metal/         wgpu/
```

Each backend `#error`s if compiled on the wrong platform. There is no
`#ifdef` fan-out in the callers — main.c just calls `ne_renderer_create` and
gets whichever backend the build linked in.

---

## The Ownership Chain

Everything follows a strict ownership chain. Destroy something high up and
everything below dies with it — like pulling a tablecloth.

```
NEApp
 └─── owns ──→ NEWindow(s)
                 │
NERenderer  (singleton)
 ├─── owns ──→ NERenderSurface(s)  ←── one per window
 ├─── owns ──→ Buffers   (pool)
 ├─── owns ──→ Shaders   (pool)
 └─── owns ──→ Pipelines (pool)     (images live in the buffers pool)
```

You create top-down, you destroy bottom-up:

```c
app      = ne_app_create();
window   = ne_window_create(app, ...);
renderer = ne_renderer_create(&desc);
surface  = ne_renderer_create_surface(renderer, window, &sdesc);

// ... run ...

ne_renderer_destroy_surface(renderer, surface);
ne_renderer_destroy(renderer);
ne_window_destroy(window);
ne_app_destroy(app);
```

The renderer is a **singleton** — only one may exist per process. The surface is
the marriage between a renderer and a window — it's where pixels actually
appear on screen.

---

## The Render Dispatch Pattern

Frames are triggered differently on every platform. The engine decouples
"what to draw" from "when to draw."

```
┌──────────────────────────────────────────────────────────┐
│  NEFrameContext                                          │
│                                                          │
│  "Everything needed to draw one frame"                   │
│                                                          │
│    renderer ──────▶ the GPU                              │
│    surface  ──────▶ where pixels go                      │
│    pipeline ──────▶ how to draw                          │
│    vertex_buffer ─▶ what to draw                         │
│    vertex_count ──▶ how many                             │
│    on_update ─────▶ per-frame logic (rotation, etc)      │
└──────────────────────────────────────────────────────────┘

ne_render_frame(void *user):
    ctx  = (NEFrameContext *)user;
    pass = begin_frame(ctx->renderer, ctx->surface);
    ctx->on_update(ctx, pass);              // user logic
    set_pipeline    (pass, ctx->pipeline);
    set_vertex_buffer(pass, 0, ctx->vertex_buffer);
    draw            (pass, 0, ctx->vertex_count);
    end_frame(ctx->renderer, ctx->surface);
```

The window stores a callback: `void (*render)(void *user)`. The platform calls
it whenever a frame is needed. Same function; three drivers:

```
Win32   (pull) : main loop calls it every iteration; also inline during WM_SIZE
macOS   (push) : CADisplayLink fires it at display refresh
Web     (push) : emscripten_set_main_loop_arg fires it via requestAnimationFrame
```

The function doesn't know or care who called it.

---

## How a Frame Happens (Vulkan)

The heartbeat of the engine. Every frame:

```
   CPU                                          GPU
   ───                                          ───

1. "Is the GPU done with frame N-2?"
   ┌─────────────────┐
   │ Wait for fence  │─────── blocks until ────────▶ ✓ done
   └─────────────────┘

2. "Give me a swapchain image to draw into"
   ┌─────────────────┐
   │ Acquire image   │─────── reserves image ──────▶ image #K
   └─────────────────┘

3. "Record my draw commands"
   ┌─────────────────┐
   │ Begin cmd buffer│
   │ Begin renderpass│ (clears screen)
   │ Bind pipeline   │
   │ Bind vertex buf │
   │ Draw 3 vertices │
   │ End renderpass  │
   │ End cmd buffer  │
   └─────────────────┘

4. "GPU, execute this"
   ┌─────────────────┐
   │ Queue submit    │─────── sends to GPU ────────▶ starts drawing
   └─────────────────┘

5. "Show it on screen when done"
   ┌─────────────────┐
   │ Queue present   │─────── schedules flip ──────▶ waits for draw,
   └─────────────────┘                                then shows image
```

`MAX_FRAMES_IN_FLIGHT = 2`. With two flight slots, at any moment the CPU may
be recording frame N+1 while the GPU is executing frame N — one frame of
pipelining. The fence in step 1 prevents the CPU from getting further ahead.

---

## The Surface = Window + GPU

A surface is the bridge between the window system and the GPU. It holds
everything needed to present frames to a specific window.

```
┌─────────────────────────────────────────────────────────────────┐
│  NERenderSurface                                                │
│                                                                 │
│  ┌────────────┐        ┌──────────────────────────────────────┐ │
│  │  VkSurface │        │  Swapchain (interface pointer)       │ │
│  │  (OS link) │        │                                      │ │
│  └────────────┘        │  images[0] ──▶ image_views[0]        │ │
│                        │  images[1] ──▶ image_views[1]        │ │
│  ┌────────────┐        │  images[2] ──▶ image_views[2]        │ │
│  │  Window    │        │       │                              │ │
│  │  (HWND)    │        │       ▼                              │ │
│  └────────────┘        │  framebuffers[0,1,2]                 │ │
│                        │                                      │ │
│  ┌────────────┐        │  format · extent · needs_recreate    │ │
│  │ RenderPass │        └──────────────────────────────────────┘ │
│  └────────────┘                                                 │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  Sync (per flight frame, not per swapchain image)       │    │
│  │                                                         │    │
│  │  Slot 0:  cmd_buf[0]  sem_available[0]  fence[0]        │    │
│  │  Slot 1:  cmd_buf[1]  sem_available[1]  fence[1]        │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
│  clear_color · vsync · frame_index (wraps at 2)                 │
│  present_backend (WSI or DXGI, chosen at surface creation)      │
└─────────────────────────────────────────────────────────────────┘
```

Two things are worth calling out:

1. **The swapchain is an interface**, not a concrete struct. `surface->swapchain`
   is a `NESwapchain*` — see next section.
2. Sync tracks the **flight frame** (0..1), not the swapchain image (0..N).
   The driver may hand you any image; the CPU/GPU sync is decoupled from that.

---

## Two Swapchains, One Surface API

The Vulkan backend has a real vtable for swapchains — `NESwapchainOps`. Choose
per surface at creation time via `NERenderSurfaceDesc.present_backend`:

```
NESwapchain (base)          ← inherited by placing "NESwapchain base;" first
    │
    ├── NESwapchainVulkanWSI     ← vkCreateSwapchainKHR  (standard)
    │     +  ne_swapchain_vulkan_wsi.c   (440 LOC)
    │
    └── NESwapchainDXGI          ← DXGI + DirectComposition
          +  ne_swapchain_dxgi.c        (777 LOC)
          +  images imported via VK_KHR_external_memory_win32
          +  DXGI_ALPHA_MODE_PREMULTIPLIED  →  per-pixel transparency
```

```c
typedef struct NESwapchainOps {
    void (*destroy) (NESwapchain*);
    bool (*recreate)(NESwapchain*, bool vsync);
    bool (*acquire) (NESwapchain*, VkSemaphore image_available);
    bool (*present) (NESwapchain*, VkQueue, VkSemaphore render_finished);
} NESwapchainOps;
```

The renderer's frame loop is written against `ops->…` — it doesn't know or care
which one is behind the pointer. The demo uses `NE_PRESENT_BACKEND_DXGI` so the
window gets a transparent, borderless-with-shadow look.

---

## The Handle System

The engine never gives you raw pointers to GPU resources. Instead, you get
**handles** — small integer tickets you trade in when you want to use the
resource.

```
What you hold:          What the engine holds:
─────────────────       ──────────────────────────────────

NEBufferHandle          Pool of slots:
┌──────┐                ┌───┬────────────────────────────┐
│ id=3 │──── means ────▶│ 2 │ VkBuffer, VkDeviceMemory   │
└──────┘                ├───┼────────────────────────────┤
                        │ 0 │ (empty)                    │
                        ├───┼────────────────────────────┤
                        │ 1 │ VkBuffer, VkDeviceMemory   │
                        ├───┼────────────────────────────┤
                        │ 2 │ VkBuffer, VkDeviceMemory   │ ◀── this one
                        └───┴────────────────────────────┘

        handle.id = pool_index + 1        (so id=0 is null)
```

Why not raw pointers?

1. **Validation is a single integer compare** — `handle.id == 0` → invalid.
2. **Dangling references are caught** — the slot has an `occupied` byte. If
   you use a handle after destroying the resource, the engine sees
   `occupied == false` and warns instead of crashing.
3. **Memory stays compact** — one flat array. Good for cache, good for sanity.

> Convention: every handle is `struct { uint32_t id; }` — except `NEImageHandle`,
> which is a bare `uint64_t`. That inconsistency should be fixed.

---

## The Pool Allocator

Behind every handle is a pool. Dead simple: a flat array of fixed-size slots.

```
              hint (where to start looking)
              │
              ▼
┌──────┬──────┬──────┬──────┬──────┬──────┬──────┐
│ USED │ USED │ FREE │ USED │ USED │ FREE │ USED │
└──────┴──────┴──────┴──────┴──────┴──────┴──────┘
  [0]    [1]    [2]    [3]    [4]    [5]    [6]

Allocate: scan from hint → find first FREE → mark USED → return index
Free:     mark slot FREE → rewind hint if this slot is before it
Grow:     double the array, zero-fill new slots (FREE by default)
```

Every slot starts with `bool occupied` as its first byte. The pool doesn't know
what's in the rest — it just checks that byte.

Same pattern id Software used in Quake for entity management: simple, fast, no
fragmentation within the pool.

---

## Static vs Dynamic Buffers

Two strategies for getting data to the GPU, chosen at creation time.

### Static (set once, read many)

For geometry that doesn't change (a mesh, a map, UI quads).

```
  CPU side                                    GPU side
  ────────                                    ────────

  Your data                                   Device-local memory
  ┌────────┐     ┌──────────────┐            ┌────────────────┐
  │ vertex │────▶│ staging buf  │───copy────▶│ GPU buffer     │
  │  data  │     │ (CPU-visible)│  command   │ (fast for GPU) │
  └────────┘     └──────────────┘            └────────────────┘

  One-time cost. After this, the GPU reads at full speed.
```

The staging buffer is shared across all static uploads — the renderer keeps
exactly one, reused via a dedicated transfer command buffer + fence.

### Dynamic (changes every frame)

For data that changes every frame (rotating vertices, animated particles).

```
  CPU side                                    GPU side
  ────────                                    ────────

  Frame 0:   write ──▶ ┌─ copy 0 ─┐ ◀── GPU reads (frame 0)
                        └──────────┘

  Frame 1:   write ──▶ ┌─ copy 1 ─┐ ◀── GPU reads (frame 1)
                        └──────────┘

  Frame 2:   write ──▶ ┌─ copy 0 ─┐ ◀── GPU has finished frame 0
                        └──────────┘      (fence blocked us until now)
```

Two persistently-mapped host-visible copies (one per flight frame). The CPU
writes one while the GPU reads the other. No staging, no copy command — just a
`memcpy` into a mapped pointer.

---

## Deferred Pipeline Compilation

A "pipeline" is the full recipe for drawing: which shaders, what vertex layout,
what blend mode, what topology. Compiling one is expensive and requires
knowing the render pass format.

Problem: when you call `ne_pipeline_create()`, the surface (and its render
pass) may not exist yet, or its format may change on resize.

Solution: **don't compile immediately. Store the recipe. Compile on first use.**

```
ne_pipeline_create():
┌─────────────────────────────────────────┐
│ "Here's my recipe. Store it for later." │
│                                         │
│   vertex shader   = ✓ resolved          │
│   fragment shader = ✓ resolved          │
│   vertex layout   = ✓ deep-copied       │
│   blend state     = ✓ stored            │
│   pipeline layout = ✓ created           │
│   VkPipeline      = NULL (not yet!)     │
│   needs_compile   = true                │
└─────────────────────────────────────────┘

... later, during the first frame ...

ne_render_pass_set_pipeline():
┌─────────────────────────────────────────┐
│ "First time? OK, I have the render pass │
│  now. Let me compile."                  │
│                                         │
│   vkCreateGraphicsPipelines(...)        │
│   needs_compile = false                 │
│   VkPipeline = ✓ ready                  │
│                                         │
│   vkCmdBindPipeline(...)                │
└─────────────────────────────────────────┘
```

After the first frame, `set_pipeline` is just a single Vulkan call. The
deferred cost is paid once.

Vertex bindings/attributes and entry-point strings are **deep-copied** into
the slot at create time so the caller's `NEPipelineDesc` is free to go out of
scope.

---

## The Render Pass Commands

Once you have a pass (from `begin_frame`), you issue commands. These are
recorded into a command buffer — they don't execute immediately. They're a
to-do list for the GPU.

```
ne_renderer_begin_frame()
│
▼
┌────────────────────────────────────────────────────────┐
│  NERenderPass  (valid until end_frame)                 │
│                                                        │
│  Commands you can issue:                               │
│                                                        │
│  set_pipeline(pass, pipeline)     "use this recipe"    │
│  set_vertex_buffer(pass, 0, buf)  "vertices are here"  │
│  set_index_buffer(pass, buf, u16) "indices are here"   │
│  set_uniform_data(pass, stage, slot, data, size)       │
│                                    "here's a matrix"   │
│  update_buffer(pass, buf, data, size, offset)          │
│                                    "new vertex data"   │
│  draw(pass, first, count)         "draw N vertices"    │
│  draw_indexed(pass, count, first, offset)              │
│                                    "draw by index"     │
│                                                        │
└────────────────────────────────────────────────────────┘
│
▼
ne_renderer_end_frame()  →  submit to GPU  →  present on screen
```

The commands map almost 1:1 to Vulkan / Metal / WebGPU calls, with handle
indirection resolved at the call site.

---

## Uniforms via Push Constants

Small per-draw data (a transform matrix, a color) uses Vulkan **push constants**.
This is the fastest path — data goes straight into the command buffer, no
buffer allocation, no descriptor sets.

```
                    Push Constant Range (128 bytes)
┌───────────────────────────────────────────────────────┐
│  Vertex stage:   offset 0, up to 128 bytes            │
│  Fragment stage: offset 0, up to 128 bytes            │
│                                                       │
│  Both stages share the same 128 bytes.                │
│                                                       │
│  From CPU:                                            │
│    ne_render_pass_set_uniform_data(pass, VERTEX,      │
│                                    0, &mvp, 64);      │
│                                                       │
│  Maps to: vkCmdPushConstants(cmd, layout,             │
│           VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &mvp);   │
└───────────────────────────────────────────────────────┘
```

128 bytes is the guaranteed minimum on all Vulkan hardware. Enough for a 4×4
matrix (64 B) plus extras. Descriptor sets for larger uniform buffers are
planned but not yet implemented.

---

## Shader System — Two Paths

```
                    ┌─────────────────────────────────┐
                    │       ne_shader_create()        │
                    │                                 │
  PRODUCTION:       │   You provide SPIR-V bytes      │
  ─────────────     │   (pre-compiled offline)        │
                    │                                 │
  .spv file         │   → vkCreateShaderModule        │
  on disk ─────────▶│   → store in pool               │
                    │   → return handle               │
                    └─────────────────────────────────┘


                    ┌─────────────────────────────────┐
                    │  ne_shader_create_from_source() │
                    │                                 │
  DEVELOPMENT:      │   You provide GLSL text        │
  ─────────────     │                                 │
                    │   → glslang preprocess          │
  .vert/.frag       │   → glslang parse               │
  file on disk ────▶│   → glslang link                │
                    │   → glslang generate SPIR-V     │
                    │   → ne_shader_create()  ────────│──▶ same path
                    │   → return handle               │
                    └─────────────────────────────────┘
```

Both paths are fully implemented on the Vulkan backend. In development you edit
`.glsl` files and let the engine compile them at startup; for shipping,
pre-compile to `.spv` and skip the glslang dependency entirely. The WebGPU
backend consumes WGSL directly; Metal consumes MSL.

---

## Memory Architecture Summary

```
┌─────────── CPU Memory ──────────────┐    ┌─────── GPU Memory ────────┐
│                                     │    │                           │
│  NEApp         (heap, singleton)    │    │  Static buffers           │
│  NEWindow      (heap, linked list)  │    │  (DEVICE_LOCAL)           │
│  NERenderer    (heap, singleton)    │    │  ├─ vertex data           │
│  NERenderSurface (heap, list)       │    │  ├─ index data            │
│                                     │    │  └─ image data            │
│  Pool arrays:                       │    │                           │
│  ├─ buffers.slots[]  (also images)  │    │  Swapchain images         │
│  ├─ shaders.slots[]                 │    │  (owned by driver or DXGI) │
│  └─ pipelines.slots[]               │    │                           │
│                                     │    └───────────────────────────┘
│  Dynamic buffer mappings:           │
│  ├─ dyn_mapped[0] ──────────────────│───▶ HOST_VISIBLE buffer copy 0
│  └─ dyn_mapped[1] ──────────────────│───▶ HOST_VISIBLE buffer copy 1
│                                     │
│  Staging buffer mapping:            │
│  └─ staging_mapped ─────────────────│───▶ HOST_VISIBLE staging area
│                                     │
└─────────────────────────────────────┘
```

Key insight: **dynamic buffers live in shared memory** (visible to both CPU and
GPU). The CPU writes them directly via mapped pointers. No copy commands. This
is why they're fast for per-frame updates but slower for GPU reads compared to
device-local memory.

There is currently **no allocator layer** — each buffer/image calls its own
`vkAllocateMemory`. That's on the roadmap.

---

## What Makes This Architecture Good

1. **Flat, predictable memory** — pools instead of scattered allocations. You
   know exactly where everything lives and how it grows.
2. **Explicit ownership** — no GC, no refcounts on GPU resources. You create
   it, you destroy it. The order is clear.
3. **Frame-aware by design** — double-buffered dynamic buffers and
   fence-per-flight-frame mean the CPU and GPU never fight over the same
   memory.
4. **Portable core** — the same public API drives Vulkan, Metal, and WebGPU.
   The frame dispatch is agnostic to who calls it (pull loop, display link,
   rAF).
5. **Runtime present-backend choice** — same Vulkan renderer can present via
   standard WSI *or* via DXGI + DirectComposition for transparent windows,
   selected per surface.
6. **Deferred decisions** — pipeline compilation waits until the render pass
   format is known. No speculative work.
7. **Single queue, simple sync** — one graphics+present queue, two flight
   frames, fence-based sync. Handles the overwhelming majority of workloads
   and is easy to reason about.

---

## Known Rough Edges

Called out here so future work can find them:

- **`ne_renderer_create` signature drifts across backends** — header vs. Metal/WGPU disagree on whether it takes an `NEApp*`.
- **`NEImageHandle` breaks the handle convention** (`uint64_t` vs. `struct { uint32_t id; }`).
- **No GPU memory allocator** — every resource does its own `vkAllocateMemory`.
- **No descriptor sets** — uniform data is push-constants-only (128 bytes).
- **Compute is stubbed** in the Vulkan backend.
- **Two doc comments are stale**: `ne_vulkan_shaders.c:41` (calls the source path a "no-op stub" — it isn't) and `ne_renderer_buffer.h:127` (claims destruction is deferred — it's immediate).
