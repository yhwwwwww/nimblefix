#pragma once

#include <string>
#include <utility>

namespace fastfix::base {

enum class ErrorCode {
    kOk = 0,
    kInvalidArgument,
    kIoError,
    kBusy,
    kFormatError,
    kVersionMismatch,
    kNotFound,
    kAlreadyExists,
};

class Status {
  public:
    Status() = default;

    Status(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {
    }

    [[nodiscard]] static Status Ok() {
        return {};
    }

    [[nodiscard]] static Status InvalidArgument(std::string message) {
        return {ErrorCode::kInvalidArgument, std::move(message)};
    }

    [[nodiscard]] static Status IoError(std::string message) {
        return {ErrorCode::kIoError, std::move(message)};
    }

    [[nodiscard]] static Status Busy(std::string message) {
        return {ErrorCode::kBusy, std::move(message)};
    }

    [[nodiscard]] static Status FormatError(std::string message) {
        return {ErrorCode::kFormatError, std::move(message)};
    }

    [[nodiscard]] static Status VersionMismatch(std::string message) {
        return {ErrorCode::kVersionMismatch, std::move(message)};
    }

    [[nodiscard]] static Status NotFound(std::string message) {
        return {ErrorCode::kNotFound, std::move(message)};
    }

    [[nodiscard]] static Status AlreadyExists(std::string message) {
        return {ErrorCode::kAlreadyExists, std::move(message)};
    }

    [[nodiscard]] bool ok() const {
        return code_ == ErrorCode::kOk;
    }

    [[nodiscard]] ErrorCode code() const {
        return code_;
    }

    [[nodiscard]] const std::string& message() const {
        return message_;
    }

    explicit operator bool() const {
        return ok();
    }

  private:
    ErrorCode code_{ErrorCode::kOk};
    std::string message_;
};

}  // namespace fastfix::base
