# Cocoa/Metal frameworks and Objective-C come with Xcode Command Line Tools.
$(call require_probe,Xcode Command Line Tools,xcode-select -p,/,Install with: xcode-select --install)

SRC_M += src/platform/macos/window_macos.m
SRC_M += src/renderer/metal/ne_renderer_metal.m

# Link against the system frameworks the Cocoa + Metal backends need.
# Toolchain (LD=clang) and empty LDFLAGS are inherited from the top-level Makefile.
LDFLAGS += -framework Cocoa
LDFLAGS += -framework Metal -framework QuartzCore
