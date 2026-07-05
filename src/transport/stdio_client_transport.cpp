#include <mcp/transport/stdio_client_transport.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

#include "line_io.hpp"

extern char** environ;

namespace mcp {

namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

}  // namespace

StdioClientTransport::StdioClientTransport(StdioServerParameters parameters)
    : parameters_(std::move(parameters)) {}

StdioClientTransport::~StdioClientTransport() { disconnect(); }

void StdioClientTransport::set_stderr_handler(
    std::function<void(std::string)> handler) {
    std::lock_guard<std::mutex> lock(stderr_handler_mutex_);
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
    // Writing to a dead child must surface as a write error, not SIGPIPE.
    ::signal(SIGPIPE, SIG_IGN);
    spawn();
    if (child_pid_.load() < 0) {
        running_.store(false);
        return;
    }
    stdout_thread_ = std::thread([this] { stdout_loop(); });
    stderr_thread_ = std::thread([this] { stderr_loop(); });
}

void StdioClientTransport::spawn() {
    int in_pipe[2];   // parent -> child stdin
    int out_pipe[2];  // child stdout -> parent
    int err_pipe[2];  // child stderr -> parent
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        emit_error(Error(ErrorCode::InternalError, "failed to create pipes"));
        return;
    }

    // Build argv/envp before forking (only async-signal-safe calls after).
    std::vector<std::string> argv_storage;
    argv_storage.push_back(parameters_.command);
    for (const auto& arg : parameters_.args) {
        argv_storage.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string entry(*e);
        const auto key = entry.substr(0, entry.find('='));
        if (parameters_.env.find(key) == parameters_.env.end()) {
            env_storage.push_back(entry);
        }
    }
    for (const auto& [key, value] : parameters_.env) {
        env_storage.push_back(key + "=" + value);
    }
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& s : env_storage) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        emit_error(Error(ErrorCode::InternalError, "fork failed"));
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return;
    }

    if (pid == 0) {
        // Child.
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        if (parameters_.cwd && ::chdir(parameters_.cwd->c_str()) != 0) {
            ::_exit(126);
        }
        environ = envp.data();
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    // Parent.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    stderr_fd_ = err_pipe[0];
    child_pid_.store(pid);
}

void StdioClientTransport::write_line(const std::string& line) {
    if (stdin_fd_ < 0) {
        emit_error(Error(ErrorCode::InternalError, "transport is not connected"));
        return;
    }
    std::string framed = line;
    framed.push_back('\n');
    if (!detail::write_all(stdin_fd_, framed.data(), framed.size())) {
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
            std::lock_guard<std::mutex> lock(stderr_handler_mutex_);
            handler = stderr_handler_;
        }
        if (handler) {
            handler(line);
        }
    }
}

bool StdioClientTransport::wait_for_exit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const pid_t pid = child_pid_.load();
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exit_status_.store(status);
            exited_.store(true);
            return true;
        }
        if (r < 0) {
            return true;  // No such child (already reaped).
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void StdioClientTransport::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }
    const pid_t pid = child_pid_.load();

    // FR-TRAN-004: close stdin → wait → SIGTERM → wait → SIGKILL.
    close_fd(stdin_fd_);
    if (pid > 0 && !exited_.load()) {
        if (!wait_for_exit(grace_)) {
            ::kill(pid, SIGTERM);
            if (!wait_for_exit(grace_)) {
                ::kill(pid, SIGKILL);
                wait_for_exit(grace_);
            }
        }
    }

    const auto self = std::this_thread::get_id();
    if (stdout_thread_.joinable() && stdout_thread_.get_id() != self) {
        stdout_thread_.join();
    }
    if (stderr_thread_.joinable() && stderr_thread_.get_id() != self) {
        stderr_thread_.join();
    }
    close_fd(stdout_fd_);
    close_fd(stderr_fd_);
}

}  // namespace mcp
