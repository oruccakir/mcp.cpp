#include "sse_channel.hpp"

#include <cstdlib>
#include <map>
#include <vector>

#include <mcp/sys/threading.hpp>

#include "../../platform/pal.hpp"
#include "http_codec.hpp"

namespace mcp::detail {

SseChannel::SseChannel(std::size_t replay_buffer_size, int retry_ms)
    : replay_buffer_size_(replay_buffer_size), retry_ms_(retry_ms) {}

void SseChannel::enqueue(const std::string& payload) {
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        ring_.push_back(Item{next_event_id_++, payload});
        while (ring_.size() > replay_buffer_size_) {
            ring_.pop_front();
        }
    }
    cv_.notify_all();
}

void SseChannel::attach_stream(std::intptr_t fd, const std::string& last_event_id,
                               const std::string& protocol_version_header) {
    std::uint64_t my_generation;
    std::uint64_t cursor;
    std::uint64_t last_assigned;
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        my_generation = ++stream_generation_;  // newest stream wins
        last_assigned = next_event_id_ - 1;
        if (!last_event_id.empty()) {
            // Resume after the given id (FR-TRAN-009).
            cursor = std::strtoull(last_event_id.c_str(), nullptr, 10) + 1;
        } else {
            // Fresh stream: first never-delivered event, so messages queued
            // while no stream was attached are not lost (FR-TRAN-007).
            cursor = next_undelivered_;
        }
    }
    cv_.notify_all();

    const auto header = serialize_response(
        200, "OK",
        {{"Content-Type", "text/event-stream"},
         {"Cache-Control", "no-cache"},
         {"Connection", "keep-alive"},
         {"MCP-Protocol-Version", protocol_version_header}},
        "", /*streaming=*/true);
    if (!pal::write_all(fd, header.data(), header.size())) {
        return;
    }

    // Initial event: id baseline + retry hint (FR-TRAN-007).
    SseEvent initial;
    initial.id = std::to_string(last_assigned);
    initial.retry_ms = retry_ms_;
    const auto initial_text = format_sse_event(initial);
    if (!pal::write_all(fd, initial_text.data(), initial_text.size())) {
        return;
    }

    for (;;) {
        std::vector<Item> pending;
        {
            std::unique_lock<mcp::sys::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return !running_ || stream_generation_ != my_generation ||
                       (!ring_.empty() && ring_.back().id >= cursor);
            });
            if (!running_ || stream_generation_ != my_generation) {
                return;  // shutdown or a newer stream took over
            }
            for (const auto& item : ring_) {
                if (item.id >= cursor) {
                    pending.push_back(item);
                }
            }
            if (!pending.empty()) {
                cursor = pending.back().id + 1;
                if (cursor > next_undelivered_) {
                    next_undelivered_ = cursor;
                }
            }
        }
        for (const auto& item : pending) {
            SseEvent event;
            event.id = std::to_string(item.id);
            event.data = item.data;
            const auto text = format_sse_event(event);
            if (!pal::write_all(fd, text.data(), text.size())) {
                return;  // client gone; may reconnect with Last-Event-ID
            }
        }
    }
}

void SseChannel::shutdown() {
    {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
}

// ---------------------------------------------------------- PostCapture
//
// Correlates POST responses with the connection thread that is dispatching a
// POST (dispatch is synchronous on that thread). Keyed by thread id rather
// than a thread_local: kernel-module (DKM) builds have no TLS
// (__cxa_thread_atexit / DKM_TLS_SIZE), and a mutex-guarded map is portable.
// A thread's presence in the map marks an active capture.

namespace {

std::map<mcp::sys::thread::id, std::vector<json>>& capture_table() {
    static std::map<mcp::sys::thread::id, std::vector<json>> table;
    return table;
}
mcp::sys::mutex& capture_mutex() {
    static mcp::sys::mutex mutex;
    return mutex;
}

}  // namespace

PostCapture::PostCapture() {
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    capture_table()[mcp::sys::this_thread::get_id()].clear();
}

PostCapture::~PostCapture() {
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    capture_table().erase(mcp::sys::this_thread::get_id());
}

bool PostCapture::try_capture(const Message& message) {
    if (!std::holds_alternative<JsonRpcResponse>(message)) {
        return false;
    }
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    const auto it = capture_table().find(mcp::sys::this_thread::get_id());
    if (it == capture_table().end()) {
        return false;  // no active capture on this thread
    }
    it->second.push_back(message_to_json(message));
    return true;
}

void PostCapture::add_error_response(const Error& error) {
    JsonRpcResponse response;
    response.error = error;
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    const auto it = capture_table().find(mcp::sys::this_thread::get_id());
    if (it != capture_table().end()) {
        it->second.push_back(message_to_json(Message(response)));
    }
}

bool PostCapture::empty() const {
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    const auto it = capture_table().find(mcp::sys::this_thread::get_id());
    return it == capture_table().end() || it->second.empty();
}

std::string PostCapture::body(bool as_batch) const {
    std::lock_guard<mcp::sys::mutex> lock(capture_mutex());
    const auto& responses = capture_table().at(mcp::sys::this_thread::get_id());
    if (as_batch) {
        json arr = json::array();
        for (const auto& response : responses) {
            arr.push_back(response);
        }
        return arr.dump();
    }
    return responses.front().dump();
}

}  // namespace mcp::detail
