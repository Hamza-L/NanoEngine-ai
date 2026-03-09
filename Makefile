CC := clang
OBJC := clang
BUILD_DIR := build

# Public target name (what users type): no extension, consistent across platforms.
APP_NAME := NanoEngine

# Actual output file name differs on Windows.
ifeq ($(OS),Windows_NT)
EXE_EXT := .exe
else
EXE_EXT :=
endif

OUTPUT := $(BUILD_DIR)/$(APP_NAME)$(EXE_EXT)

# Dependency makefiles may be included before targets are defined; make sure the
# default goal stays consistent.
.DEFAULT_GOAL := all

CFLAGS := -std=c2x -O2 -g -Wall -Wextra -Wpedantic -Werror
OBJCFLAGS := -fobjc-arc
LDFLAGS :=

# Platform-independent sources (platform makefiles append platform-specific ones).
SRC_C := src/main.c src/ne_log.c src/ne_file.c
SRC_M :=

# Platform makefiles can append to this to force prerequisites before any
# object is compiled (e.g. downloaded headers).
EXTRA_OBJECT_DEPS :=

# Default platform settings (may be overridden by platform makefiles).
OBJ_EXT := o
mkdir_p = mkdir -p "$(1)"
rmdir_rf = rm -rf "$(1)"

ifeq ($(OS),Windows_NT)
include MakeFile_win32.mk
else
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
include MakeFile_macos.mk
else
$(error Unsupported platform for step 1. Add Linux target if needed)
endif
endif

OBJ_C := $(SRC_C:%.c=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJ_M := $(SRC_M:%.m=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJS := $(OBJ_C) $(OBJ_M)

DEPS := $(OBJS:.$(OBJ_EXT)=.d)

# Automatically include generated dependency files (if present).
-include $(DEPS)

.PHONY: all clean $(APP_NAME)

# Default build (same command on every platform):
#   make
# builds the actual output file (with .exe on Windows).
all: $(APP_NAME)

# User-facing target without extension.
$(APP_NAME): $(OUTPUT)

# Real file target used for correct incremental linking.
$(OUTPUT): $(BUILD_DIR) $(OBJS)
	@echo LINK $(OUTPUT)
	@$(CC) $(OBJS) -o $(OUTPUT) $(LDFLAGS)

$(BUILD_DIR):
	@$(call mkdir_p,$(BUILD_DIR))

# Generate dependencies alongside objects using Clang's -MMD/-MP.
DEPFLAGS := -MMD -MP

$(BUILD_DIR)/%.$(OBJ_EXT): %.c $(EXTRA_OBJECT_DEPS)
	@echo CC $<
	@$(call mkdir_p,$(dir $@))
	@$(CC) $(CFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

$(BUILD_DIR)/%.$(OBJ_EXT): %.m $(EXTRA_OBJECT_DEPS)
	@echo OBJC $<
	@$(call mkdir_p,$(dir $@))
	@$(OBJC) $(CFLAGS) $(OBJCFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

clean:
	@$(call rmdir_rf,$(BUILD_DIR))
