#include <mcp/detail/uri_template.hpp>

#include <vector>

namespace mcp::detail {

namespace {

struct Segment {
    std::string text;
    bool is_variable = false;
};

std::optional<std::vector<Segment>> tokenize(const std::string& uri_template) {
    std::vector<Segment> segments;
    std::string current;
    for (std::size_t i = 0; i < uri_template.size(); ++i) {
        const char c = uri_template[i];
        if (c == '{') {
            if (!current.empty()) {
                segments.push_back({current, false});
                current.clear();
            }
            const auto close = uri_template.find('}', i);
            if (close == std::string::npos || close == i + 1) {
                return std::nullopt;  // Unterminated or empty expression.
            }
            segments.push_back({uri_template.substr(i + 1, close - i - 1), true});
            i = close;
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        segments.push_back({current, false});
    }
    return segments;
}

}  // namespace

std::optional<std::map<std::string, std::string>> match_uri_template(
    const std::string& uri_template, const std::string& uri) {
    const auto segments = tokenize(uri_template);
    if (!segments) {
        return std::nullopt;
    }

    std::map<std::string, std::string> variables;
    std::size_t pos = 0;
    for (std::size_t s = 0; s < segments->size(); ++s) {
        const auto& segment = (*segments)[s];
        if (!segment.is_variable) {
            if (uri.compare(pos, segment.text.size(), segment.text) != 0) {
                return std::nullopt;
            }
            pos += segment.text.size();
            continue;
        }

        // Variable: capture up to the next literal, or to the end if last.
        std::size_t value_end;
        if (s + 1 < segments->size()) {
            const auto& next_literal = (*segments)[s + 1];
            // Adjacent variables are ambiguous; refuse to match.
            if (next_literal.is_variable) {
                return std::nullopt;
            }
            value_end = uri.find(next_literal.text, pos);
            if (value_end == std::string::npos) {
                return std::nullopt;
            }
        } else {
            value_end = uri.size();
        }
        if (value_end == pos) {
            return std::nullopt;  // Variables must match at least one char.
        }
        variables[segment.text] = uri.substr(pos, value_end - pos);
        pos = value_end;
    }

    if (pos != uri.size()) {
        return std::nullopt;  // Trailing unmatched input.
    }
    return variables;
}

}  // namespace mcp::detail
