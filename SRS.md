# Software Requirements Document — Official MCP C++ SDK

**Project:** Model Context Protocol (MCP) C++ Software Development Kit
**Author:** Oruç Çakir
**Date:** 2026-07-05
**Status:** Draft / Long-Horizon Planning
**Version:** 0.1.0

---

## 1. Executive Summary

The Model Context Protocol (MCP) is an open standard for connecting AI applications to external systems — data sources, tools, and workflows. While official SDKs exist for Python (`mcp`) and TypeScript (`@modelcontextprotocol/sdk`), **no official C++ SDK exists**. This project aims to fill that gap by providing a production-grade, cross-platform C++ SDK that implements the full MCP specification.

The SDK targets two primary audiences:
- **Embedded & real-time developers** (VxWorks, FreeRTOS, bare-metal) who cannot run Python/JS runtimes
- **Performance-critical applications** (EW systems, radar processing, high-frequency trading, game engines) that require zero-GC, deterministic latency

---

## 2. References

| Reference | Source |
|-----------|--------|
| MCP Specification (Latest) | https://modelcontextprotocol.io/specification/latest |
| MCP Architecture | https://modelcontextprotocol.io/specification/latest/architecture |
| MCP Base Protocol | https://modelcontextprotocol.io/specification/latest/basic |
| MCP Transports | https://modelcontextprotocol.io/specification/latest/basic/transports |
| MCP Lifecycle | https://modelcontextprotocol.io/specification/latest/basic/lifecycle |
| MCP Messages | https://modelcontextprotocol.io/specification/latest/basic/messages |
| MCP Utilities | https://modelcontextprotocol.io/specification/latest/basic/utilities |
| MCP Server Tools | https://modelcontextprotocol.io/specification/latest/server/tools |
| MCP Server Resources | https://modelcontextprotocol.io/specification/latest/server/resources |
| MCP Server Prompts | https://modelcontextprotocol.io/specification/latest/server/prompts |
| MCP Client Sampling | https://modelcontextprotocol.io/specification/latest/client/sampling |
| MCP Client Roots | https://modelcontextprotocol.io/specification/latest/client/roots |
| JSON-RPC 2.0 Specification | https://www.jsonrpc.org/specification |
| JSON Schema 2020-12 | https://json-schema.org/specification |
| TypeScript Schema (Reference) | https://github.com/modelcontextprotocol/specification/blob/main/schema.ts |

---

## 3. Design Goals & Non-Goals

### 3.1 Goals

| Priority | Goal | Rationale |
|----------|------|-----------|
| P0 | **Full MCP spec compliance** (protocol version 2025-11-25) | Interoperability with all MCP clients/servers |
| P0 | **C++17 minimum** (C++20 recommended) | Broad compiler support; C++20 for `std::format`, concepts, coroutines |
| P0 | **Dual transport: stdio + Streamable HTTP** | Required by spec; stdio for local, HTTP for remote |
| P0 | **Header-only or minimal-link core** | Embedded-friendly; zero runtime dependencies in core |
| P1 | **JSON-RPC 2.0 engine** | Message serialization, deserialization, request/response/notification routing |
| P1 | **Server SDK** | Tools, Resources, Prompts, Logging, Completions |
| P1 | **Client SDK** | Sampling, Roots, Elicitation |
| P1 | **Capability negotiation** | Dynamic feature discovery per spec lifecycle |
| P1 | **CMake build system** | Industry standard for C++ |
| P2 | **Optional async I/O** (Boost.Asio / standalone Asio / libuv) | For high-concurrency HTTP servers |
| P2 | **JSON Schema validation** | Tool input validation at registration time |
| P2 | **Comprehensive test suite** | Unit tests, integration tests, spec conformance tests |
| P2 | **Package manager support** | vcpkg, Conan |
| P3 | **Coroutine-based API** (C++20) | Clean async code without callback hell |
| P3 | **Embedded profile** | No exceptions, no RTTI, minimal memory allocation |
| P3 | **Documentation site** | Docusaurus or mdBook |

### 3.2 Non-Goals

- Not a replacement for Python/TS SDKs in their primary use cases
- No built-in LLM inference — sampling delegates to the host client
- No GUI or dashboard — this is a library, not an application
- No Windows-only APIs — must be cross-platform from day one

---

## 4. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    MCP C++ SDK                          │
│                                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │              Core Library (mcp-core)              │   │
│  │  ┌──────────┐  ┌──────────┐  ┌────────────────┐ │   │
│  │  │ JSON-RPC │  │ Session  │  │ Capability      │ │   │
│  │  │ Engine   │  │ Manager  │  │ Negotiation     │ │   │
│  │  └──────────┘  └──────────┘  └────────────────┘ │   │
│  │  ┌──────────┐  ┌──────────┐  ┌────────────────┐ │   │
│  │  │ Message  │  │ Progress │  │ Cancellation   │ │   │
│  │  │ Router   │  │ Tracker  │  │ Handler        │ │   │
│  │  └──────────┘  └──────────┘  └────────────────┘ │   │
│  └──────────────────────────────────────────────────┘   │
│                                                         │
│  ┌──────────────────┐  ┌────────────────────────────┐   │
│  │  Transport Layer  │  │     Serialization Layer    │   │
│  │  (mcp-transport)  │  │      (mcp-serialize)       │   │
│  │                   │  │                            │   │
│  │  • stdio          │  │  • JSON (nlohmann / simdjson)│  │
│  │  • Streamable HTTP│  │  • JSON Schema validation  │   │
│  │  • Custom (extend)│  │  • Binary format (future)  │   │
│  └──────────────────┘  └────────────────────────────┘   │
│                                                         │
│  ┌──────────────────────┐  ┌────────────────────────┐   │
│  │    Server SDK         │  │     Client SDK          │   │
│  │    (mcp-server)       │  │     (mcp-client)        │   │
│  │                       │  │                         │   │
│  │  • Tool Registry      │  │  • Sampling Requester   │   │
│  │  • Resource Provider  │  │  • Roots Provider       │   │
│  │  • Prompt Templates   │  │  • Elicitation Handler  │   │
│  │  • Logging Emitter    │  │  • Task Support         │   │
│  │  • Completion Helper  │  │                         │   │
│  └──────────────────────┘  └────────────────────────┘   │
│                                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │           Examples & Integration Tests            │   │
│  │  • echo-server  • file-server  • calculator      │   │
│  │  • chat-client  • spec-conformance-suite         │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

---

## 5. Detailed Requirements

### 5.1 Core Library (`mcp-core`)

#### 5.1.1 JSON-RPC 2.0 Engine

**FR-CORE-001:** The SDK MUST implement a full JSON-RPC 2.0 message layer supporting:
- **Requests** — method calls with unique `id`, `method` string, and optional `params`
- **Responses** — successful results with `result` field, or errors with `error` object (code, message, optional data)
- **Notifications** — fire-and-forget messages with `method` and optional `params`, no `id`
- **Batch** — array of requests/notifications sent together, responses returned as array

**FR-CORE-002:** Standard JSON-RPC error codes MUST be defined:
| Code | Meaning |
|------|---------|
| `-32700` | Parse error |
| `-32600` | Invalid Request |
| `-32601` | Method not found |
| `-32602` | Invalid params |
| `-32603` | Internal error |
| `-32000` to `-32099` | Server error (reserved) |

**FR-CORE-003:** MCP-specific error codes MUST be defined:
| Code | Meaning |
|------|---------|
| `-32000` | Connection not initialized |
| `-32001` | Request already in progress |
| `-32002` | Capability not supported |
| `-32003` | Resource not found |
| `-32004` | Tool execution failed |
| `-32005` | Invalid URI |
| `-32006` | Pagination error |

**FR-CORE-004:** The engine MUST support configurable timeouts per-request with progress-notification reset.

#### 5.1.2 Session Manager

**FR-CORE-005:** The SDK MUST manage a stateful session per connection with three lifecycle phases:
1. **Initialization** — capability negotiation, version handshake
2. **Operation** — active message exchange within negotiated capabilities
3. **Shutdown** — clean transport termination

**FR-CORE-006:** The session MUST enforce protocol version compatibility:
- Client sends `protocolVersion` in `initialize` request
- Server responds with its supported version
- If versions mismatch and are incompatible, session MUST reject

**FR-CORE-007:** The session MUST track negotiated capabilities and reject any request for an un-negotiated capability with error code `-32002`.

#### 5.1.3 Capability Negotiation

**FR-CORE-008:** Server capabilities that MUST be representable:
```cpp
struct ServerCapabilities {
    std::optional<LoggingCapability> logging;
    std::optional<PromptsCapability> prompts;     // listChanged
    std::optional<ResourcesCapability> resources; // subscribe, listChanged
    std::optional<ToolsCapability> tools;         // listChanged
    std::optional<CompletionsCapability> completions;
    std::optional<TasksCapability> tasks;
    std::optional<ExperimentalCapability> experimental;
};
```

**FR-CORE-009:** Client capabilities that MUST be representable:
```cpp
struct ClientCapabilities {
    std::optional<RootsCapability> roots;         // listChanged
    std::optional<SamplingCapability> sampling;   // tools, context
    std::optional<ElicitationCapability> elicitation; // form, url
    std::optional<TasksCapability> tasks;
    std::optional<ExperimentalCapability> experimental;
};
```

**FR-CORE-010:** The `initialize` handshake MUST follow this exact sequence:
1. Client → Server: `initialize` request (version + capabilities + clientInfo)
2. Server → Client: `initialize` response (version + capabilities + serverInfo + optional instructions)
3. Client → Server: `notifications/initialized` notification

#### 5.1.4 Message Router

**FR-CORE-011:** The SDK MUST provide a message routing system that dispatches incoming JSON-RPC messages to registered handlers based on method name.

**FR-CORE-012:** The router MUST support three dispatch categories:
- **Request handlers** — receive params, return result or error
- **Notification handlers** — receive params, fire-and-forget
- **Response handlers** — match responses to pending requests by `id`

**FR-CORE-013:** The router MUST support handler registration with wildcard patterns for experimental/extension methods.

#### 5.1.5 Progress Tracking

**FR-CORE-014:** The SDK MUST support progress notifications per the spec:
- Sender includes `progressToken` in request params
- Receiver sends `notifications/progress` with `progressToken`, `progress`, and `total` (optional)
- Progress MUST reset timeout clocks on the receiving end

#### 5.1.6 Cancellation

**FR-CORE-015:** The SDK MUST support cancellation via `notifications/cancelled`:
- Contains `requestId` (the request to cancel) and optional `reason`
- MUST NOT cancel the `initialize` request
- Receiver SHOULD stop processing and free resources
- Race conditions (cancellation arriving after completion) MUST be handled gracefully

**FR-CORE-016:** For task-augmented requests, `tasks/cancel` MUST be used instead of `notifications/cancelled`.

#### 5.1.7 Ping

**FR-CORE-017:** The SDK MUST support `ping` requests (no params) with an empty response `{}` for health checking.

---

### 5.2 Transport Layer (`mcp-transport`)

#### 5.2.1 Transport Interface

**FR-TRAN-001:** The SDK MUST define an abstract transport interface:
```cpp
class Transport {
public:
    virtual ~Transport() = default;
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void send(Message message) = 0;
    virtual void setMessageHandler(std::function<void(Message)>) = 0;
    virtual void setErrorHandler(std::function<void(Error)>) = 0;
    virtual void setCloseHandler(std::function<void()>) = 0;
};
```

#### 5.2.2 stdio Transport

**FR-TRAN-002:** The stdio transport MUST:
- Launch the MCP server as a subprocess
- Read newline-delimited JSON from `stdin`
- Write newline-delimited JSON to `stdout`
- Capture UTF-8 logs from `stderr` (informational only)
- Support configurable process launch (command, args, environment, working directory)

**FR-TRAN-003:** Messages on stdio MUST be newline-delimited and MUST NOT contain embedded newlines.

**FR-TRAN-004:** On shutdown, the client SHOULD: close stdin → wait for process exit → SIGTERM → SIGKILL (escalating).

#### 5.2.3 Streamable HTTP Transport

**FR-TRAN-005:** The Streamable HTTP transport MUST:
- Expose a single HTTP endpoint supporting both POST and GET
- POST: client sends JSON-RPC messages to the server
- GET: client opens an SSE stream to receive server-initiated messages
- Support `Accept: application/json` and `Accept: text/event-stream` content negotiation

**FR-TRAN-006:** POST response handling:
| Input Type | Server Response |
|------------|----------------|
| Response or notification | HTTP 202 Accepted, no body |
| Request (non-streaming) | HTTP 200, `Content-Type: application/json`, single JSON-RPC response |
| Request (streaming) | HTTP 200, `Content-Type: text/event-stream`, SSE stream |

**FR-TRAN-007:** SSE stream behavior:
- Server SHOULD send an initial event with event ID and empty `data` (for reconnection)
- Server MAY close the connection at any time (client reconnects with `Last-Event-ID`)
- Server SHOULD send `retry` field before closing
- Server MUST send each JSON-RPC message on only one connected stream
- Client MAY maintain multiple SSE streams simultaneously

**FR-TRAN-008:** Security requirements:
- Validate `Origin` header — respond HTTP 403 if invalid
- Bind to `127.0.0.1` only when running locally
- Implement proper authentication for all connections
- Include `MCP-Protocol-Version` header on all requests

**FR-TRAN-009:** The transport MUST support resumability with globally unique SSE event IDs.

#### 5.2.4 Custom Transport Extension

**FR-TRAN-010:** The SDK MUST allow users to implement custom transports by subclassing the `Transport` interface, enabling WebSocket, Unix sockets, shared memory, or other transports.

---

### 5.3 Serialization Layer (`mcp-serialize`)

**FR-SER-001:** The SDK MUST serialize/deserialize all MCP message types to/from JSON.

**FR-SER-002:** The SDK MUST support the following content types in messages:
- `text` — plain text content
- `image` — base64-encoded image with MIME type
- `audio` — base64-encoded audio with MIME type
- `resource` — embedded resource with URI, MIME type, and text or blob data
- `tool_use` — tool invocation with id, name, and input

**FR-SER-003:** The SDK MUST support JSON Schema 2020-12 for tool `inputSchema` validation.

**FR-SER-004:** The SDK SHOULD support pluggable JSON backends:
- Default: nlohmann/json (header-only, broad compatibility)
- Optional: simdjson (performance-critical paths)
- Embedded: minimal custom JSON parser (no exceptions, no dynamic allocation)

**FR-SER-005:** All URIs in the protocol MUST be validated for correctness.

---

### 5.4 Server SDK (`mcp-server`)

#### 5.4.1 Tool System

**FR-SRV-001:** The SDK MUST provide a tool registration API:
```cpp
class ToolRegistry {
public:
    void register_tool(Tool tool);
    std::vector<Tool> list_tools(std::optional<std::string> cursor);
    CallToolResult call_tool(const std::string& name, const json& arguments);
};
```

**FR-SRV-002:** Tool definition structure:
```cpp
struct Tool {
    std::string name;                    // 1-128 chars, [A-Za-z0-9._-]
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::vector<Icon>> icons;
    json inputSchema;                    // JSON Schema 2020-12
    std::optional<json> outputSchema;
    std::optional<Annotations> annotations;
    std::optional<ExecutionConfig> execution;
};
```

**FR-SRV-003:** Tool name validation: 1–128 chars, `[A-Za-z0-9._-]`, no spaces/commas, unique per server.

**FR-SRV-004:** The SDK MUST implement `tools/list` with pagination support via `cursor`.

**FR-SRV-005:** The SDK MUST implement `tools/call` returning:
```cpp
struct CallToolResult {
    std::vector<Content> content;
    bool isError = false;
};
```

**FR-SRV-006:** If `listChanged` capability is declared, emit `notifications/tools/list_changed` on tool list changes.

**FR-SRV-007:** Tool handlers MUST be invocable as `std::function<CallToolResult(const json&)>`.

#### 5.4.2 Resource System

**FR-SRV-008:** The SDK MUST provide a resource registration API:
```cpp
class ResourceProvider {
public:
    void add_resource(Resource resource);
    void add_resource_template(ResourceTemplate tmpl);
    std::vector<Resource> list_resources(std::optional<std::string> cursor);
    std::vector<ResourceTemplate> list_resource_templates();
    ReadResourceResult read_resource(const std::string& uri);
};
```

**FR-SRV-009:** Resource definition with `uri`, `name`, optional `title`, `description`, `mimeType`, `icons`, `annotations`.

**FR-SRV-010:** Resource template with `uriTemplate` (RFC 6570), `name`, optional metadata.

**FR-SRV-011:** Implement `resources/list`, `resources/read`, `resources/templates/list` with pagination.

**FR-SRV-012:** If `subscribe` declared, support `resources/subscribe` and emit `notifications/resources/updated`.

**FR-SRV-013:** If `listChanged` declared, emit `notifications/resources/list_changed`.

#### 5.4.3 Prompt System

**FR-SRV-014:** The SDK MUST provide a prompt registration API:
```cpp
class PromptProvider {
public:
    void add_prompt(Prompt prompt);
    std::vector<Prompt> list_prompts(std::optional<std::string> cursor);
    GetPromptResult get_prompt(const std::string& name, const json& arguments);
};
```

**FR-SRV-015:** Prompt with `name`, optional `title`, `description`, `icons`, `arguments`.

**FR-SRV-016:** Prompt message content types: `text`, `image`, `audio`, `resource`.

**FR-SRV-017:** Implement `prompts/list` with pagination and `prompts/get`.

**FR-SRV-018:** If `listChanged` declared, emit `notifications/prompts/list_changed`.

#### 5.4.4 Logging System

**FR-SRV-019:** Structured logging with levels: Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency.

**FR-SRV-020:** Implement `logging/setLevel` for client-controlled verbosity.

**FR-SRV-021:** Emit `notifications/message` for each log event at or above configured level.

#### 5.4.5 Completion System

**FR-SRV-022:** Support argument autocompletion for prompts and resource templates.

**FR-SRV-023:** Implement `completion/complete` with `ref/prompt` and `ref/resource` reference types.

---

### 5.5 Client SDK (`mcp-client`)

#### 5.5.1 Sampling

**FR-CLI-001:** Implement `sampling/createMessage` request handler with messages, model preferences, system prompt, max tokens, tools, tool choice, and metadata.

**FR-CLI-002:** Support multi-turn tool-use sampling loop (server → LLM with tools → tool_use response → server executes → server sends new request with results → loop until endTurn).

**FR-CLI-003:** Model preferences with `hints`, `intelligencePriority` (0.0–1.0), `speedPriority` (0.0–1.0).

#### 5.5.2 Roots

**FR-CLI-004:** Implement `roots/list` returning `Root` objects with `uri` (file://) and optional `name`.

**FR-CLI-005:** If `listChanged` declared, emit `notifications/roots/list_changed`.

#### 5.5.3 Elicitation

**FR-CLI-006:** Support server-initiated elicitation requests with title, description, form (text, select, boolean, file), or URL.

---

### 5.6 Task System (Optional Extension)

**FR-TASK-001:** Support `tasks/list`, `tasks/cancel` (returns final state), `tasks/send`.

**FR-TASK-002:** Task cancellation returns final state (unlike fire-and-forget `notifications/cancelled`).

---

### 5.7 Error Handling

**FR-ERR-001:** All errors propagate through JSON-RPC error mechanism.

**FR-ERR-002:** C++ exception hierarchy (when enabled): `McpError` → `JsonRpcError`, `TransportError`, `CapabilityError`, `SessionError`.

**FR-ERR-003:** For embedded/no-exception builds, use `std::expected<T, Error>` or `Result<T, Error>`.

**FR-ERR-004:** Transport errors trigger error handler and optionally close handler.

---

### 5.8 Concurrency & Thread Safety

**FR-CONC-001:** Core library thread-safe for concurrent message sending, handler registration, session state reads.

**FR-CONC-002:** Document thread-safety guarantees per component.

**FR-CONC-003:** Optional async API using C++20 coroutines or Boost.Asio.

**FR-CONC-004:** stdio transport uses dedicated read thread or async I/O.

---

### 5.9 Embedded Profile

**FR-EMB-001:** Embedded build with `-fno-exceptions`, `-fno-rtti`, configurable allocator, minimal JSON parser, static allocation options.

**FR-EMB-002:** Target ARM Cortex-M/M4/M7 (FreeRTOS), ARM Cortex-A (VxWorks, Linux), x86_64.

**FR-EMB-003:** Configurable buffer sizes for constrained environments.

---

### 5.10 Build System & Packaging

**FR-BUILD-001:** CMake 3.20+ with targets: `mcp-core`, `mcp-transport`, `mcp-serialize`, `mcp-server`, `mcp-client`, `mcp::all`.

**FR-BUILD-002:** CMake options: `MCP_BUILD_SERVER`, `MCP_BUILD_CLIENT`, `MCP_BUILD_TESTS`, `MCP_BUILD_EXAMPLES`, `MCP_USE_EXCEPTIONS`, `MCP_USE_RTTI`, `MCP_USE_ASIO`, `MCP_USE_SIMDJSON`, `MCP_EMBEDDED_PROFILE`.

**FR-BUILD-003:** Support vcpkg, Conan, manual CMake install.

**FR-BUILD-004:** Dependencies: nlohmann/json 3.11+ (header-only, required), Boost.Asio (optional), libcurl (optional), simdjson (optional).

---

### 5.11 Testing Requirements

**FR-TEST-001:** Unit tests for JSON-RPC, session lifecycle, capability negotiation, tool/resource/prompt operations, progress/cancellation, error handling, transport edge cases.

**FR-TEST-002:** Integration tests for full initialize handshake, tool call round-trip (stdio + HTTP), resource read with subscription, sampling, concurrent messages, reconnection.

**FR-TEST-003:** Spec conformance tests against official MCP spec.

**FR-TEST-004:** ≥90% line coverage.

---

### 5.12 Documentation Requirements

**FR-DOC-001:** Doxygen API reference from source comments.

**FR-DOC-002:** Getting started guide, architecture overview, server/client tutorials, transport configuration, embedded deployment guide, API reference, migration guide.

**FR-DOC-003:** Every public API has a documented example.

---

## 6. Example Applications

### 6.1 Echo Server
```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::Server server("echo-server", "1.0.0");
    server.register_tool("echo", {
        .description = "Echoes back the input",
        .inputSchema = {{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}, {"required", {"message"}}},
        .handler = [](const json& args) -> mcp::CallToolResult {
            return {{mcp::text_content(args["message"])}};
        }
    });
    server.run(mcp::StdioTransport());
}
```

### 6.2 Calculator Server
```cpp
server.register_tool("calculate", {
    .description = "Evaluate a mathematical expression",
    .inputSchema = {{"type", "object"}, {"properties", {{"expression", {{"type", "string"}}}}}, {"required", {"expression"}}},
    .handler = [](const json& args) -> mcp::CallToolResult {
        try {
            return {{mcp::text_content(std::to_string(evaluate(args["expression"])))}};
        } catch (...) {
            return {{mcp::text_content("Evaluation failed")}, .isError = true};
        }
    }
});
```

### 6.3 File Resource Server
```cpp
mcp::ResourceProvider resources;
resources.add_resource_template({
    .uriTemplate = "file:///{path}",
    .name = "Project Files",
    .mimeType = "application/octet-stream"
});
resources.set_read_handler([](const std::string& uri) {
    auto content = read_file(parse_uri_template(uri));
    return mcp::ReadResourceResult{{{
        .uri = uri, .mimeType = detect_mime(uri), .text = content
    }}};
});
```

---

## 7. Implementation Phases

### Phase 1: Foundation (Weeks 1–4)
- Project scaffolding (CMake, directory structure, CI)
- JSON-RPC 2.0 engine (message types, serialization, routing)
- Core types (capabilities, session state, error codes)
- stdio transport
- Unit tests for core + stdio

### Phase 2: Server SDK (Weeks 5–8)
- Tool system (registration, listing, calling, validation)
- Resource system (registration, listing, reading, templates, subscriptions)
- Prompt system (registration, listing, getting)
- Logging system
- Completion system
- Integration tests

### Phase 3: Client SDK (Weeks 9–10)
- Sampling (basic + tool-use)
- Roots
- Elicitation
- Client integration tests

### Phase 4: HTTP Transport (Weeks 11–12)
- Streamable HTTP server transport
- Streamable HTTP client transport
- SSE implementation
- Security (Origin validation, auth)
- HTTP integration tests

### Phase 5: Polish & Embedded (Weeks 13–16)
- Embedded profile (no-exceptions, no-RTTI, custom allocator)
- Performance benchmarking
- Documentation site
- Package manager support (vcpkg, Conan)
- Spec conformance suite
- Examples (echo, file, calculator, chat)

### Phase 6: Long-Term (Ongoing)
- C++20 coroutine API
- Binary transport format (future spec)
- Community contributions
- Spec version updates

---

## 8. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| MCP spec changes during development | Rework | Pin to 2025-11-25; design for extensibility |
| JSON Schema validation complexity | Schedule slip | Start basic; full 2020-12 in Phase 5 |
| Embedded profile constraints | Limited feature set | Feature-gate; document limitations |
| HTTP transport security | Vulnerabilities | Follow spec guidelines; security audit before 1.0 |
| Compiler/platform fragmentation | Portability issues | CI on GCC, Clang, MSVC, ARM GCC |
| No official spec conformance suite | Compliance uncertainty | Build our own from spec |

---

## 9. Success Criteria

| Criterion | Target |
|-----------|--------|
| Spec compliance | Pass 100% of spec-defined message flows |
| Build targets | Linux (GCC/Clang), macOS (Clang), Windows (MSVC), ARM embedded (GCC) |
| Performance | <1ms message round-trip (stdio, local), <5ms (HTTP, local) |
| Memory (embedded) | <64KB RAM, <128KB flash for minimal server |
| Test coverage | ≥90% line coverage |
| Package availability | vcpkg + Conan |
| Documentation | API reference + 4+ tutorials + 3+ examples |

---

## 10. Appendix: MCP Message Catalog

### Server Requests (Client → Server)
| Method | Description |
|--------|-------------|
| `initialize` | Session initialization |
| `ping` | Health check |
| `tools/list` | List available tools |
| `tools/call` | Invoke a tool |
| `resources/list` | List resources |
| `resources/read` | Read a resource |
| `resources/templates/list` | List resource templates |
| `resources/subscribe` | Subscribe to resource changes |
| `resources/unsubscribe` | Unsubscribe from resource changes |
| `prompts/list` | List prompts |
| `prompts/get` | Get a prompt |
| `completion/complete` | Autocomplete arguments |
| `logging/setLevel` | Set logging level |

### Client Notifications (Client → Server)
| Method | Description |
|--------|-------------|
| `notifications/initialized` | Client ready after initialize |
| `notifications/cancelled` | Cancel a request |
| `notifications/roots/list_changed` | Roots list changed |

### Server Requests (Server → Client)
| Method | Description |
|--------|-------------|
| `sampling/createMessage` | Request LLM sampling |
| `roots/list` | List client roots |

### Server Notifications (Server → Client)
| Method | Description |
|--------|-------------|
| `notifications/message` | Log message |
| `notifications/cancelled` | Cancel a request |
| `notifications/resources/updated` | Resource content changed |
| `notifications/resources/list_changed` | Resource list changed |
| `notifications/tools/list_changed` | Tool list changed |
| `notifications/prompts/list_changed` | Prompt list changed |
| `notifications/progress` | Progress update |

---

*End of Document*
