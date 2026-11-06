#pragma once

#include "oscillate/analysis/key.hpp"
#include "oscillate/analysis/tempo.hpp"
#include "oscillate/analysis/waveform.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::analysis {

// Canonical little-endian bytes, independent of host layout. Waveform remains
// v1; tempo v3 is the constant-tempo analysis (ADR-0052), with bounded onset
// positions; key v1 is a distinct kind.
// Old incompatible payloads are rebuilt.
// CacheStore supplies atomic publication and a stronger outer file digest.
inline constexpr std::uint16_t analysis_cache_payload_version = 1;
inline constexpr std::uint16_t tempo_cache_payload_version = 3;
inline constexpr std::uint16_t key_cache_payload_version = 1;

struct SourceMetadata {
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint64_t frames = 0;
};

// Worker/control side only. The caller additionally compares current source
// identity and generation when installing a result after decoding.
[[nodiscard]] foundation::Result<std::vector<std::byte>>
encode_waveform_cache(const WaveformSummary& summary);
[[nodiscard]] foundation::Result<WaveformSummary>
decode_waveform_cache(std::span<const std::byte> bytes, const WaveformIdentity& expected,
                      SourceMetadata source);

// TempoEstimate does not carry channel count; pass it from the verified reader.
[[nodiscard]] foundation::Result<std::vector<std::byte>>
encode_tempo_cache(const TempoEstimate& estimate, std::uint32_t source_channels);
[[nodiscard]] foundation::Result<TempoEstimate> decode_tempo_cache(std::span<const std::byte> bytes,
                                                                   const TempoIdentity& expected,
                                                                   SourceMetadata source);

// Key identity.source_key is the canonical source-analysis cache key produced
// from encoded source bytes and key-estimator algorithm version. Generation is
// transient; decode adopts the current generation after source validation.
[[nodiscard]] foundation::Result<std::vector<std::byte>>
encode_key_cache(const KeyEstimate& estimate, std::uint32_t source_channels);
[[nodiscard]] foundation::Result<KeyEstimate> decode_key_cache(std::span<const std::byte> bytes,
                                                               const KeyIdentity& expected,
                                                               SourceMetadata source);

} // namespace oscillate::analysis
