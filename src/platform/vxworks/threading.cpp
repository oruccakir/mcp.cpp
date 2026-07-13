// VxWorks (RTP + DKM) native implementation of mcp::sys threading.
//
// mutex          -> mutual-exclusion semaphore (semMCreate)
// condition_var  -> counting semaphore + binary-semaphore-guarded waiter count
// thread         -> taskSpawn + a done-semaphore for join()
//
// First compiled on the Wind River toolchain; if a symbol here is missing,
// see docs/vxworks-port.md (VSB component checklist).

#include <mcp/sys/threading_vxworks.hpp>

#include <cstdio>
#include <cstring>

#include <semLib.h>
#include <sysLib.h>
#include <taskLib.h>

namespace mcp::sys {

namespace {

/// chrono milliseconds -> clock ticks (rounded up so a short wait never
/// collapses to 0 = no-wait).
int ms_to_ticks(long long ms) {
    if (ms <= 0) {
        return 0;
    }
    const long long rate = sysClkRateGet();  // ticks per second
    long long ticks = (ms * rate + 999) / 1000;
    if (ticks < 1) {
        ticks = 1;
    }
    return static_cast<int>(ticks);
}

}  // namespace

// ------------------------------------------------------------------- mutex

mutex::mutex()
    : sem_(semMCreate(SEM_Q_PRIORITY | SEM_INVERSION_SAFE | SEM_DELETE_SAFE)) {}

mutex::~mutex() {
    if (sem_ != nullptr) {
        semDelete(static_cast<SEM_ID>(sem_));
    }
}

void mutex::lock() { semTake(static_cast<SEM_ID>(sem_), WAIT_FOREVER); }

void mutex::unlock() { semGive(static_cast<SEM_ID>(sem_)); }

bool mutex::try_lock() {
    return semTake(static_cast<SEM_ID>(sem_), NO_WAIT) == OK;
}

// -------------------------------------------------------- condition_variable

condition_variable::condition_variable()
    : sema_(semCCreate(SEM_Q_FIFO, 0)),
      internal_(semBCreate(SEM_Q_PRIORITY, SEM_FULL)) {}

condition_variable::~condition_variable() {
    if (sema_ != nullptr) {
        semDelete(static_cast<SEM_ID>(sema_));
    }
    if (internal_ != nullptr) {
        semDelete(static_cast<SEM_ID>(internal_));
    }
}

void condition_variable::notify_all() {
    SEM_ID internal = static_cast<SEM_ID>(internal_);
    SEM_ID sema = static_cast<SEM_ID>(sema_);
    semTake(internal, WAIT_FOREVER);
    // Release one count per current waiter; each waiter consumes exactly one.
    while (waiters_ > 0) {
        --waiters_;
        semGive(sema);
    }
    semGive(internal);
}

void condition_variable::wait_for_ms(std::unique_lock<mutex>& lock,
                                     long long timeout_ms) {
    SEM_ID internal = static_cast<SEM_ID>(internal_);
    SEM_ID sema = static_cast<SEM_ID>(sema_);

    semTake(internal, WAIT_FOREVER);
    ++waiters_;
    semGive(internal);

    lock.unlock();  // release the caller's mutex while blocked
    const int ticks = timeout_ms < 0 ? WAIT_FOREVER : ms_to_ticks(timeout_ms);
    const STATUS status = semTake(sema, ticks);

    // Reconcile the waiter count with any concurrent notify_all.
    semTake(internal, WAIT_FOREVER);
    if (status != OK) {
        // Timed out. Either we are still counted (remove ourselves), or a
        // notify raced in after the timeout and already gave us a count —
        // consume it so it does not leak to the next waiter.
        if (waiters_ > 0) {
            --waiters_;
        } else {
            semTake(sema, NO_WAIT);
        }
    }
    semGive(internal);

    lock.lock();  // reacquire before returning (matches std::condition_variable)
}

// ------------------------------------------------------------------ thread

namespace {

struct TaskState {
    std::function<void()> fn;
    SEM_ID done;
    TASK_ID tid;
};

int task_trampoline(_Vx_usr_arg_t arg) {
    TaskState* state = reinterpret_cast<TaskState*>(arg);
    state->fn();
    semGive(state->done);  // last touch of `state` from this task
    return 0;
}

constexpr int kTaskPriority = 100;
constexpr int kTaskStackBytes = 0x10000;  // 64 KiB

}  // namespace

void thread::start(std::function<void()> fn) {
    TaskState* state = new TaskState{std::move(fn),
                                     semBCreate(SEM_Q_FIFO, SEM_EMPTY),
                                     TASK_ID_NULL};
    state->tid = taskSpawn(
        const_cast<char*>("tMcp"), kTaskPriority, VX_FP_TASK, kTaskStackBytes,
        reinterpret_cast<FUNCPTR>(&task_trampoline),
        reinterpret_cast<_Vx_usr_arg_t>(state), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (state->tid == TASK_ID_ERROR) {
        semDelete(state->done);
        delete state;
        state_ = nullptr;
        return;
    }
    state_ = state;
}

void thread::join() {
    TaskState* state = static_cast<TaskState*>(state_);
    if (state == nullptr) {
        return;
    }
    semTake(state->done, WAIT_FOREVER);  // task's last act is semGive(done)
    semDelete(state->done);
    delete state;
    state_ = nullptr;
}

thread::id thread::get_id() const noexcept {
    TaskState* state = static_cast<TaskState*>(state_);
    return state ? id(reinterpret_cast<void*>(state->tid)) : id();
}

// -------------------------------------------------------------- this_thread

namespace this_thread {

thread::id get_id() noexcept {
    return thread::id(reinterpret_cast<void*>(taskIdSelf()));
}

void sleep_for_ms(long long milliseconds) {
    if (milliseconds <= 0) {
        taskDelay(0);  // yield
        return;
    }
    taskDelay(ms_to_ticks(milliseconds));
}

}  // namespace this_thread

}  // namespace mcp::sys
