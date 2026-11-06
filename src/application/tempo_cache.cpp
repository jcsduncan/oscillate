#include "oscillate/application/tempo_cache.hpp"

#include "source_analysis_cache.hpp"

#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/media/cache_identity.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {
template <typename T> using Result = foundation::Result<T>;

constexpr analysis::TempoOptions tempo_options{};

} // namespace

Result<media::CacheKey> tempo_cache_key(const project::SourceRecord& source) {
  std::vector<std::byte> settings;
  settings.reserve(40);
  detail::append_u64(settings, std::bit_cast<std::uint64_t>(tempo_options.minimum_bpm));
  detail::append_u64(settings, std::bit_cast<std::uint64_t>(tempo_options.maximum_bpm));
  detail::append_u64(settings, tempo_options.max_envelope_bins);
  detail::append_u64(settings, tempo_options.max_suggestions);
  detail::append_u64(settings, tempo_options.max_onsets);
  return media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                .source = source,
                                .clip = std::nullopt,
                                .project_rate = std::nullopt,
                                .algorithm_id = "tempo-suggestions",
                                .algorithm_version = 3,
                                .quality = 1,
                                .canonical_settings = std::move(settings),
                                .tempo_sha256 = std::nullopt});
}

Result<TempoCacheResult>
load_tempo_suggestions(persistence::ProjectStore& store, media::CacheStore* cache,
                       const project::SourceRecord& source, std::uint64_t generation,
                       const std::function<bool()>& cancelled, const std::stop_token& stop) {
  return detail::load_cached_source_analysis<TempoCacheResult, decltype(TempoCacheResult::estimate),
                                             analysis::TempoIdentity>(
      {store, cache, source, generation, cancelled, stop, "Tempo request cancelled", true},
      &TempoCacheResult::estimate, [&] { return tempo_cache_key(source); },
      [](const auto& payload, const auto& identity, const auto& metadata) {
        return analysis::decode_tempo_cache(payload, identity, metadata);
      },
      [&](auto& reader, const auto& identity) {
        return analysis::analyze_tempo(reader, identity, tempo_options, cancelled);
      },
      [&](const auto& value) { return analysis::encode_tempo_cache(value, source.channels); });
}

} // namespace oscillate::application
