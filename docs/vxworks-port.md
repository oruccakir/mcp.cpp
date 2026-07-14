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

### 3.1 System-monitor MCP server (DKM)

`examples/vxworks_monitor/` builds `vxworks_monitor_server` — an MCP server
exposing task/stack/heap/CPU/module/semaphore introspection as tools (see its
README for the tool catalog). On VxWorks it links the real kernel backend;
host builds get simulated data. As a DKM:

```
-> ld < vxworks_monitor_server.out
-> mcp_monitor_start 3001     # value = 3001 (bound port), -1 on failure
-> mcp_monitor_stop           # value = 0
```

Entries are stdio-free (same `__stdioFp` rationale as `mcp_echo_http_start`),
and the backend uses only info-get APIs — no `*Show()` console printers.

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
| `undefined symbol poll` (DKM) | Fixed: the VxWorks backend uses `select()` (available in kernel mode), not `poll()`. |
| `__cxa_thread_atexit` / `DKM_TLS_SIZE (0) too small` | Fixed: the one `thread_local` (HTTP POST correlation) is now a thread-id-keyed map, so the module needs no TLS. |
| `undefined symbol __stdioFp` (DKM), then a Page Fault when you call the entry | The module references C stdio (`printf`/`fprintf`), which needs `__stdioFp` (the `stdout`/`stderr` fp table). A DKM image without the ANSI stdio component leaves it null, so the first `printf` faults (Page Fault Addr `0x4`, signal 11). The `echo_server_http` DKM entries are now **stdio-free** — status is the return value (`value = <port>` on success, negative on error), so they run without the component. To use stdio anyway (e.g. `MCP_LOG`, or examples' `printf`), enable `INCLUDE_ANSI_STDIO` (+ the fp table) in the VSB. If your own DKM code still references `__stdioFp`, it's a `printf`/`fprintf` on a path that runs — drop it or add the VSB component. |
| Link errors on `shutdown`/sockets | RTP needs the network libs; wr-* wrappers normally add them — check VSB link groups |
| `SIGPIPE` undeclared | Fine — the code guards it; report if other signal errors appear |
| `failed to create wake event` at startup | Fixed: WakeEvent now uses a loopback socket pair instead of `pipe()` (the pipe driver is often absent in RTP images). Needs `lo0` up — see next row. |
| Everything compiles, selftest `tcp.*` fails, or wake pair won't form | Loopback (`lo0`) not configured on the target image — check `ifconfig` in the kernel shell; the SDK needs 127.0.0.1 reachable |

Each report round trips back into this branch as fixes; the selftest is the
contract for "the port works".
