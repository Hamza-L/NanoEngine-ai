# NanoEngine2 — Architecture

## Design Philosophy

NanoEngine2 is a **simple, modular, portable, and performant** rendering engine built on four core principles:

1. **Pure C (c2x)** — No C++, no exceptions, no RTTI. Objective-C is used only in Apple platform backends, hidden behind the C API boundary.
2. **Zero runtime dependencies** — The engine links only against OS-provided frameworks and libraries. Vulkan is loaded dynamically at runtime; Metal and Cocoa are system frameworks. No GLFW, SDL, or middleware.
3. **Opaque-handle API** — All engine objects (`NEApp`, `NEWindow`, `NERenderer`, etc.) are exposed as opaque pointers or lightweight handle structs. Internal layout is never visible to the consumer.
4. **Platform parity** — Every public API function has identical semantics across all supported platforms. Platform-specific code is isolated in `src/platform/` and `src/renderer/`.

---

## Module Map

The engine is organized into **8 public modules**, each corresponding to a header in `include/`:

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                             │
│  ne_app.h          Lifecycle, event loop, quit control          │
│  ne_window.h       Window creation, input events, callbacks     │
│  ne_log.h          Logging, sinks, assertions                   │
└─────────────────────┬───────────────────────────────────────────┘
                      │ uses
┌─────────────────────▼───────────────────────────────────────────┐
│                        Renderer Core                            │
│  ne_renderer.h     Renderer + surface lifecycle, frame begin/end│
└─────────────────────┬───────────────────────────────────────────┘
                      │ uses
┌─────────────────────▼───────────────────────────────────────────┐
│                       Renderer Resources                        │
│  ne_renderer_buffer.h    GPU buffers (vertex, index, uniform)   │
│  ne_renderer_shader.h    Shader modules (bytecode + source)     │
│  ne_renderer_pipeline.h  Graphics & compute pipeline state      │
└─────────────────────┬───────────────────────────────────────────┘
                      │ uses
┌─────────────────────▼───────────────────────────────────────────┐
│                       Command Recording                         │
│  ne_renderer_pass.h  Render pass + compute pass commands        │
└─────────────────────────────────────────────────────────────────┘
```

### Header Dependency Graph

```
ne_renderer_pass.h
  ├── ne_renderer_buffer.h
  ├── ne_renderer_pipeline.h
  │     └── ne_renderer_shader.h
  └── ne_renderer_shader.h

ne_renderer.h          (standalone, forward-declares NEApp/NEWindow)
ne_app.h               (standalone)
ne_window.h            (standalone, forward-declares NEApp)
ne_log.h               (standalone)
```

---

## Directory Structure

```
NanoEngine2/
├── include/                    Public C API headers
│   ├── ne_app.h
│   ├── ne_log.h
│   ├── ne_renderer.h
│   ├── ne_renderer_buffer.h
│   ├── ne_renderer_pass.h
│   ├── ne_renderer_pipeline.h
│   ├── ne_renderer_shader.h
│   └── ne_window.h
├── src/
│   ├── main.c                  Entry point / demo application
│   ├── ne_log.c                Logging implementation (cross-platform)
│   ├── platform/               Windowing backends
│   │   ├── macos/
│   │   │   └── window_macos.m      Cocoa / AppKit backend
│   │   └── win32/
│   │       └── window_win32.c      Win32 API backend
│   └── renderer/               GPU backends
│       ├── metal/
│       │   └── ne_renderer_metal.m  Metal backend
│       └── vulkan/
│           └── ne_renderer_vulkan.c Vulkan backend
├── Makefile                    Top-level build (platform-dispatching)
├── MakeFile_macos.mk           macOS sources + frameworks
├── MakeFile_win32.mk           Win32 sources + Vulkan header download
└── docs/                       Documentation
```

---

## Platform Abstraction Pattern

Both platform backends follow an identical pattern to present a pure-C API while wrapping platform-native objects:

### macOS (Objective-C / ARC)

Platform objects (`NSWindow`, `NSView`, `id<MTLDevice>`, etc.) are stored in C structs as `void*` pointers using ARC bridging:

```
NEWindow (C struct)
├── void *ns_window     ← (__bridge_retained void*) NSWindow*
├── void *ns_view       ← (__bridge_retained void*) NSView*
└── void *delegate      ← (__bridge_retained void*) NEMacWindowDelegate*
```

- **Retain**: `(__bridge_retained void*)` transfers ownership from ARC into the `void*` field.
- **Use**: `(__bridge NSWindow*)` for temporary access without ownership change.
- **Release**: `CFBridgingRelease()` transfers ownership back to ARC for deallocation.

### Windows (Win32 / Runtime Loading)

Win32 objects (`HWND`, `HINSTANCE`) are stored directly as opaque handles. The Vulkan backend dynamically loads `vulkan-1.dll` at runtime and resolves all function pointers via `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`:

```
NERenderer (C struct)
├── HMODULE vulkan_dll          ← LoadLibraryA("vulkan-1.dll")
├── VkInstance instance         ← Created via resolved vkCreateInstance
├── VkDevice device             ← Created via resolved vkCreateDevice
└── PFN_vk* function_pointers   ← Resolved per-instance and per-device
```

This means **no Vulkan SDK is required at link time** — only the headers are needed at compile time (auto-downloaded by the build system).

---

## Rendering Pipeline Lifecycle

The rendering pipeline follows a strict lifecycle. Each frame proceeds through these phases:

```
1. INIT (once)
   ne_renderer_create()        → Initialize GPU device + queues
   ne_renderer_create_surface() → Create swapchain tied to a window

2. FRAME LOOP (per frame)
   ne_renderer_begin_frame()    → Acquire swapchain image, begin command recording
     ├── ne_render_pass_set_pipeline()
     ├── ne_render_pass_set_vertex_buffer()
     ├── ne_render_pass_set_index_buffer()
     ├── ne_render_pass_set_uniform_data()
     ├── ne_render_pass_draw() / ne_render_pass_draw_indexed()
     │
     ├── [optional] ne_render_pass_begin_compute()
     │     ├── ne_compute_pass_set_pipeline()
     │     ├── ne_compute_pass_set_storage_buffer()
     │     ├── ne_compute_pass_set_uniform_data()
     │     └── ne_compute_pass_dispatch()
     └── [optional] ne_render_pass_end_compute()
   ne_renderer_end_frame()      → End recording, submit, present

3. TEARDOWN (once)
   ne_renderer_destroy_surface()
   ne_renderer_destroy()
```

### Resource Management

GPU resources (buffers, shaders, pipelines) are managed via **handle-based pools**:

- Each resource type uses a lightweight handle struct: `NEBufferHandle`, `NEShaderHandle`, `NEPipelineHandle` — each wrapping a `uint32_t id`.
- A null handle has `id == 0`. Validity is checked via `ne_*_handle_valid()`.
- Resources are created against an `NERenderer*` and must be destroyed before the renderer itself.

### Swapchain Strategy

| Aspect | Vulkan | Metal |
|---|---|---|
| Swapchain model | `VkSwapchainKHR` with explicit image management | `CAMetalLayer` with automatic drawable management |
| Frames in flight | 2 (semaphore + fence paced) | 1 (static pass — to be improved) |
| Resize handling | Swapchain recreation on `VK_ERROR_OUT_OF_DATE` | Automatic via `CAMetalLayer` |
| Vsync | `VK_PRESENT_MODE_FIFO` vs `MAILBOX` | `displaySyncEnabled` on `CAMetalLayer` |

---

## Backend Feature Matrix

### Implemented ✅ / Not Yet Implemented ❌

| Feature | API Header | Vulkan (Win32) | Metal (macOS) |
|---|---|---|---|
| App lifecycle | `ne_app.h` | ✅ | ✅ |
| Windowing | `ne_window.h` | ✅ | ✅ |
| Keyboard input | `ne_window.h` | ✅ | ✅ |
| Mouse input | `ne_window.h` | ✅ | ✅ |
| Text input (Unicode) | `ne_window.h` | ✅ | ✅ |
| Logging | `ne_log.h` | ✅ | ✅ |
| Renderer init | `ne_renderer.h` | ✅ | ✅ |
| Render surface | `ne_renderer.h` | ✅ | ✅ |
| Clear screen | `ne_renderer.h` | ✅ | ✅ |
| GPU Buffers | `ne_renderer_buffer.h` | ❌ | ❌ |
| Shaders | `ne_renderer_shader.h` | ❌ | ❌ |
| Graphics pipelines | `ne_renderer_pipeline.h` | ❌ | ❌ |
| Compute pipelines | `ne_renderer_pipeline.h` | ❌ | ❌ |
| Render pass commands | `ne_renderer_pass.h` | ❌ | ❌ |
| Compute pass commands | `ne_renderer_pass.h` | ❌ | ❌ |

---

## Key Design Decisions

### Why no abstraction layer between backends?

Each backend directly implements the public API functions — there is no internal "backend vtable" or function pointer dispatch. This is intentional:

- Only one backend is compiled per platform (selected at build time via Makefile).
- No runtime overhead from indirection.
- Each backend can use platform-idiomatic patterns without forcing a common internal interface.

### Why opaque handles instead of opaque pointers for GPU resources?

Lightweight handle structs (`{ uint32_t id; }`) offer several advantages over raw pointers:

- **Pool-friendly** — resources are stored in contiguous arrays indexed by id.
- **Null-safe** — a zero id is always invalid, checked via `ne_*_handle_valid()`.
- **Type-safe** — `NEBufferHandle` and `NEShaderHandle` are distinct types, unlike `void*`.
- **ABI-stable** — handle size doesn't change when internal resource layout changes.

### Why dynamic Vulkan loading?

By loading `vulkan-1.dll` at runtime and resolving all symbols manually, the engine:

- Requires no Vulkan SDK installed on the build machine (only headers, auto-downloaded).
- Can gracefully fail if the Vulkan runtime is not available.
- Avoids link-time dependency on `vulkan-1.lib`.
