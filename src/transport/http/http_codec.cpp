#include "http_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mcp::detail {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

}  // namespace

std::string HttpHead::header(const std::string& lowercase_name) const {
    const auto it = headers.find(lowercase_name);
    return it == headers.end() ? std::string() : it->second;
}

bool HttpHead::header_contains(const std::string& lowercase_name,
                               const std::string& token) const {
    return to_lower(header(lowercase_name)).find(to_lower(token)) !=
           std::string::npos;
}

bool parse_head(const std::string& buffer, bool request_mode, HttpHead& head,
                std::size_t& consumed, std::string& error) {
    error.clear();
    const auto end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        // Also tolerate bare-LF peers.
        const auto lf_end = buffer.find("\n\n");
        if (lf_end == std::string::npos) {
            return false;  // incomplete
        }
    }

    // Normalize: split into lines on '\n', trimming '\r'.
    const std::size_t head_end =
        end != std::string::npos ? end + 4 : buffer.find("\n\n") + 2;
    consumed = head_end;

    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < head_end) {
        auto nl = buffer.find('\n', pos);
        if (nl == std::string::npos || nl >= head_end) {
            break;
        }
        std::string line = buffer.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    if (lines.empty() || lines[0].empty()) {
        error = "empty start line";
        return false;
    }

    head = HttpHead{};
    const std::string& start = lines[0];
    if (request_mode) {
        // METHOD SP TARGET SP VERSION
        const auto sp1 = start.find(' ');
        const auto sp2 = start.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos ||
            start.compare(sp2 + 1, 7, "HTTP/1.") != 0) {
            error = "malformed request line";
            return false;
        }
        head.method = start.substr(0, sp1);
        head.target = start.substr(sp1 + 1, sp2 - sp1 - 1);
    } else {
        // VERSION SP STATUS SP REASON
        if (start.compare(0, 7, "HTTP/1.") != 0) {
            error = "malformed status line";
            return false;
        }
        const auto sp1 = start.find(' ');
        if (sp1 == std::string::npos || sp1 + 4 > start.size()) {
            error = "malformed status line";
            return false;
        }
        head.status = std::atoi(start.c_str() + sp1 + 1);
        if (head.status < 100 || head.status > 599) {
            error = "invalid status code";
            return false;
        }
        const auto sp2 = start.find(' ', sp1 + 1);
        head.reason = sp2 == std::string::npos ? "" : start.substr(sp2 + 1);
    }

    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            break;
        }
        const auto colon = lines[i].find(':');
        if (colon == std::string::npos) {
            error = "malformed header line";
            return false;
        }
        head.headers[to_lower(trim(lines[i].substr(0, colon)))] =
            trim(lines[i].substr(colon + 1));
    }
    return true;
}

std::string serialize_request(const std::string& method,
                              const std::string& target,
                              const HeaderList& headers,
                              const std::string& body) {
    std::string out = method + " " + target + " HTTP/1.1\r\n";
    bool has_length = false;
    for (const auto& [name, value] : headers) {
        out += name + ": " + value + "\r\n";
        if (to_lower(name) == "content-length") {
            has_length = true;
        }
    }
    if (!has_length && (!body.empty() || method == "POST")) {
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    out += "\r\n";
    out += body;
    return out;
}

std::string serialize_response(int status, const std::string& reason,
                               const HeaderList& headers,
                               const std::string& body, bool streaming) {
    std::string out =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    for (const auto& [name, value] : headers) {
        out += name + ": " + value + "\r\n";
    }
    if (!streaming) {
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    out += "\r\n";
    out += body;
    return out;
}

ChunkedDecoder::Status ChunkedDecoder::feed(std::string& buffer,
                                            std::string& out) {
    for (;;) {
        switch (state_) {
            case State::Size: {
                const auto nl = buffer.find("\r\n");
                if (nl == std::string::npos) {
                    return Status::NeedMore;
                }
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 2);
                std::size_t size = 0;
                bool any = false;
                for (const char c : line) {
                    if (c == ';') {
                        break;  // chunk extensions ignored
                    }
                    const int digit =
                        c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
                    if (digit < 0) {
                        return Status::Error;
                    }
                    size = size * 16 + static_cast<std::size_t>(digit);
                    any = true;
                }
                if (!any) {
                    return Status::Error;
                }
                if (size == 0) {
                    state_ = State::Trailers;
                    break;
                }
                remaining_ = size;
                state_ = State::Data;
                break;
            }
            case State::Data: {
                const std::size_t take = std::min(remaining_, buffer.size());
                out.append(buffer, 0, take);
                buffer.erase(0, take);
                remaining_ -= take;
                if (remaining_ > 0) {
                    return Status::NeedMore;
                }
                state_ = State::DataCrlf;
                break;
            }
            case State::DataCrlf: {
                if (buffer.size() < 2) {
                    return Status::NeedMore;
                }
                if (buffer.compare(0, 2, "\r\n") != 0) {
                    return Status::Error;
                }
                buffer.erase(0, 2);
                state_ = State::Size;
                break;
            }
            case State::Trailers: {
                const auto nl = buffer.find("\r\n");
                if (nl == std::string::npos) {
                    return Status::NeedMore;
                }
                const bool blank = nl == 0;
                buffer.erase(0, nl + 2);
                if (blank) {
                    return Status::Done;
                }
                break;  // skip trailer header line
            }
        }
    }
}

std::string format_sse_event(const SseEvent& event) {
    std::string out;
    if (event.retry_ms) {
        out += "retry: " + std::to_string(*event.retry_ms) + "\n";
    }
    if (event.event) {
        out += "event: " + *event.event + "\n";
    }
    if (event.id) {
        out += "id: " + *event.id + "\n";
    }
    // Emit a data field per line so embedded newlines survive framing.
    std::size_t pos = 0;
    do {
        const auto nl = event.data.find('\n', pos);
        const std::string line =
            nl == std::string::npos ? event.data.substr(pos)
                                    : event.data.substr(pos, nl - pos);
        out += "data: " + line + "\n";
        pos = nl == std::string::npos ? std::string::npos : nl + 1;
    } while (pos != std::string::npos);
    out += "\n";
    return out;
}

void SseParser::feed(const char* data, std::size_t size,
                     std::vector<SseEvent>& out) {
    buffer_.append(data, size);
    std::size_t nl;
    while ((nl = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        take_line(line, out);
    }
}

void SseParser::take_line(const std::string& line, std::vector<SseEvent>& out) {
    if (line.empty()) {
        if (any_field_) {
            out.push_back(std::move(current_));
        }
        current_ = SseEvent{};
        any_field_ = false;
        return;
    }
    if (line[0] == ':') {
        return;  // comment / keep-alive
    }

    std::string field = line;
    std::string value;
    if (const auto colon = line.find(':'); colon != std::string::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }
    }

    if (field == "data") {
        if (any_field_ && !current_.data.empty()) {
            current_.data += '\n';
        }
        current_.data += value;
        any_field_ = true;
    } else if (field == "id") {
        current_.id = value;
        any_field_ = true;
    } else if (field == "event") {
        current_.event = value;
        any_field_ = true;
    } else if (field == "retry") {
        bool digits = !value.empty();
        for (const char c : value) {
            digits = digits && c >= '0' && c <= '9';
        }
        if (digits) {
            current_.retry_ms = std::atoi(value.c_str());
            any_field_ = true;
        }
    }
    // Unknown fields are ignored per the SSE spec.
}

}  // namespace mcp::detail
