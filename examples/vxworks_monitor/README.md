# VxWorks System Monitor — MCP Server

An MCP server that exposes VxWorks 7 RTOS introspection as tools, so an AI
agent (or any MCP client) can inspect a live target: tasks, stacks, kernel
heap, CPU usage, loaded modules, semaphores, and message queues. Runs as a
**DKM (kernel module)**, an RTP, or — with simulated data — on a host build.

## Layout

| file | role |
|---|---|
| `vxworks_monitor_server.cpp` | MCP server + tools, HTTP transport, `main()` + DKM entry points |
| `monitor_backend.hpp` | backend interface (plain structs, no VxWorks types) |
| `monitor_backend_vxworks.cpp` | real implementation — the only file with VxWorks headers |
| `monitor_backend_sim.cpp` | simulated data for host builds and CI |

CMake picks the backend: `CMAKE_SYSTEM_NAME STREQUAL "VxWorks"` → real,
otherwise simulated. Every tool response carries `"backend": "vxworks"` or
`"backend": "simulated"` so the two can never be confused.

## Tools

| tool | arguments | returns |
|---|---|---|
| `list_tasks` | `statusFilter` (optional, e.g. `"PEND"`) | all tasks: priority, status, CPU index, stack usage |
| `task_info` | `task` — name, decimal or hex ID | full detail incl. exception stack, entry point |
| `stack_summary` | — | per-task peak stack use %, sorted desc, `HIGH_USAGE` / `LOW_MARGIN` warnings |
| `memory_info` | — | kernel heap: allocated/free/peak, largest free block, fragmentation |
| `cpu_usage` | `seconds` (1–10, default 3), `topN` (default 10) | per-task CPU ticks + percent over a sampling window |
| `system_info` | — | uptime, tick rate, task/module counts, heap summary |
| `list_modules` | — | loaded modules with text/data/bss segments |
| `semaphore_info` | `id` (hex, e.g. `"0x60377000"`) | type, count/full/owner, blocked tasks |
| `msgq_info` | `id` (hex) | queued messages, blocked tasks, timeout counters, limits |

## Run

**Host (simulated backend):**

```bash
cmake --preset dev && cmake --build --preset dev -j
./build/dev/examples/vxworks_monitor_server --port 3003
# then POST JSON-RPC to http://127.0.0.1:3003/mcp
```

**VxWorks DKM** (build with the VxWorks toolchain per docs/vxworks-port.md,
then from the kernel shell):

```
-> ld < vxworks_monitor_server.out
-> mcp_monitor_start 3001        # binds 0.0.0.0:3001, returns the port
value = 3001
-> mcp_monitor_stop              # stops and frees everything
value = 0
```

The DKM entries are **stdio-free**: a kernel image without the ANSI stdio VSB
component has no `__stdioFp`, so any `printf` would fault (see
docs/vxworks-port.md). Status is the return value the shell echoes. For the
same reason the backend never calls `*Show()` routines (`spyReport`,
`semShow`, `msgQShow`, `moduleShow` all print to the console) — everything
comes from programmatic info-get APIs (`taskInfoGet`, `memPartInfoGet`,
`moduleInfoGet`, `semInfoGet`, `msgQInfoGet`).

**VxWorks RTP:**

```
-> rtpSp "/path/vxworks_monitor_server --host 0.0.0.0 --port 3001"
```

## Example session

```bash
BASE=http://<target>:3001/mcp
# initialize — capture the Mcp-Session-Id response header
SID=$(curl -si -X POST $BASE -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}' \
  | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
curl -s -X POST $BASE -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
# who is eating the CPU?
curl -s -X POST $BASE -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"cpu_usage","arguments":{"seconds":3,"topN":5}}}'
```

## CPU sampling: how and limits

spyLib only reports via `printf`, so `cpu_usage` samples itself: a
`taskSwitchHookAdd` hook attributes `tickGet()` deltas to the task being
switched out, for `seconds`, then the hook is removed. Notes:

- **Tick resolution** (`sysClkRateGet()`, typically 60–100 Hz): switches that
  happen within one tick attribute 0 ticks. Short windows undercount busy
  short-lived tasks; `unattributedTicks` is mostly idle time plus that
  rounding.
- **SMP:** a single last-switch timestamp is shared by all cores, so numbers
  are approximate on multi-core targets.
- The handler **blocks its MCP session** for the whole window (a plain sleep —
  no client round-trip, so no deadlock risk). One window at a time; a
  concurrent call returns an error.

## Security

Same posture as `echo_server_http` (FR-TRAN-008): no TLS, bind stays
`127.0.0.1` unless `--host`/DKM start overrides it. The DKM entry binds
`0.0.0.0` for lab use — on anything but an isolated bench network, front it
with a reverse proxy and configure `allowed_origins` / `authorize`. The tools
are read-only introspection (no task control, no memory writes) by design.
