# --- Windows build settings ----------------------------------------------
# Force cmd.exe recipes when building from a non-MSYS environment.
SHELL := cmd.exe
.SHELLFLAGS := /C

OBJ_EXT := obj

# `cmd.exe` does not support `mkdir -p`.
mkdir_p = if not exist "$(subst /,\\,$(1))" mkdir "$(subst /,\\,$(1))"
rmdir_rf = if exist "$(subst /,\\,$(1))" rmdir /s /q "$(subst /,\\,$(1))"

# --- Sources --------------------------------------------------------------
SRC_C += src/platform/win32/window_win32.c
SRC_C += src/renderer/vulkan/ne_renderer_vulkan.c

# --- Compile flags --------------------------------------------------------
# Windows UCRT headers mark fopen/strcpy/etc as deprecated unless this is set.
# We keep standard C APIs and simply silence the deprecation warnings.
CFLAGS += -D_CRT_SECURE_NO_WARNINGS

# We explicitly call the wide (W) Win32 APIs; define UNICODE to keep resource
# macros (e.g. IDC_ARROW) type-correct.
CFLAGS += -DUNICODE -D_UNICODE

SHADERS_DIR := $(BUILD_DIR)/shaders

# --- Link flags -----------------------------------------------------------
# Win32 platform deps only (renderer backend comes later).
# Keep MSVC-style .lib names. Use -Xlinker so clang doesn't require the .lib
# to be present in the current working directory; the linker will resolve it
# via its library search paths.
LDFLAGS += /link
LDFLAGS += user32.lib
LDFLAGS += gdi32.lib
LDFLAGS += dwmapi.lib

LDASANFLAGS := clang_rt.asan-x86_64.lib

# --- Dependencies: Vulkan-Headers (headers only) --------------------------
# Vulkan runtime (vulkan-1.dll) is provided by the driver/runtime installer.
# To keep dependencies minimal we dynamically load it, so we only need headers.
VULKAN_HEADERS_VERSION ?= 1.3.280
VULKAN_HEADERS_DIR := external/vulkan-headers
VULKAN_HEADERS_INCLUDE_DIR := $(VULKAN_HEADERS_DIR)/include
VULKAN_HEADERS_MARKER := $(VULKAN_HEADERS_INCLUDE_DIR)/vulkan/vulkan.h
VULKAN_HEADERS_ZIP := $(BUILD_DIR)/deps/vulkan-headers-$(VULKAN_HEADERS_VERSION).zip
VULKAN_HEADERS_URL := https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$(VULKAN_HEADERS_VERSION).zip

# Ensure headers exist before compiling any translation unit.
EXTRA_OBJECT_DEPS += $(VULKAN_HEADERS_MARKER)
CFLAGS += -I$(VULKAN_HEADERS_INCLUDE_DIR)

# --- SPIR-V shader compilation (glslc from Vulkan SDK) -------------------
# glslc must be on PATH.  The Vulkan SDK installer adds it automatically:
#   C:\VulkanSDK\<version>\Bin\glslc.exe
#
# Output .spv files land in $(SHADERS_DIR) so they are co-located with the
# build artefacts.  main.c loads them at runtime relative to the working
# directory, so the executable must be run from the repo root (the default
# when invoking through the Makefile).
GLSLC ?= glslc

SPIRV_VERT := $(SHADERS_DIR)/basic.vert.spv
SPIRV_FRAG := $(SHADERS_DIR)/basic.frag.spv

# Feed the compiled .spv paths to main.c via preprocessor defines so the
# paths stay in one place and don't need to be hard-coded in C source.
CFLAGS += -DNE_SPIRV_VERT_PATH="\"$(subst \,/,$(SHADERS_DIR))/basic.vert.spv\""
CFLAGS += -DNE_SPIRV_FRAG_PATH="\"$(subst \,/,$(SHADERS_DIR))/basic.frag.spv\""

$(SPIRV_VERT): shaders/glsl/basic.vert
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@$(GLSLC) -fshader-stage=vert -o "$(subst /,\\,$@)" "$<"

$(SPIRV_FRAG): shaders/glsl/basic.frag
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@$(GLSLC) -fshader-stage=frag -o "$(subst /,\\,$@)" "$<"

# Compiled shaders must exist before the executable is linked / run.
EXTRA_OBJECT_DEPS += $(SPIRV_VERT) $(SPIRV_FRAG)

# --- Dependencies: glslang (runtime GLSL → SPIR-V compilation) -----------
#
# glslang binaries are fetched from github. A copy of those binaries exist in the vulkan SDF
# but here we make no assumption about the existence of the SDK on the user's machine
#

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

.PHONY: deps deps-vulkan-headers deps-shaders deps-glslang

deps: deps-vulkan-headers deps-shaders deps-glslang

deps-shaders: $(SPIRV_VERT) $(SPIRV_FRAG)

deps-glslang: $(GLSLANG_HEADER_MARKER)

$(GLSLANG_HEADER_MARKER):
	@echo Downloading glslang lib...
	@$(call mkdir_p,external/deps)
	@$(call mkdir_p,$(GLSLANG_DIR))
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri '$(GLSLANG_URL)' -OutFile '$(subst /,\\,$(GLSLANG_ZIP))';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Expand-Archive -Force '$(subst /,\\,$(GLSLANG_ZIP))' '$(subst /,\\,$(GLSLANG_DIR))';"

deps-vulkan-headers: $(VULKAN_HEADERS_MARKER)

$(VULKAN_HEADERS_MARKER):
	@echo DEPS Vulkan-Headers v$(VULKAN_HEADERS_VERSION)
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri '$(VULKAN_HEADERS_URL)' -OutFile '$(subst /,\\,$(VULKAN_HEADERS_ZIP))';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Expand-Archive -Force '$(subst /,\\,$(VULKAN_HEADERS_ZIP))' '$(subst /,\\,$(BUILD_DIR))\\deps';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; if (Test-Path '$(subst /,\\,$(VULKAN_HEADERS_DIR))') { Remove-Item -Recurse -Force '$(subst /,\\,$(VULKAN_HEADERS_DIR))' };"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; $$src = Join-Path '$(subst /,\\,$(BUILD_DIR))\\deps' 'Vulkan-Headers-$(VULKAN_HEADERS_VERSION)'; Move-Item -Force $$src '$(subst /,\\,$(VULKAN_HEADERS_DIR))';"
