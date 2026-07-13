#pragma once

// mcp::sys — the SDK's threading primitives, abstracted so a platform
// without pthreads (VxWorks kernel/DKM mode) can supply native ones.
//
// On host platforms (Linux, macOS, Windows) these are *pure aliases* to the
// std:: types — zero overhead, identical semantics, so the whole host test
// suite (and ThreadSanitizer) exercises the real std primitives. On VxWorks
// (MCP_SYS_VXWORKS_THREADS) they are native classes backed by taskSpawn +
// semaphores, usable in both RTP and DKM.
//
// The SDK uses `mcp::sys::{thread,mutex,condition_variable}` and
// `mcp::sys::this_thread::{sleep_for,get_id}`. `std::lock_guard`,
// `std::unique_lock`, and `std::atomic` are used directly — they are generic
// / OS-free (lock_guard<sys::mutex> works by BasicLockable) and need no
// abstraction.

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#if defined(MCP_SYS_VXWORKS_THREADS)
#include <mcp/sys/threading_vxworks.hpp>
#else  // host: std-backed

#include <condition_variable>
#include <thread>

namespace mcp::sys {

using thread = std::thread;
using mutex = std::mutex;
using condition_variable = std::condition_variable;

namespace this_thread {
using std::this_thread::get_id;
using std::this_thread::sleep_for;
}  // namespace this_thread

}  // namespace mcp::sys

#endif  // MCP_SYS_VXWORKS_THREADS

namespace mcp::sys {

/// Set-once value handoff: one thread set()s, another get()s (blocking until
/// set). Replaces the std::promise/std::future pairs used for synchronous
/// request/close waits — implemented on the sys primitives so it works on
/// every backend. Single-producer / single-consumer.
template <typename T>
class OneShot {
public:
    void set(T value) {
        // Notify under the lock: get() must not reacquire the mutex (and
        // return, letting *this be destroyed) until set() has finished
        // touching the condvar. A spurious wake could otherwise let get()
        // return before notify runs, destroying the cv mid-notify.
        std::lock_guard<mutex> lock(mutex_);
        value_ = std::move(value);
        ready_ = true;
        cv_.notify_all();
    }

    T get() {
        std::unique_lock<mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
        return std::move(*value_);
    }

private:
    mutex mutex_;
    condition_variable cv_;
    std::optional<T> value_;
    bool ready_ = false;
};

/// void specialization: a one-shot latch (signal / wait).
template <>
class OneShot<void> {
public:
    void set() {
        // Notify under the lock (see OneShot<T>::set): guards against the
        // waiter destroying *this while set() is still in the condvar.
        std::lock_guard<mutex> lock(mutex_);
        ready_ = true;
        cv_.notify_all();
    }

    void get() {
        std::unique_lock<mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
    }

private:
    mutex mutex_;
    condition_variable cv_;
    bool ready_ = false;
};

}  // namespace mcp::sys
