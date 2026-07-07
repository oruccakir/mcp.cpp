#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <mcp/error.hpp>
#include <mcp/result.hpp>

namespace mcp::detail {

template <typename T>
struct Page {
    std::vector<T> items;
    std::optional<std::string> next_cursor;
};

/// Cursor-based pagination over a stable snapshot (FR-SRV-004/011/017).
/// Cursors are opaque stringified indices; anything else is a client error
/// answered with -32006 (FR-CORE-003).
template <typename T>
Result<Page<T>> paginate(const std::vector<T>& items,
                         const std::optional<std::string>& cursor,
                         std::size_t page_size) {
    std::size_t start = 0;
    if (cursor) {
        if (cursor->empty()) {
            return Error(ErrorCode::PaginationError, "invalid cursor");
        }
        for (const char c : *cursor) {
            if (c < '0' || c > '9') {
                return Error(ErrorCode::PaginationError, "invalid cursor");
            }
        }
        start = 0;
        for (const char c : *cursor) {
            start = start * 10 + static_cast<std::size_t>(c - '0');
            if (start > items.size()) {
                return Error(ErrorCode::PaginationError, "cursor out of range");
            }
        }
    }

    Page<T> page;
    const std::size_t end = std::min(start + page_size, items.size());
    page.items.assign(items.begin() + static_cast<std::ptrdiff_t>(start),
                      items.begin() + static_cast<std::ptrdiff_t>(end));
    if (end < items.size()) {
        page.next_cursor = std::to_string(end);
    }
    return page;
}

}  // namespace mcp::detail
