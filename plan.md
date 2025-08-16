Alright! Below are two text files, copy‑paste ready. Each is a **LLM‑oriented implementation roadmap**, including **instructions to avoid common pitfalls** (like not testing after changes, or forgetting to commit to git). I’ve added meta‑sections so if you run this through another LLM, it will understand it’s part of a supervised workflow.  

---

# File: `asyncinput_c_plan.txt`

# asyncinput – C Implementation Plan

## LLM Instructions
- You are implementing `libasyncinput` in **C**.
- Always:
  1. **Write complete code to files before testing**. Never test partially written functions.  
  2. **Compile and test after each functional change.** Do not skip testing.  
  3. **Commit** to git after successful build and tests.  
  4. When writing files, include **at the top of each file**: `// Agent: [YOUR NAME], Date: [YYYY-MM-DD], Observation: …` to document who (which agent) wrote this chunk of code and what happened.  
- Never leave TODOs unresolved — if something is missing, explain why.  
- Keep API strictly minimal and ABI‑stable.  

---

## Build System
- Use **CMake**.
- Output artifacts:
  - Shared library: `libasyncinput.so`
  - Static library: `libasyncinput.a`
- Project tree:
  ```
  asyncinput/
    src/
      libasyncinput.c
      device_linux.c
    include/
      asyncinput.h
    examples/
      read_keys.c
      callback_demo.c
    tests/
      test_hotplug.c
    CMakeLists.txt
  ```

---

## Dependencies
- Only system headers and syscalls: `<linux/input.h>`, `<linux/uinput.h>`.
- Use `pthread` for threading.
- Optional: `libudev` for hotplug detection (Linux).
- Optional: `CMocka` for unit testing.

---

## Public C API (`include/asyncinput.h`)
```c
typedef struct ni_event {
    int device_id;
    int type;      // EV_KEY, EV_REL etc.
    int code;      // KEY_A, REL_X etc.
    int value;     // press/release/delta
    long long timestamp_ns;
} ni_event;

typedef void (*ni_callback)(const ni_event *ev, void *user_data);

int ni_init(int flags);
int ni_register_callback(ni_callback cb, void *user_data, int flags);
int ni_poll(ni_event *evts, int max_events);
int ni_shutdown(void);
```

---

## Internal Implementation (Linux MVP)
- **Enumeration**: Scan `/dev/input/event*`.
- **Capabilities**: `ioctl(EVIOCGBIT)`, `ioctl(EVIOCGRAB)`.
- **Event Reading**: Non‑blocking `read` on device FDs in worker thread. Use `poll`/`epoll`.
- **Callback Dispatch**:
  - Worker thread callbacks invoked directly.
  - Main thread callbacks put in queue, popped via `ni_poll`.
- **Hotplug**: `inotify` watching `/dev/input`, optional `libudev`.

---

## Testing and Benchmarking
- Use `/dev/uinput` to inject fake events (e.g. key press A).
- Test both `ni_poll()` reporting and callback dispatch.
- Log timestamps for latency verification.
- Benchmark throughput (events per second).

---

## Deliverables
- `libasyncinput.so`, `libasyncinput.a`
- Header: `asyncinput.h`
- Example binaries in `examples/`
- Test suite with synthetic device injection

---

# File: `asyncinput_rust_plan.txt`

# asyncinput – Rust Implementation Plan

## LLM Instructions
- You are implementing `libasyncinput` in **Rust**.  
- It MUST expose the **same C API** as defined in `asyncinput.h`.  
- Always:
  1. **Write complete code to files before testing.**  
  2. Run `cargo build && cargo test` after each change.  
  3. Commit to git frequently.  
  4. Each file must start with `// Agent: [YOUR NAME], Date: [YYYY-MM-DD], Observation: …`.  
- When wrapping unsafe FFI, always guard with `catch_unwind` so panics do not cross FFI boundary.  

---

## Build System
- Use `Cargo`.
- Build as `cdylib` (`Cargo.toml`):
  ```
  [lib]
  name = "asyncinput"
  crate-type = ["cdylib", "staticlib"]
  ```
- Generate `libasyncinput.so` identical in exported symbols to C version.
- Include `include/asyncinput.h` for consumers (copied verbatim from C plan).

---

## Dependencies
- Crate `nix` for syscalls (`open`, `read`, `ioctl`, `poll`).
- `inotify` crate (Linux hotplug).
- `std::thread`, `std::sync::mpsc` (threading, queues).
- FFI: `libc`.

---

## Public API (FFI Glue)
```rust
#[repr(C)]
pub struct ni_event {
    device_id: i32,
    type_: i32,
    code: i32,
    value: i32,
    timestamp_ns: i64,
}

pub type ni_callback = Option<extern "C" fn(*const ni_event, *mut std::ffi::c_void)>;

#[no_mangle]
pub extern "C" fn ni_init(flags: i32) -> i32 { ... }

#[no_mangle]
pub extern "C" fn ni_register_callback(
    cb: ni_callback,
    user_data: *mut std::ffi::c_void,
    flags: i32
) -> i32 { ... }

#[no_mangle]
pub extern "C" fn ni_poll(evts: *mut ni_event, max_events: i32) -> i32 { ... }

#[no_mangle]
pub extern "C" fn ni_shutdown() -> i32 { ... }
```

---

## Internal Implementation (Linux MVP)
- **Device Handling**: `nix::fcntl::open` on `/dev/input/event*`.
- **Reading**: Worker threads with blocking read and parse `input_event`.
- **Event Queue**: `mpsc` channels to main thread for polling.
- **Callback Registry**: Stored in `Arc<Mutex<..>>`.
- **Hotplug**: Inotify watching `/dev/input`.  
- **Safety**: Wrap all user callbacks in `catch_unwind` to prevent unwinding.

---

## Testing and Benchmarking
- Use `/dev/uinput` FFI via `nix`.
- Send synthetic events, measure latency.
- Compare to C baseline measurements.
- Provide Rust example binary (integration test).

---

## Deliverables
- `libasyncinput.so` / `libasyncinput.a`
- `include/asyncinput.h`
- Rust example programs in `examples/`
- Cargo tests for safety

---

✅ Both plans line up: **same API, different internal languages**. This means any consumer language can link to either version transparently, and you can benchmark them directly against each other.  
