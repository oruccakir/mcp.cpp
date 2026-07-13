# VxWorks 7 (RTP) Port — Air-Gapped Bring-Up Guide

Target: VxWorks 7, LLVM/clang toolchain (C++17), **RTP** (user-mode process).
The PAL backend lives in `src/platform/vxworks/{io,net}.cpp`. Child-process
support is compiled out on VxWorks (`MCP_PAL_HAS_PROCESS` unset): the stdio
*client* transport is host-only; everything else — server SDK, client SDK,
stdio server transport, Streamable HTTP with sessions — is expected to work.

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
   usual flow) and launch it as an RTP (e.g. from the kernel shell:
   `rtpSp "/path/pal_selftest"` or via Workbench "Run RTP on Target").
2. It prints one `[ OK ]/[FAIL]` line per check (wake event, poll, TCP
   loopback, then a full MCP client↔server session over HTTP on 127.0.0.1)
   and a final `RESULT:` line. Exit code 0 = all good.
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
| `std::thread`/`mutex` errors | Toolchain not in C++17/libc++ mode or Diab selected — verify LLVM toolchain and `-std=c++17` |
| Link errors on `shutdown`/sockets | RTP needs the network libs; wr-* wrappers normally add them — check VSB link groups |
| `SIGPIPE` undeclared | Fine — the code guards it; report if other signal errors appear |
| `failed to create wake event` at startup | Fixed: WakeEvent now uses a loopback socket pair instead of `pipe()` (the pipe driver is often absent in RTP images). Needs `lo0` up — see next row. |
| Everything compiles, selftest `tcp.*` fails, or wake pair won't form | Loopback (`lo0`) not configured on the target image — check `ifconfig` in the kernel shell; the SDK needs 127.0.0.1 reachable |

Each report round trips back into this branch as fixes; the selftest is the
contract for "the port works".
