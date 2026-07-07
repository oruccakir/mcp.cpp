#include "http_codec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace mcp::detail::http {

namespace {

std::string trim(std::string s) {
    const auto first = std::find_if_not(
        s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(
        s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); })
                          .base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool parse_int(std::string_view text, std::size_t& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    auto [ptr, ec] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (ec != std::errc()) {
        return false;
    }
    out = value;
    return true;
}

// Finds the end of the header block, returning the index of the first body
// byte (i.e. just past the blank line). Returns std::string::npos if no
// terminator yet.
std::size_t find_header_end(const std::string& buf) {
    if (auto pos = buf.find("\r\n\r\n"); pos != std::string::npos) {
        return pos + 4;
    }
    if (auto pos = buf.find("\n\n"); pos != std::string::npos) {
        return pos + 2;
    }
    return std::string::npos;
}

// Splits a header block (without the trailing blank line) into the request/
// status line plus header lines.
std::vector<std::string> split_lines(std::string_view block) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < block.size()) {
        auto cr = block.find('\n', start);
        if (cr == std::string_view::npos) {
            lines.emplace_back(block.substr(start));
            break;
        }
        std::size_t end = cr;
        if (end > start && block[end - 1] == '\r') {
            --end;
        }
        lines.emplace_back(block.substr(start, end - start));
        start = cr + 1;
    }
    return lines;
}

}  // namespace

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

const std::string* header_get(const Headers& h, std::string_view name) {
    const auto key = lower(std::string(name));
    auto it = h.find(key);
    return it == h.end() ? nullptr : &it->second;
}

// --- Serializers ----------------------------------------------------------

std::string serialize_request(const HttpRequest& req) {
    std::string out;
    out.reserve(128 + req.body.size());
    out += req.method;
    out += " ";
    out += req.target;
    out += " HTTP/1.1\r\n";
    for (const auto& [k, v] : req.headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    out += "\r\n";
    out += req.body;
    return out;
}

std::string serialize_response(const HttpResponse& res) {
    std::string out;
    out.reserve(128 + res.body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(res.status);
    out += " ";
    out += res.reason;
    out += "\r\n";
    for (const auto& [k, v] : res.headers) {
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    out += "\r\n";
    out += res.body;
    return out;
}

// --- HttpRequestParser ----------------------------------------------------

ParseStatus HttpRequestParser::feed(const char* data, std::size_t size) {
    if (size > 0) {
        buf_.append(data, size);
    }

    for (;;) {
        if (phase_ == Phase::RequestLine || phase_ == Phase::Headers) {
            const auto hend = find_header_end(buf_);
            if (hend == std::string::npos) {
                return ParseStatus::NeedMore;
            }
            body_start_ = hend;
            if (!parse_headers()) {
                phase_ = Phase::Complete;  // mark done so caller can reset
                return ParseStatus::Error;
            }
            phase_ = Phase::Body;
            // fall through to body handling
        }

        if (phase_ == Phase::Body) {
            if (!decode_body()) {
                return ParseStatus::NeedMore;
            }
            phase_ = Phase::Complete;
            return ParseStatus::Done;
        }

        return ParseStatus::Done;  // already complete (shouldn't happen)
    }
}

bool HttpRequestParser::parse_headers() {
    const std::string block = buf_.substr(0, body_start_);
    // Strip the trailing blank line from the block before splitting.
    std::string_view hblock(block.data(),
                            block.size() >= 4 && block.compare(block.size() - 4, 4, "\r\n\r\n") == 0
                                ? block.size() - 4
                                : (block.size() >= 2 ? block.size() - 2 : block.size()));
    auto lines = split_lines(hblock);
    if (lines.empty()) {
        return false;
    }

    // Request line: "METHOD SP target SP HTTP-version".
    {
        std::istringstream rl(lines[0]);
        std::string version;
        rl >> request_.method >> request_.target >> version;
        if (rl.fail() || request_.method.empty() || request_.target.empty() ||
            (version != "HTTP/1.1" && version != "HTTP/1.0")) {
            return false;
        }
        request_.keep_alive = version == "HTTP/1.1";
    }

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::string name = lower(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (name == "connection") {
            const auto v = lower(value);
            if (v == "close") {
                request_.keep_alive = false;
            } else if (v == "keep-alive") {
                request_.keep_alive = true;
            }
        }
        request_.headers[std::move(name)] = std::move(value);
    }

    if (const auto* te = header_get(request_.headers, "transfer-encoding")) {
        // We do not accept chunked requests (our client always sends
        // Content-Length); reject so the caller answers 400.
        if (lower(*te).find("chunked") != std::string::npos) {
            return false;
        }
    }
    if (const auto* cl = header_get(request_.headers, "content-length")) {
        if (!parse_int(*cl, body_length_)) {
            return false;
        }
    } else {
        body_length_ = 0;
    }
    return true;
}

bool HttpRequestParser::decode_body() {
    const std::size_t available = buf_.size() - body_start_;
    if (available < body_length_) {
        return false;  // need more bytes
    }
    request_.body.assign(buf_, body_start_, body_length_);
    return true;
}

HttpRequest HttpRequestParser::take() {
    HttpRequest out = std::move(request_);
    reset();
    return out;
}

void HttpRequestParser::reset() {
    phase_ = Phase::RequestLine;
    buf_.clear();
    request_ = {};
    body_start_ = 0;
    body_length_ = 0;
}

// --- HttpResponseParser ---------------------------------------------------

ParseStatus HttpResponseParser::feed(const char* data, std::size_t size) {
    if (size > 0) {
        buf_.append(data, size);
    }

    for (;;) {
        if (phase_ == Phase::StatusLine || phase_ == Phase::Headers) {
            const auto hend = find_header_end(buf_);
            if (hend == std::string::npos) {
                return ParseStatus::NeedMore;
            }
            body_start_ = hend;
            if (!parse_status_and_headers()) {
                phase_ = Phase::Complete;
                return ParseStatus::Error;
            }
            if (chunked_) {
                phase_ = Phase::ChunkBody;
                reading_chunk_header_ = true;
                body_length_ = 0;
            } else if (length_known_) {
                phase_ = Phase::Body;
            } else {
                // No body framing: treat as done with empty body (e.g. a
                // connection close would deliver the rest; we don't rely on
                // this path for MCP).
                phase_ = Phase::Complete;
                return ParseStatus::Done;
            }
        }

        if (phase_ == Phase::Body) {
            const std::size_t available = buf_.size() - body_start_;
            if (available < body_length_) {
                return ParseStatus::NeedMore;
            }
            response_.body.assign(buf_, body_start_, body_length_);
            phase_ = Phase::Complete;
            return ParseStatus::Done;
        }

        if (phase_ == Phase::ChunkBody) {
            if (!decode_chunked()) {
                return ParseStatus::NeedMore;
            }
            phase_ = Phase::Complete;
            return ParseStatus::Done;
        }

        return ParseStatus::Done;
    }
}

bool HttpResponseParser::parse_status_and_headers() {
    const std::string block = buf_.substr(0, body_start_);
    std::string_view hblock(block.data(),
                            block.size() >= 4 && block.compare(block.size() - 4, 4, "\r\n\r\n") == 0
                                ? block.size() - 4
                                : (block.size() >= 2 ? block.size() - 2 : block.size()));
    auto lines = split_lines(hblock);
    if (lines.empty()) {
        return false;
    }

    // Status line: "HTTP/1.1 SP status SP reason".
    {
        std::istringstream sl(lines[0]);
        std::string version;
        sl >> version >> response_.status;
        if (sl.fail() || (version != "HTTP/1.1" && version != "HTTP/1.0")) {
            return false;
        }
        auto pos = sl.tellg();
        response_.reason = trim(
            pos >= 0 ? sl.str().substr(static_cast<std::size_t>(pos)) : "");
    }

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::string name = lower(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        response_.headers[std::move(name)] = std::move(value);
    }

    if (const auto* te = header_get(response_.headers, "transfer-encoding")) {
        if (lower(*te).find("chunked") != std::string::npos) {
            chunked_ = true;
            length_known_ = false;
            return true;
        }
    }
    if (const auto* cl = header_get(response_.headers, "content-length")) {
        std::size_t len = 0;
        if (parse_int(*cl, len)) {
            body_length_ = len;
            length_known_ = true;
        }
    }
    return true;
}

bool HttpResponseParser::decode_chunked() {
    // Walk the buffer from body_start_, consuming "<size>\r\n<data>\r\n"* and
    // a terminating "0\r\n\r\n". Decoded bytes accumulate into response_.body.
    std::string decoded;
    std::size_t pos = body_start_;
    for (;;) {
        if (reading_chunk_header_) {
            auto line_end = buf_.find('\n', pos);
            if (line_end == std::string::npos) {
                return false;
            }
            std::size_t hdr_end = line_end;
            if (hdr_end > pos && buf_[hdr_end - 1] == '\r') {
                --hdr_end;
            }
            std::string hline = buf_.substr(pos, hdr_end - pos);
            // chunk size may have ";ext"
            if (auto semi = hline.find(';'); semi != std::string::npos) {
                hline = hline.substr(0, semi);
            }
            hline = trim(hline);
            std::size_t chunk_size = 0;
            if (!parse_int(hline, chunk_size)) {
                return false;
            }
            pos = line_end + 1;
            if (chunk_size == 0) {
                // Expect trailing CRLF (or CRLFCRLF). The blank line after the
                // zero chunk terminates the body.
                if (pos + 1 < buf_.size() && buf_[pos] == '\r' &&
                    buf_[pos + 1] == '\n') {
                    pos += 2;
                } else if (pos < buf_.size() && buf_[pos] == '\n') {
                    pos += 1;
                }
                body_start_ = pos;
                response_.body = std::move(decoded);
                return true;
            }
            body_length_ = chunk_size;
            reading_chunk_header_ = false;
        } else {
            if (buf_.size() - pos < body_length_ + 2) {
                return false;  // need chunk data + trailing CRLF
            }
            decoded.append(buf_, pos, body_length_);
            pos += body_length_;
            // skip trailing CRLF
            if (pos + 1 < buf_.size() && buf_[pos] == '\r' &&
                buf_[pos + 1] == '\n') {
                pos += 2;
            } else if (pos < buf_.size() && buf_[pos] == '\n') {
                pos += 1;
            }
            reading_chunk_header_ = true;
        }
    }
}

HttpResponse HttpResponseParser::take() {
    HttpResponse out = std::move(response_);
    reset();
    return out;
}

void HttpResponseParser::reset() {
    phase_ = Phase::StatusLine;
    buf_.clear();
    response_ = {};
    body_start_ = 0;
    body_length_ = 0;
    chunked_ = false;
    length_known_ = false;
    reading_chunk_header_ = true;
}

// --- SSE ------------------------------------------------------------------

std::string format_sse_event(const std::optional<std::string>& id,
                             std::string_view data,
                             const std::optional<std::int64_t>& retry_ms) {
    std::string out;
    out.reserve(data.size() + 64);
    if (id) {
        out += "id:";
        out += *id;
        out += "\n";
    }
    if (retry_ms) {
        out += "retry:";
        out += std::to_string(*retry_ms);
        out += "\n";
    }
    if (data.empty()) {
        out += "data:\n";
    } else {
        std::size_t start = 0;
        while (start <= data.size()) {
            auto nl = data.find('\n', start);
            if (nl == std::string_view::npos) {
                out += "data:";
                out += std::string(data.substr(start));
                out += "\n";
                break;
            }
            out += "data:";
            out += std::string(data.substr(start, nl - start));
            out += "\n";
            start = nl + 1;
        }
    }
    out += "\n";
    return out;
}

void SseParser::handle_line() {
    if (line_.empty()) {
        // blank line dispatches the event
        return;
    }
    // A line starting with ':' is a comment; ignore.
    if (line_[0] == ':') {
        return;
    }
    std::string field;
    std::string value;
    if (auto colon = line_.find(':'); colon != std::string::npos) {
        field = line_.substr(0, colon);
        // A single leading space after the colon is stripped.
        std::size_t vstart = colon + 1;
        if (vstart < line_.size() && line_[vstart] == ' ') {
            ++vstart;
        }
        value = line_.substr(vstart);
    } else {
        field = line_;
    }

    if (field == "event") {
        // We don't dispatch named events for MCP; ignore but keep parser correct.
    } else if (field == "data") {
        if (!pending_data_.empty()) {
            pending_data_ += "\n";
        }
        pending_data_ += value;
    } else if (field == "id") {
        pending_id_ = value;
        last_event_id_ = value;
    } else if (field == "retry") {
        std::size_t ms = 0;
        if (parse_int(value, ms)) {
            pending_retry_ = static_cast<std::int64_t>(ms);
        }
    }
    // unknown fields ignored
}

std::vector<SseEvent> SseParser::flush_event() {
    std::vector<SseEvent> out;
    // Per spec an event is dispatched on a blank line; we only emit when data
    // is non-empty OR an id was set (the initial "ping" event has empty data).
    if (pending_data_.empty() && !pending_id_ && !pending_retry_) {
        pending_id_.reset();
        pending_retry_.reset();
        return out;
    }
    SseEvent ev;
    ev.id = pending_id_;
    ev.data = std::move(pending_data_);
    ev.retry_ms = pending_retry_;
    pending_data_.clear();
    pending_id_.reset();
    pending_retry_.reset();
    out.push_back(std::move(ev));
    return out;
}

std::vector<SseEvent> SseParser::feed(const char* data, std::size_t size) {
    std::vector<SseEvent> events;
    if (size > 0) {
        buf_.append(data, size);
    }
    std::size_t start = 0;
    while (start < buf_.size()) {
        auto nl = buf_.find('\n', start);
        if (nl == std::string::npos) {
            break;
        }
        line_.assign(buf_, start, nl - start);
        if (!line_.empty() && line_.back() == '\r') {
            line_.pop_back();
        }
        start = nl + 1;

        if (line_.empty()) {
            auto flushed = flush_event();
            for (auto& e : flushed) {
                events.push_back(std::move(e));
            }
        } else {
            handle_line();
        }
    }
    buf_.erase(0, start);
    return events;
}

}  // namespace mcp::detail::http