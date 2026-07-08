#pragma once

// Per-session server->client SSE machinery (FR-TRAN-007/009), shared by the
// sessionless HttpServerTransport and the multi-session HttpSessionServer.

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include <mcp/jsonrpc/message.hpp>

namespace mcp::detail {

/// One logical SSE stream endpoint: event-id sequence, bounded replay ring
/// with a never-delivered watermark, and single-active-stream semantics
/// (newest GET wins).
class SseChannel {
public:
    SseChannel(std::size_t replay_buffer_size, int retry_ms);

    /// Queues one serialized JSON-RPC payload; assigns the next event id.
    void enqueue(const std::string& payload);

    /// Blocking: writes the SSE response head + initial event, replays from
    /// `last_event_id` (wire header value; empty = first never-delivered),
    /// then streams until takeover, shutdown, or write failure. The caller
    /// owns/closes the fd afterwards.
    void attach_stream(std::intptr_t fd, const std::string& last_event_id,
                       const std::string& protocol_version_header);

    /// Wakes and detaches any active stream; enqueue becomes a no-op.
    void shutdown();

private:
    const std::size_t replay_buffer_size_;
    const int retry_ms_;

    struct Item {
        std::uint64_t id;
        std::string data;
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = true;
    std::deque<Item> ring_;
    std::uint64_t next_event_id_ = 1;
    std::uint64_t next_undelivered_ = 1;
    std::uint64_t stream_generation_ = 0;
};

/// POST-response capture: dispatch is synchronous on the connection thread,
/// so responses produced while dispatching a POST's messages are correlated
/// through a thread_local (FR-TRAN-006).
class PostCapture {
public:
    PostCapture();
    ~PostCapture();
    PostCapture(const PostCapture&) = delete;
    PostCapture& operator=(const PostCapture&) = delete;

    /// Send-path hook: captures `message` if it is a JsonRpcResponse and a
    /// capture is active on this thread.
    static bool try_capture(const Message& message);

    void add_error_response(const Error& error);
    bool empty() const;
    /// 200 body: single object, or array when `as_batch`.
    std::string body(bool as_batch) const;
};

}  // namespace mcp::detail
