#pragma once

// Internal HTTP/1.1 + Server-Sent Events codec for the Streamable HTTP
// transport (FR-TRAN-005..009). A narrow subset: request-line / status-line,
// headers (case-insensitive), Content-Length bodies, and chunked
// transfer-encoding decoding on the client side (for interop with other MCP
// servers). SSE events are framed per the HTML spec.
//
// Unit-testable without sockets: parsers are incremental (`feed` accepts
// arbitrary byte runs and returns NeedMore until a complete message is in).
//
// Not installed.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::detail::http {

/// Lowercased-key header map; lookups via header_get (case-insensitive).
using Headers = std::map<std::string, std::string>;

/// Lowercases an ASCII header name in place.
std::string lower(std::string s);

/// Case-insensitive lookup; returns nullptr when absent.
const std::string* header_get(const Headers& h, std::string_view name);

struct HttpRequest {
    std::string method;   // "GET", "POST"
    std::string target;    // "/mcp"
    Headers headers;
    std::string body;
    bool keep_alive = false;  // derived from Connection header / HTTP version
};

struct HttpResponse {
    int status = 200;
    std::string reason;  // "OK", "Not Found", ...
    Headers headers;
    std::string body;
};

/// Serializers (CRLF-terminated lines, blank line before body).
std::string serialize_request(const HttpRequest& req);
std::string serialize_response(const HttpResponse& res);

// --- Incremental request parser (server side) -----------------------------

enum class ParseStatus { NeedMore, Done, Error };

class HttpRequestParser {
public:
    /// Feeds bytes; returns Done when a complete request is available, NeedMore
    /// for more bytes, Error on a malformed frame. After Done, the parser must
    /// be reset() before reuse.
    ParseStatus feed(const char* data, std::size_t size);
    ParseStatus feed(std::string_view data) {
        return feed(data.data(), data.size());
    }

    const HttpRequest& request() const { return request_; }
    HttpRequest take();

    void reset();

private:
    bool parse_headers();
    bool decode_body();

    enum class Phase { RequestLine, Headers, Body, Complete } phase_ = Phase::RequestLine;
    std::string buf_;
    HttpRequest request_;
    std::size_t body_start_ = 0;
    std::size_t body_length_ = 0;
    bool chunked_ = false;
};

// --- Incremental response parser (client side; supports chunked body) ------

class HttpResponseParser {
public:
    ParseStatus feed(const char* data, std::size_t size);
    ParseStatus feed(std::string_view data) {
        return feed(data.data(), data.size());
    }

    const HttpResponse& response() const { return response_; }
    HttpResponse take();

    /// Bytes read past the headers that were not consumed as a framed body
    /// (e.g. the start of an SSE stream on a Content-Length-less response).
    /// Valid after Done; cleared by reset()/take().
    std::string leftover_body() const;

    void reset();

private:
    bool parse_status_and_headers();
    bool decode_chunked();

    enum class Phase { StatusLine, Headers, Body, ChunkBody, Complete } phase_ = Phase::StatusLine;
    std::string buf_;
    HttpResponse response_;
    std::size_t body_start_ = 0;
    std::size_t body_length_ = 0;  // Content-Length, or current chunk size
    bool chunked_ = false;
    bool length_known_ = false;
    bool reading_chunk_header_ = true;
};

// --- Server-Sent Events ---------------------------------------------------

struct SseEvent {
    std::optional<std::string> id;
    std::string data;            // multiple `data:` lines joined by '\n'
    std::optional<std::int64_t> retry_ms;
};

/// Frames one event per the HTML SSE wire format. `id` (if present) emits an
/// `id:` line; `data` may contain newlines (each emits its own `data:` line);
/// `retry_ms` (if present) emits a `retry:` line. A trailing blank line
/// terminates the event.
std::string format_sse_event(const std::optional<std::string>& id,
                             std::string_view data,
                             const std::optional<std::int64_t>& retry_ms);

/// Incremental SSE parser. Lines accumulate until a blank line dispatches an
/// event. `last_event_id` tracks the most recent id seen (per spec, retained
/// across events for reconnect Last-Event-ID).
class SseParser {
public:
    /// Feeds bytes; returns events completed by this feed (blank line closes).
    std::vector<SseEvent> feed(const char* data, std::size_t size);
    std::vector<SseEvent> feed(std::string_view data) {
        return feed(data.data(), data.size());
    }

    const std::string& last_event_id() const { return last_event_id_; }

private:
    std::string buf_;
    std::string line_;          // current line accumulator (without LF)
    std::optional<std::string> pending_id_;
    std::string pending_data_;
    std::optional<std::int64_t> pending_retry_;
    std::string last_event_id_;

    void handle_line();
    std::vector<SseEvent> flush_event();
};

/// Incremental chunked transfer-encoding decoder for an open-ended stream
/// (e.g. a chunked SSE response). Feed raw socket bytes; `take()` drains the
/// decoded bytes. `ended()` is true once the terminating zero-length chunk is
/// seen.
class ChunkDecoder {
public:
    std::string feed(const char* data, std::size_t size);
    std::string feed(std::string_view data) {
        return feed(data.data(), data.size());
    }
    /// Drains and returns decoded bytes accumulated so far.
    std::string take() {
        std::string out;
        out.swap(decoded_);
        return out;
    }
    bool ended() const { return ended_; }
    void reset();

private:
    std::string buf_;
    std::string decoded_;
    enum class Phase { Header, Data } phase_{Phase::Header};
    std::size_t remaining_{0};  // bytes left in the current chunk
    bool ended_{false};
};

}  // namespace mcp::detail::http