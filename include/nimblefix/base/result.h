#pragma once

#include <cassert>
#include <optional>
#include <utility>

#include "nimblefix/base/status.h"

namespace nimble::base {

template<typename T>
class Result
{
public:
  Result(T value)
    : status_(Status::Ok())
    , value_(std::move(value))
  {
  }

  Result(Status status)
    : status_(std::move(status))
  {
    assert(!status_.ok());
  }

  [[nodiscard]] bool ok() const { return status_.ok(); }

  [[nodiscard]] const Status& status() const { return status_; }

  [[nodiscard]] const T& value() const&
  {
    assert(ok());
    return *value_;
  }

  [[nodiscard]] T& value() &
  {
    assert(ok());
    return *value_;
  }

  [[nodiscard]] T&& value() &&
  {
    assert(ok());
    return std::move(*value_);
  }

private:
  Status status_;
  std::optional<T> value_;
};

} // namespace nimble::base
