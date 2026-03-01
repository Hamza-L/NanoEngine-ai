CC := clang
OBJC := clang
BUILD_DIR := build
TARGET := NanoEngine

CFLAGS := -std=c2x -O2 -g -Wall -Wextra -Wpedantic -Werror
OBJCFLAGS := -fobjc-arc
LDFLAGS :=
SRC_C :=
SRC_M :=

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

OBJ_C := $(SRC_C:%.c=$(BUILD_DIR)/%.o)
OBJ_M := $(SRC_M:%.m=$(BUILD_DIR)/%.o)
OBJS := $(OBJ_C) $(OBJ_M)

DEPS := $(OBJS:.o=.d)

# Automatically include generated dependency files (if present).
-include $(DEPS)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(BUILD_DIR) $(OBJS)
	$(CC) $(OBJS) -o $(BUILD_DIR)/$@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/src/platform/macos
	mkdir -p $(BUILD_DIR)/src/platform
	mkdir -p $(BUILD_DIR)/src

# Generate dependencies alongside objects using Clang's -MMD/-MP.
DEPFLAGS := -MMD -MP

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.o: %.m
	mkdir -p $(dir $@)
	$(OBJC) $(CFLAGS) $(OBJCFLAGS) -Iinclude $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# Include the .d makefiles. Initially, all the .d files will be missing
-include $(DEPS)
