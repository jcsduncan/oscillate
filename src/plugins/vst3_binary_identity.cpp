#include "vst3_binary_identity.hpp"

#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"

#include <array>
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

std::optional<Vst3BinaryIdentity> fingerprint_vst3_binary(const std::filesystem::path& bundle) {
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
  if (error || !is_within(canonical_binary, canonical_bundle) ||
      !std::filesystem::is_regular_file(canonical_binary, error) || error)
    return std::nullopt;
  const auto size = std::filesystem::file_size(canonical_binary, error);
  if (error || size == 0 || size > max_binary_bytes)
    return std::nullopt;
  const auto modified_before = std::filesystem::last_write_time(canonical_binary, error);
  if (error)
    return std::nullopt;
  std::ifstream input(canonical_binary, std::ios::binary);
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
  valid = valid && input.eof() && total == size;
  auto digest = hasher.value().finish();
  const auto modified_after = std::filesystem::last_write_time(canonical_binary, error);
  if (!valid || !digest || error || modified_before != modified_after)
    return std::nullopt;
  auto hex = foundation::to_hex(std::span(digest.value()));
  return Vst3BinaryIdentity{canonical_bundle, canonical_binary, size, std::move(hex)};
}

} // namespace oscillate::plugins::detail
