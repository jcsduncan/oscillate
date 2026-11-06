#pragma once

#include "oscillate/foundation/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// SHA-256 for content identity (target oscillate::foundation_digest).
namespace oscillate::foundation {

inline constexpr std::size_t sha256_bytes = 32;
using Sha256Digest = std::array<std::uint8_t, sha256_bytes>;

[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::byte> bytes);
[[nodiscard]] inline Result<Sha256Digest> sha256(std::string_view text) {
  return sha256(std::as_bytes(std::span(text.data(), text.size())));
}

// Incremental hashing for streamed input.
class Sha256 {
public:
  [[nodiscard]] static Result<Sha256> create();

  [[nodiscard]] Result<void> update(std::span<const std::byte> bytes);
  // Finishes the digest. The hasher cannot be updated afterwards.
  [[nodiscard]] Result<Sha256Digest> finish();

private:
  struct Context;
  struct ContextDeleter {
    void operator()(Context* context) const noexcept;
  };
  explicit Sha256(std::unique_ptr<Context, ContextDeleter> context) noexcept;

  std::unique_ptr<Context, ContextDeleter> context_;
};

} // namespace oscillate::foundation
