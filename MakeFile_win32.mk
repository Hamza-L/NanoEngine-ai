# --- Windows build settings ----------------------------------------------

# Detect shell: if /bin/sh exists we're in MSYS2/Git Bash, otherwise cmd.exe.
ifneq ($(wildcard /bin/sh),)
SHELL := /bin/sh
mkdir_p = mkdir -p "$(1)"
rmdir_rf = rm -rf "$(1)"
else
SHELL := cmd.exe
.SHELLFLAGS := /C
mkdir_p = if not exist "$(subst /,\,$(patsubst %/,%,$(1)))" mkdir "$(subst /,\,$(patsubst %/,%,$(1)))"
rmdir_rf = if exist "$(subst /,\,$(patsubst %/,%,$(1)))" rmdir /s /q "$(subst /,\,$(patsubst %/,%,$(1)))"
endif

OBJ_EXT := obj

# --- Sources --------------------------------------------------------------
SRC_C += src/platform/win32/window_win32.c
SRC_C += src/renderer/vulkan/ne_renderer_vulkan.c
SRC_C += src/renderer/vulkan/ne_swapchain_vulkan_wsi.c
SRC_C += src/renderer/vulkan/ne_swapchain_dxgi.c

# internal
SRC_C += src/renderer/vulkan/internal/ne_vulkan_shaders.c

# --- Compile flags --------------------------------------------------------
CFLAGS += -D_CRT_SECURE_NO_WARNINGS
CFLAGS += -DUNICODE -D_UNICODE
CFLAGS += -Isrc/renderer/vulkan

SHADERS_DIR := $(BUILD_DIR)/shaders

# --- Toolchain ------------------------------------------------------------
LD := clang-cl

# --- Link flags -----------------------------------------------------------
LDFLAGS += -Z7 -MDd
LDFLAGS += -link
LDFLAGS += user32.lib
LDFLAGS += gdi32.lib
LDFLAGS += dwmapi.lib
LDFLAGS += d3d11.lib
LDFLAGS += dxgi.lib

LDASANFLAGS := clang_rt.asan-x86_64.lib

# --- Dependencies: Vulkan-Headers (headers only) --------------------------
VULKAN_HEADERS_VERSION ?= 1.3.280
VULKAN_HEADERS_DIR := external/vulkan-headers
VULKAN_HEADERS_INCLUDE_DIR := $(VULKAN_HEADERS_DIR)/include
VULKAN_HEADERS_MARKER := $(VULKAN_HEADERS_INCLUDE_DIR)/vulkan/vulkan.h
VULKAN_HEADERS_ZIP := $(BUILD_DIR)/deps/vulkan-headers-$(VULKAN_HEADERS_VERSION).zip
VULKAN_HEADERS_URL := https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$(VULKAN_HEADERS_VERSION).zip

EXTRA_OBJECT_DEPS += $(VULKAN_HEADERS_MARKER)
CFLAGS += -I$(VULKAN_HEADERS_INCLUDE_DIR)

# --- Dependencies: glslang (runtime GLSL -> SPIR-V compilation) -----------
GLSLANG_DIR := external/glslang
GLSLANG_INCLUDE_DIR := $(GLSLANG_DIR)/include/glslang/Include
GLSLANG_HEADER_MARKER := $(GLSLANG_INCLUDE_DIR)/glslang_c_interface.h
GLSLANG_ZIP := external/deps/glslang-binaries.zip
GLSLANG_URL := https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-master-windows-Release.zip

EXTRA_OBJECT_DEPS += $(GLSLANG_HEADER_MARKER)
CFLAGS += -I$(GLSLANG_INCLUDE_DIR)
LDFLAGS += $(GLSLANG_DIR)/lib/glslang.lib
LDFLAGS += $(GLSLANG_DIR)/lib/glslang-default-resource-limits.lib
LDFLAGS += $(GLSLANG_DIR)/lib/SPIRV-Tools-opt.lib
LDFLAGS += $(GLSLANG_DIR)/lib/SPIRV-Tools.lib

# --- SPIR-V shader compilation (local glslangValidator) -------------------
GLSLANG_BIN := $(GLSLANG_DIR)/bin/glslangValidator.exe

SPIRV_VERT := $(SHADERS_DIR)/basic.vert.spv
SPIRV_FRAG := $(SHADERS_DIR)/basic.frag.spv

CFLAGS += -DNE_SPIRV_VERT_PATH="\"$(SHADERS_DIR)/basic.vert.spv\""
CFLAGS += -DNE_SPIRV_FRAG_PATH="\"$(SHADERS_DIR)/basic.frag.spv\""

$(SPIRV_VERT): shaders/glsl/basic.vert $(GLSLANG_HEADER_MARKER)
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@"$(GLSLANG_BIN)" -V -S vert -o "$@" "$<"

$(SPIRV_FRAG): shaders/glsl/basic.frag $(GLSLANG_HEADER_MARKER)
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@"$(GLSLANG_BIN)" -V -S frag -o "$@" "$<"

EXTRA_OBJECT_DEPS += $(SPIRV_VERT) $(SPIRV_FRAG)

# --- Dependency download rules --------------------------------------------
.PHONY: deps deps-vulkan-headers deps-shaders deps-glslang

deps: deps-vulkan-headers deps-shaders deps-glslang

deps-shaders: $(SPIRV_VERT) $(SPIRV_FRAG)

deps-glslang: $(GLSLANG_HEADER_MARKER)

deps-vulkan-headers: $(VULKAN_HEADERS_MARKER)

$(GLSLANG_HEADER_MARKER):
	@echo "DEPS glslang (downloading...)"
	@$(call mkdir_p,external/deps)
	@$(call mkdir_p,$(GLSLANG_DIR))
	@powershell -NoProfile -Command "$$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '$(GLSLANG_URL)' -OutFile '$(GLSLANG_ZIP)'"
	@powershell -NoProfile -Command "Expand-Archive -Force '$(GLSLANG_ZIP)' '$(GLSLANG_DIR)'"

$(VULKAN_HEADERS_MARKER):
	@echo "DEPS Vulkan-Headers v$(VULKAN_HEADERS_VERSION)"
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "$$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '$(VULKAN_HEADERS_URL)' -OutFile '$(VULKAN_HEADERS_ZIP)'"
	@powershell -NoProfile -Command "Expand-Archive -Force '$(VULKAN_HEADERS_ZIP)' '$(BUILD_DIR)/deps'"
	@powershell -NoProfile -Command "if (Test-Path '$(VULKAN_HEADERS_DIR)') { Remove-Item -Recurse -Force '$(VULKAN_HEADERS_DIR)' }"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "Move-Item -Force '$(BUILD_DIR)/deps/Vulkan-Headers-$(VULKAN_HEADERS_VERSION)' '$(VULKAN_HEADERS_DIR)'"
