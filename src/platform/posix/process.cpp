// POSIX backend: child-process management for the stdio client transport
// (fork/exec pipes, waitpid, SIGTERM/SIGKILL).

#include "../pal.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mcp::pal {

bool spawn(const ProcessSpec& spec, Process& out, std::string& error) {
    int in_pipe[2];   // parent -> child stdin
    int out_pipe[2];  // child stdout -> parent
    int err_pipe[2];  // child stderr -> parent
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        error = "failed to create pipes";
        return false;
    }

    // Build argv/envp before forking (only async-signal-safe calls after).
    std::vector<std::string> argv_storage;
    argv_storage.push_back(spec.command);
    for (const auto& arg : spec.args) {
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
        if (spec.env.find(key) == spec.env.end()) {
            env_storage.push_back(entry);
        }
    }
    for (const auto& [key, value] : spec.env) {
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
        error = std::strerror(errno);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                       err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return false;
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
        if (spec.cwd && ::chdir(spec.cwd->c_str()) != 0) {
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
    out.pid = pid;
    out.stdin_fd = in_pipe[1];
    out.stdout_fd = out_pipe[0];
    out.stderr_fd = err_pipe[0];
    return true;
}

bool wait_exit(Process& process, int timeout_ms, int& status) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    const pid_t pid = static_cast<pid_t>(process.pid);
    for (;;) {
        int raw_status = 0;
        const pid_t r = ::waitpid(pid, &raw_status, WNOHANG);
        if (r == pid) {
            status = raw_status;
            return true;
        }
        if (r < 0) {
            status = -1;
            return true;  // no such child (already reaped)
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void terminate(Process& process, bool force) {
    if (process.pid > 0) {
        ::kill(static_cast<pid_t>(process.pid), force ? SIGKILL : SIGTERM);
    }
}

bool exited_normally(int status) { return WIFEXITED(status); }

int exit_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace mcp::pal
