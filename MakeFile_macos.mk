# --- Preflight ------------------------------------------------------------
# Cocoa/Metal frameworks and the Objective-C compiler come with Xcode Command
# Line Tools. Without them clang emits opaque "framework 'Cocoa' not found"
# errors deep in the link stage.
ifeq ($(shell xcode-select -p 2>/dev/null),)
$(error Xcode Command Line Tools not found. Install with: xcode-select --install)
endif

SRC_M += src/platform/macos/window_macos.m
SRC_M += src/renderer/metal/ne_renderer_metal.m

# Link against the system frameworks the Cocoa + Metal backends need.
# Toolchain (LD=clang) and empty LDFLAGS are inherited from the top-level Makefile.
LDFLAGS += -framework Cocoa
LDFLAGS += -framework Metal -framework QuartzCore
