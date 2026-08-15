# Toolchain defaults are platform-neutral; platform makefiles (included below)
# override LD/LDFLAGS as needed (e.g. Win32 uses the MSVC-style clang-cl driver).
CC := clang
LD := clang
OBJC := clang

# Public target name (what users type): no extension, consistent across platforms.
APP_NAME := NanoEngine

# Platform detection (works from cmd.exe, PowerShell, and MSYS2/Git Bash).
# On native Windows, $(OS) is always Windows_NT — skip uname entirely.
IS_WINDOWS :=
ifeq ($(OS),Windows_NT)
IS_WINDOWS := 1
else
UNAME_S := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
IS_WINDOWS := 1
endif
endif

ifeq ($(IS_WINDOWS),1)
EXE_EXT := .exe
else
EXE_EXT :=
endif

# Determine if this is a test build based on MAKECMDGOALS
ifeq ($(filter test,$(MAKECMDGOALS)),test)
BUILD_DIR := build/test
BUILD_MODE := test
TESTING_ENABLED := 1
else
BUILD_DIR := build
BUILD_MODE := normal
TESTING_ENABLED := 0
endif

OUTPUT := $(BUILD_DIR)/$(APP_NAME)$(EXE_EXT)
TEST_OUTPUT := $(BUILD_DIR)/$(APP_NAME)_test$(EXE_EXT)

# Dependency makefiles may be included before targets are defined; make sure the
# default goal stays consistent.
.DEFAULT_GOAL := all

# `make V=1` echoes the underlying compile/link commands.
ifeq ($(V),1)
Q :=
else
Q := @
endif

CFLAGS := -std=c23 -g -DDEBUG -Wall -Wextra -Wpedantic -Werror -DTESTING_ENABLED=$(TESTING_ENABLED)
CFLAGS += -Iexternal
OBJCFLAGS := -fobjc-arc
LDFLAGS :=
LDLIBS :=

ifeq ($(ASAN),1)
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
endif

# Platform-independent sources (platform makefiles append platform-specific ones).
SRC_C := src/main.c src/ne_log.c src/ne_file.c src/ne_alloc.c src/ne_frame.c
SRC_M :=

# Test sources (only compiled when TESTING_ENABLED=1)
ifeq ($(TESTING_ENABLED),1)
SRC_C += src/test/test_buffer.c
endif

# Platform makefiles can append to this to force prerequisites before any
# object is compiled (e.g. downloaded headers).
EXTRA_OBJECT_DEPS :=

# Default platform settings (may be overridden by platform makefiles).
OBJ_EXT := o
mkdir_p = mkdir -p "$(1)"
rmdir_rf = rm -rf "$(1)"

# --- Dependencies: stb_image (single-header) ------------------------------
STB_IMAGE_COMMIT ?= 013ac3beddff3dbffafd5177e7972067cd2b5083
STB_DIR := external/stb
STB_IMAGE_HEADER := $(STB_DIR)/stb_image.h
STB_IMAGE_URL := https://raw.githubusercontent.com/nothings/stb/$(STB_IMAGE_COMMIT)/stb_image.h

EXTRA_OBJECT_DEPS += $(STB_IMAGE_HEADER)

$(STB_IMAGE_HEADER):
	@echo "DEPS stb_image ($(STB_IMAGE_COMMIT))"
	@$(call mkdir_p,$(STB_DIR))
	@curl -sSfL -o "$(STB_IMAGE_HEADER)" "$(STB_IMAGE_URL)"

# Preflight checks — parse-time, fail fast with an install hint before any
# recipe runs. NOTE: commas in the hint must be written as $(comma) —
# $(call) splits on ",".
comma := ,
# $(1) = binary, $(2) = expected substring in `--version`, $(3) = install hint.
define require_tool
$(if $(findstring $(2),$(shell $(1) --version 2>&1)),,$(error Missing required tool '$(1)'. $(3)))
endef
# $(1) = label for the error, $(2) = probe command, $(3) = expected substring
# in its output, $(4) = install hint. Use when --version doesn't fit (custom
# subcommand, tools without --version, or output filtering).
define require_probe
$(if $(findstring $(3),$(shell $(2) 2>&1)),,$(error Missing $(1). $(4)))
endef

# Cross-platform tools needed before we even reach the platform include.
$(call require_tool,clang,clang version,Install LLVM/Clang from https://releases.llvm.org and put it on PATH.)
$(call require_tool,curl,curl,curl ships with Windows 10+ and macOS. Install Git for Windows or your distro's curl package.)

# Web (Emscripten/WebGPU) is an explicit opt-in: `make PLATFORM=web`.
ifeq ($(PLATFORM),web)
include MakeFile_emscripten.mk
else ifeq ($(IS_WINDOWS),1)
include MakeFile_win32.mk
else ifeq ($(UNAME_S),Darwin)
include MakeFile_macos.mk
else
$(error Unsupported platform: $(UNAME_S))
endif

OBJ_C := $(SRC_C:%.c=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJ_M := $(SRC_M:%.m=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJS := $(OBJ_C) $(OBJ_M)

DEPS := $(OBJS:.$(OBJ_EXT)=.d)

# Automatically include generated dependency files (if present).
-include $(DEPS)

.PHONY: all clean clean-full test $(APP_NAME)

# Default build (same command on every platform):
#   make
# builds the actual output file (with .exe on Windows).
all: $(APP_NAME)

# Test build target:
#   make test
# builds with TESTING_ENABLED=1 and test sources included.
test: $(APP_NAME)

# User-facing target without extension.
$(APP_NAME): $(OUTPUT)

$(OUTPUT): $(BUILD_DIR) $(OBJS)
	@echo LINK $(OUTPUT)
	$(Q)$(LD) $(OBJS) -o $(OUTPUT) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	$(Q)$(call mkdir_p,$(BUILD_DIR))

# Generate dependencies alongside objects using Clang's -MMD/-MP.
DEPFLAGS := -MMD -MP

$(BUILD_DIR)/%.$(OBJ_EXT): %.c $(EXTRA_OBJECT_DEPS)
	@echo CC $<
	$(Q)$(call mkdir_p,$(dir $@))
	$(Q)$(CC) $(CFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

$(BUILD_DIR)/%.$(OBJ_EXT): %.m $(EXTRA_OBJECT_DEPS)
	@echo OBJC $<
	$(Q)$(call mkdir_p,$(dir $@))
	$(Q)$(OBJC) $(CFLAGS) $(OBJCFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

clean:
	$(Q)$(call rmdir_rf,build)

clean-full: clean
	$(Q)$(call rmdir_rf,external)
