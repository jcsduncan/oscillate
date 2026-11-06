#include "oscillate/foundation/sha256.hpp"

#include <openssl/evp.h>
#include <utility>

namespace oscillate::foundation {

struct Sha256::Context {
  EVP_MD_CTX* digest = nullptr;
};

void Sha256::ContextDeleter::operator()(Context* context) const noexcept {
  EVP_MD_CTX_free(context->digest);
  delete context;
}

Sha256::Sha256(std::unique_ptr<Context, ContextDeleter> context) noexcept
    : context_(std::move(context)) {}

Result<Sha256> Sha256::create() {
  std::unique_ptr<Context, ContextDeleter> context(new Context{EVP_MD_CTX_new()});
  if (context->digest == nullptr || EVP_DigestInit_ex(context->digest, EVP_sha256(), nullptr) != 1)
    return Result<Sha256>::failure({ErrorCode::internal, "SHA-256 initialization failed"});
  return Result<Sha256>::success(Sha256(std::move(context)));
}

Result<void> Sha256::update(std::span<const std::byte> bytes) {
  if (!context_ || EVP_DigestUpdate(context_->digest, bytes.data(), bytes.size()) != 1)
    return Result<void>::failure({ErrorCode::internal, "SHA-256 update failed"});
  return Result<void>::success();
}

Result<Sha256Digest> Sha256::finish() {
  Sha256Digest digest{};
  unsigned length = 0;
  const auto finished =
      context_ && EVP_DigestFinal_ex(context_->digest, digest.data(), &length) == 1;
  context_.reset();
  if (!finished || length != digest.size())
    return Result<Sha256Digest>::failure({ErrorCode::internal, "SHA-256 finalization failed"});
  return Result<Sha256Digest>::success(digest);
}

Result<Sha256Digest> sha256(std::span<const std::byte> bytes) {
  Sha256Digest digest{};
  unsigned length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.size())
    return Result<Sha256Digest>::failure({ErrorCode::internal, "SHA-256 failed"});
  return Result<Sha256Digest>::success(digest);
}

} // namespace oscillate::foundation
