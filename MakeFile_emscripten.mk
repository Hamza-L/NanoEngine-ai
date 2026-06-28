# --- Web (Emscripten / WebGPU) build settings -----------------------------
#
# Opt in with:  make PLATFORM=web
# Run with:     emrun build/NanoEngine.html   (a WebGPU-capable browser)
#
# No preinstalled toolchain required: if the Emscripten SDK is not present it is
# fetched, installed, and activated automatically (same spirit as the Win32
# Vulkan-Headers / glslang fetch). To reuse an existing SDK instead, point at it:
#   make PLATFORM=web EMSDK_DIR=/path/to/emsdk
#
# NOTE: the WebGPU C ABI has changed across emsdk versions; this targets the
# pinned EMSDK_VERSION below. Bump deliberately and re-verify the wgpu* call
# sites flagged in src/renderer/wgpu/ne_renderer_wgpu.c.

OBJ_EXT := o

# --- Toolchain: Emscripten SDK (fetched if absent) ------------------------
EMSDK_VERSION ?= 5.0.2
EMSDK_DIR     ?= external/emsdk
EMSDK_URL     ?= https://github.com/emscripten-core/emsdk.git

# The emcc launcher inside the SDK is the marker that it is installed+activated.
EMCC_BIN := $(EMSDK_DIR)/upstream/emscripten/emcc
EMSDK_ENV := $(EMSDK_DIR)/emsdk_env.sh

# Invoke emcc through a sourced env so it finds its config + bundled node,
# exactly as the documented workflow does. Works whether or not the SDK is on
# the user's PATH.
CC := . $(EMSDK_ENV) >/dev/null 2>&1 && emcc
LD := . $(EMSDK_ENV) >/dev/null 2>&1 && emcc

# Make every object depend on the SDK so the fetch runs before any compile.
EXTRA_OBJECT_DEPS += $(EMCC_BIN)

$(EMCC_BIN):
	@echo DEPS Emscripten SDK $(EMSDK_VERSION)
	@if [ ! -d "$(EMSDK_DIR)" ]; then git clone "$(EMSDK_URL)" "$(EMSDK_DIR)"; fi
	@"$(EMSDK_DIR)/emsdk" install $(EMSDK_VERSION)
	@"$(EMSDK_DIR)/emsdk" activate $(EMSDK_VERSION)

# --- Sources --------------------------------------------------------------
SRC_C += src/platform/web/window_web.c
SRC_C += src/renderer/wgpu/ne_renderer_wgpu.c

# --- Compile / link flags -------------------------------------------------
# WebGPU bindings (compile + link) via the emdawnwebgpu port (emsdk >= ~3.1.x
# replaced -sUSE_WEBGPU=1 with this; it ships Dawn's standardized webgpu.h).
# No -sASYNCIFY: device init uses the non-blocking callback design.
CFLAGS  += --use-port=emdawnwebgpu
LDFLAGS += --use-port=emdawnwebgpu
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
