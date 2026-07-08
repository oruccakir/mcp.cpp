// win32 PAL backend: child processes (CreateProcess + anonymous pipes).
// Parent pipe ends are wrapped as CRT fds so all I/O flows through the
// same pal::read_some/write_all paths as POSIX.

#include "win32_common.hpp"

#include <cstring>

#include <fcntl.h>
#include <io.h>

namespace mcp::pal {

namespace {

/// Microsoft's ArgvQuote: quotes one argument so CommandLineToArgvW /
/// the CRT parse it back verbatim (backslash and quote escaping).
void append_quoted(std::string& out, const std::string& arg) {
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        out += arg;
        return;
    }
    out += '"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += *it;
        }
    }
    out += '"';
}

/// Inherited environment merged with overrides, as a CreateProcess block
/// ("K=V\0K=V\0\0"). Windows env names are case-insensitive.
std::string build_env_block(const std::map<std::string, std::string>& overrides) {
    std::string block;
    char* strings = ::GetEnvironmentStringsA();
    if (strings != nullptr) {
        for (const char* entry = strings; *entry != '\0';
             entry += std::strlen(entry) + 1) {
            const std::string text(entry);
            const auto eq = text.find('=', 1);  // skip cmd's "=C:=..." entries
            const std::string key = eq == std::string::npos ? text
                                                            : text.substr(0, eq);
            bool overridden = false;
            for (const auto& [name, value] : overrides) {
                if (::_stricmp(name.c_str(), key.c_str()) == 0) {
                    overridden = true;
                    break;
                }
            }
            if (!overridden) {
                block += text;
                block += '\0';
            }
        }
        ::FreeEnvironmentStringsA(strings);
    }
    for (const auto& [name, value] : overrides) {
        block += name + "=" + value;
        block += '\0';
    }
    block += '\0';
    return block;
}

bool make_inheritable_pipe(HANDLE& read_end, HANDLE& write_end,
                           bool child_reads) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) {
        return false;
    }
    // Only the child's end stays inheritable.
    ::SetHandleInformation(child_reads ? write_end : read_end,
                           HANDLE_FLAG_INHERIT, 0);
    return true;
}

}  // namespace

bool spawn(const ProcessSpec& spec, Process& out, std::string& error) {
    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    if (!make_inheritable_pipe(in_read, in_write, /*child_reads=*/true) ||
        !make_inheritable_pipe(out_read, out_write, /*child_reads=*/false) ||
        !make_inheritable_pipe(err_read, err_write, /*child_reads=*/false)) {
        error = "failed to create pipes";
        return false;
    }

    std::string command_line;
    append_quoted(command_line, spec.command);
    for (const auto& arg : spec.args) {
        command_line += ' ';
        append_quoted(command_line, arg);
    }
    std::string env_block = build_env_block(spec.env);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(
        nullptr, command_line.data(), nullptr, nullptr,
        /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW, env_block.data(),
        spec.cwd ? spec.cwd->c_str() : nullptr, &si, &pi);

    // Child ends are duplicated into the child; close our copies.
    ::CloseHandle(in_read);
    ::CloseHandle(out_write);
    ::CloseHandle(err_write);

    if (!ok) {
        error = "CreateProcess failed: " +
                std::to_string(::GetLastError());
        ::CloseHandle(in_write);
        ::CloseHandle(out_read);
        ::CloseHandle(err_read);
        return false;
    }
    ::CloseHandle(pi.hThread);

    out.pid = static_cast<std::int64_t>(pi.dwProcessId);
    out.native_handle = reinterpret_cast<std::intptr_t>(pi.hProcess);
    out.stdin_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(in_write),
                                     _O_BINARY);
    out.stdout_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(out_read),
                                      _O_BINARY);
    out.stderr_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(err_read),
                                      _O_BINARY);
    return true;
}

bool wait_exit(Process& process, int timeout_ms, int& status) {
    const HANDLE handle = reinterpret_cast<HANDLE>(process.native_handle);
    if (handle == nullptr) {
        status = -1;
        return true;
    }
    const DWORD waited =
        ::WaitForSingleObject(handle, timeout_ms < 0 ? INFINITE
                                                     : static_cast<DWORD>(timeout_ms));
    if (waited != WAIT_OBJECT_0) {
        return false;  // still running (or wait failed -> treat as running)
    }
    DWORD code = 0;
    ::GetExitCodeProcess(handle, &code);
    status = static_cast<int>(code);
    ::CloseHandle(handle);
    process.native_handle = 0;
    return true;
}

void terminate(Process& process, bool force) {
    (void)force;  // No graceful signal on Windows (documented in pal.hpp).
    const HANDLE handle = reinterpret_cast<HANDLE>(process.native_handle);
    if (handle != nullptr) {
        ::TerminateProcess(handle, 1);
    }
}

bool exited_normally(int) { return true; }

int exit_code(int status) { return status; }

}  // namespace mcp::pal
