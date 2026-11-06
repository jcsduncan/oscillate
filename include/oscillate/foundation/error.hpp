#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace oscillate::foundation {

// Stable numeric values also identify failures in diagnostic records. Append new codes.
enum class ErrorCode : std::uint8_t {
  invalid_argument = 1,
  not_found = 2,
  conflict = 3,
  unsupported = 4,
  resource_exhausted = 5,
  io_failure = 6,
  corrupt_data = 7,
  unavailable = 8,
  internal = 9,
  // The caller or its stop token cancelled the work; nothing is wrong.
  cancelled = 10,
};

// Error messages are for control/UI threads. Do not construct one in the audio callback.
struct Error {
  ErrorCode code;
  std::string message;
};

template <typename T> class [[nodiscard]] Result {
  static_assert(!std::is_same_v<T, Error>);
  static_assert(!std::is_reference_v<T>);

public:
  [[nodiscard]] static Result success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }
  [[nodiscard]] static Result failure(Error error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept { return state_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  // Call only after checking has_value(). A wrong alternative throws std::bad_variant_access.
  [[nodiscard]] T& value() & { return std::get<0>(state_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(state_); }
  // An rvalue Result hands its payload out by value, so nothing refers into a dead Result.
  [[nodiscard]] T value() && { return std::get<0>(std::move(state_)); }
  [[nodiscard]] Error& error() & { return std::get<1>(state_); }
  [[nodiscard]] const Error& error() const& { return std::get<1>(state_); }

private:
  template <typename... Args>
  explicit Result(std::in_place_index_t<0>, Args&&... args)
      : state_(std::in_place_index<0>, std::forward<Args>(args)...) {}
  template <typename... Args>
  explicit Result(std::in_place_index_t<1>, Args&&... args)
      : state_(std::in_place_index<1>, std::forward<Args>(args)...) {}

  std::variant<T, Error> state_;
};

template <> class [[nodiscard]] Result<void> {
public:
  [[nodiscard]] static Result success() { return Result(); }
  [[nodiscard]] static Result failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool has_value() const noexcept { return state_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] Error& error() & { return std::get<1>(state_); }
  [[nodiscard]] const Error& error() const& { return std::get<1>(state_); }

private:
  Result() = default;
  explicit Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}

  std::variant<std::monostate, Error> state_;
};

} // namespace oscillate::foundation
