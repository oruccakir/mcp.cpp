# VxWorks 7 Port — Air-Gapped Bring-Up Guide

Target: VxWorks 7, LLVM/clang toolchain (C++17). Supports **RTP** (user-mode
process, runtime-verified) and **DKM** (kernel module). The PAL backend lives
in `src/platform/vxworks/{io,net,threading}.cpp`. Child-process support is
compiled out on VxWorks (`MCP_PAL_HAS_PROCESS` unset): the stdio *client*
transport is host-only; everything else — server SDK, client SDK, stdio
server transport, Streamable HTTP with sessions — works.

**Threading (RTP + DKM):** the SDK does not use `std::thread`/pthreads
directly — all threading goes through `mcp::sys`, and the VxWorks backend
(`threading.cpp`) implements it on native `taskSpawn` + semaphores. This is
why DKM works despite kernel mode having no pthreads: `find_package(Threads)`
is skipped on VxWorks and no pthread library is needed. The configure banner
prints `threading : vxworks-tasks` to confirm the native path is selected.

## 1. What to copy to the internal machine

The whole repository directory (this branch). Configure needs **no network**:
nlohmann/json is vendored, and with `-DMCP_BUILD_TESTS=OFF` nothing tries to
fetch GoogleTest.

## 2. Configure & build

Preferred: if your Wind River installation provides CMake integration
(wrenv/Workbench-generated toolchain file), use that. Otherwise copy
`cmake/toolchains/vxworks-rtp.cmake.example` to `vxworks-rtp.cmake`, fill the
`<PLACEHOLDERS>`, and:

```bash
# inside a wrenv-initialized shell
cmake -B build-vxworks \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/vxworks-rtp.cmake \
    -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_EXAMPLES=ON -DMCP_BUILD_TOOLS=ON
cmake --build build-vxworks -j
```

Expected artifacts: `libmcp-*.a`, `build-vxworks/tools/pal_selftest` (RTP
executable, `.vxe` depending on toolchain), `build-vxworks/examples/echo_server_http`.

## 3. Run on the target

1. Deploy `pal_selftest` to the target (Workbench download, ftp/tftp, or your
   usual flow). **RTP:** launch from the kernel C-interp shell with
   `rtpSp "/path/pal_selftest --args"` (whole command line as one string), or
   via Workbench "Run RTP on Target". **DKM:** build the module, load it
   (`ld < pal_selftest.o` or Workbench "Download Kernel Module"), and call its
   entry from the shell.
2. It prints one `[ OK ]/[FAIL]` line per check (wake event, poll, TCP
   loopback, then a full MCP client↔server session over HTTP on 127.0.0.1 —
   this exercises the native task/semaphore threading end to end) and a final
   `RESULT:` line. Exit code 0 = all good.
3. If the selftest passes, optionally run `echo_server_http --host 0.0.0.0
   --port 3001` on the target and, from another machine on the (internal)
   network, POST an `initialize` to `http://<target-ip>:3001/mcp`.

## 4. What to bring back (report checklist)

Bring back (photo or typed — everything below is short):

1. Compiler identification: output of `wr-cc --version` (or
   `clang --version`) from the toolchain used.
2. If **configure** failed: the last ~20 lines of CMake output.
3. If **compile** failed: the *first* failing file's full error text (later
   errors are usually cascades — the first one is the one that matters).
4. If it built: the complete `pal_selftest` output.
5. If the HTTP smoke ran: the curl/POST response (or error) text.

## 5. Likely first-compile issues (quick triage)

| Symptom | Likely cause / fix direction |
|---|---|
| `poll.h` not found | VSB built without POSIX poll component — check `INCLUDE_POSIX_*` layers in the VSB config |
| `inet_pton` undeclared | Missing network stack headers in the VSB; confirm `arpa/inet.h` exists in the sysroot |
| `CMAKE_HAVE_LIBC_PTHREAD ... not found` (DKM) | Expected — kernel mode has no pthreads. Handled: `find_package(Threads)` is skipped on VxWorks and `mcp::sys` uses native tasks. If it still hard-fails, confirm `CMAKE_SYSTEM_NAME` is `VxWorks` in your toolchain file. |
| `std::thread`/`mutex` errors from SDK code | Should not happen — the SDK uses `mcp::sys`, not `std::thread`. If seen, a stray `std::` slipped in; report the file/line. |
| `semMCreate`/`taskSpawn`/`taskDelay` undeclared | VSB missing the core kernel libs (semLib/taskLib/sysLib) in the include path — normally always present; check the sysroot. |
| condition-variable waits never wake, or hang | `taskDelay`/`sysClkRateGet` tick conversion issue, or priority inversion — report; the timer/SSE waits are the likely spots. |
| `thread_local` link/runtime error (DKM) | Kernel task-local storage may need a VSB TLS component. Only `PostCapture` uses it (HTTP POST correlation); if it fails, this is the one spot to convert to a `taskIdSelf()`-keyed map — flag it and I will. |
| Link errors on `shutdown`/sockets | RTP needs the network libs; wr-* wrappers normally add them — check VSB link groups |
| `SIGPIPE` undeclared | Fine — the code guards it; report if other signal errors appear |
| `failed to create wake event` at startup | Fixed: WakeEvent now uses a loopback socket pair instead of `pipe()` (the pipe driver is often absent in RTP images). Needs `lo0` up — see next row. |
| Everything compiles, selftest `tcp.*` fails, or wake pair won't form | Loopback (`lo0`) not configured on the target image — check `ifconfig` in the kernel shell; the SDK needs 127.0.0.1 reachable |

Each report round trips back into this branch as fixes; the selftest is the
contract for "the port works".
