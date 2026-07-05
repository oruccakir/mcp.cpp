#pragma once

#include <optional>
#include <utility>
#include <variant>

#include <mcp/error.hpp>

namespace mcp {

/// Minimal expected-like result type (FR-ERR-003). Used throughout the SDK so
/// the embedded (no-exception) profile can share the same API surface later.
template <typename T, typename E = Error>
class Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

    static Result ok(T value) { return Result(std::move(value)); }
    static Result err(E error) { return Result(std::move(error)); }

    bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }

    E& error() & { return std::get<1>(storage_); }
    const E& error() const& { return std::get<1>(storage_); }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> storage_;
};

template <typename E>
class Result<void, E> {
public:
    Result() = default;
    Result(E error) : error_(std::move(error)) {}

    static Result ok() { return Result(); }
    static Result err(E error) { return Result(std::move(error)); }

    bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    E& error() & { return *error_; }
    const E& error() const& { return *error_; }

private:
    std::optional<E> error_;
};

}  // namespace mcp
