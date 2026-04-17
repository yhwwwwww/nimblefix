#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fastfix::base {

enum class ErrorCode
{
  kOk = 0,
  kInvalidArgument,
  kIoError,
  kBusy,
  kFormatError,
  kVersionMismatch,
  kNotFound,
  kAlreadyExists,
};

class Status
{
public:
  Status() noexcept = default;

  Status(ErrorCode code, std::string message)
    : code_(code)
    , message_(std::make_unique<std::string>(std::move(message)))
  {
  }

  Status(const Status& other)
    : code_(other.code_)
    , message_(other.message_ ? std::make_unique<std::string>(*other.message_) : nullptr)
  {
  }

  Status& operator=(const Status& other)
  {
    if (this != &other) {
      code_ = other.code_;
      message_ = other.message_ ? std::make_unique<std::string>(*other.message_) : nullptr;
    }
    return *this;
  }

  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) noexcept = default;

  [[nodiscard]] static Status Ok() noexcept { return {}; }

  [[nodiscard]] static Status InvalidArgument(std::string message)
  {
    return { ErrorCode::kInvalidArgument, std::move(message) };
  }

  [[nodiscard]] static Status IoError(std::string message) { return { ErrorCode::kIoError, std::move(message) }; }

  [[nodiscard]] static Status Busy(std::string message) { return { ErrorCode::kBusy, std::move(message) }; }

  [[nodiscard]] static Status FormatError(std::string message)
  {
    return { ErrorCode::kFormatError, std::move(message) };
  }

  [[nodiscard]] static Status VersionMismatch(std::string message)
  {
    return { ErrorCode::kVersionMismatch, std::move(message) };
  }

  [[nodiscard]] static Status NotFound(std::string message) { return { ErrorCode::kNotFound, std::move(message) }; }

  [[nodiscard]] static Status AlreadyExists(std::string message)
  {
    return { ErrorCode::kAlreadyExists, std::move(message) };
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  [[nodiscard]] std::string_view message() const noexcept
  {
    return message_ ? std::string_view(*message_) : std::string_view();
  }

  explicit operator bool() const noexcept { return ok(); }

private:
  ErrorCode code_{ ErrorCode::kOk };
  std::unique_ptr<std::string> message_;
};

} // namespace fastfix::base
