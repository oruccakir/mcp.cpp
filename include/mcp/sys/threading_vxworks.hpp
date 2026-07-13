#pragma once

// VxWorks (RTP + DKM) threading primitives for mcp::sys, backed by native
// tasks and semaphores — kernel mode has no pthreads. Interface-compatible
// with the std:: types the SDK uses (see include/mcp/sys/threading.hpp).
//
// Kernel handles are held as void* so vxWorks headers stay out of the SDK's
// translation units; the casts live in src/platform/vxworks/threading.cpp.
// First compiled on the target toolchain — see docs/vxworks-port.md.

#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

namespace mcp::sys {

/// Mutual-exclusion semaphore (semMCreate). BasicLockable + Lockable, so it
/// works with std::lock_guard / std::unique_lock unchanged.
class mutex {
public:
    mutex();
    ~mutex();
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock();
    void unlock();
    bool try_lock();

    void* native_handle() const { return sem_; }

private:
    void* sem_ = nullptr;  // SEM_ID
};

/// Condition variable built on a counting semaphore + a small internal lock.
/// Only the operations the SDK uses are provided (wait / wait predicate /
/// wait_until / notify_all). Extra (spurious) wakeups are possible and
/// harmless — every SDK wait is either predicate-guarded or re-checks state
/// in a loop.
class condition_variable {
public:
    condition_variable();
    ~condition_variable();
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    void notify_all();

    void wait(std::unique_lock<mutex>& lock) { wait_for_ms(lock, -1); }

    template <class Predicate>
    void wait(std::unique_lock<mutex>& lock, Predicate pred) {
        while (!pred()) {
            wait_for_ms(lock, -1);
        }
    }

    template <class Clock, class Duration>
    void wait_until(std::unique_lock<mutex>& lock,
                    const std::chrono::time_point<Clock, Duration>& deadline) {
        const auto now = Clock::now();
        const long long ms =
            deadline > now
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - now)
                      .count()
                : 0;
        wait_for_ms(lock, ms);
    }

private:
    // timeout_ms < 0 waits forever; releases `lock` while blocked, reacquires
    // before returning.
    void wait_for_ms(std::unique_lock<mutex>& lock, long long timeout_ms);

    void* sema_ = nullptr;      // counting semaphore (semCCreate)
    void* internal_ = nullptr;  // binary semaphore guarding waiters_
    int waiters_ = 0;
};

/// Task-backed thread. Move-only; matches std::thread's "terminate if a
/// joinable thread is destroyed/overwritten" contract.
class thread {
public:
    class id {
    public:
        id() = default;
        explicit id(void* task) : task_(task) {}
        friend bool operator==(id a, id b) { return a.task_ == b.task_; }
        friend bool operator!=(id a, id b) { return a.task_ != b.task_; }

    private:
        void* task_ = nullptr;  // TASK_ID
    };

    thread() = default;

    template <class Fn>
    explicit thread(Fn&& fn) {
        start(std::function<void()>(std::forward<Fn>(fn)));
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& other) noexcept { std::swap(state_, other.state_); }
    thread& operator=(thread&& other) noexcept {
        if (joinable()) {
            std::terminate();
        }
        std::swap(state_, other.state_);
        return *this;
    }

    ~thread() {
        if (joinable()) {
            std::terminate();
        }
    }

    bool joinable() const noexcept { return state_ != nullptr; }
    void join();
    id get_id() const noexcept;

private:
    void start(std::function<void()> fn);

    void* state_ = nullptr;  // heap-owned task state (see threading.cpp)
};

namespace this_thread {

thread::id get_id() noexcept;
void sleep_for_ms(long long milliseconds);

template <class Rep, class Period>
void sleep_for(const std::chrono::duration<Rep, Period>& duration) {
    sleep_for_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

}  // namespace this_thread

}  // namespace mcp::sys
