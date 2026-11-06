#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oscillate::analysis {

struct KeyIdentity {
  std::string source_key;
  std::uint64_t generation = 0;
};

enum class KeyMode : std::uint8_t { major, minor };
enum class KeyRelation : std::uint8_t { relative, dominant, subdominant };

struct MusicalKey {
  // C = 0, C sharp = 1, ..., B = 11. This is pitch class, not spelling.
  std::uint8_t tonic = 0;
  KeyMode mode = KeyMode::major;
};

struct CompatibleKey {
  MusicalKey key;
  KeyRelation relation;
};

struct KeyOptions {
  // Uniformly sample a bounded number of source windows. A larger value can
  // improve coverage of long material but increases CPU cost linearly.
  std::size_t max_windows = 1'024;
};

struct KeyEstimate {
  KeyIdentity identity;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  // No key means that the evidence is insufficient or ambiguous. Confidence
  // is 0 in that case. This result is a suggestion, never a project edit.
  std::optional<MusicalKey> key;
  double confidence = 0.0;
  std::vector<CompatibleKey> compatible_keys;
};

// Worker-only analysis. Caller must validate identity/generation before
// installing a result. Cancellation and decoder failure return no partial
// result. No method of AudioReader or this function is callback-safe.
[[nodiscard]] foundation::Result<KeyEstimate>
analyze_key(media::AudioReader& reader, KeyIdentity identity, KeyOptions options = {},
            const std::function<bool()>& cancelled = {});

} // namespace oscillate::analysis
