#define MCP_LOG_COMPONENT "stdio"

#include <mcp/transport/stdio_client_transport.hpp>

#include <mcp/log.hpp>

#include "../platform/pal.hpp"
#include "line_io.hpp"

namespace mcp {

StdioClientTransport::StdioClientTransport(StdioServerParameters parameters)
    : parameters_(std::move(parameters)) {}

StdioClientTransport::~StdioClientTransport() { disconnect(); }

void StdioClientTransport::set_stderr_handler(
    std::function<void(std::string)> handler) {
    std::lock_guard<mcp::sys::mutex> lock(stderr_handler_mutex_);
    stderr_handler_ = std::move(handler);
}

std::optional<int> StdioClientTransport::exit_status() const {
    if (!exited_.load()) {
        return std::nullopt;
    }
    return exit_status_.load();
}

void StdioClientTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    // Writing to a dead child must surface as a write error, not a signal.
    pal::ignore_broken_pipe_signals();

    pal::ProcessSpec spec;
    spec.command = parameters_.command;
    spec.args = parameters_.args;
    spec.env = parameters_.env;
    spec.cwd = parameters_.cwd;

    pal::Process process;
    std::string error;
    if (!pal::spawn(spec, process, error)) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "spawn failed: " + error));
        return;
    }
    MCP_LOG(info, "spawned: " << parameters_.command << " (pid " << process.pid
                              << ")");
    child_pid_.store(process.pid);
    child_handle_.store(process.native_handle);
    // Pipe descriptors are CRT fds on every backend: they fit in int.
    stdin_fd_ = static_cast<int>(process.stdin_fd);
    stdout_fd_ = static_cast<int>(process.stdout_fd);
    stderr_fd_ = static_cast<int>(process.stderr_fd);

    stdout_thread_ = mcp::sys::thread([this] { stdout_loop(); });
    stderr_thread_ = mcp::sys::thread([this] { stderr_loop(); });
}

void StdioClientTransport::write_line(const std::string& line) {
    if (stdin_fd_ < 0) {
        emit_error(Error(ErrorCode::InternalError, "transport is not connected"));
        return;
    }
    std::string framed = line;
    framed.push_back('\n');
    if (!pal::write_all(stdin_fd_, framed.data(), framed.size())) {
        emit_error(Error(ErrorCode::InternalError,
                         "write to server process failed"));
    }
}

void StdioClientTransport::stdout_loop() {
    detail::LineReader reader(stdout_fd_);
    std::string line;
    for (;;) {
        const auto status = reader.next(line);
        if (status == detail::LineReader::Status::Line) {
            process_line(line);
            continue;
        }
        if (status == detail::LineReader::Status::IoError && running_.load()) {
            emit_error(Error(ErrorCode::InternalError,
                             "read from server process failed"));
        }
        break;
    }
    if (running_.load()) {
        emit_close();
    }
}

void StdioClientTransport::stderr_loop() {
    detail::LineReader reader(stderr_fd_);
    std::string line;
    while (reader.next(line) == detail::LineReader::Status::Line) {
        std::function<void(std::string)> handler;
        {
            std::lock_guard<mcp::sys::mutex> lock(stderr_handler_mutex_);
            handler = stderr_handler_;
        }
        if (handler) {
            handler(line);
        }
    }
}

void StdioClientTransport::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }

    pal::Process process;
    process.pid = child_pid_.load();
    process.native_handle = child_handle_.load();
    process.stdin_fd = stdin_fd_;
    process.stdout_fd = stdout_fd_;
    process.stderr_fd = stderr_fd_;

    // FR-TRAN-004: close stdin -> wait -> SIGTERM -> wait -> SIGKILL.
    pal::close_fd(stdin_fd_);
    const int grace = static_cast<int>(grace_.count());
    if (process.pid > 0 && !exited_.load()) {
        int status = -1;
        bool exited = pal::wait_exit(process, grace, status);
        if (!exited) {
            MCP_LOG(warn, "child unresponsive after stdin close; terminating");
            pal::terminate(process, /*force=*/false);
            exited = pal::wait_exit(process, grace, status);
        }
        if (!exited) {
            MCP_LOG(warn, "child still running; forcing termination");
            pal::terminate(process, /*force=*/true);
            exited = pal::wait_exit(process, grace, status);
        }
        if (exited) {
            MCP_LOG(info, "child exited (raw status " << status << ")");
            exit_status_.store(status);
            exited_.store(true);
        }
    }

    const auto self = mcp::sys::this_thread::get_id();
    if (stdout_thread_.joinable() && stdout_thread_.get_id() != self) {
        stdout_thread_.join();
    }
    if (stderr_thread_.joinable() && stderr_thread_.get_id() != self) {
        stderr_thread_.join();
    }
    pal::close_fd(stdout_fd_);
    pal::close_fd(stderr_fd_);
}

}  // namespace mcp
