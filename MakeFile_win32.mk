SRC_C += src/main.c
SRC_C += src/ne_log.c
SRC_C += src/platform/win32/window_win32.c

# Temporary stub renderer so the demo links while we bring up Win32.
SRC_C += src/renderer/stub/ne_renderer_stub.c

# Windows UCRT headers mark fopen/strcpy/etc as deprecated unless this is set.
# We keep standard C APIs and simply silence the deprecation warnings.
CFLAGS += -D_CRT_SECURE_NO_WARNINGS

# We explicitly call the wide (W) Win32 APIs; define UNICODE to keep resource
# macros (e.g. IDC_ARROW) type-correct.
CFLAGS += -DUNICODE -D_UNICODE

SHADERS_DIR := $(BUILD_DIR)/shader-code

# Win32 platform deps only (renderer backend comes later).
# Keep MSVC-style .lib names. Use -Xlinker so clang doesn't require the .lib
# to be present in the current working directory; the linker will resolve it
# via its library search paths.
LDFLAGS += -Xlinker user32.lib
LDFLAGS += -Xlinker gdi32.lib
LDFLAGS += -Xlinker dwmapi.lib

LDASANFLAGS := clang_rt.asan-x86_64.lib
