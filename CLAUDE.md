# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

**Phases 1–4 are implemented.** Phase 1 (Foundation): JSON-RPC 2.0 engine, message router, session manager (server + client, full initialize handshake, timeouts/progress/cancellation/ping), stdio transports (server-side and subprocess-spawning client-side). Phase 2 (Server SDK): content model, tool registry, resource provider (RFC 6570 level-1 templates, subscriptions), prompt provider, RFC 5424 logging, completions, JSON Schema subset validator, and the high-level `mcp::Server` facade (include/mcp/server/server.hpp) that auto-derives capabilities from populated registries. Phase 3 (Client SDK): sampling types + multi-turn tool-use loop (`run_sampling_tool_loop`), roots provider, elicitation, and the `mcp::Client` facade (include/mcp/client/client.hpp) with typed synchronous wrappers for the whole server surface. Phase 4 (Streamable HTTP transport, FR-TRAN-005..009): a hand-rolled HTTP/1.1 + SSE codec and POSIX socket helpers (no new dependencies, like stdio), `HttpServerTransport` (single endpoint: POST = JSON-RPC request/response, GET = SSE stream for server-initiated messages, SSE resumability via event IDs backed by a ring buffer, Origin validation → 403, pluggable authorize hook → 401, bind 127.0.0.1 by default, MCP-Protocol-Version header, 404 on wrong path, 400/-32700 on malformed JSON, 406 on GET without `Accept: text/event-stream`), `HttpClientTransport` (POST for requests, auto SSE GET with Last-Event-ID resume + reconnect, Mcp-Session-Id capture/echo + DELETE on disconnect), and `HttpSessionServer` — the production multi-session path: each initialize creates its own ServerSession + SSE channel keyed by a server-assigned `Mcp-Session-Id` (missing id → 400, unknown/expired → 404, DELETE terminates, idle expiry). `Server` supports multiple attached sessions with notification/logging fan-out (`attach`/`detach`); the session factory pattern keeps mcp-transport independent of mcp-server (see examples/echo_server_http.cpp). Internals live in src/transport/http/ (`HttpEndpoint` listener, `SseChannel` per-session stream+replay, `PostCapture`). 140+ GTest suite including real-subprocess integration tests in both stdio and HTTP directions. Phase 5+ (embedded profile, packaging, coroutine API) is not started — see SRS §7 for sequencing.

**HTTP transport security note (FR-TRAN-008):** TLS is intentionally out of scope — deploy behind a reverse proxy that terminates TLS. The server binds 127.0.0.1 by default. Only `localhost` origins are accepted unless `HttpServerOptions::allowed_origins` is populated; use the `authorize` hook for bearer-token / custom auth (return false → 401).

**Platform Abstraction Layer (PAL):** all OS access goes through `mcp::pal` (src/platform/pal.hpp) — descriptor I/O, `WakeEvent` (cross-thread poll unblocking), TCP, child processes, signal setup. Backends are selected at build time in src/CMakeLists.txt (`MCP_PAL_SOURCES`); posix/ (Linux + macOS, CI-verified) is the only backend so far, win32/vxworks/freertos are future additions. **Never include OS headers (`<unistd.h>`, `<sys/*>`, `<poll.h>`, `<csignal>`, sockets) outside src/platform/<backend>/** — the guard-rail is `grep -rln 'include <sys/\|include <unistd\|include <arpa\|include <netinet\|include <poll\|include <csignal' src/` returning only platform backend files. Keep the pal surface narrow: add a function only when a transport needs it and it differs across platforms.

**Concurrency gotcha:** message dispatch is synchronous on the transport read thread. A server request handler must never block waiting on a client round-trip (deadlock) — do that work on a separate thread (see tests/tools/prober_stdio.cpp and the `Server::session()` doc comment).

**`SRS.md` is the authoritative source of truth** for all requirements, architecture, and design decisions. Every requirement has an ID (e.g., `FR-CORE-001`) that should be traceable to implementation and tests.

## What This Project Is

A production-grade, cross-platform C++ SDK implementing the full MCP specification (pinned to protocol version 2025-11-25), targeting embedded/real-time systems (VxWorks, FreeRTOS, bare-metal) and performance-critical applications. No official C++ SDK exists; this fills that gap.

Key constraints from the SRS:
- **C++17 minimum**, C++20 recommended (coroutine API is a later phase)
- Header-only or minimal-link core; zero runtime dependencies in core
- Must support an **embedded profile**: `-fno-exceptions`, `-fno-rtti`, configurable allocator, static allocation (targets: <64KB RAM, <128KB flash for minimal server)
- Dual error handling: exception hierarchy (`McpError` → `JsonRpcError`, `TransportError`, `CapabilityError`, `SessionError`) when exceptions enabled; `Result<T, Error>`/`std::expected` for no-exception builds
- Cross-platform from day one: Linux (GCC/Clang), macOS, Windows (MSVC), ARM embedded — no platform-only APIs

## Planned Architecture (SRS §4–5)

Five CMake library targets, layered:

- **`mcp-core`** — JSON-RPC 2.0 engine, session manager (3-phase lifecycle: initialization → operation → shutdown), capability negotiation, message router (request/notification/response dispatch by method name), progress tracking, cancellation, ping
- **`mcp-transport`** — abstract `Transport` interface; stdio transport (newline-delimited JSON, subprocess management); Streamable HTTP transport (single endpoint, POST + GET/SSE, resumability via event IDs); user-extensible for custom transports
- **`mcp-serialize`** — JSON serialization with pluggable backends (nlohmann/json default, simdjson optional, minimal custom parser for embedded); JSON Schema 2020-12 validation
- **`mcp-server`** — Tool registry, resource provider (incl. RFC 6570 URI templates, subscriptions), prompt provider, logging (RFC 5424 levels), completions
- **`mcp-client`** — Sampling (incl. multi-turn tool-use loop), roots, elicitation

Umbrella target: `mcp::all`. Error codes: standard JSON-RPC (-32700…-32603) plus MCP-specific (-32000…-32006) — see FR-CORE-002/003.

## Build & Test

```bash
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev   # preferred (build/dev/)
ctest --preset dev -R 'ServerSession.Handshake'     # single test (regex on Suite.Name)

cmake -B build -DMCP_BUILD_TESTS=ON   # classic flow still works (fetches nlohmann/json if needed)
cmake --build build -j && ctest --test-dir build --output-on-failure
```

Presets (CMakePresets.json, schema v3/CMake ≥3.21): `dev` (Debug+logging), `debug`, `release`, `ci` (Werror). Per-user overrides belong in `CMakeUserPresets.json` (gitignored).

Options wired so far: `MCP_BUILD_TESTS`, `MCP_BUILD_EXAMPLES` (both default ON at top level), `MCP_USE_EXCEPTIONS` (ON; the OFF path is Phase 5), `MCP_WERROR` (OFF locally, ON in CI), `MCP_ENABLE_LOGGING` (OFF; compiles in the `MCP_LOG` stderr diagnostics — see include/mcp/log.hpp; runtime level via the `MCP_LOG` env var; distinct from the protocol-level `mcp::Logger`. When adding MCP_LOG lines, anything referenced *only* inside them must not trigger unused warnings in OFF builds). Further options planned per FR-BUILD-002: `MCP_BUILD_SERVER`, `MCP_BUILD_CLIENT`, `MCP_USE_ASIO`, `MCP_USE_SIMDJSON`, `MCP_EMBEDDED_PROFILE`.

Library targets: `mcp::core` (engine, sessions, content, schema), `mcp::transport` (stdio + Streamable HTTP), `mcp::server` (tools/resources/prompts/logging/completions + `mcp::Server` facade), `mcp::client` (sampling/roots/elicitation + `mcp::Client` facade; links mcp-server for the shared feature types). Example servers live in examples/ (echo, calculator, echo_server_http).

Dependencies: nlohmann/json 3.11+ (required, FetchContent fallback), GoogleTest (system or FetchContent), Threads. CI (`.github/workflows/ci.yml`) runs ubuntu {gcc, clang} × {Debug, Release} with `-DMCP_WERROR=ON`.

Manual smoke test of the protocol: pipe newline-delimited JSON-RPC into `./build/tests/echo_stdio` (see tests/tools/echo_stdio.cpp), or run `./build/examples/echo_server_http --port 3001` and POST JSON-RPC to `http://127.0.0.1:3001/mcp` (GET that URL with `Accept: text/event-stream` for the SSE stream).

## Code Conventions (established in Phase 1)

- Namespace `mcp`; snake_case API; wire JSON keys stay camelCase (`listChanged`), C++ members snake_case (`list_changed`).
- `std::optional` fields serialize with omit-if-absent semantics via `mcp::detail::set_optional/get_optional` (include/mcp/detail/json_util.hpp) — never emit null for absent fields.
- Handlers return `mcp::Result<json>` (include/mcp/result.hpp); returning an `Error` becomes a JSON-RPC error response. Keep new APIs `Result`-friendly so the future no-exception profile doesn't break them.
- Exception-dependent code is guarded with `#if defined(__cpp_exceptions)`.
- Requirement-relevant code and tests cite their SRS ID (e.g. `// FR-CORE-015`) — keep doing this.
- Tests use `mcp_test::MockTransport` (tests/mock_transport.hpp) for session-level tests; real pipes/subprocesses only in test_stdio.cpp, test_integration.cpp, test_http_transport.cpp (in-process real sockets), and test_http_integration.cpp (spawned echo_server_http).

## Testing Expectations (SRS §5.11)

- Unit tests per component; integration tests for full protocol flows (initialize handshake, tool round-trips over both transports, subscriptions, sampling, reconnection)
- A spec-conformance suite must be built in-house (no official one exists)
- Target: ≥90% line coverage

## Implementation Order (SRS §7)

Phases are sequenced deliberately — respect the dependency order: (1) scaffolding + JSON-RPC engine + core types + stdio transport, (2) server SDK, (3) client SDK, (4) HTTP/SSE transport, (5) embedded profile + packaging + docs, (6) coroutine API and beyond.

## Protocol Reference

The MCP message catalog (all methods and notifications the SDK must handle) is in SRS §10. Spec URLs are in SRS §2; the TypeScript schema at `modelcontextprotocol/specification` is the reference for exact message shapes.
