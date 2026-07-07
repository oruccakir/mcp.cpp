# mcp.cpp

A production-grade, cross-platform C++ SDK implementing the full
[Model Context Protocol](https://modelcontextprotocol.io/) (pinned to protocol
version **2025-11-25**), targeting embedded/real-time systems (VxWorks,
FreeRTOS, bare-metal) and performance-critical applications. No official C++
SDK exists; this fills that gap.

`SRS.md` is the authoritative source of truth for requirements, architecture,
and design decisions — every requirement has an ID (e.g. `FR-CORE-001`)
traceable to the implementation and tests. See `CLAUDE.md` for contributor
notes and conventions.

## Status

Phases 1–4 are implemented; Phase 5+ (embedded profile, packaging, coroutine
API) is not started — see `SRS.md` §7 for sequencing.

| Phase | Scope |
|------|-------|
| 1 — Foundation | JSON-RPC 2.0 engine, message router, session manager (server + client, full initialize handshake, timeouts/progress/cancellation/ping), stdio transports |
| 2 — Server SDK | content model, tool registry, resource provider (RFC 6570 URI templates, subscriptions), prompt provider, RFC 5424 logging, completions, JSON Schema subset validator, `mcp::Server` facade |
| 3 — Client SDK | sampling (multi-turn tool-use loop), roots provider, elicitation, `mcp::Client` facade with typed synchronous wrappers |
| 4 — Streamable HTTP | hand-rolled HTTP/1.1 + SSE codec, `HttpServerTransport` + `HttpClientTransport`, `echo_server_http` example |

## Requirements

- **C++17 minimum** (C++20 recommended)
- CMake ≥ 3.16
- nlohmann/json ≥ 3.11 (fetched automatically if not system-installed)
- GoogleTest (fetched automatically if not system-installed)
- A POSIX platform for the stdio and HTTP transports (Linux / macOS now;
  Windows sockets are a later phase)

## Build

```bash
cmake -B build -DMCP_BUILD_TESTS=ON -DMCP_BUILD_EXAMPLES=ON
cmake --build build -j
```

Common CMake options:

| Option | Default | Purpose |
|--------|---------|---------|
| `MCP_BUILD_TESTS` | `ON` (top level) | Build the GoogleTest suite |
| `MCP_BUILD_EXAMPLES` | `ON` (top level) | Build the example servers |
| `MCP_USE_EXCEPTIONS` | `ON` | Exception hierarchy (`OFF` → `Result<T,Error>` path, Phase 5) |
| `MCP_WERROR` | `OFF` locally, `ON` in CI | Treat warnings as errors |

### Library targets

- `mcp::core` — JSON-RPC engine, sessions, content model, JSON Schema validator
- `mcp::transport` — stdio + Streamable HTTP transports
- `mcp::server` — tools / resources / prompts / logging / completions + `mcp::Server` facade
- `mcp::client` — sampling / roots / elicitation + `mcp::Client` facade
  (links `mcp-server` for the shared feature types)
- `mcp::all` — umbrella

## Run

### Example: stdio echo server

Pipe newline-delimited JSON-RPC into a stdio server:

```bash
./build/tests/echo_stdio          # source: tests/tools/echo_stdio.cpp
# or the calculator:
./build/examples/calculator_server
```

```bash
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cli","version":"0"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi"}}}' \
  | ./build/tests/echo_stdio
```

### Example: Streamable HTTP echo server

```bash
./build/examples/echo_server_http --port 3001
# listening on http://127.0.0.1:3001/mcp
```

Then from another shell:

```bash
# initialize
curl -s -X POST localhost:3001/mcp -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{"tools":{"listChanged":true}},"clientInfo":{"name":"cli","version":"0"}}}'

# notifications/initialized has no id -> server answers 202
curl -s -o /dev/null -w '%{http_code}\n' -X POST localhost:3001/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

# tools/call echo
curl -s -X POST localhost:3001/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi-from-curl"}}}'

# server-initiated messages stream over SSE (use `Accept: text/event-stream`)
curl -N localhost:3001/mcp -H 'Accept: text/event-stream'
```

**Security:** the HTTP server binds `127.0.0.1` by default. TLS is out of
scope — deploy behind a reverse proxy that terminates TLS. Only `localhost`
origins are accepted unless `HttpServerOptions::allowed_origins` is populated;
use the `authorize` hook for bearer-token / custom auth (return false → 401).

## Test

The suite is a single GoogleTest binary (`mcp_tests`) driven by CTest,
covering unit, in-process transport (real sockets on ephemeral ports), and
end-to-end (spawned subprocess) tests in both the stdio and HTTP directions.

```bash
ctest --test-dir build --output-on-failure          # all tests
ctest --test-dir build -j4                           # parallel
ctest --test-dir build -R 'HttpTransport'            # by suite/regex
ctest --test-dir build -R 'HttpIntegration'          # spawned echo_server_http
ctest --test-dir build -R 'ServerSession.Handshake'  # single test
```

Run the binary directly for finer control (filtering, repetition):

```bash
./build/tests/mcp_tests --gtest_filter='Http*'
./build/tests/mcp_tests --gtest_repeat=10 --gtest_break_on_failure
```

Integration tests spawn helper binaries whose paths are baked in at configure
time (`ECHO_STDIO_PATH`, `TOOLBOX_STDIO_PATH`, `PROBER_STDIO_PATH`,
`ECHO_SERVER_HTTP_PATH`), so the build must succeed before CTest can run them.

## Project layout

```
include/mcp/         public headers (umbrella: mcp.hpp)
  core/              JSON-RPC engine, sessions, content, schema
  transport/         Transport interface, stdio + Streamable HTTP transports
  server/            tool registry, resource/prompt providers, logging, Server facade
  client/            sampling, roots, elicitation, Client facade
src/                 implementation (+ http codec & socket utils under transport/http/)
examples/            echo_server, calculator_server, echo_server_http
tests/               GoogleTest suite + helper binaries (tools/)
SRS.md               requirements & architecture (authoritative)
CLAUDE.md            contributor notes & conventions
```