#pragma once

#include <oscillate/foundation/error.hpp>
#include <oscillate/foundation/sha256.hpp>
#include <oscillate/project/model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::media {

// Version 1 is a canonical binary format, independent of host byte order and
// compiler layout. A future incompatible encoding must change this version.
inline constexpr std::uint32_t cache_key_format_version = 1;

enum class CacheArtifact : std::uint8_t { source_analysis = 1, clip_processed = 2 };

// The producer owns canonical_settings serialization. It must include every
// setting that changes output, use deterministic byte order, and bump its
// algorithm_version whenever those bytes or their interpretation change.
// Include tempo_sha256 only when the artifact depends on tempo state; include
// project_rate only when output depends on the project's nominal rate.
struct CacheDescriptor {
  CacheArtifact artifact;
  project::SourceRecord source;
  std::optional<project::ClipId> clip;
  std::optional<std::uint32_t> project_rate;
  std::string algorithm_id;
  std::uint32_t algorithm_version;
  std::uint32_t quality;
  std::vector<std::byte> canonical_settings;
  std::optional<std::string> tempo_sha256;
};

class CacheKey {
public:
  [[nodiscard]] std::string hex() const;
  bool operator==(const CacheKey&) const = default;

private:
  friend foundation::Result<CacheKey> make_cache_key(const CacheDescriptor&);
  explicit CacheKey(foundation::Sha256Digest digest) noexcept : digest_(digest) {}
  foundation::Sha256Digest digest_;
};

// Control/worker side only. The key describes semantic input, not a project
// revision, source path, timestamp, or job generation. SourceId is deliberately
// omitted so identical immutable encoded bytes can share source-only results.
[[nodiscard]] foundation::Result<CacheKey> make_cache_key(const CacheDescriptor& descriptor);

} // namespace oscillate::media
