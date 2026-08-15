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

# --- Preflight ------------------------------------------------------------
# The web build's recipes are bash-based (sourcing emsdk_env.sh, `if [ -d ]`).
# On Windows we require a POSIX shell (Git for Windows ships /bin/sh); on Unix
# it is always present. Fail early with a clear message if it is missing.
ifeq ($(IS_WINDOWS),1)
ifeq ($(wildcard /bin/sh),)
$(error PLATFORM=web on Windows requires a POSIX shell. Install Git for Windows (https://git-scm.com/downloads) and invoke make from Git Bash$(comma) or ensure C:\Program Files\Git\usr\bin is on PATH.)
endif
SHELL := /bin/sh
endif

# emsdk bootstraps itself, but it needs git to clone and python at runtime.
$(call require_tool,git,git version,Install Git — https://git-scm.com/downloads (or your package manager).)
$(call require_tool,python,Python,emsdk needs Python 3. Install from https://python.org or your package manager.)

# --- Toolchain: Emscripten SDK (fetched if absent) ------------------------
EMSDK_VERSION ?= 5.0.2
EMSDK_DIR     ?= external/emsdk
EMSDK_URL     ?= https://github.com/emscripten-core/emsdk.git

# Marker that the SDK is installed+activated. emcc.py is the platform-neutral
# Python entry point that exists after `emsdk install` on every OS (the bare
# `emcc` shell wrapper may be absent on Windows).
EMCC_BIN := $(EMSDK_DIR)/upstream/emscripten/emcc.py
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
