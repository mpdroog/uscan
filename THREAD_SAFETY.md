# Thread Safety Implementation

## Overview

Comprehensive thread safety infrastructure with compile-time and runtime checking to prevent data races and crashes in the multi-threaded symbol search system.

## Thread Safety Features Implemented

### 1. Clang Thread Safety Annotations (`src/thread_safety.hpp`)

Compile-time thread safety analysis using Clang's static analyzer:

- `GUARDED_BY(mutex)` - Documents which mutex protects a variable
- `REQUIRES(mutex)` - Documents that a function must be called with mutex held
- `EXCLUDES(mutex)` - Documents that a function must NOT hold mutex when called
- `ACQUIRE(mutex)` / `RELEASE(mutex)` - Documents lock acquisition/release
- `SCOPED_CAPABILITY` - Marks RAII lock guards

**Benefit**: Catches threading bugs at compile time before they can cause crashes.

### 2. Annotated Mutex Wrappers

- `uscan::Mutex` - Thread-safe mutex with capability annotations
- `uscan::MutexLock` - RAII lock guard with scoped capability

### 3. Protected Data Structures

#### IQFeedClient Thread-Safe Members:

```cpp
// Atomic file descriptors (lock-free)
std::atomic<int> socket_fd_{-1};
std::atomic<int> lookup_fd_{-1};

// Callback protection
Mutex callback_mutex_;
SymbolListCallback symbol_search_callback_ GUARDED_BY(callback_mutex_);
SymbolListCallback incremental_save_callback_ GUARDED_BY(callback_mutex_);

// Symbol search data protection
Mutex symbol_search_mutex_;
std::vector<SymbolInfo> symbol_search_results_ GUARDED_BY(symbol_search_mutex_);
std::size_t last_incremental_save_count_ GUARDED_BY(symbol_search_mutex_);

// Lock-free counters for UI
std::atomic<bool> searching_symbols_{false};
std::atomic<std::size_t> symbols_received_count_{0};
```

### 4. Thread Sanitizer (TSAN) Support

Build with ThreadSanitizer for runtime data race detection:

```bash
make test-tsan
```

TSAN detects:
- Data races between threads
- Lock order inversions (potential deadlocks)
- Use-after-free in multithreaded code

### 5. Makefile Build Modes

**Release Build** (default):
```bash
make
```
- Optimized (-O2)
- No sanitizers
- Production-ready

**Debug Build** (AddressSanitizer + UBSan):
```bash
DEBUG=1 make
```
- Memory error detection (buffer overflows, use-after-free)
- Undefined behavior detection

**ThreadSanitizer Build**:
```bash
TSAN=1 make
```
- Data race detection
- Deadlock detection
- Cannot combine with ASan (mutually exclusive)

## Runtime Checks

### ThreadSanitizer Options (set in Makefile)

```bash
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7
```

- `halt_on_error=1` - Stop on first race (fail fast)
- `second_deadlock_stack=1` - Show both lock acquisition stacks for deadlocks
- `history_size=7` - Track last 7 memory accesses per location

## Known Thread Safety Patterns

### 1. Background Thread Symbol Search

**Pattern**: Background worker thread with lock-free communication

```
Main Thread                    Background Thread
-----------                    -----------------
request_symbol_search()
  ├─ Set searching_symbols_
  └─ Start thread ──────────►  symbol_search_worker()
                                 ├─ Read from socket (tight loop)
                                 ├─ Lock symbol_search_mutex_
                                 ├─ Add to symbol_search_results_
                                 ├─ Update atomic counter
                                 └─ Unlock

disconnect()
  ├─ Clear callbacks (lock)         Check searching_symbols_
  ├─ Close sockets (atomic)         ↓ (exits loop)
  └─ Join thread ◄──────────────────

```

**Safety**:
- Socket FDs are atomic (no race on close)
- Callbacks copied under lock before invocation
- Symbol data protected by mutex
- Atomic flags for lock-free checks

### 2. Copy-on-Callback Pattern

Never hold mutex during callback invocation:

```cpp
// WRONG - holds lock during callback
{
    MutexLock lock(callback_mutex_);
    if (callback_) callback(data);  // Deadlock risk!
}

// CORRECT - copy callback, release lock, then invoke
SymbolListCallback cb;
{
    MutexLock lock(callback_mutex_);
    cb = callback_;
}
if (cb) cb(data);  // Safe - no lock held
```

### 3. Lock-Free UI Updates

Use atomics for frequently-read counters:

```cpp
// Writer (background thread) - lock-free
symbols_received_count_ = current_count;

// Reader (UI thread) - lock-free
auto count = client.symbols_received_count();
```

## Testing

### Comprehensive Test Suite (`tests/test_thread_safety.cpp`)

12 tests covering all critical code paths:

1. **callback_race_on_disconnect** - Callback race during disconnect
2. **lock_free_counter_stress** - Atomic counter stress test (4 threads)
3. **rapid_connect_disconnect** - Socket close races
4. **set_callbacks_during_search** - Concurrent callback updates
5. **callback_cleared_before_invocation** - Callback null check races
6. **background_thread_data_race** - Symbol data corruption
7. **destroy_during_background_thread** - Destructor races
8. **multiple_concurrent_clients** - Multiple client instances
9. **error_path_thread_safety** - Error handling under threading
10. **process_not_thread_safe_warning** - Documents main-thread-only calls
11. **no_deadlock_on_mutex_acquisition** - Deadlock detection
12. **thread_safety_annotations_enforced** - Verifies annotations compile

### Run Tests

```bash
# All tests with sanitizers
make test-all

# Thread safety tests only (with TSAN)
make test-tsan

# Quick threading tests (no sanitizer)
make test-threading
```

## Issues Found and Fixed

### Issue 1: Socket FD Data Race (Found by TSAN)

**Problem**: Main thread closing socket while background thread reading FD

```
WARNING: ThreadSanitizer: data race
  Write of size 4 at 0x... by main thread:
    #0 IQFeedClient::disconnect() iqfeed_client.cpp:227

  Previous read of size 4 at 0x... by thread T9:
    #0 IQFeedClient::read_lookup_data() iqfeed_client.cpp:463
```

**Fix**: Made socket file descriptors atomic

```cpp
std::atomic<int> socket_fd_{-1};
std::atomic<int> lookup_fd_{-1};
```

### Issue 2: Callback Invocation After Disconnect

**Problem**: Callbacks invoked after scanner destroyed

**Fix**:
1. Clear callbacks under lock in `disconnect()`
2. Copy callbacks before invoking (copy-on-callback pattern)
3. Never hold callback_mutex_ during callback invocation

## Best Practices

### DO:
✅ Use atomic for simple counters/flags
✅ Copy data before calling callbacks
✅ Document lock requirements with annotations
✅ Test with ThreadSanitizer regularly
✅ Make locks as fine-grained as possible
✅ Use RAII lock guards (MutexLock)

### DON'T:
❌ Hold locks during callbacks (deadlock risk)
❌ Access guarded data without locks
❌ Assume operations are atomic (use std::atomic explicitly)
❌ Ignore TSAN warnings
❌ Share mutable data without synchronization
❌ Use raw mutexes (use annotated wrappers)

## Performance Impact

### Atomics vs Mutexes

**Lock-free atomics** (symbols_received_count_):
- No blocking
- Cache-line contention only
- Perfect for high-frequency reads

**Mutexes** (symbol_search_results_):
- Blocking on contention
- Necessary for complex data structures
- Minimize critical section time

### Overhead

- **Release build**: Zero overhead (annotations are compile-time only)
- **TSAN build**: ~5-10x slower (only for testing)
- **ASan build**: ~2x slower (only for testing)

## Future Improvements

- [ ] Add `-Wthread-safety-negative` once nested scope warnings resolved
- [ ] Consider lock-free queue for symbol batches
- [ ] Add thread naming for better TSAN reports
- [ ] Document lock acquisition order to prevent deadlocks
- [ ] Add stress tests with real IQFeed connection

## References

- [Clang Thread Safety Analysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)
- [ThreadSanitizer](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
- [C++ Memory Model](https://en.cppreference.com/w/cpp/atomic/memory_order)
