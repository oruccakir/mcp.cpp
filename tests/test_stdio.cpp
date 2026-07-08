#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <mcp/transport/stdio_transport.hpp>

namespace {

using namespace mcp;

/// Collects transport events with waiting helpers.
struct Sink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Message> messages;
    std::vector<Error> errors;
    bool closed = false;

    void attach(Transport& transport) {
        transport.set_message_handler([this](Message m) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                messages.push_back(std::move(m));
            }
            cv.notify_all();
        });
        transport.set_error_handler([this](Error e) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                errors.push_back(std::move(e));
            }
            cv.notify_all();
        });
        transport.set_close_handler([this] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
            }
            cv.notify_all();
        });
    }

    template <typename Pred>
    bool wait_for(Pred pred,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, pred);
    }
};

struct Pipe {
    int fds[2] = {-1, -1};
#if defined(_WIN32)
    Pipe() { EXPECT_EQ(::_pipe(fds, 4096, 0), 0); }
#else
    Pipe() { EXPECT_EQ(::pipe(fds), 0); }
#endif
    int read_fd() const { return fds[0]; }
    int write_fd() const { return fds[1]; }
};

// CRT-portable raw fd helpers for injecting bytes past the transport.
long fd_write(int fd, const void* data, std::size_t size) {
#if defined(_WIN32)
    return ::_write(fd, data, static_cast<unsigned>(size));
#else
    return static_cast<long>(::write(fd, data, size));
#endif
}
void fd_close(int fd) {
#if defined(_WIN32)
    ::_close(fd);
#else
    ::close(fd);
#endif
}

TEST(StdioTransport, SendReceiveAcrossPipes) {
    Pipe a_to_b;
    Pipe b_to_a;
    // Each transport owns the ends it holds.
    StdioTransport a(b_to_a.read_fd(), a_to_b.write_fd());
    StdioTransport b(a_to_b.read_fd(), b_to_a.write_fd());
    Sink sink_a;
    Sink sink_b;
    sink_a.attach(a);
    sink_b.attach(b);
    a.connect();
    b.connect();

    a.send(Message(JsonRpcRequest{RequestId(std::int64_t{1}), "ping", {}}));
    ASSERT_TRUE(sink_b.wait_for([&] { return sink_b.messages.size() == 1; }));
    EXPECT_EQ(std::get<JsonRpcRequest>(sink_b.messages[0]).method, "ping");

    JsonRpcResponse response;
    response.id = RequestId(std::int64_t{1});
    response.result = json::object();
    b.send(Message(response));
    ASSERT_TRUE(sink_a.wait_for([&] { return sink_a.messages.size() == 1; }));
    EXPECT_FALSE(std::get<JsonRpcResponse>(sink_a.messages[0]).is_error());
}

TEST(StdioTransport, BatchDeliversAllMessages) {
    Pipe a_to_b;
    Pipe b_to_a;
    StdioTransport a(b_to_a.read_fd(), a_to_b.write_fd());
    StdioTransport b(a_to_b.read_fd(), b_to_a.write_fd());
    Sink sink_b;
    sink_b.attach(b);
    a.connect();
    b.connect();

    std::vector<Message> batch;
    batch.emplace_back(JsonRpcRequest{RequestId(std::int64_t{1}), "ping", {}});
    batch.emplace_back(JsonRpcNotification{"notifications/initialized", {}});
    a.send_batch(batch);

    ASSERT_TRUE(sink_b.wait_for([&] { return sink_b.messages.size() == 2; }));
    EXPECT_TRUE(std::holds_alternative<JsonRpcRequest>(sink_b.messages[0]));
    EXPECT_TRUE(std::holds_alternative<JsonRpcNotification>(sink_b.messages[1]));
}

TEST(StdioTransport, NewlinesInPayloadSurviveFraming) {
    // FR-TRAN-003: framing is by raw newline; embedded ones are escaped.
    Pipe a_to_b;
    Pipe b_to_a;
    StdioTransport a(b_to_a.read_fd(), a_to_b.write_fd());
    StdioTransport b(a_to_b.read_fd(), b_to_a.write_fd());
    Sink sink_b;
    sink_b.attach(b);
    a.connect();
    b.connect();

    a.send(Message(JsonRpcRequest{RequestId(std::int64_t{2}), "echo",
                                  json{{"text", "one\ntwo\nthree"}}}));
    ASSERT_TRUE(sink_b.wait_for([&] { return sink_b.messages.size() == 1; }));
    const auto& request = std::get<JsonRpcRequest>(sink_b.messages[0]);
    EXPECT_EQ(request.params->at("text").get<std::string>(), "one\ntwo\nthree");
}

TEST(StdioTransport, GarbageLineSurfacesParseError) {
    Pipe input;
    Pipe output;
    StdioTransport t(input.read_fd(), output.write_fd());
    Sink sink;
    sink.attach(t);
    t.connect();

    const char garbage[] = "this is not json\n";
    ASSERT_EQ(fd_write(input.write_fd(), garbage, sizeof(garbage) - 1),
              static_cast<long>(sizeof(garbage) - 1));

    ASSERT_TRUE(sink.wait_for([&] { return sink.errors.size() == 1; }));
    EXPECT_EQ(sink.errors[0].code, static_cast<int>(ErrorCode::ParseError));

    // Closing the peer's write end delivers EOF -> close event.
    fd_close(input.write_fd());
    ASSERT_TRUE(sink.wait_for([&] { return sink.closed; }));
    fd_close(output.read_fd());
}

TEST(StdioTransport, MultipleMessagesInOneWrite) {
    Pipe input;
    Pipe output;
    StdioTransport t(input.read_fd(), output.write_fd());
    Sink sink;
    sink.attach(t);
    t.connect();

    const std::string two =
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})" "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n";
    ASSERT_EQ(fd_write(input.write_fd(), two.data(), two.size()),
              static_cast<long>(two.size()));

    ASSERT_TRUE(sink.wait_for([&] { return sink.messages.size() == 2; }));
    fd_close(input.write_fd());
    fd_close(output.read_fd());
}

TEST(StdioTransport, DisconnectUnblocksIdleReader) {
    Pipe input;
    Pipe output;
    StdioTransport t(input.read_fd(), output.write_fd());
    Sink sink;
    sink.attach(t);
    t.connect();

    // No data ever arrives; disconnect must return promptly regardless.
    const auto start = std::chrono::steady_clock::now();
    t.disconnect();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(1));

    fd_close(input.write_fd());
    fd_close(output.read_fd());
}

}  // namespace
