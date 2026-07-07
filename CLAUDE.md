# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

**Phases 1–3 are implemented.** Phase 1 (Foundation): JSON-RPC 2.0 engine, message router, session manager (server + client, full initialize handshake, timeouts/progress/cancellation/ping), stdio transports (server-side and subprocess-spawning client-side). Phase 2 (Server SDK): content model, tool registry, resource provider (RFC 6570 level-1 templates, subscriptions), prompt provider, RFC 5424 logging, completions, JSON Schema subset validator, and the high-level `mcp::Server` facade (include/mcp/server/server.hpp) that auto-derives capabilities from populated registries. Phase 3 (Client SDK): sampling types + multi-turn tool-use loop (`run_sampling_tool_loop`), roots provider, elicitation, and the `mcp::Client` facade (include/mcp/client/client.hpp) with typed synchronous wrappers for the whole server surface. 110+ GTest suite including real-subprocess integration tests in both directions. Phases 4+ (HTTP transport, embedded profile) are not started — see SRS §7 for sequencing.

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
cmake -B build -DMCP_BUILD_TESTS=ON   # configure (fetches nlohmann/json if not system-installed)
cmake --build build -j                # build
ctest --test-dir build --output-on-failure          # all tests
ctest --test-dir build -R 'ServerSession.Handshake' # single test (regex on Suite.Name)
```

Options wired so far: `MCP_BUILD_TESTS`, `MCP_BUILD_EXAMPLES` (both default ON at top level), `MCP_USE_EXCEPTIONS` (ON; the OFF path is Phase 5), `MCP_WERROR` (OFF locally, ON in CI). Further options planned per FR-BUILD-002: `MCP_BUILD_SERVER`, `MCP_BUILD_CLIENT`, `MCP_USE_ASIO`, `MCP_USE_SIMDJSON`, `MCP_EMBEDDED_PROFILE`.

Library targets: `mcp::core` (engine, sessions, content, schema), `mcp::transport` (stdio), `mcp::server` (tools/resources/prompts/logging/completions + `mcp::Server` facade), `mcp::client` (sampling/roots/elicitation + `mcp::Client` facade; links mcp-server for the shared feature types). Example servers live in examples/ (echo, calculator).

Dependencies: nlohmann/json 3.11+ (required, FetchContent fallback), GoogleTest (system or FetchContent), Threads. CI (`.github/workflows/ci.yml`) runs ubuntu {gcc, clang} × {Debug, Release} with `-DMCP_WERROR=ON`.

Manual smoke test of the protocol: pipe newline-delimited JSON-RPC into `./build/tests/echo_stdio` (see tests/tools/echo_stdio.cpp).

## Code Conventions (established in Phase 1)

- Namespace `mcp`; snake_case API; wire JSON keys stay camelCase (`listChanged`), C++ members snake_case (`list_changed`).
- `std::optional` fields serialize with omit-if-absent semantics via `mcp::detail::set_optional/get_optional` (include/mcp/detail/json_util.hpp) — never emit null for absent fields.
- Handlers return `mcp::Result<json>` (include/mcp/result.hpp); returning an `Error` becomes a JSON-RPC error response. Keep new APIs `Result`-friendly so the future no-exception profile doesn't break them.
- Exception-dependent code is guarded with `#if defined(__cpp_exceptions)`.
- Requirement-relevant code and tests cite their SRS ID (e.g. `// FR-CORE-015`) — keep doing this.
- Tests use `mcp_test::MockTransport` (tests/mock_transport.hpp) for session-level tests; real pipes/subprocesses only in test_stdio.cpp and test_integration.cpp.

## Testing Expectations (SRS §5.11)

- Unit tests per component; integration tests for full protocol flows (initialize handshake, tool round-trips over both transports, subscriptions, sampling, reconnection)
- A spec-conformance suite must be built in-house (no official one exists)
- Target: ≥90% line coverage

## Implementation Order (SRS §7)

Phases are sequenced deliberately — respect the dependency order: (1) scaffolding + JSON-RPC engine + core types + stdio transport, (2) server SDK, (3) client SDK, (4) HTTP/SSE transport, (5) embedded profile + packaging + docs, (6) coroutine API and beyond.

## Protocol Reference

The MCP message catalog (all methods and notifications the SDK must handle) is in SRS §10. Spec URLs are in SRS §2; the TypeScript schema at `modelcontextprotocol/specification` is the reference for exact message shapes.
