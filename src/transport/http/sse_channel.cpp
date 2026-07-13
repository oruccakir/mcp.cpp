#include "sse_channel.hpp"

#include <cstdlib>
#include <vector>

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

namespace {
struct CaptureState {
    bool active = false;
    std::vector<json> responses;
};
thread_local CaptureState t_capture;
}  // namespace

PostCapture::PostCapture() {
    t_capture.active = true;
    t_capture.responses.clear();
}

PostCapture::~PostCapture() {
    t_capture.active = false;
    t_capture.responses.clear();
}

bool PostCapture::try_capture(const Message& message) {
    if (!t_capture.active ||
        !std::holds_alternative<JsonRpcResponse>(message)) {
        return false;
    }
    t_capture.responses.push_back(message_to_json(message));
    return true;
}

void PostCapture::add_error_response(const Error& error) {
    JsonRpcResponse response;
    response.error = error;
    t_capture.responses.push_back(message_to_json(Message(response)));
}

bool PostCapture::empty() const { return t_capture.responses.empty(); }

std::string PostCapture::body(bool as_batch) const {
    if (as_batch) {
        json arr = json::array();
        for (const auto& response : t_capture.responses) {
            arr.push_back(response);
        }
        return arr.dump();
    }
    return t_capture.responses.front().dump();
}

}  // namespace mcp::detail
