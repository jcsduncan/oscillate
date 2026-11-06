#pragma once

#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/clay_dsp/take.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/media/cache_store.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

// Clay in playback and export (ADR-0081 phase 2). A clip with a Clay take
// plays a rendered copy of its source: same rate, channels and frame count,
// with the edited notes rendered in. Every path that reads a clip's audio
// (routed playback, warp, export) swaps the source record for that render's
// record and opens the render instead, so warp, fades and export apply on top
// exactly as they do for the source. Renders and analyses are disposable
// derived data in the project's cache directory, recomputed when missing.
// Nothing here may run on the audio callback.
namespace oscillate::processing::clay {

// Rendered takes live as float WAVs under this directory of the project cache.
inline constexpr std::string_view render_directory = "clay";
// Bump when the render's bytes change for the same inputs.
inline constexpr std::uint32_t render_version = 1;

[[nodiscard]] clay_dsp::Mode dsp_mode(project::ClayMode mode) noexcept;
// The key's scale; absent is chromatic.
[[nodiscard]] clay_dsp::Scale dsp_scale(const std::optional<project::MusicalKey>& key);

// The take's edits as the DSP takes them, duplicates after the notes' own
// edits in voice order. With `for_render` a muted note or voice sounds at the
// lowest gain; otherwise its gain is kept for the macros.
[[nodiscard]] std::vector<clay_dsp::NoteEdit> dsp_edits(const project::ClayTake& take,
                                                        bool for_render);
// The take with its edits replaced by a macro's result. Identity edits are
// dropped; a voice keeps its mute when it survives.
[[nodiscard]] project::ClayTake with_dsp_edits(const project::ClayTake& take,
                                               std::span<const clay_dsp::NoteEdit> edits);

// Replays the take's splits and joins on an analysed take.
[[nodiscard]] foundation::Result<void> apply_ops(clay_dsp::Take& take,
                                                 std::span<const project::ClayStructuralOp> ops);

// A whole source decoded at its own rate. `right` is empty for mono.
struct DecodedSource {
  std::vector<float> left;
  std::vector<float> right;
  std::uint32_t sample_rate = 0;
};
[[nodiscard]] foundation::Result<DecodedSource>
decode_source(const persistence::ProjectStore& store, const project::SourceRecord& source,
              const std::stop_token& stop = {});

// The source's analysis in `mode`, from the cache when it holds one (a null
// cache always analyses). Deterministic, so a miss recomputes the same notes.
[[nodiscard]] foundation::Result<clay_dsp::Take>
analysed_take(media::CacheStore* cache, const project::SourceRecord& source, project::ClayMode mode,
              const DecodedSource& audio, const std::stop_token& stop = {});

// The analysis with the take's splits and joins replayed: what the edits'
// note ids refer to.
[[nodiscard]] foundation::Result<clay_dsp::Take> edited_take(media::CacheStore* cache,
                                                             const project::SourceRecord& source,
                                                             const project::ClayTake& take,
                                                             const DecodedSource& audio,
                                                             const std::stop_token& stop = {});

// The record of a take's render: the source's rate, channels and frames, a
// float WAV, its sha256 field the render's cache key. A pure function of the
// source record and the take, so every path names the same render.
[[nodiscard]] foundation::Result<project::SourceRecord>
rendered_record(const project::SourceRecord& source, const project::ClayTake& take);

// Renders `audio` (the decoded `source`) with the take's edits.
[[nodiscard]] foundation::Result<void>
render_take(media::CacheStore* cache, const project::SourceRecord& source,
            const project::ClayTake& take, const DecodedSource& audio, std::span<float> left,
            std::span<float> right, const std::stop_token& stop = {});

// An open render: a pinned descriptor and the record that describes it.
class RenderedAudio {
public:
  RenderedAudio(int fd, project::SourceRecord record) noexcept
      : fd_(fd), record_(std::move(record)) {}
  RenderedAudio(const RenderedAudio&) = delete;
  RenderedAudio& operator=(const RenderedAudio&) = delete;
  RenderedAudio(RenderedAudio&& other) noexcept;
  RenderedAudio& operator=(RenderedAudio&& other) noexcept;
  ~RenderedAudio();
  // The caller keeps ownership; readers duplicate it.
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] const project::SourceRecord& record() const noexcept { return record_; }

private:
  int fd_ = -1;
  project::SourceRecord record_;
};

// Opens the take's render, rendering it first when the cache lacks it (or
// holds a damaged one). Worker-side: verifying, decoding, analysing and
// rendering can take seconds for a long source.
[[nodiscard]] foundation::Result<RenderedAudio>
open_rendered(const persistence::ProjectStore& store, const project::SourceRecord& source,
              const project::ClayTake& take, const std::stop_token& stop = {});

// Whether the take's render is already in the cache (any thread but the
// audio callback; a cheap name lookup, not a verification).
[[nodiscard]] bool render_cached(const persistence::ProjectStore& store,
                                 const project::SourceRecord& source,
                                 const project::ClayTake& take);

// The clip's source record as its audio paths read it: the Clay render's
// record when the clip has a take, else the source itself.
[[nodiscard]] foundation::Result<project::SourceRecord>
clip_audio_record(const project::Clip& clip, const project::SourceRecord& source);

} // namespace oscillate::processing::clay
