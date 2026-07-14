// Simulated monitor backend: lets the server build and run on host (Linux /
// macOS / Windows) so the MCP surface can be smoke-tested without a VxWorks
// target. Numbers are plausible fabrications; backend_name() returns
// "simulated" and the server stamps that into every response.

#include "monitor_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace monitor {
namespace {

struct SimTask {
    const char* name;
    std::uint64_t id;
    int priority;
    const char* status;
    std::uint64_t stack_size;
    std::uint64_t stack_high;
    std::uint64_t cpu_ticks;  // per 3-second window
};

// A believable VxWorks task set, including one stack hog and one CPU hog.
const SimTask kTasks[] = {
    {"tShell0", 0x60351ba8, 1, "READY", 65536, 22816, 4},
    {"tWdbTask", 0x60355c30, 3, "PEND", 16384, 3120, 0},
    {"tSpyTask", 0x60359f10, 5, "DELAY", 8192, 2048, 1},
    {"tNetTask", 0x6035e2c0, 50, "PEND", 32768, 9800, 12},
    {"tAioIoTask0", 0x60362a80, 50, "PEND", 16384, 2560, 0},
    {"tMcpHttp0", 0x60367140, 100, "PEND", 32768, 11264, 6},
    {"tWorker0", 0x6036b900, 120, "READY", 8192, 7420, 148},
    {"tLogTask", 0x6036fd20, 0, "PEND", 16384, 1980, 0},
};

std::uint64_t sim_ticks() {
    // Monotonic host clock at a simulated 60 Hz tick rate.
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return (std::uint64_t)
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() *
        60 / 1000;
}

TaskSnapshot make_snapshot(const SimTask& t) {
    TaskSnapshot snap;
    snap.name = t.name;
    snap.id = t.id;
    snap.priority = t.priority;
    snap.status = t.status;
    snap.cpu_index = 0;
    snap.entry = t.id - 0x1000;
    snap.stack_size = t.stack_size;
    snap.stack_current = t.stack_high / 2;
    snap.stack_high = t.stack_high;
    snap.stack_margin = (std::int64_t)(t.stack_size - t.stack_high);
    snap.exc_stack_size = 4096;
    snap.exc_stack_current = 256;
    snap.exc_stack_high = 512;
    snap.exc_stack_margin = 4096 - 512;
    return snap;
}

KernelHeapStats sim_heap() {
    KernelHeapStats heap;
    heap.bytes_alloc = 18u * 1024 * 1024;
    heap.blocks_alloc = 4210;
    heap.bytes_free = 46u * 1024 * 1024;
    heap.blocks_free = 87;
    heap.peak_bytes_alloc = 21u * 1024 * 1024;
    heap.max_free_block = 40u * 1024 * 1024;
    return heap;
}

}  // namespace

const char* backend_name() { return "simulated"; }

bool list_tasks(std::vector<TaskSnapshot>& out, std::string&) {
    out.clear();
    for (const SimTask& t : kTasks) out.push_back(make_snapshot(t));
    return true;
}

bool task_info(const std::string& name_or_id, TaskSnapshot& out,
               std::string& error) {
    for (const SimTask& t : kTasks) {
        char hex_id[32];
        std::snprintf(hex_id, sizeof hex_id, "0x%llx",
                      (unsigned long long)t.id);
        if (name_or_id == t.name || name_or_id == hex_id ||
            name_or_id == std::to_string(t.id)) {
            out = make_snapshot(t);
            return true;
        }
    }
    error = "task not found: " + name_or_id;
    return false;
}

bool kernel_heap(KernelHeapStats& out, std::string&) {
    out = sim_heap();
    return true;
}

bool system_info(SystemSnapshot& out, std::string&) {
    out.ticks = sim_ticks();
    out.tick_rate_hz = 60;
    out.uptime_seconds = (double)out.ticks / 60.0;
    out.task_count = (int)(sizeof kTasks / sizeof kTasks[0]);
    out.module_count = 2;
    out.heap = sim_heap();
    return true;
}

bool list_modules(std::vector<ModuleSnapshot>& out, std::string&) {
    out.clear();
    ModuleSnapshot mcp_module;
    mcp_module.name = "vxworks_monitor_server.out";
    mcp_module.id = 0x60800000;
    mcp_module.group = 2;
    mcp_module.text_addr = 0x60810000;
    mcp_module.text_size = 812 * 1024;
    mcp_module.data_addr = 0x608e0000;
    mcp_module.data_size = 96 * 1024;
    mcp_module.bss_addr = 0x608f8000;
    mcp_module.bss_size = 24 * 1024;
    out.push_back(mcp_module);

    ModuleSnapshot app_module;
    app_module.name = "myApp.out";
    app_module.id = 0x60900000;
    app_module.group = 3;
    app_module.text_addr = 0x60910000;
    app_module.text_size = 210 * 1024;
    app_module.data_addr = 0x60948000;
    app_module.data_size = 32 * 1024;
    app_module.bss_addr = 0x60950000;
    app_module.bss_size = 8 * 1024;
    out.push_back(app_module);
    return true;
}

bool cpu_usage(unsigned seconds, CpuUsageReport& out, std::string&) {
    // Returns immediately (no real window to sample on host).
    out.window_seconds = seconds;
    out.total_ticks = (std::uint64_t)seconds * 60;
    out.tasks.clear();

    std::uint64_t attributed = 0;
    for (const SimTask& t : kTasks) {
        if (t.cpu_ticks == 0) continue;
        CpuTaskUsage usage;
        usage.name = t.name;
        usage.id = t.id;
        usage.ticks = t.cpu_ticks * seconds / 3;
        usage.percent = out.total_ticks
                            ? 100.0 * (double)usage.ticks /
                                  (double)out.total_ticks
                            : 0.0;
        attributed += usage.ticks;
        out.tasks.push_back(std::move(usage));
    }
    out.unattributed_ticks =
        out.total_ticks > attributed ? out.total_ticks - attributed : 0;
    std::sort(out.tasks.begin(), out.tasks.end(),
              [](const CpuTaskUsage& a, const CpuTaskUsage& b) {
                  return a.ticks > b.ticks;
              });
    return true;
}

bool sem_info(std::uint64_t id, SemSnapshot& out, std::string& error) {
    if (id != 0x60377000) {
        error = "semInfoGet failed (invalid semaphore ID?) — the simulated "
                "backend only knows 0x60377000";
        return false;
    }
    out.id = id;
    out.type_raw = 1;
    out.type = "mutex";
    out.options = 0x0d;
    out.blocked_tasks = 2;
    out.has_owner = true;
    out.owner = kTasks[6].id;  // tWorker0 holds it
    return true;
}

bool msgq_info(std::uint64_t id, MsgQSnapshot& out, std::string& error) {
    if (id != 0x60378000) {
        error = "msgQInfoGet failed (invalid message queue ID?) — the "
                "simulated backend only knows 0x60378000";
        return false;
    }
    out.id = id;
    out.num_msgs = 3;
    out.num_tasks = 0;
    out.send_timeouts = 0;
    out.recv_timeouts = 7;
    out.options = 0x01;
    out.max_msgs = 64;
    out.max_msg_length = 512;
    return true;
}

}  // namespace monitor
