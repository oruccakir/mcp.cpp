# mcp.cpp

[![CI](https://github.com/oruccakir/mcp.cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/oruccakir/mcp.cpp/actions/workflows/ci.yml)

A production-grade **C++ SDK for the [Model Context Protocol (MCP)](https://modelcontextprotocol.io)** — servers *and* clients, over stdio and Streamable HTTP, with zero runtime dependencies beyond [nlohmann/json](https://github.com/nlohmann/json).

Official MCP SDKs exist for Python and TypeScript; **no official C++ SDK exists**. This project fills that gap, targeting performance-critical applications and (eventually) embedded/real-time systems. Pinned to protocol version **2025-11-25**.

```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::Server server("echo-server", "1.0.0");

    mcp::ToolSpec echo;
    echo.description = "Echoes back the input";
    echo.input_schema = mcp::json{
        {"type", "object"},
        {"properties", {{"message", {{"type", "string"}}}}},
        {"required", mcp::json::array({"message"})}};
    echo.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        return {{mcp::text_content(args.at("message").get<std::string>())}};
    };
    server.register_tool("echo", std::move(echo));

    return server.run(std::make_shared<mcp::StdioTransport>());
}
```

That's a complete MCP server: capabilities are derived automatically from what you register, the initialize handshake, capability gating, pagination, schema validation of tool arguments, and error mapping are all handled by the SDK.

## Features

**Protocol core**
- Full JSON-RPC 2.0 engine: requests, responses, notifications, batches
- Three-phase session lifecycle with version negotiation and capability gating (un-negotiated features are rejected with `-32002`)
- Per-request timeouts that reset on `notifications/progress`, cancellation semantics, built-in `ping`
- Standard JSON-RPC error codes plus the MCP-specific range (`-32000…-32006`)

**Server SDK** (`mcp::Server`)
- **Tools** — registration with name validation, JSON Schema validation of arguments before dispatch, cursor pagination, `isError` result semantics (handler failures are results, not protocol errors)
- **Resources** — static resources and RFC 6570 templates (`file:///{path}` matches nested paths), subscriptions with `notifications/resources/updated`
- **Prompts** — required-argument validation, typed message content
- **Logging** — RFC 5424 levels, client-controlled threshold via `logging/setLevel`
- **Completions** — argument autocompletion for prompts and resource templates
- `list_changed` notifications emitted automatically when registries change — broadcast to every connected session

**Client SDK** (`mcp::Client`)
- Typed synchronous wrappers for the whole server surface: `list_tools()`, `call_tool()`, `read_resource()`, `get_prompt()`, `complete_prompt()`, …
- **Sampling** — plug your LLM in via `set_sampling_handler`; includes `run_sampling_tool_loop`, the multi-turn tool-use loop (server executes `tool_use` blocks and feeds `tool_result` turns back until the model ends its turn)
- **Roots** — `file://` roots with `list_changed` notifications
- **Elicitation** — form (schema-validated) and URL modes
- Client capabilities derived from what you configure, so gating is consistent by construction

**Transports**
- **stdio** — newline-delimited JSON; server side reads stdin on a dedicated thread; client side spawns the server subprocess (argv/env/cwd), captures stderr for logging, and shuts down with the spec's close-stdin → SIGTERM → SIGKILL escalation
- **Streamable HTTP** — hand-rolled HTTP/1.1 + SSE over POSIX sockets (no libcurl, no Asio):
  - `HttpSessionServer` — production multi-session serving: each `initialize` gets its own session keyed by a server-assigned `Mcp-Session-Id` (missing id → 400, unknown/expired → 404, `DELETE` terminates, idle expiry). Multiple clients — e.g. several MCP Inspector tabs — connect concurrently.
  - `HttpServerTransport` — sessionless single-session variant for embedded/simple setups
  - `HttpClientTransport` — POST per request, automatic SSE stream with `Last-Event-ID` resume and reconnect, session-id capture/echo, `DELETE` on disconnect
  - SSE resumability via monotonically increasing event ids backed by a replay ring; messages queued while no stream is attached are delivered exactly once
  - Security per spec: binds `127.0.0.1` by default, Origin allowlist (403), pluggable `authorize` hook (401), `MCP-Protocol-Version` header
- Custom transports: subclass `mcp::Transport` (WebSocket, Unix sockets, shared memory, …)

## Building

Requirements: CMake ≥ 3.20, a C++17 compiler (GCC/Clang tested in CI). nlohmann/json and GoogleTest are found on the system or fetched automatically.

```bash
cmake -B build -DMCP_BUILD_TESTS=ON -DMCP_BUILD_EXAMPLES=ON
cmake --build build -j
ctest --test-dir build --output-on-failure    # 140+ tests
```

CMake targets to link against: `mcp::core`, `mcp::transport`, `mcp::server`, `mcp::client`.

## Usage

### Serving over HTTP (multi-session)

```cpp
mcp::Server server("my-server", "1.0.0");
// ... register tools/resources/prompts ...

mcp::HttpSessionServerOptions options;
options.http.port = 3001;

mcp::HttpSessionServer http(
    options,
    [&server](std::shared_ptr<mcp::Transport> transport) {
        auto session = std::make_unique<mcp::ServerSession>(
            std::move(transport), server.server_options());
        server.attach(*session);
        return session;
    },
    [&server](mcp::ServerSession& session) { server.detach(session); });

std::string error;
http.start(error);   // serves until http.stop()
```

### Writing a client

```cpp
mcp::Client client("my-host", "1.0.0");

// Optional: answer server-initiated sampling with your LLM of choice.
client.set_sampling_handler(
    [](const mcp::CreateMessageParams& params)
        -> mcp::Result<mcp::CreateMessageResult> {
        mcp::CreateMessageResult result;
        result.model = "my-llm";
        result.stop_reason = "endTurn";
        result.content.push_back(mcp::text_content(call_my_llm(params)));
        return result;
    });

// Spawn and talk to a local stdio server...
mcp::StdioServerParameters params;
params.command = "./build/examples/echo_server";
auto init = client.connect(
    std::make_shared<mcp::StdioClientTransport>(params));

// ...or connect to a remote Streamable HTTP server.
// mcp::HttpClientOptions http{.port = 3001};
// auto init = client.connect(std::make_shared<mcp::HttpClientTransport>(http));

auto tools = client.list_tools();
auto result = client.call_tool("echo", {{"message", "merhaba"}});
```

### Resources, prompts, logging

```cpp
mcp::ResourceTemplate files;
files.uri_template = "file:///{path}";
files.name = "Project files";
server.resources().add_resource_template(files);
server.resources().set_read_handler(
    [](const std::string& uri) -> mcp::Result<mcp::ReadResourceResult> {
        mcp::ResourceContents contents;
        contents.uri = uri;
        contents.text = read_file_somehow(uri);
        return mcp::ReadResourceResult{{contents}};
    });

server.logger().info({{"event", "started"}});   // notifications/message
```

## Diagnostic logging

Build with `-DMCP_ENABLE_LOGGING=ON` to compile in the `MCP_LOG` diagnostics (default OFF: zero overhead, statements compile away). Logs go to **stderr** — on the stdio transport stdout is the protocol wire. Runtime verbosity via the `MCP_LOG` env var (`trace|debug|info|warn|error|off`, default `info`):

```
$ MCP_LOG=debug ./echo_server_http --port 3001
[20:19:42.738] [info ] [registry] tool registered: "echo" (1 total)
[20:19:42.739] [info ] [http] listening on 127.0.0.1:3001 (path /mcp)
[20:19:43.147] [info ] [http] session created: 4b9043c4... (1 active)
[20:19:43.147] [info ] [session] initialize: client "log-demo" v1.0 -> protocol 2025-11-25
[20:19:43.157] [debug] [session] --> tools/call (id 2)
[20:19:43.162] [warn ] [http] HTTP 403 Forbidden: origin not allowed
```

Not to be confused with `server.logger()`, which is the MCP *protocol* logging feature (`notifications/message` sent to the connected client). Embedders can redirect the diagnostics with `mcp::set_log_sink`.

## Trying it with MCP Inspector

```bash
# stdio: Inspector spawns the binary itself
npx @modelcontextprotocol/inspector ./build/examples/echo_server

# HTTP: run the server, then connect Inspector to the URL
./build/examples/echo_server_http --port 3001
npx @modelcontextprotocol/inspector    # transport: Streamable HTTP, URL: http://127.0.0.1:3001/mcp
```

Multiple Inspector tabs can connect at once — each gets its own `Mcp-Session-Id`.

## Project layout

```
include/mcp/          public headers (umbrella: <mcp/mcp.hpp>)
  core/               router, sessions, progress, cancellation
  jsonrpc/            message types, parse/serialize
  server/             tools, resources, prompts, logging, completion, Server
  client/             sampling, roots, elicitation, Client
  transport/          Transport, stdio, Streamable HTTP (+ session server)
src/                  library sources (internal HTTP/SSE codec in src/transport/http/)
examples/             echo_server, calculator_server, echo_server_http
tests/                140+ GTest suite incl. real-subprocess integration tests
SRS.md                the authoritative requirements spec (FR-* ids traceable to code/tests)
```

## Security notes (Streamable HTTP)

TLS is intentionally out of scope — deploy behind a reverse proxy that terminates TLS. The server binds `127.0.0.1` by default; only localhost origins are accepted unless `allowed_origins` is set, and the `authorize` hook supports bearer-token/custom auth. See FR-TRAN-008 in `SRS.md`.

## Status & roadmap

Implemented (SRS phases 1–4): protocol core, server SDK, client SDK, stdio + Streamable HTTP transports with `Mcp-Session-Id` session management.

Planned (phase 5+): embedded profile (`-fno-exceptions`, `-fno-rtti`, custom allocators — the `Result<T,E>` API is already in place for it), Windows support, vcpkg/Conan packaging, full JSON Schema 2020-12 validation, spec-conformance suite, C++20 coroutine API.

## Design notes

- Wire JSON uses camelCase per the MCP spec; the C++ API is snake_case. Absent optionals are omitted, never `null`.
- Handlers return `mcp::Result<T>`; returning an `mcp::Error` becomes a JSON-RPC error response. Exceptions are supported but never required.
- Message dispatch is synchronous on the transport read thread: never block a request handler waiting on a round-trip to the peer — do that work on a separate thread (see `tests/tools/prober_stdio.cpp`).
- `SRS.md` is the source of truth; requirement ids (e.g. `FR-TRAN-009`) are cited at their implementation and test sites.
