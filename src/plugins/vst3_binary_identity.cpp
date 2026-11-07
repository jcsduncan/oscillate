#include "vst3_binary_identity.hpp"

#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"

#include <sys/stat.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <span>

namespace oscillate::plugins::detail {
namespace {

constexpr std::uint64_t max_binary_bytes = 512ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool is_within(const std::filesystem::path& child,
                             const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  if (relative.empty() || relative.is_absolute())
    return false;
  for (const auto& part : relative) {
    if (part == "..")
      return false;
  }
  return true;
}

} // namespace

std::optional<ResolvedVst3Binary> resolve_vst3_binary(const std::filesystem::path& bundle) {
  std::error_code error;
  if (bundle.extension() != ".vst3")
    return std::nullopt;
  const auto canonical_bundle = std::filesystem::canonical(bundle, error);
  if (error || !std::filesystem::is_directory(canonical_bundle, error) || error ||
      canonical_bundle.extension() != ".vst3")
    return std::nullopt;
  const auto nominal =
      canonical_bundle / "Contents" / "x86_64-linux" / (canonical_bundle.stem().string() + ".so");
  const auto canonical_binary = std::filesystem::canonical(nominal, error);
  if (error || !is_within(canonical_binary, canonical_bundle))
    return std::nullopt;
  struct stat status{};
  if (::stat(canonical_binary.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > max_binary_bytes)
    return std::nullopt;
  return ResolvedVst3Binary{
      canonical_bundle, canonical_binary,
      Vst3BinaryStamp{.device = static_cast<std::uint64_t>(status.st_dev),
                      .inode = static_cast<std::uint64_t>(status.st_ino),
                      .byte_size = static_cast<std::uint64_t>(status.st_size),
                      .modified_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
                      .modified_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec)}};
}

std::optional<Vst3HashedBinary> hash_resolved_vst3_binary(const std::filesystem::path& bundle) {
  const auto before = resolve_vst3_binary(bundle);
  if (!before)
    return std::nullopt;
  std::ifstream input(before->canonical_binary, std::ios::binary);
  if (!input)
    return std::nullopt;
  auto hasher = foundation::Sha256::create();
  if (!hasher)
    return std::nullopt;
  std::array<char, 64UL * 1024UL> buffer{};
  std::uint64_t total = 0;
  bool valid = true;
  while (valid && input) {
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    if (count > 0) {
      total += static_cast<std::uint64_t>(count);
      valid = total <= max_binary_bytes &&
              hasher.value()
                  .update(std::as_bytes(std::span(buffer).first(static_cast<std::size_t>(count))))
                  .has_value();
    }
  }
  valid = valid && input.eof() && total == before->stamp.byte_size;
  auto digest = hasher.value().finish();
  // The digest belongs to the stamp only when nothing replaced or rewrote
  // the file while it was read.
  const auto after = resolve_vst3_binary(bundle);
  if (!valid || !digest || !after || after->canonical_bundle != before->canonical_bundle ||
      after->canonical_binary != before->canonical_binary || after->stamp != before->stamp)
    return std::nullopt;
  auto hex = foundation::to_hex(std::span(digest.value()));
  return Vst3HashedBinary{
      {before->canonical_bundle, before->canonical_binary, before->stamp.byte_size, std::move(hex)},
      before->stamp};
}

std::optional<Vst3BinaryIdentity> fingerprint_vst3_binary(const std::filesystem::path& bundle) {
  auto hashed = hash_resolved_vst3_binary(bundle);
  if (!hashed)
    return std::nullopt;
  return std::move(hashed->identity);
}

} // namespace oscillate::plugins::detail
