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

# MSVC tools (link.exe, MSBuild) write scratch files under %TMP%. Under an
# MSYS2/Cygwin shell that variable can point at a Unix-style path the MSVC
# tools reject; under a locked-down account it can be C:\Windows\... which is
# not writable. Point both env vars at the build dir for every recipe.
export TMP  := $(CURDIR)/build
export TEMP := $(CURDIR)/build

OBJ_EXT := obj

# --- Preflight: fail fast with actionable messages ------------------------
$(call require_tool,clang-cl,clang version,Ships with LLVM/Clang. Ensure the LLVM bin dir is on PATH.)
$(call require_tool,cmake,cmake version,Install CMake from https://cmake.org/download/ (or `winget install Kitware.CMake`) and add it to PATH.)
$(call require_tool,tar,tar,tar ships with Windows 10 build 17063+. Update Windows or install via Git for Windows.)
$(call require_probe,tool 'powershell',powershell -NoProfile -Command "echo ok",ok,PowerShell ships with Windows$(comma) so this usually means the PowerShell exe is not on PATH. Repair your Windows install or install PowerShell 7 from https://aka.ms/powershell.)
$(call require_probe,MSVC linker (link.exe),where link.exe,link.exe,Open an "x64 Native Tools Command Prompt for VS 2022" or run vcvars64.bat before invoking make. Requires Visual Studio 2022 or "Build Tools for Visual Studio 2022" with the Desktop C++ workload.)

# --- Sources --------------------------------------------------------------
SRC_C += src/platform/win32/window_win32.c
SRC_C += src/renderer/vulkan/ne_renderer_vulkan.c
SRC_C += src/renderer/vulkan/ne_swapchain_vulkan_wsi.c
SRC_C += src/renderer/vulkan/ne_swapchain_dxgi.c

# internal
SRC_C += src/renderer/vulkan/internal/ne_vulkan_renderer.c
SRC_C += src/renderer/vulkan/internal/ne_vulkan_buffers.c
SRC_C += src/renderer/vulkan/internal/ne_vulkan_images.c
SRC_C += src/renderer/vulkan/internal/ne_vulkan_shaders.c
SRC_C += src/renderer/vulkan/internal/ne_vulkan_pipelines.c

# --- Compile flags --------------------------------------------------------
CFLAGS += -D_CRT_SECURE_NO_WARNINGS
CFLAGS += -DUNICODE -D_UNICODE
CFLAGS += -Isrc/renderer/vulkan

SHADERS_DIR := $(BUILD_DIR)/shaders

# --- Toolchain ------------------------------------------------------------
LD := clang-cl

# --- Link flags -----------------------------------------------------------
# clang-cl link line: <objs> -o <out> <LDFLAGS> <LDLIBS>. -link is the last
# LDFLAGS entry so everything in LDLIBS is forwarded to the MSVC linker.
LDFLAGS += -Z7 -MDd
LDFLAGS += -link

LDLIBS += user32.lib
LDLIBS += gdi32.lib
LDLIBS += dwmapi.lib
LDLIBS += d3d11.lib
LDLIBS += dxgi.lib

ifeq ($(ASAN),1)
LDLIBS += clang_rt.asan-x86_64.lib
endif

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
GLSLANG_VERSION ?= 16.5.0
GLSLANG_DIR := external/glslang
GLSLANG_INCLUDE_DIR := $(GLSLANG_DIR)/include/glslang/Include
GLSLANG_HEADER_MARKER := $(GLSLANG_INCLUDE_DIR)/glslang_c_interface.h
GLSLANG_ZIP := external/deps/glslang-$(GLSLANG_VERSION).zip
GLSLANG_URL := https://github.com/KhronosGroup/glslang/releases/download/$(GLSLANG_VERSION)/glslang-$(GLSLANG_VERSION)-windows-x86_64-release.zip

EXTRA_OBJECT_DEPS += $(GLSLANG_HEADER_MARKER)
CFLAGS += -I$(GLSLANG_INCLUDE_DIR)
LDLIBS += $(GLSLANG_DIR)/lib/glslang.lib
LDLIBS += $(GLSLANG_DIR)/lib/glslang-default-resource-limits.lib
LDLIBS += $(GLSLANG_DIR)/lib/SPIRV-Tools-opt.lib
LDLIBS += $(GLSLANG_DIR)/lib/SPIRV-Tools.lib

# --- SPIR-V shader compilation (local glslang) -------------------
GLSLANG_BIN := $(GLSLANG_DIR)/bin/glslang.exe

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

# --- Dependencies: FreeType (built from upstream source) ------------------
# Built once via CMake+MSBuild; the produced .lib is the marker, so a second
# `make` sees the lib and skips fetch/configure/compile entirely. CRT is /MDd
# to match the top-level build.
FREETYPE_VERSION ?= 2.13.3
FREETYPE_SRC_DIR := external/freetype-src
FREETYPE_BUILD_DIR := external/freetype-build
FREETYPE_DIR := external/freetype
FREETYPE_INCLUDE_DIR := $(FREETYPE_DIR)/include/freetype2
FREETYPE_LIB := $(FREETYPE_DIR)/lib/freetyped.lib
FREETYPE_TARBALL := external/deps/freetype-$(FREETYPE_VERSION).tar.gz
FREETYPE_URL := https://download.savannah.gnu.org/releases/freetype/freetype-$(FREETYPE_VERSION).tar.gz

EXTRA_OBJECT_DEPS += $(FREETYPE_LIB)
CFLAGS += -I$(FREETYPE_INCLUDE_DIR)
LDLIBS += $(FREETYPE_LIB)

$(FREETYPE_LIB):
	@echo "DEPS FreeType v$(FREETYPE_VERSION) (building from source)"
	@$(call mkdir_p,external/deps)
	@powershell -NoProfile -Command "Invoke-WebRequest -Uri '$(FREETYPE_URL)' -OutFile '$(FREETYPE_TARBALL)'"
	@tar -xzf $(FREETYPE_TARBALL) -C external/deps
	@powershell -NoProfile -Command "if (Test-Path '$(FREETYPE_SRC_DIR)') { Remove-Item -Recurse -Force '$(FREETYPE_SRC_DIR)' }"
	@powershell -NoProfile -Command "Move-Item -Force 'external/deps/freetype-$(FREETYPE_VERSION)' '$(FREETYPE_SRC_DIR)'"
	@cmake -S $(FREETYPE_SRC_DIR) -B $(FREETYPE_BUILD_DIR) -G "Visual Studio 17 2022" -A x64 \
		-DCMAKE_INSTALL_PREFIX=$(CURDIR)/$(FREETYPE_DIR) \
		-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL \
		-DBUILD_SHARED_LIBS=OFF \
		-DFT_DISABLE_ZLIB=TRUE \
		-DFT_DISABLE_BZIP2=TRUE \
		-DFT_DISABLE_PNG=TRUE \
		-DFT_DISABLE_HARFBUZZ=TRUE \
		-DFT_DISABLE_BROTLI=TRUE
	@cmake --build $(FREETYPE_BUILD_DIR) --config Debug --target install

# --- Dependency download rules --------------------------------------------
.PHONY: deps deps-vulkan-headers deps-shaders deps-glslang deps-freetype

deps: deps-vulkan-headers deps-shaders deps-glslang deps-freetype

deps-shaders: $(SPIRV_VERT) $(SPIRV_FRAG)

deps-glslang: $(GLSLANG_HEADER_MARKER)

deps-vulkan-headers: $(VULKAN_HEADERS_MARKER)

deps-freetype: $(FREETYPE_LIB)

$(GLSLANG_HEADER_MARKER):
	@echo "DEPS glslang (downloading...)"
	@$(call mkdir_p,external/deps)
	@$(call mkdir_p,$(GLSLANG_DIR))
	@powershell -NoProfile -Command "Invoke-WebRequest -Uri '$(GLSLANG_URL)' -OutFile '$(GLSLANG_ZIP)'"
	@powershell -NoProfile -Command "Expand-Archive -Force '$(GLSLANG_ZIP)' '$(GLSLANG_DIR)'"

$(VULKAN_HEADERS_MARKER):
	@echo "DEPS Vulkan-Headers v$(VULKAN_HEADERS_VERSION)"
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "Invoke-WebRequest -Uri '$(VULKAN_HEADERS_URL)' -OutFile '$(VULKAN_HEADERS_ZIP)'"
	@powershell -NoProfile -Command "Expand-Archive -Force '$(VULKAN_HEADERS_ZIP)' '$(BUILD_DIR)/deps'"
	@powershell -NoProfile -Command "if (Test-Path '$(VULKAN_HEADERS_DIR)') { Remove-Item -Recurse -Force '$(VULKAN_HEADERS_DIR)' }"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "Move-Item -Force '$(BUILD_DIR)/deps/Vulkan-Headers-$(VULKAN_HEADERS_VERSION)' '$(VULKAN_HEADERS_DIR)'"
