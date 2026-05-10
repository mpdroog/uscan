#pragma once

// Clang Thread Safety Analysis
// Enables compile-time detection of data races and deadlocks
// Use with: clang++ -Wthread-safety

#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(capability)
  #define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
  #define CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(capability(x))
  #define SCOPED_CAPABILITY THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
  #define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
  #define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
  #define ACQUIRED_BEFORE(...) THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))
  #define ACQUIRED_AFTER(...) THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))
  #define REQUIRES(...) THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))
  #define REQUIRES_SHARED(...) THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))
  #define ACQUIRE(...) THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))
  #define ACQUIRE_SHARED(...) THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))
  #define RELEASE(...) THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))
  #define RELEASE_SHARED(...) THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))
  #define TRY_ACQUIRE(...) THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))
  #define TRY_ACQUIRE_SHARED(...) THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))
  #define EXCLUDES(...) THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))
  #define ASSERT_CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))
  #define ASSERT_SHARED_CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))
  #define RETURN_CAPABILITY(x) THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))
  #define NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)
#else
  #define CAPABILITY(x)
  #define SCOPED_CAPABILITY
  #define GUARDED_BY(x)
  #define PT_GUARDED_BY(x)
  #define ACQUIRED_BEFORE(...)
  #define ACQUIRED_AFTER(...)
  #define REQUIRES(...)
  #define REQUIRES_SHARED(...)
  #define ACQUIRE(...)
  #define ACQUIRE_SHARED(...)
  #define RELEASE(...)
  #define RELEASE_SHARED(...)
  #define TRY_ACQUIRE(...)
  #define TRY_ACQUIRE_SHARED(...)
  #define EXCLUDES(...)
  #define ASSERT_CAPABILITY(x)
  #define ASSERT_SHARED_CAPABILITY(x)
  #define RETURN_CAPABILITY(x)
  #define NO_THREAD_SAFETY_ANALYSIS
#endif
#else
  #define CAPABILITY(x)
  #define SCOPED_CAPABILITY
  #define GUARDED_BY(x)
  #define PT_GUARDED_BY(x)
  #define ACQUIRED_BEFORE(...)
  #define ACQUIRED_AFTER(...)
  #define REQUIRES(...)
  #define REQUIRES_SHARED(...)
  #define ACQUIRE(...)
  #define ACQUIRE_SHARED(...)
  #define RELEASE(...)
  #define RELEASE_SHARED(...)
  #define TRY_ACQUIRE(...)
  #define TRY_ACQUIRE_SHARED(...)
  #define EXCLUDES(...)
  #define ASSERT_CAPABILITY(x)
  #define ASSERT_SHARED_CAPABILITY(x)
  #define RETURN_CAPABILITY(x)
  #define NO_THREAD_SAFETY_ANALYSIS
#endif

#include <mutex>

namespace uscan {

// Mutex wrapper with thread safety annotations
class CAPABILITY("mutex") Mutex {
public:
    void lock() ACQUIRE() { mutex_.lock(); }
    void unlock() RELEASE() { mutex_.unlock(); }
    bool try_lock() TRY_ACQUIRE(true) { return mutex_.try_lock(); }

    // For std::lock_guard and std::unique_lock
    std::mutex& native_handle() { return mutex_; }
    std::mutex& native() { return mutex_; }  // Alias for compatibility

private:
    std::mutex mutex_;
};

// RAII lock guard with thread safety annotations
class SCOPED_CAPABILITY MutexLock {
public:
    explicit MutexLock(Mutex& mu) ACQUIRE(mu) : mu_(mu) { mu_.lock(); }
    ~MutexLock() RELEASE() { mu_.unlock(); }

    // Non-copyable, non-movable
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;

private:
    Mutex& mu_;
};

} // namespace uscan
