#include "oscillate/application/waveform_cache.hpp"

#include "source_analysis_cache.hpp"

#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/media/cache_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {

template <typename T> using Result = foundation::Result<T>;

// Keep enough source detail for a multi-minute clip at a few-bar zoom. The
// analyzer may increase the base bucket width for longer material while the
// total in-memory/on-disk summary remains bounded.
constexpr analysis::WaveformOptions waveform_options{64, 250'000, 8};

Result<media::CacheKey> waveform_key(const project::SourceRecord& source) {
  std::vector<std::byte> settings;
  settings.reserve(24);
  detail::append_u64(settings, waveform_options.minimum_frames_per_bucket);
  detail::append_u64(settings, waveform_options.max_total_buckets);
  detail::append_u64(settings, waveform_options.max_levels);
  return media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                .source = source,
                                .clip = std::nullopt,
                                .project_rate = std::nullopt,
                                .algorithm_id = "waveform-summary",
                                .algorithm_version = 1,
                                .quality = 1,
                                .canonical_settings = std::move(settings),
                                .tempo_sha256 = std::nullopt});
}

} // namespace

Result<WaveformCacheResult>
load_waveform_summary(persistence::ProjectStore& store, media::CacheStore* cache,
                      const project::SourceRecord& source, std::uint64_t generation,
                      const std::function<bool()>& cancelled, const std::stop_token& stop,
                      WaveformCachePolicy policy) {
  return detail::load_cached_source_analysis<
      WaveformCacheResult, decltype(WaveformCacheResult::summary), analysis::WaveformIdentity>(
      {store, cache, source, generation, cancelled, stop, "Waveform request cancelled",
       policy == WaveformCachePolicy::reuse},
      &WaveformCacheResult::summary, [&] { return waveform_key(source); },
      [](const auto& payload, const auto& identity, const auto& metadata) {
        return analysis::decode_waveform_cache(payload, identity, metadata);
      },
      [&](auto& reader, const auto& identity) {
        return analysis::analyze_waveform(reader, identity, waveform_options, cancelled);
      },
      [&](const auto& value) { return analysis::encode_waveform_cache(value); });
}

} // namespace oscillate::application
