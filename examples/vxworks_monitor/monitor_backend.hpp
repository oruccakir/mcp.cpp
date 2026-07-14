// Backend interface for the VxWorks system-monitor MCP server.
//
// Two implementations, selected at build time (examples/CMakeLists.txt):
//   monitor_backend_vxworks.cpp — real taskLib/memPartLib/moduleLib data;
//                                 the ONLY file that includes VxWorks headers
//   monitor_backend_sim.cpp     — simulated data so the server builds and can
//                                 be smoke-tested on host CI (backend_name()
//                                 tells them apart)
//
// All calls return true on success; on failure they fill `error` and the
// output is unspecified. No exceptions cross this boundary and no stdio is
// used (DKM images without the ANSI stdio component have no __stdioFp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace monitor {

struct TaskSnapshot {
    std::string name;
    std::uint64_t id = 0;  // TASK_ID
    int priority = 0;
    std::string status;  // e.g. "READY", "PEND", "DELAY"
    int cpu_index = 0;
    int error_status = 0;
    int delay_ticks = 0;
    std::uint64_t entry = 0;  // entry point address
    // Execution stack.
    std::uint64_t stack_size = 0;
    std::uint64_t stack_current = 0;
    std::uint64_t stack_high = 0;  // high-water mark
    std::int64_t stack_margin = 0;
    // Exception stack.
    std::uint64_t exc_stack_size = 0;
    std::uint64_t exc_stack_current = 0;
    std::uint64_t exc_stack_high = 0;
    std::int64_t exc_stack_margin = 0;
};

struct KernelHeapStats {
    std::uint64_t bytes_alloc = 0;
    std::uint64_t blocks_alloc = 0;
    std::uint64_t bytes_free = 0;
    std::uint64_t blocks_free = 0;
    std::uint64_t peak_bytes_alloc = 0;
    std::uint64_t max_free_block = 0;  // largest contiguous free block
};

struct ModuleSnapshot {
    std::string name;
    std::uint64_t id = 0;
    int group = 0;
    std::uint64_t text_addr = 0;
    std::uint64_t text_size = 0;
    std::uint64_t data_addr = 0;
    std::uint64_t data_size = 0;
    std::uint64_t bss_addr = 0;
    std::uint64_t bss_size = 0;
};

struct CpuTaskUsage {
    std::string name;
    std::uint64_t id = 0;
    std::uint64_t ticks = 0;
    double percent = 0.0;
};

struct CpuUsageReport {
    unsigned window_seconds = 0;
    std::uint64_t total_ticks = 0;  // ticks elapsed during the window
    std::uint64_t unattributed_ticks = 0;  // total - sum(tasks); mostly idle
    std::vector<CpuTaskUsage> tasks;  // sorted by ticks, descending
};

struct SemSnapshot {
    std::uint64_t id = 0;
    int type_raw = 0;
    std::string type;  // "binary" | "mutex" | "counting" | "type-N"
    int options = 0;
    unsigned blocked_tasks = 0;
    // Which union member of SEM_INFO.state is meaningful depends on type.
    bool has_count = false;
    unsigned count = 0;
    bool has_full = false;
    bool full = false;
    bool has_owner = false;
    std::uint64_t owner = 0;
};

struct MsgQSnapshot {
    std::uint64_t id = 0;
    int num_msgs = 0;
    int num_tasks = 0;  // blocked senders or receivers, see msgQInfoGet docs
    int send_timeouts = 0;
    int recv_timeouts = 0;
    int options = 0;
    int max_msgs = 0;
    std::uint64_t max_msg_length = 0;
};

struct SystemSnapshot {
    std::uint64_t ticks = 0;
    std::uint64_t tick_rate_hz = 0;
    double uptime_seconds = 0.0;
    int task_count = 0;
    int module_count = 0;
    KernelHeapStats heap;
};

/// "vxworks" or "simulated" — stamped into every tool response.
const char* backend_name();

bool list_tasks(std::vector<TaskSnapshot>& out, std::string& error);
/// `name_or_id`: task name, decimal ID, or hex ID ("0x...").
bool task_info(const std::string& name_or_id, TaskSnapshot& out,
               std::string& error);
bool kernel_heap(KernelHeapStats& out, std::string& error);
bool system_info(SystemSnapshot& out, std::string& error);
bool list_modules(std::vector<ModuleSnapshot>& out, std::string& error);
/// Samples per-task CPU ticks for `seconds` (blocks the calling task).
bool cpu_usage(unsigned seconds, CpuUsageReport& out, std::string& error);
bool sem_info(std::uint64_t id, SemSnapshot& out, std::string& error);
bool msgq_info(std::uint64_t id, MsgQSnapshot& out, std::string& error);

}  // namespace monitor
