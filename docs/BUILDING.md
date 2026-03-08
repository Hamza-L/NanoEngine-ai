# NanoEngine2 — Building

## Prerequisites

### macOS

| Requirement | Notes |
|---|---|
| **Xcode Command Line Tools** | Provides `clang`, system headers, and frameworks |
| **make** | Included with Xcode CLT |

Install if needed:

```bash
xcode-select --install
```

No additional dependencies. Metal and Cocoa are system frameworks.

### Windows

| Requirement | Notes |
|---|---|
| **Clang** (LLVM) | `clang` must be on PATH. Install via [LLVM releases](https://releases.llvm.org/) or Visual Studio |
| **Windows SDK** | Provides `user32.lib`, `gdi32.lib`, `dwmapi.lib` |
| **GNU Make** | `make` must be on PATH. Install via [GnuWin32](http://gnuwin32.sourceforge.net/packages/make.htm), MSYS2, or chocolatey (`choco install make`) |
| **PowerShell** | Used by the build system to auto-download Vulkan headers (pre-installed on Windows 10+) |

**No Vulkan SDK required.** The build system automatically downloads the Vulkan headers (see below).

---

## Building

### Quick Start

```bash
# macOS or Windows (from project root)
make
```

This builds the demo executable at:

| Platform | Output |
|---|---|
| macOS | `build/NanoEngine` |
| Windows | `build/NanoEngine.exe` |

### Clean Build

```bash
make clean
make
```

### Targets

| Target | Description |
|---|---|
| `make` / `make all` | Build the demo executable |
| `make NanoEngine` | Same as above (explicit target name) |
| `make clean` | Remove the `build/` directory |
| `make deps` | (Windows only) Download Vulkan headers without building |

---

## How the Build System Works

### Platform Dispatch

The top-level `Makefile` detects the platform and includes the appropriate sub-makefile:

```
Makefile
├── MakeFile_macos.mk    (included on Darwin)
└── MakeFile_win32.mk    (included on Windows)
```

Each platform makefile appends:
- Platform-specific source files (`SRC_C` / `SRC_M`)
- Compiler flags (`CFLAGS`)
- Linker flags (`LDFLAGS`)
- Any extra build dependencies (`EXTRA_OBJECT_DEPS`)

### Compiler Flags

| Flag | Purpose |
|---|---|
| `-std=c2x` | C23 standard |
| `-O2` | Optimization level |
| `-g` | Debug symbols |
| `-Wall -Wextra -Wpedantic -Werror` | Strict warnings, treated as errors |
| `-fobjc-arc` | (macOS only) Automatic Reference Counting for Objective-C |
| `-DUNICODE -D_UNICODE` | (Windows only) Wide-char Win32 APIs |
| `-D_CRT_SECURE_NO_WARNINGS` | (Windows only) Silence MSVC CRT deprecation warnings |

### Dependency Files

The build generates `.d` dependency files alongside each object using Clang's `-MMD -MP` flags. These are automatically included on subsequent builds, enabling correct incremental rebuilds when headers change.

---

## Vulkan Headers (Windows — Auto-Download)

On Windows, the first build automatically downloads **Vulkan-Headers v1.3.280** from GitHub:

1. Downloads the release zip via PowerShell (`Invoke-WebRequest`).
2. Extracts to `build/deps/`.
3. Moves headers to `external/vulkan-headers/include/`.
4. Adds `-Iexternal/vulkan-headers/include` to `CFLAGS`.

The `external/` directory is in `.gitignore` — headers are downloaded per-clone, not committed.

To override the version:

```bash
make VULKAN_HEADERS_VERSION=1.4.309
```

To re-download headers:

```bash
# Delete the marker file and rebuild
del external\vulkan-headers\include\vulkan\vulkan.h
make
```

---

## Linked Libraries

### macOS

| Framework | Used By |
|---|---|
| Cocoa | Windowing (`NSWindow`, `NSApplication`, `NSView`) |
| Metal | GPU rendering (`MTLDevice`, `MTLCommandQueue`, etc.) |
| QuartzCore | Display layer (`CAMetalLayer`) |

### Windows

| Library | Used By |
|---|---|
| `user32.lib` | Windowing (`CreateWindowExW`, message pump) |
| `gdi32.lib` | GDI (basic window painting) |
| `dwmapi.lib` | Desktop Window Manager (DPI, composition) |

Vulkan is **not linked** — it is loaded at runtime via `LoadLibraryA("vulkan-1.dll")`.

---

## Editor / LSP Support

To generate a `compile_commands.json` for clangd or other LSP servers:

### macOS (using bear)

```bash
# Install bear (if not already)
brew install bear

# Generate compile_commands.json
bear -- make clean all
```

### Windows

Use [compiledb](https://github.com/nicmcd/compiledb) or generate manually. Alternatively, some editors can parse the Makefile output directly.

The generated `compile_commands.json` is in `.gitignore` and should not be committed.

---

## Troubleshooting

| Issue | Solution |
|---|---|
| `Unsupported platform` error | Linux is not yet supported. Build on macOS or Windows. |
| Vulkan header download fails | Check internet connection. Manually download from [Vulkan-Headers releases](https://github.com/KhronosGroup/Vulkan-Headers/releases) and extract to `external/vulkan-headers/`. |
| `vulkan-1.dll` not found at runtime | Install the Vulkan runtime from your GPU vendor's driver package. |
| Clang not found on Windows | Ensure LLVM's `bin/` is on your PATH, or build from a Visual Studio Developer Command Prompt with Clang installed. |
