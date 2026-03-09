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
LDFLAGS += -Xlinker user32.lib
LDFLAGS += -Xlinker gdi32.lib
LDFLAGS += -Xlinker dwmapi.lib
LDFLAGS += -g

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

$(SPIRV_VERT): shaders/spirv/basic.vert
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@$(GLSLC) -fshader-stage=vert -o "$(subst /,\\,$@)" "$<"

$(SPIRV_FRAG): shaders/spirv/basic.frag
	@echo GLSLC $<
	@$(call mkdir_p,$(SHADERS_DIR))
	@$(GLSLC) -fshader-stage=frag -o "$(subst /,\\,$@)" "$<"

# Compiled shaders must exist before the executable is linked / run.
EXTRA_OBJECT_DEPS += $(SPIRV_VERT) $(SPIRV_FRAG)

# --- Dependencies: shaderc (runtime GLSL → SPIR-V compilation) -----------
#
# Headers are fetched from the shaderc GitHub repository (source-only zip,
# no submodules — a few MB).  The DLL is NOT published as a standalone GitHub
# release; it ships as part of the Vulkan SDK.  We copy it from there so it
# is bundled next to the executable and can be found at runtime.
#
# Requires the Vulkan SDK to be installed:
#   https://vulkan.lunarg.com/sdk/home
# The installer sets the VULKAN_SDK environment variable automatically.

SHADERC_VERSION ?= 2024.3
SHADERC_DIR := external/shaderc
SHADERC_INCLUDE_DIR := $(SHADERC_DIR)/libshaderc/include
SHADERC_HEADER_MARKER := $(SHADERC_INCLUDE_DIR)/shaderc/shaderc.h
SHADERC_ZIP := $(BUILD_DIR)/deps/shaderc-$(SHADERC_VERSION).zip
SHADERC_URL := https://github.com/google/shaderc/archive/refs/tags/v$(SHADERC_VERSION).zip

# shaderc_shared.dll comes from the Vulkan SDK.
# Hard-fail during make if the SDK is not installed — the engine requires it
# for runtime shader compilation.
ifndef VULKAN_SDK
$(error VULKAN_SDK environment variable is not set. \
Install the Vulkan SDK from https://vulkan.lunarg.com/sdk/home \
to enable runtime GLSL shader compilation.)
endif

SHADERC_DLL_SRC := $(VULKAN_SDK)/Bin/shaderc_shared.dll
SHADERC_DLL_DST := $(BUILD_DIR)/shaderc_shared.dll

EXTRA_OBJECT_DEPS += $(SHADERC_HEADER_MARKER) $(SHADERC_DLL_DST)
CFLAGS += -I$(SHADERC_INCLUDE_DIR)

.PHONY: deps deps-vulkan-headers deps-shaders deps-shaderc

deps: deps-vulkan-headers deps-shaders deps-shaderc

deps-shaders: $(SPIRV_VERT) $(SPIRV_FRAG)

deps-shaderc: $(SHADERC_HEADER_MARKER) $(SHADERC_DLL_DST)

$(SHADERC_HEADER_MARKER):
	@echo DEPS shaderc headers v$(SHADERC_VERSION)
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri '$(SHADERC_URL)' -OutFile '$(subst /,\\,$(SHADERC_ZIP))';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Expand-Archive -Force '$(subst /,\\,$(SHADERC_ZIP))' '$(subst /,\\,$(BUILD_DIR))\\deps';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; if (Test-Path '$(subst /,\\,$(SHADERC_DIR))') { Remove-Item -Recurse -Force '$(subst /,\\,$(SHADERC_DIR))' };"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; $$src = Join-Path '$(subst /,\\,$(BUILD_DIR))\\deps' 'shaderc-$(SHADERC_VERSION)'; Move-Item -Force $$src '$(subst /,\\,$(SHADERC_DIR))';"

# Copy shaderc_shared.dll from the Vulkan SDK into the build output directory.
# The executable will find it at runtime because they share the same directory.
$(SHADERC_DLL_DST):
	@echo COPY shaderc_shared.dll from Vulkan SDK
	@$(call mkdir_p,$(BUILD_DIR))
	@copy /Y "$(subst /,\\,$(SHADERC_DLL_SRC))" "$(subst /,\\,$(SHADERC_DLL_DST))"

deps-vulkan-headers: $(VULKAN_HEADERS_MARKER)

$(VULKAN_HEADERS_MARKER):
	@echo DEPS Vulkan-Headers v$(VULKAN_HEADERS_VERSION)
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri '$(VULKAN_HEADERS_URL)' -OutFile '$(subst /,\\,$(VULKAN_HEADERS_ZIP))';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Expand-Archive -Force '$(subst /,\\,$(VULKAN_HEADERS_ZIP))' '$(subst /,\\,$(BUILD_DIR))\\deps';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; if (Test-Path '$(subst /,\\,$(VULKAN_HEADERS_DIR))') { Remove-Item -Recurse -Force '$(subst /,\\,$(VULKAN_HEADERS_DIR))' };"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; $$src = Join-Path '$(subst /,\\,$(BUILD_DIR))\\deps' 'Vulkan-Headers-$(VULKAN_HEADERS_VERSION)'; Move-Item -Force $$src '$(subst /,\\,$(VULKAN_HEADERS_DIR))';"
