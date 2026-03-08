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

# Vulkan renderer backend (Windows).
SRC_C += src/renderer/vulkan/ne_renderer_vulkan.c

# --- Compile flags --------------------------------------------------------
# Windows UCRT headers mark fopen/strcpy/etc as deprecated unless this is set.
# We keep standard C APIs and simply silence the deprecation warnings.
CFLAGS += -D_CRT_SECURE_NO_WARNINGS

# We explicitly call the wide (W) Win32 APIs; define UNICODE to keep resource
# macros (e.g. IDC_ARROW) type-correct.
CFLAGS += -DUNICODE -D_UNICODE

SHADERS_DIR := $(BUILD_DIR)/shader-code

# --- Link flags -----------------------------------------------------------
# Win32 platform deps only (renderer backend comes later).
# Keep MSVC-style .lib names. Use -Xlinker so clang doesn't require the .lib
# to be present in the current working directory; the linker will resolve it
# via its library search paths.
LDFLAGS += -Xlinker user32.lib
LDFLAGS += -Xlinker gdi32.lib
LDFLAGS += -Xlinker dwmapi.lib

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

.PHONY: deps deps-vulkan-headers

deps: deps-vulkan-headers

deps-vulkan-headers: $(VULKAN_HEADERS_MARKER)

$(VULKAN_HEADERS_MARKER):
	@echo DEPS Vulkan-Headers v$(VULKAN_HEADERS_VERSION)
	@$(call mkdir_p,$(BUILD_DIR)/deps)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri '$(VULKAN_HEADERS_URL)' -OutFile '$(subst /,\\,$(VULKAN_HEADERS_ZIP))';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; Expand-Archive -Force '$(subst /,\\,$(VULKAN_HEADERS_ZIP))' '$(subst /,\\,$(BUILD_DIR))\\deps';"
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; if (Test-Path '$(subst /,\\,$(VULKAN_HEADERS_DIR))') { Remove-Item -Recurse -Force '$(subst /,\\,$(VULKAN_HEADERS_DIR))' };"
	@$(call mkdir_p,external)
	@powershell -NoProfile -Command "$$ErrorActionPreference='Stop'; $$src = Join-Path '$(subst /,\\,$(BUILD_DIR))\\deps' 'Vulkan-Headers-$(VULKAN_HEADERS_VERSION)'; Move-Item -Force $$src '$(subst /,\\,$(VULKAN_HEADERS_DIR))';"
