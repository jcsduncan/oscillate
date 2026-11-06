#include <oscillate/media/cache_identity.hpp>

#include <oscillate/foundation/hex.hpp>
#include <oscillate/foundation/sha256.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace oscillate::media {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::size_t max_algorithm_id_bytes = 64;
constexpr std::size_t max_settings_bytes = std::size_t{1024} * 1024U;

[[nodiscard]] Result<CacheKey> invalid(const char* message) {
  return Result<CacheKey>::failure({ErrorCode::invalid_argument, message});
}

void u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<unsigned char>(value >> shift));
  }
}

void u64(std::vector<unsigned char>& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes.push_back(static_cast<unsigned char>(value >> shift));
  }
}

[[nodiscard]] int nibble(char digit) noexcept {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  return -1;
}

[[nodiscard]] bool append_digest(std::vector<unsigned char>& bytes, std::string_view digest) {
  if (digest.size() != 64) {
    return false;
  }
  for (std::size_t index = 0; index < digest.size(); index += 2) {
    const auto high = nibble(digest[index]);
    const auto low = nibble(digest[index + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    bytes.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

[[nodiscard]] bool algorithm_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= max_algorithm_id_bytes &&
         std::ranges::all_of(name, [](char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                  ch == '-';
         });
}

} // namespace

std::string CacheKey::hex() const { return foundation::to_hex(std::span(digest_)); }

Result<CacheKey> make_cache_key(const CacheDescriptor& descriptor) {
  if (descriptor.artifact != CacheArtifact::source_analysis &&
      descriptor.artifact != CacheArtifact::clip_processed) {
    return invalid("unsupported cache artifact type");
  }
  const auto encoding = descriptor.source.encoding;
  if ((encoding != project::SourceEncoding::wav && encoding != project::SourceEncoding::aiff &&
       encoding != project::SourceEncoding::flac && encoding != project::SourceEncoding::mp3) ||
      (encoding == project::SourceEncoding::wav ? descriptor.source.byte_length < 44
                                                : descriptor.source.byte_length == 0) ||
      descriptor.source.sample_rate < 8'000 || descriptor.source.sample_rate > 384'000 ||
      descriptor.source.channels == 0 || descriptor.source.channels > 32 ||
      descriptor.source.frames == 0 ||
      descriptor.source.frames >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return invalid("invalid cache source identity");
  }
  if ((descriptor.artifact == CacheArtifact::source_analysis && descriptor.clip) ||
      (descriptor.artifact == CacheArtifact::clip_processed &&
       (!descriptor.clip || !descriptor.project_rate))) {
    return invalid("invalid cache artifact dependencies");
  }
  if ((descriptor.project_rate && *descriptor.project_rate == 0) ||
      !algorithm_name(descriptor.algorithm_id) || descriptor.algorithm_version == 0 ||
      descriptor.quality == 0 || descriptor.canonical_settings.size() > max_settings_bytes) {
    return invalid("invalid cache algorithm or settings metadata");
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(256 + descriptor.algorithm_id.size() + descriptor.canonical_settings.size());
  constexpr std::string_view domain = "oscillate-cache-key";
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  u32(bytes, cache_key_format_version);
  bytes.push_back(static_cast<unsigned char>(descriptor.artifact));
  bytes.push_back(static_cast<unsigned char>(descriptor.source.encoding));
  u64(bytes, descriptor.source.byte_length);
  if (!append_digest(bytes, descriptor.source.sha256)) {
    return invalid("invalid source SHA-256 for cache key");
  }
  bytes.push_back(descriptor.clip ? 1U : 0U);
  if (descriptor.clip) {
    const auto bits = descriptor.clip->bits();
    u64(bytes, bits.high);
    u64(bytes, bits.low);
  }
  bytes.push_back(descriptor.project_rate ? 1U : 0U);
  if (descriptor.project_rate) {
    u32(bytes, *descriptor.project_rate);
  }
  bytes.push_back(static_cast<unsigned char>(descriptor.algorithm_id.size()));
  bytes.insert(bytes.end(), descriptor.algorithm_id.begin(), descriptor.algorithm_id.end());
  u32(bytes, descriptor.algorithm_version);
  u32(bytes, descriptor.quality);
  u32(bytes, static_cast<std::uint32_t>(descriptor.canonical_settings.size()));
  for (const auto value : descriptor.canonical_settings) {
    bytes.push_back(std::to_integer<unsigned char>(value));
  }
  bytes.push_back(descriptor.tempo_sha256 ? 1U : 0U);
  if (descriptor.tempo_sha256 && !append_digest(bytes, *descriptor.tempo_sha256)) {
    return invalid("invalid tempo SHA-256 for cache key");
  }

  auto digest = foundation::sha256(std::as_bytes(std::span(bytes)));
  if (!digest) {
    return Result<CacheKey>::failure({ErrorCode::internal, "cache key digest failed"});
  }
  return Result<CacheKey>::success(CacheKey(digest.value()));
}

} // namespace oscillate::media
