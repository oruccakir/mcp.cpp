#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include <mcp/transport/transport.hpp>

namespace mcp_test {

/// In-memory transport: records outgoing messages and lets tests inject
/// incoming messages, errors, and peer-close events.
class MockTransport : public mcp::Transport {
public:
    void connect() override { connected_ = true; }
    void disconnect() override { disconnected_ = true; }

    void send(const mcp::Message& message) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sent_.push_back(message);
        }
        cv_.notify_all();
    }

    void send_batch(const std::vector<mcp::Message>& messages) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sent_.insert(sent_.end(), messages.begin(), messages.end());
        }
        cv_.notify_all();
    }

    void set_message_handler(std::function<void(mcp::Message)> handler) override {
        message_handler_ = std::move(handler);
    }
    void set_error_handler(std::function<void(mcp::Error)> handler) override {
        error_handler_ = std::move(handler);
    }
    void set_close_handler(std::function<void()> handler) override {
        close_handler_ = std::move(handler);
    }

    // --- test helpers ---

    void deliver(const mcp::Message& message) { message_handler_(message); }
    void deliver_error(const mcp::Error& error) { error_handler_(error); }
    void peer_close() { close_handler_(); }

    bool connected() const { return connected_.load(); }
    bool disconnected() const { return disconnected_.load(); }

    std::size_t sent_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_.size();
    }

    /// Blocks until at least index+1 messages have been sent.
    std::optional<mcp::Message> wait_for_sent(
        std::size_t index,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] { return sent_.size() > index; })) {
            return std::nullopt;
        }
        return sent_[index];
    }

private:
    std::atomic<bool> connected_{false};
    std::atomic<bool> disconnected_{false};
    std::function<void(mcp::Message)> message_handler_;
    std::function<void(mcp::Error)> error_handler_;
    std::function<void()> close_handler_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<mcp::Message> sent_;
};

}  // namespace mcp_test
