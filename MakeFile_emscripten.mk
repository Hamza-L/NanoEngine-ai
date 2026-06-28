# --- Web (Emscripten / WebGPU) build settings -----------------------------
#
# Opt in with:  make PLATFORM=web
# Run with:     emrun build/NanoEngine.html   (a WebGPU-capable browser)
#
# Requires the Emscripten SDK (emcc) on PATH. webgpu.h ships in the emscripten
# sysroot under -sUSE_WEBGPU=1, so there is no header download step (unlike the
# Win32 Vulkan-Headers fetch).
#
# NOTE: pin a known-good emsdk — the WebGPU C ABI has changed across versions.
# Record the `emcc --version` this was verified against here once tested.

CC  := emcc
LD  := emcc
OBJ_EXT := o

# --- Sources --------------------------------------------------------------
SRC_C += src/platform/web/window_web.c
SRC_C += src/renderer/wgpu/ne_renderer_wgpu.c

# --- Compile / link flags -------------------------------------------------
# WebGPU bindings (compile + link). No -sASYNCIFY: device init uses the
# non-blocking callback design, not blocking waits.
CFLAGS  += -sUSE_WEBGPU=1
LDFLAGS += -sUSE_WEBGPU=1
LDFLAGS += -sALLOW_MEMORY_GROWTH=1
# ne_app_run hands the loop to the browser (rAF) and main() unwinds; keep the
# runtime alive so heap objects (renderer/app) and the rAF callback persist.
LDFLAGS += -sEXIT_RUNTIME=0
# Make the WGSL shader readable via ne_file_read at the path main.c expects.
LDFLAGS += --preload-file shaders/wgsl/basic.wgsl

# emcc links objects straight to .html + .js + .wasm. The top-level Makefile
# computes OUTPUT before this include, so override it here (setting EXE_EXT
# alone would be too late).
OUTPUT := $(BUILD_DIR)/$(APP_NAME).html
