# AsyncInput Performance Optimization Design

## Current Performance Bottlenecks

### 1. Device Lookup in Worker Thread
```c
// Current O(n) lookup in hot path (line 320-325)
for (int j = 0; j < g.ndevi; j++) {
    if (g.devices[j].id == devid) {
        fd = g.devices[j].fd;
        break;
    }
}
```

### 2. Ring Buffer Contention
- Single mutex for all ring buffer operations
- No batching of events
- Memory allocation on hot path

### 3. Global Callback Dispatcher
- Single callback for all devices/events
- Complex if-else chains in user code
- No device-specific optimizations

## Optimization Strategies

### 1. Device-Specific Callbacks

**Benefits:**
- Eliminates device ID branching in user code
- Enables device-specific optimizations
- Better debugging and profiling per device
- Potential for device-specific worker threads

**Implementation:**
```c
struct device_callback {
    int id;
    int device_id;  /* -1 for wildcard */
    ni_device_callback callback;
    void *user_data;
    int flags;
    struct device_callback *next;
};

struct device {
    int fd;
    int id;
    char path[128];
    struct ni_device_info info;  /* Cache device info */
    struct device_callback *callbacks;  /* Per-device callback chain */
    uint64_t event_count;  /* For load balancing */
    uint8_t cpu_affinity;  /* Preferred CPU */
};
```

### 2. Worker Thread Architectures

#### Option A: Dedicated Process (Recommended for Ultra-Low Latency)
```bash
# Separate binary: asyncinput-worker
# Communication via shared memory ring buffer
# Benefits:
# - Complete memory isolation
# - Real-time scheduling without affecting main process  
# - Can run with CAP_SYS_NICE for RT priorities
# - Crash isolation
# - Better cache locality
```

**Implementation:**
- Main process spawns `asyncinput-worker` subprocess
- Shared memory segment for event ring buffer
- Futex-based notification system
- Worker process can be pre-spawned and reused

#### Option B: CPU Affinity + Real-Time Thread
```c
int ni_init_with_worker_config(int flags, const struct ni_worker_config *config) {
    struct ni_worker_config {
        int cpu_affinity;     /* -1 for any CPU, 0-N for specific CPU */
        int rt_priority;      /* 1-99 for SCHED_FIFO, 0 for SCHED_OTHER */
        size_t stack_size;    /* Worker thread stack size */
        int nice_level;       /* -20 to 19 for SCHED_OTHER */
    };
}
```

#### Option C: Per-Device Worker Threads
- One worker thread per high-volume device
- Load balancing based on event rates
- Automatic scaling up/down based on device activity

### 3. Memory Optimizations

#### Lock-Free Ring Buffer
```c
struct lockfree_ringbuf {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    struct ni_event events[RING_SIZE] __attribute__((aligned(64)));
    char padding[64 - ((sizeof(uint64_t) * 2) % 64)];  /* Cache line padding */
} __attribute__((aligned(64)));
```

#### Event Memory Pools
```c
struct event_pool {
    struct ni_event *free_events;
    size_t pool_size;
    size_t allocation_size;
    pthread_mutex_t pool_lock;
};
```

#### Zero-Copy Event Batching
```c
struct ni_event_batch {
    struct ni_event *events;  /* Direct pointer to ring buffer segment */
    int count;
    int device_id;
    uint64_t batch_timestamp_ns;
};

typedef void (*ni_batch_callback)(const struct ni_event_batch *batch, void *user_data);
```

### 4. Device Lookup Optimization

#### Hash Table for Device Lookup
```c
#define DEVICE_HASH_SIZE 64
struct device_hash_entry {
    int device_id;
    struct device *device;
    struct device_hash_entry *next;
};

static struct device_hash_entry *device_hash[DEVICE_HASH_SIZE];

static inline struct device *lookup_device_fast(int device_id) {
    int bucket = device_id & (DEVICE_HASH_SIZE - 1);
    struct device_hash_entry *entry = device_hash[bucket];
    while (entry) {
        if (entry->device_id == device_id)
            return entry->device;
        entry = entry->next;
    }
    return NULL;
}
```

#### Direct FD-to-Device Mapping
```c
/* Use epoll_event.data.ptr to store device pointer directly */
struct epoll_event ev = {0};
ev.events = EPOLLIN;
ev.data.ptr = device_ptr;  /* Direct pointer instead of device_id lookup */
epoll_ctl(g.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
```

### 5. NUMA Awareness

```c
struct ni_numa_config {
    int worker_cpu_node;      /* NUMA node for worker thread */
    int memory_node;          /* NUMA node for allocations */
    int device_cpu_affinity;  /* CPU affinity based on device IRQ */
};

int ni_init_numa_aware(int flags, const struct ni_numa_config *numa_config);
```

### 6. Batch Processing APIs

```c
/* Batch polling - reduces syscall overhead */
int ni_poll_batch(struct ni_event_batch *batches, int max_batches);

/* Event coalescing - merge similar events */
#define NI_COALESCE_REL_MOTION  0x01  /* Merge REL_X/REL_Y events */
#define NI_COALESCE_REPEAT_KEYS 0x02  /* Skip key repeat events */
int ni_set_coalescing(int flags);
```

## Implementation Priority

### Phase 1: Core Optimizations (Immediate Impact)
1. **Hash-based device lookup** - Replace O(n) loop
2. **Direct epoll data.ptr mapping** - Eliminate device_id lookup entirely  
3. **Lock-free ring buffer** - Reduce contention
4. **Device-specific callbacks** - Enable user-space optimizations

### Phase 2: Advanced Threading (High Impact)
1. **CPU affinity configuration** - Pin worker to specific core
2. **Real-time scheduling support** - SCHED_FIFO for worker thread
3. **Dedicated worker process option** - Ultimate isolation

### Phase 3: Memory & NUMA (Specialized Use Cases)
1. **Memory pools** - Reduce allocation overhead
2. **NUMA-aware initialization** - Optimize for multi-socket systems
3. **Zero-copy batching** - Direct ring buffer access

## Performance Targets

Based on current benchmark results:
- **Current**: ~50-100μs average latency
- **Target with Phase 1**: ~20-50μs average latency  
- **Target with Phase 2**: ~10-30μs average latency
- **Target with Phase 3**: ~5-15μs average latency (95th percentile)

## Measurement & Validation

```c
/* Performance monitoring APIs */
struct ni_perf_stats {
    uint64_t total_events;
    uint64_t worker_thread_cpu_ns;
    uint64_t avg_latency_ns;
    uint64_t p95_latency_ns;
    uint64_t p99_latency_ns;
    uint64_t ring_buffer_overruns;
    uint64_t callback_execution_ns;
};

int ni_get_performance_stats(struct ni_perf_stats *stats);
int ni_reset_performance_stats(void);
```

This design enables incremental optimization while maintaining API compatibility.
