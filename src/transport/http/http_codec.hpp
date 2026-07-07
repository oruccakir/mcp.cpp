#pragma once

// Internal HTTP/1.1 + SSE codec for the Streamable HTTP transport
// (FR-TRAN-005..009). Deliberately minimal: exactly the subset MCP needs.
// Pure functions/state machines — unit-testable without sockets.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp::detail {

/// Parsed request or response head. Header names are lowercased.
struct HttpHead {
    // Request form
    std::string method;
    std::string target;
    // Response form (status != 0 means response)
    int status = 0;
    std::string reason;

    std::map<std::string, std::string> headers;

    bool is_response() const { return status != 0; }
    /// Header value or "" when absent.
    std::string header(const std::string& lowercase_name) const;
    /// True if the (comma-separated) header contains the token.
    bool header_contains(const std::string& lowercase_name,
                         const std::string& token) const;
};

/// Parses one head (start line + headers) from the front of `buffer`.
/// Returns false while the terminating blank line has not arrived yet.
/// On success fills `head` and sets `consumed` to the head's length.
/// Malformed input sets `error` (and returns false).
bool parse_head(const std::string& buffer, bool request_mode, HttpHead& head,
                std::size_t& consumed, std::string& error);

using HeaderList = std::vector<std::pair<std::string, std::string>>;

/// Builds a request; adds Content-Length when a body is present.
std::string serialize_request(const std::string& method,
                              const std::string& target,
                              const HeaderList& headers,
                              const std::string& body);

/// Builds a response; adds Content-Length unless `streaming` (SSE heads).
std::string serialize_response(int status, const std::string& reason,
                               const HeaderList& headers,
                               const std::string& body, bool streaming = false);

/// Incremental Transfer-Encoding: chunked decoder (client side interop).
class ChunkedDecoder {
public:
    enum class Status { NeedMore, Done, Error };

    /// Consumes decodable bytes from the front of `buffer`, appending decoded
    /// payload to `out`.
    Status feed(std::string& buffer, std::string& out);

private:
    enum class State { Size, Data, DataCrlf, Trailers } state_ = State::Size;
    std::size_t remaining_ = 0;
};

// --- Server-Sent Events (FR-TRAN-007/009) ---------------------------------

struct SseEvent {
    std::optional<std::string> id;
    std::optional<std::string> event;
    std::string data;  // multi-line data joined with '\n'
    std::optional<int> retry_ms;
};

std::string format_sse_event(const SseEvent& event);

/// Incremental SSE stream parser; complete events are appended to `out`.
class SseParser {
public:
    void feed(const char* data, std::size_t size, std::vector<SseEvent>& out);

private:
    void take_line(const std::string& line, std::vector<SseEvent>& out);

    std::string buffer_;
    SseEvent current_;
    bool any_field_ = false;
};

}  // namespace mcp::detail
