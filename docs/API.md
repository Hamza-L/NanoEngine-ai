# NanoEngine2 — API Reference

> Functions marked with ⚠️ are declared in headers but **not yet implemented** in any backend.

---

## Table of Contents

- [Application (`ne_app.h`)](#application)
- [Window (`ne_window.h`)](#window)
- [Logging (`ne_log.h`)](#logging)
- [Renderer Core (`ne_renderer.h`)](#renderer-core)
- [Buffers (`ne_renderer_buffer.h`)](#buffers)
- [Shaders (`ne_renderer_shader.h`)](#shaders)
- [Pipelines (`ne_renderer_pipeline.h`)](#pipelines)
- [Render & Compute Passes (`ne_renderer_pass.h`)](#render--compute-passes)

---

<a id="application"></a>
## Application — `ne_app.h`

Manages the application lifecycle, event loop, and quit control.

### Types

| Type | Description |
|---|---|
| `NEApp` | Opaque application handle. Singleton per process. |

### Functions

#### `NEApp *ne_app_create(void)`
Creates the application instance. On macOS, initializes `NSApplication` and builds the menu bar. On Windows, registers the window class and obtains the `HINSTANCE`. Returns `NULL` on failure.

Subsequent calls increment an internal reference count and return the same instance.

#### `void ne_app_destroy(NEApp *app)`
Decrements the reference count. Frees the application when the count reaches zero.

#### `bool ne_app_poll_events(NEApp *app)`
Processes all pending platform events (input, resize, close, etc.). Returns `true` if the application should continue running, `false` if quit was requested or all windows were closed.

Call this once per frame iteration.

#### `void ne_app_run(NEApp *app)`
Convenience function that polls events in a loop with a 1ms yield. Blocks until the app should exit.

#### `void ne_app_request_quit(NEApp *app)`
Signals the application to exit. The next call to `ne_app_poll_events` will return `false`.

#### `bool ne_app_is_running(const NEApp *app)`
Returns `true` if the application has not been requested to quit.

#### `uint32_t ne_app_get_window_count(const NEApp *app)`
Returns the number of currently open windows.

---

<a id="window"></a>
## Window — `ne_window.h`

Creates and manages OS windows with input event callbacks.

### Types

| Type | Description |
|---|---|
| `NEWindow` | Opaque window handle |
| `NEWindowDesc` | Window creation descriptor (title, position, size, flags) |
| `NEWindowCallbacks` | Aggregate of all event callback function pointers |
| `NEKey` | Keyboard key enumeration (A–Z, 0–9, Escape, Enter, Tab, Backspace, Space, arrows) |
| `NEMouseButton` | Mouse button enumeration (left, right, middle, other) |
| `NEModifiers` | Bitfield for modifier keys (Shift, Control, Alt, Super, CapsLock) |
| `NENativeHandleType` | Platform-specific handle type selector |

### Event Structs

| Struct | Fields |
|---|---|
| `NEKeyEvent` | `key`, `native_key_code`, `modifiers`, `repeat` |
| `NEMouseMoveEvent` | `x`, `y`, `delta_x`, `delta_y` |
| `NEMouseButtonEvent` | `button`, `x`, `y`, `modifiers` |
| `NEMouseScrollEvent` | `delta_x`, `delta_y` |

### Callback Typedefs

| Typedef | Signature |
|---|---|
| `NEOnCloseFn` | `void (*)(NEWindow*, void* user_data)` |
| `NEOnResizeFn` | `void (*)(NEWindow*, int32_t w, int32_t h, void* user_data)` |
| `NEOnMoveFn` | `void (*)(NEWindow*, int32_t x, int32_t y, void* user_data)` |
| `NEOnKeyFn` | `void (*)(NEWindow*, NEKeyEvent, void* user_data)` |
| `NEOnTextInputFn` | `void (*)(NEWindow*, uint32_t codepoint, void* user_data)` |
| `NEOnMouseMoveFn` | `void (*)(NEWindow*, NEMouseMoveEvent, void* user_data)` |
| `NEOnMouseButtonFn` | `void (*)(NEWindow*, NEMouseButtonEvent, void* user_data)` |
| `NEOnMouseScrollFn` | `void (*)(NEWindow*, NEMouseScrollEvent, void* user_data)` |

### `NEWindowDesc` Fields

| Field | Type | Description |
|---|---|---|
| `title` | `const char*` | Window title (UTF-8) |
| `x`, `y` | `int32_t` | Initial position in screen coordinates |
| `width`, `height` | `int32_t` | Initial content area size |
| `resizable` | `bool` | Allow user resizing |
| `undecorated` | `bool` | Borderless window (no title bar or controls) |
| `show_on_create` | `bool` | Make visible immediately after creation |

### Functions

#### `NEWindow *ne_window_create(NEApp *app, const NEWindowDesc *desc)`
Creates a new window with the given descriptor. Returns `NULL` on failure. Increments the app's window count.

#### `void ne_window_destroy(NEWindow *window)`
Destroys the window and frees all associated resources. Decrements the app's window count.

#### `void ne_window_show(NEWindow *window)`
Makes the window visible and brings it to the front.

#### `void ne_window_hide(NEWindow *window)`
Hides the window.

#### `void ne_window_request_close(NEWindow *window)`
Triggers a close event. Fires the `on_close` callback, then marks the window as closed.

#### `void ne_window_set_title(NEWindow *window, const char *title)`
Sets the window title bar text (UTF-8).

#### `void ne_window_set_position(NEWindow *window, int32_t x, int32_t y)`
Moves the window to the given screen coordinates.

#### `void ne_window_set_size(NEWindow *window, int32_t width, int32_t height)`
Resizes the window's content area.

#### `void ne_window_set_callbacks(NEWindow *window, const NEWindowCallbacks *callbacks, void *user_data)`
Registers event callbacks. `user_data` is passed to every callback invocation. Pass `NULL` callbacks to clear.

#### `bool ne_window_get_content_size(const NEWindow *window, int32_t *out_width, int32_t *out_height)`
Retrieves the content area size in logical (screen) coordinates.

#### `bool ne_window_get_framebuffer_size(const NEWindow *window, int32_t *out_width, int32_t *out_height)`
Retrieves the framebuffer size in physical pixels. This may differ from content size on high-DPI displays (e.g., Retina).

#### `bool ne_window_get_content_scale(const NEWindow *window, float *out_scale)`
Returns the DPI scale factor (e.g., `2.0` on Retina displays).

#### `void *ne_window_get_native_handle(NEWindow *window, NENativeHandleType type)`
Returns a platform-specific handle. Use `NE_NATIVE_HANDLE_COCOA_NS_VIEW` on macOS, `NE_NATIVE_HANDLE_WIN32_HWND` on Windows. Returns `NULL` for unsupported types.

#### `bool ne_window_is_mouse_button_down(const NEWindow *window, NEMouseButton button)`
Returns `true` if the given mouse button is currently held down.

#### `bool ne_window_is_open(const NEWindow *window)`
Returns `true` if the window has not been closed.

### `NENativeHandleType` Values

| Value | Platform | Returns |
|---|---|---|
| `NE_NATIVE_HANDLE_COCOA_NS_WINDOW` | macOS | `NSWindow*` |
| `NE_NATIVE_HANDLE_COCOA_NS_VIEW` | macOS | `NSView*` |
| `NE_NATIVE_HANDLE_WIN32_HWND` | Windows | `HWND` |
| `NE_NATIVE_HANDLE_X11_WINDOW` | Linux (future) | — |
| `NE_NATIVE_HANDLE_WAYLAND_SURFACE` | Linux (future) | — |

---

<a id="logging"></a>
## Logging — `ne_log.h`

Structured logging with configurable sinks, level filtering, and a global default logger.

### Types

| Type | Description |
|---|---|
| `NELogger` | Opaque logger handle |
| `NELogLevel` | Severity level enumeration |
| `NELogSinkFn` | Custom sink callback: `void (*)(void* user_data, NELogLevel, const char* message)` |

### `NELogLevel` Values

| Level | Value | Description |
|---|---|---|
| `NE_LOG_LEVEL_TRACE` | 0 | Verbose diagnostic output |
| `NE_LOG_LEVEL_DEBUG` | 1 | Debug information |
| `NE_LOG_LEVEL_INFO` | 2 | General informational messages (default) |
| `NE_LOG_LEVEL_WARN` | 3 | Potential issues |
| `NE_LOG_LEVEL_ERROR` | 4 | Recoverable errors |
| `NE_LOG_LEVEL_FATAL` | 5 | Unrecoverable errors |
| `NE_LOG_LEVEL_OFF` | 6 | Suppress all logging |

### Logger Lifecycle

#### `NELogger *ne_logger_create(void)`
Creates a new logger with default settings (INFO level, console sink).

#### `void ne_logger_destroy(NELogger *logger)`
Destroys the logger. If it was the default logger, the default is reset.

### Logger Configuration

#### `void ne_logger_set_level(NELogger *logger, NELogLevel level)`
Sets the minimum severity level. Messages below this level are discarded.

#### `NELogLevel ne_logger_get_level(const NELogger *logger)`
Returns the current minimum level.

#### `void ne_logger_set_sink(NELogger *logger, NELogSinkFn sink, void *user_data)`
Sets a custom sink function. Replaces the current sink. Closes any owned file.

#### `void ne_logger_set_output_stdout(NELogger *logger)`
Routes output to `stdout`.

#### `void ne_logger_set_output_stderr(NELogger *logger)`
Routes output to `stderr`.

#### `bool ne_logger_set_output_file(NELogger *logger, FILE *file, bool close_on_destroy)`
Routes output to an open `FILE*`. If `close_on_destroy` is `true`, the logger takes ownership and will `fclose` it.

#### `bool ne_logger_open_file(NELogger *logger, const char *path, bool append)`
Opens a file at `path` and routes output to it. The logger owns the file handle.

### Logger Output

#### `void ne_logger_log(NELogger *logger, NELogLevel level, const char *fmt, ...)`
Logs a formatted message to the given logger.

#### `void ne_logger_vlog(NELogger *logger, NELogLevel level, const char *fmt, va_list args)`
Same as above, with a `va_list`.

### Global Default Logger

#### `NELogger *ne_log_get_default_logger(void)`
Returns the default logger (lazily initialized on first call). Initially logs to console at INFO level.

#### `void ne_log_set_default_logger(NELogger *logger)`
Overrides the default logger. Pass `NULL` to reset to the built-in logger.

#### `void ne_log(NELogLevel level, const char *fmt, ...)`
Logs to the default logger.

#### `void ne_vlog(NELogLevel level, const char *fmt, va_list args)`
Same as above, with a `va_list`.

### Convenience Macros

| Macro | Equivalent |
|---|---|
| `NE_LOG_TRACE(...)` | `ne_log(NE_LOG_LEVEL_TRACE, ...)` |
| `NE_LOG_DEBUG(...)` | `ne_log(NE_LOG_LEVEL_DEBUG, ...)` |
| `NE_LOG_INFO(...)` | `ne_log(NE_LOG_LEVEL_INFO, ...)` |
| `NE_LOG_WARN(...)` | `ne_log(NE_LOG_LEVEL_WARN, ...)` |
| `NE_LOG_ERROR(...)` | `ne_log(NE_LOG_LEVEL_ERROR, ...)` |
| `NE_LOG_FATAL(...)` | `ne_log(NE_LOG_LEVEL_FATAL, ...)` |

### Assertions

#### `NE_ASSERT(cond)`
In debug builds (`NDEBUG` not defined): logs a FATAL message with file, line, and function name, then calls `abort()`. In release builds: expands to `((void)0)`.

Override with `-DNE_ENABLE_ASSERTS=0` or `-DNE_ENABLE_ASSERTS=1`.

---

<a id="renderer-core"></a>
## Renderer Core — `ne_renderer.h`

Initializes the GPU device and manages render surfaces (swapchains) and frame lifecycle.

### Types

| Type | Description |
|---|---|
| `NERenderer` | Opaque renderer handle (one per process) |
| `NERenderSurface` | Opaque surface handle, tied to a window |
| `NERenderPass` | Opaque render pass handle, returned by `begin_frame` |
| `NERendererDesc` | Renderer creation descriptor |
| `NERenderSurfaceDesc` | Surface creation descriptor |

### `NERendererDesc` Fields

| Field | Type | Description |
|---|---|---|
| `enable_validation` | `bool` | Enable GPU validation/debug layers. On Vulkan, enables `VK_LAYER_KHRONOS_validation`. On Metal, this is a no-op (use `MTL_DEBUG_LAYER` env var). |

### `NERenderSurfaceDesc` Fields

| Field | Type | Description |
|---|---|---|
| `vsync` | `bool` | Enable vertical sync. Vulkan: FIFO vs MAILBOX present mode. Metal: `displaySyncEnabled`. |
| `clear_color_rgba` | `float[4]` | Initial clear color (RGBA, 0.0–1.0) |

### Functions

#### `NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc)`
Creates the renderer. On Vulkan: loads `vulkan-1.dll`, creates `VkInstance`, picks a physical device, creates `VkDevice`. On Metal: creates `MTLDevice` and `MTLCommandQueue`. Returns `NULL` on failure.

Only one renderer may exist at a time (singleton).

#### `void ne_renderer_destroy(NERenderer *renderer)`
Destroys the renderer and all associated GPU state.

#### `NERenderSurface *ne_renderer_create_surface(NERenderer *renderer, NEWindow *window, const NERenderSurfaceDesc *desc)`
Creates a render surface (swapchain) attached to the given window. Only one surface per window is allowed. Returns `NULL` on failure.

#### `void ne_renderer_destroy_surface(NERenderer *renderer, NERenderSurface *surface)`
Destroys the render surface and releases swapchain resources.

#### `void ne_renderer_surface_set_clear_color(NERenderSurface *surface, float r, float g, float b, float a)`
Updates the clear color used when beginning a frame.

#### `NERenderPass *ne_renderer_begin_frame(NERenderer *renderer, NERenderSurface *surface)`
Begins a new frame. Acquires the next swapchain image and prepares for command recording. Returns a `NERenderPass*` for recording draw commands, or `NULL` if the surface is not ready (e.g., window minimized, swapchain out of date).

#### `void ne_renderer_end_frame(NERenderer *renderer, NERenderPass *pass)`
Ends the frame. Submits recorded commands, presents the swapchain image.

---

<a id="buffers"></a>
## Buffers — `ne_renderer_buffer.h`

> ⚠️ **Not yet implemented** in any backend.

GPU buffer management for vertex, index, uniform, and storage data.

### Types

| Type | Description |
|---|---|
| `NEBufferHandle` | Lightweight handle struct (`uint32_t id`) |
| `NEBufferUsage` | Bitfield for buffer usage flags |
| `NEBufferDesc` | Buffer creation descriptor |
| `NEIndexType` | Index element type for index buffers |

### `NEBufferUsage` Flags

| Flag | Value | Description |
|---|---|---|
| `NE_BUFFER_USAGE_VERTEX` | `1 << 0` | Vertex buffer |
| `NE_BUFFER_USAGE_INDEX` | `1 << 1` | Index buffer |
| `NE_BUFFER_USAGE_UNIFORM` | `1 << 2` | Uniform / constant buffer |
| `NE_BUFFER_USAGE_STORAGE` | `1 << 3` | Storage buffer (read/write from shaders) |

Flags may be combined: `NE_BUFFER_USAGE_VERTEX | NE_BUFFER_USAGE_STORAGE`.

### `NEBufferDesc` Fields

| Field | Type | Description |
|---|---|---|
| `size` | `uint32_t` | Buffer size in bytes |
| `usage` | `uint32_t` | Bitwise OR of `NEBufferUsage` flags |
| `initial_data` | `const void*` | Optional data to upload at creation. May be `NULL`. |

### `NEIndexType` Values

| Value | Description |
|---|---|
| `NE_INDEX_TYPE_UINT16` | 16-bit unsigned indices |
| `NE_INDEX_TYPE_UINT32` | 32-bit unsigned indices |

### Constants

| Constant | Description |
|---|---|
| `NE_BUFFER_HANDLE_NULL` | Zero-initialized null handle |

### Functions

#### ⚠️ `NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc)`
Creates a GPU buffer. If `initial_data` is non-NULL, the data is uploaded immediately. Returns `NE_BUFFER_HANDLE_NULL` on failure.

#### ⚠️ `void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle, const void *data, uint32_t size, uint32_t offset)`
Updates a portion of the buffer's contents. `offset` + `size` must not exceed the buffer's total size.

#### ⚠️ `void ne_buffer_destroy(NERenderer *renderer, NEBufferHandle handle)`
Destroys the buffer and frees GPU memory.

### Utility

#### `bool ne_buffer_handle_valid(NEBufferHandle h)`
Returns `true` if the handle is not null (`id != 0`). Inline function.

---

<a id="shaders"></a>
## Shaders — `ne_renderer_shader.h`

> ⚠️ **Not yet implemented** in any backend.

Shader module creation from pre-compiled bytecode or source.

### Types

| Type | Description |
|---|---|
| `NEShaderHandle` | Lightweight handle struct (`uint32_t id`) |
| `NEShaderStage` | Shader stage enumeration |
| `NEShaderDesc` | Descriptor for bytecode-based shader creation |
| `NEShaderSourceDesc` | Descriptor for source-based shader creation |

### `NEShaderStage` Values

| Value | Description |
|---|---|
| `NE_SHADER_STAGE_VERTEX` | Vertex shader |
| `NE_SHADER_STAGE_FRAGMENT` | Fragment / pixel shader |
| `NE_SHADER_STAGE_COMPUTE` | Compute shader |

### `NEShaderDesc` Fields

| Field | Type | Description |
|---|---|---|
| `stage` | `NEShaderStage` | Which stage this shader is for |
| `bytecode` | `const void*` | Pointer to compiled bytecode (SPIR-V on Vulkan, metallib on Metal) |
| `bytecode_size` | `size_t` | Size of bytecode in bytes |
| `entry_point` | `const char*` | Entry function name (e.g., `"main"`, `"vertexMain"`) |

### `NEShaderSourceDesc` Fields

| Field | Type | Description |
|---|---|---|
| `stage` | `NEShaderStage` | Which stage this shader is for |
| `source` | `const char*` | Shader source code (Slang) |
| `entry_point` | `const char*` | Entry function name |
| `filename` | `const char*` | Virtual filename for error messages |

### Constants

| Constant | Description |
|---|---|
| `NE_SHADER_HANDLE_NULL` | Zero-initialized null handle |

### Functions

#### ⚠️ `NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc)`
Creates a shader module from pre-compiled bytecode (SPIR-V or metallib). Returns `NE_SHADER_HANDLE_NULL` on failure.

#### ⚠️ `NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc)`
Creates a shader module by compiling source at runtime via Slang. Returns `NE_SHADER_HANDLE_NULL` on failure.

> **Note:** Runtime compilation requires the Slang compiler library. This path is planned but not yet available. Initial implementation will return a null handle.

#### ⚠️ `void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle)`
Destroys the shader module.

### Utility

#### `bool ne_shader_handle_valid(NEShaderHandle h)`
Returns `true` if the handle is not null (`id != 0`). Inline function.

---

<a id="pipelines"></a>
## Pipelines — `ne_renderer_pipeline.h`

> ⚠️ **Not yet implemented** in any backend.

Graphics and compute pipeline state objects.

### Types

| Type | Description |
|---|---|
| `NEPipelineHandle` | Graphics pipeline handle (`uint32_t id`) |
| `NEComputePipelineHandle` | Compute pipeline handle (`uint32_t id`) |
| `NEVertexFormat` | Vertex attribute data format |
| `NEVertexAttribute` | Single vertex attribute (location, format, offset) |
| `NEVertexBufferLayout` | Vertex buffer layout (stride + attributes) |
| `NEPrimitiveTopology` | How vertices are assembled into primitives |
| `NEBlendFactor` | Blend equation source/destination factors |
| `NEBlendOp` | Blend equation operation |
| `NEBlendState` | Complete blend state for a color attachment |
| `NEPipelineDesc` | Graphics pipeline descriptor |
| `NEComputePipelineDesc` | Compute pipeline descriptor |

### `NEVertexFormat` Values

| Value | Size | Description |
|---|---|---|
| `NE_VERTEX_FORMAT_FLOAT` | 4 bytes | Single 32-bit float |
| `NE_VERTEX_FORMAT_FLOAT2` | 8 bytes | 2× 32-bit float |
| `NE_VERTEX_FORMAT_FLOAT3` | 12 bytes | 3× 32-bit float |
| `NE_VERTEX_FORMAT_FLOAT4` | 16 bytes | 4× 32-bit float |
| `NE_VERTEX_FORMAT_UNORM8X4` | 4 bytes | 4× 8-bit unsigned normalized (e.g., RGBA color) |

### `NEPrimitiveTopology` Values

| Value | Description |
|---|---|
| `NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` | Every 3 vertices form a triangle |
| `NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP` | Each vertex after the first two forms a triangle with the previous two |
| `NE_PRIMITIVE_TOPOLOGY_LINE_LIST` | Every 2 vertices form a line |
| `NE_PRIMITIVE_TOPOLOGY_LINE_STRIP` | Each vertex after the first forms a line with the previous vertex |
| `NE_PRIMITIVE_TOPOLOGY_POINT_LIST` | Each vertex is a point |

### `NEBlendFactor` Values

| Value | Description |
|---|---|
| `NE_BLEND_FACTOR_ZERO` | Factor = 0 |
| `NE_BLEND_FACTOR_ONE` | Factor = 1 |
| `NE_BLEND_FACTOR_SRC_ALPHA` | Factor = source alpha |
| `NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA` | Factor = 1 − source alpha |
| `NE_BLEND_FACTOR_DST_ALPHA` | Factor = destination alpha |
| `NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA` | Factor = 1 − destination alpha |

### `NEBlendOp` Values

| Value | Description |
|---|---|
| `NE_BLEND_OP_ADD` | result = src + dst |
| `NE_BLEND_OP_SUBTRACT` | result = src − dst |
| `NE_BLEND_OP_REVERSE_SUBTRACT` | result = dst − src |
| `NE_BLEND_OP_MIN` | result = min(src, dst) |
| `NE_BLEND_OP_MAX` | result = max(src, dst) |

### `NEBlendState` Fields

| Field | Type | Description |
|---|---|---|
| `enabled` | `bool` | Enable blending (if false, writes are opaque) |
| `src_color` | `NEBlendFactor` | Source color factor |
| `dst_color` | `NEBlendFactor` | Destination color factor |
| `color_op` | `NEBlendOp` | Color blend operation |
| `src_alpha` | `NEBlendFactor` | Source alpha factor |
| `dst_alpha` | `NEBlendFactor` | Destination alpha factor |
| `alpha_op` | `NEBlendOp` | Alpha blend operation |

### `NEPipelineDesc` Fields

| Field | Type | Description |
|---|---|---|
| `vertex_shader` | `NEShaderHandle` | Vertex shader module |
| `fragment_shader` | `NEShaderHandle` | Fragment shader module |
| `vertex_layouts` | `const NEVertexBufferLayout*` | Array of vertex buffer layouts |
| `vertex_layout_count` | `uint32_t` | Number of vertex buffer layouts |
| `topology` | `NEPrimitiveTopology` | Primitive assembly mode |
| `blend` | `NEBlendState` | Color attachment blend state |

### `NEComputePipelineDesc` Fields

| Field | Type | Description |
|---|---|---|
| `compute_shader` | `NEShaderHandle` | Compute shader module |

### Constants

| Constant | Description |
|---|---|
| `NE_PIPELINE_HANDLE_NULL` | Zero-initialized null graphics pipeline handle |
| `NE_COMPUTE_PIPELINE_HANDLE_NULL` | Zero-initialized null compute pipeline handle |

### Functions

#### ⚠️ `NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc)`
Creates a graphics pipeline. Compiles vertex layout, shaders, topology, and blend state into a GPU pipeline object. Returns `NE_PIPELINE_HANDLE_NULL` on failure.

#### ⚠️ `void ne_pipeline_destroy(NERenderer *renderer, NEPipelineHandle handle)`
Destroys the graphics pipeline.

#### ⚠️ `NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer, const NEComputePipelineDesc *desc)`
Creates a compute pipeline from a compute shader. Returns `NE_COMPUTE_PIPELINE_HANDLE_NULL` on failure.

#### ⚠️ `void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle)`
Destroys the compute pipeline.

### Utility

#### `bool ne_pipeline_handle_valid(NEPipelineHandle h)`
Returns `true` if the handle is not null. Inline function.

#### `bool ne_compute_pipeline_handle_valid(NEComputePipelineHandle h)`
Returns `true` if the handle is not null. Inline function.

---

<a id="render--compute-passes"></a>
## Render & Compute Passes — `ne_renderer_pass.h`

> ⚠️ **Not yet implemented** in any backend.

Command recording within a frame. A `NERenderPass` is obtained from `ne_renderer_begin_frame()` and used to record graphics and compute commands.

### Types

| Type | Description |
|---|---|
| `NERenderPass` | Opaque render pass handle for graphics commands |
| `NEComputePass` | Opaque compute pass handle for compute commands |

### Graphics Commands

#### ⚠️ `void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline)`
Binds a graphics pipeline for subsequent draw calls.

#### ⚠️ `void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint32_t slot, NEBufferHandle buffer)`
Binds a vertex buffer to the given slot. Slot indices correspond to `vertex_layouts` in the pipeline descriptor.

#### ⚠️ `void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer, NEIndexType type)`
Binds an index buffer for indexed draw calls.

#### ⚠️ `void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage, uint32_t slot, const void *data, uint32_t size)`
Sets uniform / push constant data for the given shader stage and slot. On Vulkan, uses push constants. On Metal, uses `setVertexBytes` / `setFragmentBytes`.

#### ⚠️ `void ne_render_pass_draw(NERenderPass *pass, uint32_t first_vertex, uint32_t vertex_count)`
Draws non-indexed primitives.

#### ⚠️ `void ne_render_pass_draw_indexed(NERenderPass *pass, uint32_t index_count, uint32_t first_index, int32_t vertex_offset)`
Draws indexed primitives.

### Compute Pass Lifecycle

#### ⚠️ `NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass)`
Begins a compute pass within the current frame. Returns a `NEComputePass*` for recording compute commands.

#### ⚠️ `void ne_render_pass_end_compute(NERenderPass *pass, NEComputePass *compute)`
Ends the compute pass.

### Compute Commands

#### ⚠️ `void ne_compute_pass_set_pipeline(NEComputePass *pass, NEComputePipelineHandle pipeline)`
Binds a compute pipeline.

#### ⚠️ `void ne_compute_pass_set_storage_buffer(NEComputePass *pass, uint32_t slot, NEBufferHandle buffer)`
Binds a storage buffer to the given slot.

#### ⚠️ `void ne_compute_pass_set_uniform_data(NEComputePass *pass, uint32_t slot, const void *data, uint32_t size)`
Sets uniform data for the compute shader.

#### ⚠️ `void ne_compute_pass_dispatch(NEComputePass *pass, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)`
Dispatches compute work groups.
