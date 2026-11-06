#include "oscillate/application/key_cache.hpp"

#include "source_analysis_cache.hpp"

#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/media/cache_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace oscillate::application {
namespace {
template <typename T> using Result = foundation::Result<T>;

constexpr analysis::KeyOptions key_options{};

} // namespace

Result<media::CacheKey> key_cache_key(const project::SourceRecord& source) {
  std::vector<std::byte> settings;
  settings.reserve(8);
  detail::append_u64(settings, key_options.max_windows);
  return media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                .source = source,
                                .clip = std::nullopt,
                                .project_rate = std::nullopt,
                                .algorithm_id = "musical-key-suggestion",
                                .algorithm_version = 2,
                                .quality = 1,
                                .canonical_settings = std::move(settings),
                                .tempo_sha256 = std::nullopt});
}

Result<KeyCacheResult>
load_key_suggestion(persistence::ProjectStore& store, media::CacheStore* cache,
                    const project::SourceRecord& source, std::uint64_t generation,
                    const std::function<bool()>& cancelled, const std::stop_token& stop) {
  return detail::load_cached_source_analysis<KeyCacheResult, decltype(KeyCacheResult::estimate),
                                             analysis::KeyIdentity>(
      {store, cache, source, generation, cancelled, stop, "Key request cancelled", true},
      &KeyCacheResult::estimate, [&] { return key_cache_key(source); },
      [](const auto& payload, const auto& identity, const auto& metadata) {
        return analysis::decode_key_cache(payload, identity, metadata);
      },
      [&](auto& reader, const auto& identity) {
        return analysis::analyze_key(reader, identity, key_options, cancelled);
      },
      [&](const auto& value) { return analysis::encode_key_cache(value, source.channels); });
}

} // namespace oscillate::application
