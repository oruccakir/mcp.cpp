// Real VxWorks 7 implementation of the monitor backend (DKM primary, RTP
// best-effort). This is the only file in the example that includes VxWorks
// headers.
//
// Deliberately stdio-free: DKM images without the ANSI stdio VSB component
// have no __stdioFp, so any printf faults (see docs/vxworks-port.md). All
// data comes from programmatic info-get APIs, never from *Show() routines
// (spyReport/semShow/msgQShow/moduleShow all print to the console).
//
// CPU usage: spyLib only reports via printf, so we sample ourselves with a
// task-switch hook that attributes tickGet() deltas to the task being
// switched out. Tick resolution (sysClkRateGet(), typically 60–100 Hz) —
// switches within one tick attribute 0 — and on SMP the single
// last-switch-tick makes numbers approximate. Good enough for "who is eating
// the CPU".

#include "monitor_backend.hpp"

#include <vxWorks.h>

#include <taskLib.h>
#include <taskLibCommon.h>
#include <taskHookLib.h>
#include <tickLib.h>
#include <sysLib.h>
#include <memPartLib.h>
#include <memLib.h>
#include <semLib.h>
#include <msgQLib.h>
#include <moduleLib.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>

extern "C" {
extern PART_ID memSysPartId;  // kernel heap (RTP: the process heap partition)
}

namespace monitor {
namespace {

constexpr int kMaxTasks = 256;
constexpr int kMaxModules = 64;

std::uint64_t to_u64(const void* p) {
    return (std::uint64_t)(uintptr_t)p;
}

std::uint64_t tid_to_u64(TASK_ID tid) {
    return (std::uint64_t)(uintptr_t)tid;
}

// Same lookup order as the shell helpers: hex "0x..." ID, decimal ID, name.
TASK_ID task_lookup(const std::string& name_or_id) {
    const char* s = name_or_id.c_str();
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        TASK_ID tid = (TASK_ID)(uintptr_t)std::strtoull(s + 2, nullptr, 16);
        if (taskIdVerify(tid) == OK) return tid;
    }
    if (s[0] >= '0' && s[0] <= '9') {
        TASK_ID tid = (TASK_ID)(uintptr_t)std::strtoull(s, nullptr, 10);
        if (taskIdVerify(tid) == OK) return tid;
    }
    TASK_ID tid = taskNameToId((char*)s);
    if (tid != (TASK_ID)ERROR && tid != 0) return tid;
    return (TASK_ID)ERROR;
}

bool fill_task(TASK_ID tid, TaskSnapshot& out) {
    TASK_DESC desc;
    if (taskInfoGet(tid, &desc) != OK) return false;

    char status_str[32];
    status_str[0] = '\0';
    taskStatusString(tid, status_str);

    out.name = desc.td_name ? desc.td_name : "";
    out.id = tid_to_u64(tid);
    out.priority = desc.td_priority;
    out.status = status_str;
    out.cpu_index = desc.td_cpuIndex;
    out.error_status = desc.td_errorStatus;
    out.delay_ticks = desc.td_delay;
    out.entry = (std::uint64_t)(uintptr_t)desc.td_entry;
    out.stack_size = (std::uint64_t)desc.td_stackSize;
    out.stack_current = (std::uint64_t)desc.td_stackCurrent;
    out.stack_high = (std::uint64_t)desc.td_stackHigh;
    out.stack_margin = (std::int64_t)desc.td_stackMargin;
    out.exc_stack_size = (std::uint64_t)desc.td_excStackSize;
    out.exc_stack_current = (std::uint64_t)desc.td_excStackCurrent;
    out.exc_stack_high = (std::uint64_t)desc.td_excStackHigh;
    out.exc_stack_margin = (std::int64_t)desc.td_excStackMargin;
    return true;
}

bool fill_heap(KernelHeapStats& out, std::string& error) {
    MEM_PART_STATS stats;
    if (memPartInfoGet(memSysPartId, &stats) != OK) {
        error = "memPartInfoGet failed for the system partition";
        return false;
    }
    out.bytes_alloc = (std::uint64_t)stats.numBytesAlloc;
    out.blocks_alloc = (std::uint64_t)stats.numBlocksAlloc;
    out.bytes_free = (std::uint64_t)stats.numBytesFree;
    out.blocks_free = (std::uint64_t)stats.numBlocksFree;
    out.peak_bytes_alloc = (std::uint64_t)stats.maxBytesAlloc;
    out.max_free_block = (std::uint64_t)stats.maxBlockSizeFree;
    return true;
}

// --- CPU sampling ----------------------------------------------------------
// The switch hook runs inside the kernel critical section: fixed storage,
// no allocation, no locks, only tickGet() and array writes.

struct CpuSlot {
    TASK_ID tid;
    unsigned long long ticks;
};

CpuSlot g_cpu_slots[kMaxTasks];
int g_cpu_slot_count = 0;
unsigned long long g_last_switch_tick = 0;
std::atomic<bool> g_sampling{false};

extern "C" void mcp_monitor_switch_hook(void* pOldTcb, void* /*pNewTcb*/) {
    unsigned long long now = (unsigned long long)tickGet();
    unsigned long long delta = now - g_last_switch_tick;
    g_last_switch_tick = now;
    if (delta == 0) return;

    TASK_ID tid = (TASK_ID)pOldTcb;
    for (int i = 0; i < g_cpu_slot_count; ++i) {
        if (g_cpu_slots[i].tid == tid) {
            g_cpu_slots[i].ticks += delta;
            return;
        }
    }
    if (g_cpu_slot_count < kMaxTasks) {
        g_cpu_slots[g_cpu_slot_count].tid = tid;
        g_cpu_slots[g_cpu_slot_count].ticks = delta;
        ++g_cpu_slot_count;
    }
}

}  // namespace

const char* backend_name() { return "vxworks"; }

bool list_tasks(std::vector<TaskSnapshot>& out, std::string& error) {
    TASK_ID id_list[kMaxTasks];
    int n = taskIdListGet(id_list, kMaxTasks);
    if (n < 0) {
        error = "taskIdListGet failed";
        return false;
    }
    out.clear();
    out.reserve((std::size_t)n);
    for (int i = 0; i < n; ++i) {
        TaskSnapshot snap;
        if (fill_task(id_list[i], snap)) out.push_back(std::move(snap));
        // Tasks can exit between list and info — skip silently.
    }
    return true;
}

bool task_info(const std::string& name_or_id, TaskSnapshot& out,
               std::string& error) {
    TASK_ID tid = task_lookup(name_or_id);
    if (tid == (TASK_ID)ERROR) {
        error = "task not found: " + name_or_id;
        return false;
    }
    if (!fill_task(tid, out)) {
        error = "taskInfoGet failed for: " + name_or_id;
        return false;
    }
    return true;
}

bool kernel_heap(KernelHeapStats& out, std::string& error) {
    return fill_heap(out, error);
}

bool system_info(SystemSnapshot& out, std::string& error) {
    out.ticks = (std::uint64_t)tickGet();
    out.tick_rate_hz = (std::uint64_t)sysClkRateGet();
    out.uptime_seconds =
        out.tick_rate_hz ? (double)out.ticks / (double)out.tick_rate_hz : 0.0;
    out.task_count = taskIdListGet(nullptr, 0);

    MODULE_ID module_ids[kMaxModules];
    out.module_count = moduleIdListGet(module_ids, kMaxModules);
    if (out.module_count < 0) out.module_count = 0;

    return fill_heap(out.heap, error);
}

bool list_modules(std::vector<ModuleSnapshot>& out, std::string& error) {
    MODULE_ID module_ids[kMaxModules];
    int n = moduleIdListGet(module_ids, kMaxModules);
    if (n < 0) {
        error = "moduleIdListGet failed";
        return false;
    }
    out.clear();
    out.reserve((std::size_t)n);
    for (int i = 0; i < n; ++i) {
        MODULE_INFO info;
        std::memset(&info, 0, sizeof info);
        if (moduleInfoGet(module_ids[i], &info) != OK) continue;

        ModuleSnapshot snap;
        snap.name = info.name;
        snap.id = to_u64(module_ids[i]);
        snap.group = (int)info.group;
        snap.text_addr = to_u64(info.segInfo.textAddr);
        snap.text_size = (std::uint64_t)info.segInfo.textSize;
        snap.data_addr = to_u64(info.segInfo.dataAddr);
        snap.data_size = (std::uint64_t)info.segInfo.dataSize;
        snap.bss_addr = to_u64(info.segInfo.bssAddr);
        snap.bss_size = (std::uint64_t)info.segInfo.bssSize;
        out.push_back(std::move(snap));
    }
    return true;
}

bool cpu_usage(unsigned seconds, CpuUsageReport& out, std::string& error) {
    bool expected = false;
    if (!g_sampling.compare_exchange_strong(expected, true)) {
        error = "a CPU sampling window is already running";
        return false;
    }

    g_cpu_slot_count = 0;
    unsigned long long start = (unsigned long long)tickGet();
    g_last_switch_tick = start;

    if (taskSwitchHookAdd((FUNCPTR)mcp_monitor_switch_hook) != OK) {
        g_sampling.store(false);
        error = "taskSwitchHookAdd failed (hook table full?)";
        return false;
    }
    taskDelay((int)(seconds * (unsigned)sysClkRateGet()));
    taskSwitchHookDelete((FUNCPTR)mcp_monitor_switch_hook);

    unsigned long long total = (unsigned long long)tickGet() - start;
    out.window_seconds = seconds;
    out.total_ticks = total;
    out.tasks.clear();

    unsigned long long attributed = 0;
    for (int i = 0; i < g_cpu_slot_count; ++i) {
        CpuTaskUsage usage;
        usage.id = tid_to_u64(g_cpu_slots[i].tid);
        usage.ticks = g_cpu_slots[i].ticks;
        usage.percent =
            total ? 100.0 * (double)usage.ticks / (double)total : 0.0;

        TASK_DESC desc;
        if (taskIdVerify(g_cpu_slots[i].tid) == OK &&
            taskInfoGet(g_cpu_slots[i].tid, &desc) == OK) {
            usage.name = desc.td_name ? desc.td_name : "";
        } else {
            usage.name = "(exited)";
        }
        attributed += usage.ticks;
        out.tasks.push_back(std::move(usage));
    }
    g_sampling.store(false);

    out.unattributed_ticks = total > attributed ? total - attributed : 0;
    std::sort(out.tasks.begin(), out.tasks.end(),
              [](const CpuTaskUsage& a, const CpuTaskUsage& b) {
                  return a.ticks > b.ticks;
              });
    (void)error;
    return true;
}

bool sem_info(std::uint64_t id, SemSnapshot& out, std::string& error) {
    SEM_ID sem_id = (SEM_ID)(uintptr_t)id;
    SEM_INFO info;
    std::memset(&info, 0, sizeof info);
    if (semInfoGet(sem_id, &info) != OK) {
        error = "semInfoGet failed (invalid semaphore ID?)";
        return false;
    }
    out.id = id;
    out.type_raw = (int)info.semType;
    out.options = info.options;
    out.blocked_tasks = (unsigned)info.numTasks;
    switch (info.semType) {
        case SEM_TYPE_BINARY:
            out.type = "binary";
            out.has_full = true;
            out.full = info.state.full != FALSE;
            break;
        case SEM_TYPE_MUTEX:
            out.type = "mutex";
            out.has_owner = true;
            out.owner = tid_to_u64(info.state.owner);
            break;
        case SEM_TYPE_COUNTING:
            out.type = "counting";
            out.has_count = true;
            out.count = (unsigned)info.state.count;
            break;
        default:
            out.type = "type-" + std::to_string(out.type_raw);
            break;
    }
    return true;
}

bool msgq_info(std::uint64_t id, MsgQSnapshot& out, std::string& error) {
    MSG_Q_ID q_id = (MSG_Q_ID)(uintptr_t)id;
    MSG_Q_INFO info;
    std::memset(&info, 0, sizeof info);
    if (msgQInfoGet(q_id, &info) != OK) {
        error = "msgQInfoGet failed (invalid message queue ID?)";
        return false;
    }
    out.id = id;
    out.num_msgs = info.numMsgs;
    out.num_tasks = info.numTasks;
    out.send_timeouts = info.sendTimeouts;
    out.recv_timeouts = info.recvTimeouts;
    out.options = info.options;
    out.max_msgs = info.maxMsgs;
    out.max_msg_length = (std::uint64_t)info.maxMsgLength;
    return true;
}

}  // namespace monitor
