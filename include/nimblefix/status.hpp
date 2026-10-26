#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>

namespace nimblefix {

enum class ErrorCode {
    none,
    need_more,
    invalid_frame,
    invalid_body_length,
    invalid_checksum,
    invalid_tag,
    invalid_value,
    invalid_header,
    too_large,
    capacity_exceeded,
    missing_field,
    duplicate_field,
    managed_field,
    unsupported,
    invalid_decimal,
    invalid_timestamp,
    required_field_missing,
};

struct [[nodiscard]] Status {
    ErrorCode code{ErrorCode::none};
    int tag{};
    std::size_t offset{};

    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::none; }
    [[nodiscard]] static constexpr Status success() noexcept { return {}; }
    [[nodiscard]] constexpr std::string_view message() const noexcept {
        switch (code) {
        case ErrorCode::none: return "success";
        case ErrorCode::need_more: return "incomplete frame";
        case ErrorCode::invalid_frame: return "malformed FIX frame";
        case ErrorCode::invalid_body_length: return "invalid BodyLength";
        case ErrorCode::invalid_checksum: return "invalid CheckSum";
        case ErrorCode::invalid_tag: return "invalid field tag";
        case ErrorCode::invalid_value: return "invalid field value";
        case ErrorCode::invalid_header: return "invalid FIX header";
        case ErrorCode::too_large: return "frame exceeds configured limit";
        case ErrorCode::capacity_exceeded: return "bounded storage exhausted";
        case ErrorCode::missing_field: return "field is missing";
        case ErrorCode::duplicate_field: return "field occurs more than once";
        case ErrorCode::managed_field: return "field is managed by the engine";
        case ErrorCode::unsupported: return "unsupported capability";
        case ErrorCode::invalid_decimal: return "invalid exact decimal";
        case ErrorCode::invalid_timestamp: return "invalid UTC timestamp";
        case ErrorCode::required_field_missing: return "required field is missing";
        }
        return "unknown error";
    }
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Status error) : storage_(std::in_place_index<1>, error.ok()
        ? Status{ErrorCode::invalid_value} : error) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] Status status() const noexcept {
        return ok() ? Status::success() : std::get<1>(storage_);
    }
    [[nodiscard]] Status error() const noexcept { return status(); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

private:
    std::variant<T, Status> storage_;
};

} // namespace nimblefix
