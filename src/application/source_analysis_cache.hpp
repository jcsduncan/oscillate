#pragma once

#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/cache_identity.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The shared worker path of the key, tempo and waveform caches: verify the
// managed source, reuse a valid cached result or analyse, then publish.
namespace oscillate::application::detail {

inline constexpr unsigned bits_per_byte = 8;
inline constexpr unsigned bits_per_u64 = 64;
inline constexpr std::uint64_t byte_mask = 0xffU;

// Little-endian bytes for canonical cache-key settings.
inline void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < bits_per_u64; shift += bits_per_byte)
    bytes.push_back(static_cast<std::byte>((value >> shift) & byte_mask));
}

struct SourceAnalysisRequest {
  persistence::ProjectStore& store;
  media::CacheStore* cache;
  const project::SourceRecord& source;
  std::uint64_t generation;
  const std::function<bool()>& cancelled;
  const std::stop_token& stop;
  std::string_view cancelled_message;
  bool reuse_cached = true;
};

// `Output` is the public result type: `field` receives the value; it also has
// `cache_hit` and `cache_warning`. `make_key()` returns Result<CacheKey>;
// `decode(payload, identity, metadata)`, `analyze(reader, identity)` and
// `encode(value)` wrap the analysis module's codec and analyser.
template <typename Output, typename Value, typename Identity, typename MakeKey, typename Decode,
          typename Analyze, typename Encode>
[[nodiscard]] foundation::Result<Output>
load_cached_source_analysis(const SourceAnalysisRequest& request, Value Output::* field,
                            MakeKey make_key, Decode decode, Analyze analyze, Encode encode) {
  using Result = foundation::Result<Output>;
  const auto stopped = [&] {
    return request.stop.stop_requested() || (request.cancelled && request.cancelled());
  };
  const auto cancelled = [&] {
    return Result::failure(
        {foundation::ErrorCode::cancelled, std::string(request.cancelled_message)});
  };
  if (request.generation == 0 || stopped())
    return cancelled();
  auto descriptor = request.store.open_verified_source(request.source, request.stop);
  if (!descriptor)
    return Result::failure(descriptor.error());
  auto reader = media::AudioReader::open_fd(descriptor.value(), request.source);
  (void)::close(descriptor.value());
  if (!reader)
    return Result::failure(reader.error());
  if (stopped())
    return cancelled();
  auto key = make_key();
  if (!key)
    return Result::failure(key.error());
  const Identity identity{key.value().hex(), request.generation};
  const analysis::SourceMetadata metadata{request.source.sample_rate, request.source.channels,
                                          request.source.frames};
  Output output;
  if (request.cache != nullptr && request.reuse_cached) {
    auto cached = request.cache->get(key.value());
    if (cached) {
      auto payload = std::move(cached.value());
      if (payload.has_value()) {
        auto decoded = decode(payload.value(), identity, metadata);
        if (decoded && !stopped()) {
          output.*field = std::move(decoded.value());
          output.cache_hit = true;
          return Result::success(std::move(output));
        }
      }
    } else {
      output.cache_warning = cached.error().message;
    }
  }
  auto analyzed = analyze(reader.value(), identity);
  if (!analyzed)
    return Result::failure(analyzed.error());
  output.*field = std::move(analyzed.value());
  if (request.cache != nullptr && !stopped()) {
    auto encoded = encode(output.*field);
    if (encoded) {
      auto written = request.cache->put(key.value(), encoded.value());
      if (!written)
        output.cache_warning = written.error().message;
    } else {
      output.cache_warning = encoded.error().message;
    }
  }
  if (stopped())
    return cancelled();
  return Result::success(std::move(output));
}

} // namespace oscillate::application::detail
