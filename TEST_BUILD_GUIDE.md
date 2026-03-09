# NanoEngine Test Build Guide

## Overview

The NanoEngine build system now supports separate normal and test builds using the Makefile. This ensures that:

1. **Production builds** (`make`) compile only production code without any test code
2. **Test builds** (`make test`) include all test infrastructure and automatically run tests
3. Test code is completely isolated via `TESTING_ENABLED` macro guards
4. Both build modes use separate build directories to avoid object file conflicts

## Build Commands

### Normal Production Build
```bash
make                # Builds to build/NanoEngine.exe with TESTING_ENABLED=0
make clean          # Removes both build/ and build/test/ directories
make clean && make  # Clean rebuild of production version
```

**Output:**
- Executable: `build/NanoEngine.exe`
- Test code: NOT compiled
- TESTING_ENABLED: 0
- Macro guards: All test functions excluded at compile time

**Behavior:**
- Runs normal application loop
- No test functions called
- Smaller executable size (~183 KB)

### Test Build
```bash
make test                # Builds to build/test/NanoEngine.exe with TESTING_ENABLED=1
make clean && make test  # Clean rebuild of test version
```

**Output:**
- Executable: `build/test/NanoEngine.exe`
- Test code: Fully compiled
- TESTING_ENABLED: 1
- Macro guards: All test functions included

**Behavior:**
- Runs complete test suite
- Automatically executes all test functions
- Slightly larger executable size (~186 KB)

## Build Directory Structure

```
project-root/
├── Makefile                    # Main build configuration
├── build/                      # Normal build output
│   ├── NanoEngine.exe          # Production executable
│   ├── src/
│   │   ├── main.o
│   │   ├── ne_log.o
│   │   ├── test_buffer.o       # NOT compiled in normal build
│   │   └── ...
│   └── ...
├── build/test/                 # Test build output
│   ├── NanoEngine.exe          # Test executable
│   ├── src/
│   │   ├── main.o             # Different from build/ (different macros)
│   │   ├── ne_log.o
│   │   ├── test_buffer.o      # Compiled in test build
│   │   └── ...
│   └── ...
└── src/
    ├── main.c
    ├── test_buffer.c           # Only compiled for test builds
    └── ...
```

## Macro Guards Implementation

### TESTING_ENABLED Macro

The compiler flag `-DTESTING_ENABLED=(1|0)` is automatically set based on the build target:

```c
// Defined in Makefile:
// Normal:  -DTESTING_ENABLED=0
// Test:    -DTESTING_ENABLED=1
```

### Test Function Guards

All test code uses macro guards to exclude from production builds:

**include/ne_test_buffer.h:**
```c
#if TESTING_ENABLED
bool test_buffer(NEApp *app, NEWindow *window);
#endif
```

**src/test_buffer.c:**
```c
#if TESTING_ENABLED
// ... entire test implementation ...
#endif
```

**src/main.c:**
```c
#if TESTING_ENABLED
    bool test_passed = test_buffer(app, window);
    // ... test exit path ...
#else
    // ... normal application loop ...
#endif
```

## How It Works

### 1. Makefile Detection
```makefile
ifeq ($(filter test,$(MAKECMDGOALS)),test)
    BUILD_DIR := build/test
    TESTING_ENABLED := 1
else
    BUILD_DIR := build
    TESTING_ENABLED := 0
endif
```

The Makefile checks if `test` is in the make goals and sets the appropriate variables.

### 2. Conditional Source Inclusion
```makefile
SRC_C := src/main.c src/ne_log.c src/ne_file.c

ifeq ($(TESTING_ENABLED),1)
    SRC_C += src/test_buffer.c  # Only for test builds
endif
```

### 3. Compiler Flags
```makefile
CFLAGS := ... -DTESTING_ENABLED=$(TESTING_ENABLED)
```

The preprocessor macro is passed to the compiler, allowing conditional compilation.

### 4. Separate Directories
Both builds use different `BUILD_DIR` values to prevent object file conflicts:
- Normal: `build/` 
- Test: `build/test/`

This ensures that switching between `make` and `make test` doesn't reuse incompatible object files.

## Test Suite Details

### Available Test Suites

Currently implemented: **Buffer Tests** (`src/test_buffer.c`)

Test functions:
- `test_buffer_create_destroy()` - Basic allocation/deallocation
- `test_buffer_initial_data()` - Creation with initial GPU data
- `test_buffer_update()` - Partial buffer updates
- `test_buffer_usage_flags()` - All buffer usage type combinations

### Adding New Tests

To add a new test module:

1. Create test file: `src/test_myfeature.c`
2. Create header: `include/ne_test_myfeature.h`
3. Add macro guards:
   ```c
   #if TESTING_ENABLED
   // ... test code ...
   #endif
   ```
4. Add to Makefile:
   ```makefile
   ifeq ($(TESTING_ENABLED),1)
       SRC_C += src/test_buffer.c src/test_myfeature.c
   endif
   ```
5. Call from main.c with guards:
   ```c
   #if TESTING_ENABLED
       if (!test_myfeature(app, window)) all_passed = false;
   #endif
   ```

## Build Performance

### Rebuild Times
- **Normal build after `make test`**: ~6-8 seconds (recompiles src/main.c, etc. with different flags)
- **Test build after `make`**: ~6-8 seconds (adds test_buffer.c compilation)
- **Incremental within same mode**: ~2-3 seconds (only changed files)

### File Size Comparison
- Normal executable: ~183 KB
- Test executable: ~186 KB
- Difference: ~3 KB (just test code)

## Testing Workflow

### Quick Test During Development
```bash
# Build and run tests
make test && ./build/test/NanoEngine.exe

# Or test-specific build with fast rebuild
make clean && make test
```

### Full Test Cycle
```bash
# Clean, build test version, run tests
make clean && make test && ./build/test/NanoEngine.exe

# Then verify production build still works
make && ./build/NanoEngine.exe
```

### Continuous Integration
```bash
# Full CI pipeline
make clean && make test     # Verify test build works
./build/test/NanoEngine.exe # Run tests (exit code 0 = pass)
make clean && make          # Verify production build works
```

## Troubleshooting

### Test executable shows "normal mode (no tests enabled)"
**Problem:** Test executable running test code from old build directory.

**Solution:** 
```bash
rm -r build          # Remove entire build directory
make test           # Clean rebuild
```

### Build fails with undefined references to test functions
**Problem:** Object files from different build modes are being mixed.

**Solution:**
```bash
make clean && make test  # Full rebuild with clean slate
```

### TESTING_ENABLED not defined
**Problem:** Header files may be cached by IDE.

**Solution:**
1. Close IDE
2. `make clean`
3. `make test`
4. Reopen IDE

## Makefile Variables Reference

| Variable | Normal Build | Test Build |
|----------|--------------|-----------|
| `BUILD_DIR` | `build` | `build/test` |
| `BUILD_MODE` | `normal` | `test` |
| `TESTING_ENABLED` | `0` | `1` |
| `SRC_C` includes | main, log, file, platform, vulkan | main, log, file, **test_buffer**, platform, vulkan |
| Output executable | `build/NanoEngine.exe` | `build/test/NanoEngine.exe` |

## Phony Targets

```makefile
.PHONY: all clean test $(APP_NAME)

all         # Default target, builds to build/NanoEngine.exe
test        # Test target, builds to build/test/NanoEngine.exe  
clean       # Removes entire build/ directory (both normal and test)
$(APP_NAME) # User-facing target without extension
```

## Key Design Decisions

1. **Separate Build Directories**: Prevents object file conflicts and allows independent incremental builds
2. **Macro Guards**: Ensures test code is completely absent from production executables at compile time
3. **Single Makefile**: Simple configuration without maintaining multiple build files
4. **Auto-Detection**: No manual configuration needed - just run `make` or `make test`
5. **Clean Integration**: Test infrastructure doesn't pollute normal code paths

## Notes for Future Development

- Test infrastructure is extensible: easy to add more test modules
- Each test file can be developed and tested independently
- Test build is ideal for CI/CD pipelines
- Production build is guaranteed test-free and minimal

