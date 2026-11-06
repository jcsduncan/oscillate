#pragma once

// Source media records and their reviewed tempo, onset and key analysis.

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

// A source record describes immutable encoded bytes. Clips retain SourceId even
// when the record or referenced media is unavailable, so edits remain recoverable.
enum class SourceEncoding : std::uint8_t { wav = 1, aiff = 2, flac = 3, mp3 = 4 };
inline constexpr std::size_t max_source_records = 10'000;
inline constexpr std::size_t max_external_source_path_bytes = 4096;
struct ExternalSourceLink {
  std::string last_known_path;
  bool operator==(const ExternalSourceLink&) const = default;
};
struct SourceRecord {
  SourceId id;
  SourceEncoding encoding;
  std::string sha256; // Exactly 64 lowercase hex digits of the encoded source bytes.
  std::uint64_t byte_length;
  std::uint32_t sample_rate;
  std::uint32_t channels;
  std::uint64_t frames;
  // Absent means a managed asset. An external link is never an asset or cache entry.
  std::optional<ExternalSourceLink> external_link = std::nullopt;
  bool operator==(const SourceRecord&) const = default;
};
[[nodiscard]] foundation::Result<void> validate_source_record(const SourceRecord& source);

inline constexpr std::size_t max_tempo_markers_per_source = 20'000;
inline constexpr std::size_t max_tempo_markers_per_project = 200'000;

// Essential, source-relative review state. Generated suggestions and user
// markers have distinct provenance; the corrected BPM takes precedence for
// display but does not silently conform the global tempo map.
// Durable transient (onset) review for warp suggestions:
// generated_onset_frames come from the warp transient detector and are
// replaced only by an accepted reanalysis; dismissed_onset_frames are user
// corrections ("not a transient") that reanalysis never replaces. A generated
// onset within max_onset_dismiss_distance source frames of a dismissed one is
// dropped. Applied warp markers live on clips, never here.
inline constexpr std::uint64_t max_onset_dismiss_distance = 64;
struct SourceTempoReview {
  SourceId source;
  double detected_bpm = 0.0;
  double confidence = 0.0;
  std::optional<double> corrected_bpm = std::nullopt;
  std::vector<std::uint64_t> generated_beat_frames = {};
  std::vector<std::uint64_t> user_beat_frames = {};
  bool suggestions_truncated = false;
  std::vector<std::uint64_t> generated_onset_frames = {};
  std::vector<std::uint64_t> dismissed_onset_frames = {};
  bool onsets_truncated = false;
  // The source frame of a bar's first beat, set by the user
  // (the downbeat flag). Absent, the first beat of the grid is bar 1.
  std::optional<std::uint64_t> downbeat_frame = std::nullopt;
  bool operator==(const SourceTempoReview&) const = default;
};
// True when `frame` lies within max_onset_dismiss_distance of a dismissed
// onset (`dismissed` ascending).
[[nodiscard]] bool onset_dismissed(std::span<const std::uint64_t> dismissed,
                                   std::uint64_t frame) noexcept;

// Pitch class uses C=0 through B=11. This type is independent of the worker.
enum class KeyMode : std::uint8_t { major = 1, minor = 2 };
enum class KeyRelation : std::uint8_t { relative = 1, dominant = 2, subdominant = 3 };
struct MusicalKey {
  std::uint8_t tonic = 0;
  KeyMode mode = KeyMode::major;
  bool operator==(const MusicalKey&) const = default;
};
struct CompatibleKey {
  MusicalKey key;
  KeyRelation relation;
  bool operator==(const CompatibleKey&) const = default;
};
inline constexpr std::size_t max_compatible_keys_per_source = 3;
struct SourceKeyReview {
  SourceId source;
  std::optional<MusicalKey> detected;
  double confidence = 0.0;
  std::vector<CompatibleKey> compatible;
  std::optional<MusicalKey> correction;
  bool operator==(const SourceKeyReview&) const = default;
};

} // namespace oscillate::project
