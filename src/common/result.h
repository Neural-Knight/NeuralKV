#pragma once

#include <cassert>
#include <utility>
#include <variant>

#include "common/status.h"

namespace neuralkv {

// Holds either a T or the Status describing why there isn't one. Callers
// must check ok() before calling value(); accessing value() on an error
// result is a programmer error and asserts.
template <typename T>
class Result {
 public:
  Result(T value) : state_(std::move(value)) {}
  Result(Status status) : state_(std::move(status)) {
    assert(!std::get<Status>(state_).ok() &&
           "Result constructed with an Ok status carries no value");
  }

  bool ok() const { return std::holds_alternative<T>(state_); }

  const Status& status() const {
    static const Status kOk = Status::Ok();
    return ok() ? kOk : std::get<Status>(state_);
  }

  T& value() & {
    assert(ok());
    return std::get<T>(state_);
  }
  const T& value() const& {
    assert(ok());
    return std::get<T>(state_);
  }
  T&& value() && {
    assert(ok());
    return std::get<T>(std::move(state_));
  }

 private:
  std::variant<T, Status> state_;
};

}  // namespace neuralkv
