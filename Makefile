CC := clang
OBJC := clang
BUILD_DIR := build

# On Windows, generate a proper .exe file name.
ifeq ($(OS),Windows_NT)
TARGET := NanoEngine.exe
else
TARGET := NanoEngine
endif

CFLAGS := -std=c2x -O2 -g -Wall -Wextra -Wpedantic -Werror
OBJCFLAGS := -fobjc-arc
LDFLAGS :=
SRC_C :=
SRC_M :=

ifeq ($(OS),Windows_NT)
# Force cmd.exe recipes when building from a non-MSYS environment.
# This avoids relying on `sh.exe` being present and makes `mkdir`/`rmdir` work.
SHELL := cmd.exe
.SHELLFLAGS := /C
endif

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

ifeq ($(OS),Windows_NT)
OBJ_EXT := obj

# `cmd.exe` does not support `mkdir -p`.
mkdir_p = if not exist "$(subst /,\\,$(1))" mkdir "$(subst /,\\,$(1))"
rmdir_rf = if exist "$(subst /,\\,$(1))" rmdir /s /q "$(subst /,\\,$(1))"
else
OBJ_EXT := o
mkdir_p = mkdir -p "$(1)"
rmdir_rf = rm -rf "$(1)"
endif

OBJ_C := $(SRC_C:%.c=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJ_M := $(SRC_M:%.m=$(BUILD_DIR)/%.$(OBJ_EXT))
OBJS := $(OBJ_C) $(OBJ_M)

DEPS := $(OBJS:.$(OBJ_EXT)=.d)

# Automatically include generated dependency files (if present).
-include $(DEPS)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(BUILD_DIR) $(OBJS)
	@$(CC) $(OBJS) -o $(BUILD_DIR)/$@ $(LDFLAGS)

$(BUILD_DIR):
	@$(call mkdir_p,$(BUILD_DIR))

# Generate dependencies alongside objects using Clang's -MMD/-MP.
DEPFLAGS := -MMD -MP

$(BUILD_DIR)/%.$(OBJ_EXT): %.c
	@echo CC $<
	@$(call mkdir_p,$(dir $@))
	@$(CC) $(CFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

$(BUILD_DIR)/%.$(OBJ_EXT): %.m
	@echo OBJC $<
	@$(call mkdir_p,$(dir $@))
	@$(OBJC) $(CFLAGS) $(OBJCFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.$(OBJ_EXT)=.d) -c $< -o $@

clean:
	@$(call rmdir_rf,$(BUILD_DIR))

# Include the .d makefiles. Initially, all the .d files will be missing
-include $(DEPS)
