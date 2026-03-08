SRC_M += src/platform/macos/window_macos.m
SRC_M += src/renderer/metal/ne_renderer_metal.m

LDFLAGS += -framework Cocoa
LDFLAGS += -framework Metal -framework QuartzCore
