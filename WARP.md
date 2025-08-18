# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview

asyncinput is a minimal, zero-cost C library for asynchronous input on Linux (with future cross-platform support planned). It provides low-latency input event handling with both callback and polling consumption models.

### Architecture

- **Core Library**: Single threaded worker that uses epoll to monitor `/dev/input/event*` devices
- **Zero-cost Abstraction**: NI_* constants that map directly to Linux evdev codes, no runtime overhead
- **Dual Consumption Models**: 
  - Callback mode: Events delivered directly in worker thread context
  - Polling mode: Events queued in ring buffer for main thread consumption
- **Device Management**: Automatic hotplug detection via inotify, device filtering support
- **Timestamp Precision**: Uses kernel-provided timestamps from input_event.time for accurate latency measurements

### Key Files

- `include/asyncinput.h`: Public C API with zero-cost constants and inline helpers
- `src/libasyncinput.c`: Complete Linux implementation with epoll/inotify
- `examples/`: Demonstration programs for different use cases
- `CMakeLists.txt`: Build configuration for library and examples

## Common Development Commands

### Building
```bash
# Configure with debug symbols
mkdir -p build && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build library and examples
cmake --build build -j

# Build with specific options
cmake -S . -B build-static -DASYNCINPUT_BUILD_SHARED=OFF -DASYNCINPUT_BUILD_EXAMPLES=ON
```

### Testing & Benchmarking
```bash
# Basic functionality test (requires input permissions)
./build/read_keys 5

# Mouse input test
./build/mouse_demo 10

# High-rate latency benchmark (requires /dev/uinput access)
sudo ./build/benchmark_asyncinput 10 10000

# SDL3 integration example (if SDL3 available)
./build/sdl3_asyncinput
```

### Running Single Tests
Currently no automated test suite exists. Testing is done through example programs:

```bash
# Test specific functionality
./build/callback_demo    # Callback API with synthetic events
./build/mouse_demo 3     # Mouse tracking for 3 seconds
./build/read_keys 5      # Polling API for 5 seconds
```

## Development Architecture

### Thread Model
- **Main Thread**: Calls ni_init(), ni_poll(), ni_shutdown()
- **Worker Thread**: Runs epoll loop, reads device FDs, dispatches events
- **Optional Mice Thread**: Reads /dev/input/mice for PS/2 mouse support

### Event Flow
1. Worker thread monitors devices via epoll
2. input_event read from device FDs
3. Converted to ni_event structure
4. Either:
   - Immediate callback dispatch (callback mode)
   - Ring buffer enqueue (polling mode)

### Device Lifecycle
1. Initial scan of `/dev/input/event*` at startup
2. Inotify watches `/dev/input` for hotplug events
3. Device filtering via ni_set_device_filter() if configured
4. Automatic cleanup on device removal

### Zero-Cost Constants
The NI_* constants are designed to compile away completely:
- On Linux: Direct `#define` to Linux evdev constants
- Other platforms: Stable library-defined values
- Inline helpers like `ni_is_key_event()` optimize to simple comparisons

## Permissions Requirements

Input device access typically requires elevated permissions:

```bash
# Option 1: Run as root (not recommended for development)
sudo ./build/read_keys

# Option 2: Add user to input group (recommended)
sudo usermod -a -G input $USER
# Log out and back in for group changes to take effect

# Option 3: Set capabilities for specific binaries (advanced)
sudo setcap cap_sys_admin,cap_net_admin,cap_sys_rawio+ep build/benchmark_asyncinput
```

For uinput-based testing (benchmark programs), additional permissions needed:
```bash
sudo usermod -a -G uinput $USER
```

## Coding Standards

Follow the existing patterns established in CODING_STYLE.md:
- 8-character tabs for indentation
- K&R brace style with function braces on separate lines
- Return type on separate line for function declarations
- 80-character line limit
- Snake_case for variables and functions
- C89/C99 compatible code only
- No C++ style comments (`//` only in examples)

## Platform-Specific Notes

### Linux Implementation
- Uses epoll for efficient device monitoring
- Direct mapping to evdev constants for zero cost
- Supports both event devices and /dev/input/mice
- Inotify-based hotplug detection

### Future Platforms
- Windows: Planned Raw Input backend
- macOS: Planned IOHIDManager backend
- Same API surface, different internal implementation

## Library Design Principles

1. **Zero Runtime Cost**: NI_* constants compile to native platform values
2. **Minimal Dependencies**: Only pthreads and standard Linux headers required
3. **Thread Safety**: All public APIs are thread-safe
4. **Latency Focus**: Preserves kernel timestamps for accurate measurements
5. **Simple Integration**: Single header include, minimal setup required

## Troubleshooting

### Permission Errors
- Ensure user has access to `/dev/input/event*` devices
- Check group membership: `groups $USER`
- Verify device permissions: `ls -la /dev/input/`

### No Events Received
- Check if devices are being opened: run with `strace` to see open() calls
- Verify device capabilities match filter if using ni_set_device_filter()
- Test with simple polling example first before callback approach

### High Latency
- Use benchmark_asyncinput to measure actual latency
- Check for CPU governor settings (performance vs powersave)
- Verify using callback mode for lowest latency path
