#pragma once

#include "oscillate/export/wav_writer.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/render_graph/vst3_stage.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::persistence {
class ProjectStore;
}

namespace oscillate::exporting {

enum class RevisionApplication : std::uint8_t { pending, active, failed };

// The application copies this from its control-side revision status after
// observing graph activation. Saved project intent alone is not render proof.
struct LiveRevisionState {
  std::uint64_t model_revision = 0;
  std::uint64_t active_graph_revision = 0;
  RevisionApplication application = RevisionApplication::pending;
  std::optional<foundation::Error> failure;
};

// Optional post-render measurement of the exact interleaved stereo float
// samples handed to a float32 WAV writer at a supported output rate. Empty LKFS means the
// duration is below 400 ms or every complete block is below the absolute gate.
// Rolling readings describe the final complete window of the published file.
// True peak is a 4x intersample estimate, not a certified standard measurement.
// This report does not describe integer WAV or encoded output.
struct RenderLoudnessReport {
  std::uint64_t frames = 0;
  std::optional<double> integrated_lkfs;
  std::optional<double> final_momentary_lkfs;
  std::optional<double> final_short_term_lkfs;
  float left_sample_peak = 0.0F;
  float right_sample_peak = 0.0F;
  float left_true_peak = 0.0F;
  float right_true_peak = 0.0F;
};

struct StemLoudnessReport {
  project::TrackId track;
  RenderLoudnessReport loudness;
};

// A trimmed tail ends at the last stretch of tail_window_seconds whose level
// is within tail_quiet_below_music_db of the music's loudest such stretch
// (or above tail_floor_dbfs, whichever is higher), then fades over
// tail_fade_seconds. Noise beds (Dust) and long reverb ends stop with the
// music instead of running for every effect's worst-case tail.
inline constexpr double tail_quiet_below_music_db = 50.0;
inline constexpr double tail_floor_dbfs = -90.0;
inline constexpr double tail_window_seconds = 0.4;
inline constexpr double tail_fade_seconds = 0.05;

struct MasterRenderOptions {
  std::filesystem::path target;
  WavFormat format = WavFormat::float32;
  std::uint64_t dither_seed = 0;
  // Zero selects the project rate. Otherwise one of 44.1/48/88.2/96 kHz.
  std::uint32_t sample_rate = 0;
  // Optional output. Set only after a successful float32 WAV render.
  // Requesting a report for another format rejects the render.
  RenderLoudnessReport* loudness_report = nullptr;
  // End the file where the sound after the last note or clip falls
  // tail_quiet_below_music_db under the music's loudest moment, instead of
  // at every effect's worst-case tail.
  bool trim_silent_tail = false;
};

// One aligned WAV per audio/instrument source track, named track-<stable UUID>.wav.
// Each source is isolated before its saved main/send routes and the master
// native insert chain. Bus-only tracks have no independent source stem.
// Saved automation uses the shared curve evaluator.
// Optional convolution processing verifies a managed WAV IR before rendering.
// A new directory is published only after every stem is durable. Existing
// target directories are rejected rather than replaced.
struct StemRenderOptions {
  std::filesystem::path target_directory;
  WavFormat format = WavFormat::float32;
  std::uint64_t dither_seed = 0;
  std::uint32_t sample_rate = 0;
  // Reports follow source-track order and appear only after bundle publication.
  std::vector<StemLoudnessReport>* loudness_reports = nullptr;
};

enum class EncodedMasterFormat : std::uint8_t { flac16, flac24, mp3 };

struct EncodedMasterOptions {
  std::filesystem::path target;
  EncodedMasterFormat format = EncodedMasterFormat::flac24;
  // Applies to integer FLAC only; MP3 encoding is lossy and uses no dither.
  std::uint64_t dither_seed = 0;
  // Normalized libsndfile MP3 encoder control. No exact bitrate is promised.
  double compression_level = 0.5;
  std::uint32_t sample_rate = 0;
  // As MasterRenderOptions::trim_silent_tail.
  bool trim_silent_tail = false;
};

// One master target: WAV (float32/PCM) or, in a codec build, FLAC/MP3.
using MasterOutputOptions = std::variant<MasterRenderOptions, EncodedMasterOptions>;

// Acquire on the project control sequencer from the same transport gate used
// for playback. The gate must forbid transport start while its lease exists.
// Move this one-shot authority into an offline render; it is held through
// preflight, output publication or failure, and then released.
class StoppedExportAuthority {
public:
  [[nodiscard]] static foundation::Result<StoppedExportAuthority>
  acquire(const project::Snapshot& snapshot, project::TransportGate& gate);

  StoppedExportAuthority(StoppedExportAuthority&&) noexcept = default;
  StoppedExportAuthority& operator=(StoppedExportAuthority&&) noexcept = default;
  StoppedExportAuthority(const StoppedExportAuthority&) = delete;
  StoppedExportAuthority& operator=(const StoppedExportAuthority&) = delete;

  [[nodiscard]] bool matches(const project::Snapshot& snapshot) const noexcept;

private:
  StoppedExportAuthority(project::ProjectId project_id, std::uint64_t revision,
                         std::unique_ptr<project::StoppedTransportLease> lease)
      : project_id_(project_id), revision_(revision), lease_(std::move(lease)) {}

  project::ProjectId project_id_;
  std::uint64_t revision_;
  std::unique_ptr<project::StoppedTransportLease> lease_;
};

// Worker/control thread only. Renders managed audio clips and built-in
// instrument MIDI tracks through saved main/send/group/return routing to a
// stereo master. Saved automation uses the shared curve evaluator. An
// unsupported project state fails explicitly; the writer never publishes a
// partial master. At most 128 instrument tracks render.
[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                  const LiveRevisionState& live, const MasterRenderOptions& options);

// Stopped-project path. This builds the requested snapshot offline under an
// exclusive transport lease and never claims that a live graph is active.
// `vst3` prepares the snapshot's VST3 instruments and effects, which are
// chain stages like built-in effects (ADR-0050); without it a saved VST3
// stage fails before any output opens. The VST3 host supplies it
// (application/vst3/offline_export.hpp).
[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                  StoppedExportAuthority authority, const MasterRenderOptions& options,
                  const render_graph::Vst3StagePreparer& vst3 = {});

[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_stems(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                 const LiveRevisionState& live, const StemRenderOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_stems(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                 StoppedExportAuthority authority, const StemRenderOptions& options,
                 const render_graph::Vst3StagePreparer& vst3 = {});

// One source track's stem, exactly as render_wav_stems writes it into its
// bundle: as long as every other stem, published alone at options.target.
[[nodiscard]] foundation::Result<PublishOutcome>
render_wav_stem(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                StoppedExportAuthority authority, project::TrackId track,
                const MasterRenderOptions& options,
                const render_graph::Vst3StagePreparer& vst3 = {});

// Worker-only encoded master path; available with OSCILLATE_BUILD_CODEC_ADAPTER.
// The same project rendering and revision/transport guards as WAV apply.
[[nodiscard]] foundation::Result<PublishOutcome>
render_encoded_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                      const LiveRevisionState& live, const EncodedMasterOptions& options);

[[nodiscard]] foundation::Result<PublishOutcome>
render_encoded_master(const project::Snapshot& snapshot, const persistence::ProjectStore& store,
                      StoppedExportAuthority authority, const EncodedMasterOptions& options,
                      const render_graph::Vst3StagePreparer& vst3 = {});

// One track's own sound (ADR-0059: freeze, bounce, flatten): its instrument,
// note effects and inserts, keyed by the project's sidechains and moved by its
// automation, before its fader, pan, sends and the master chain. Frozen tracks
// play their renders, so a frozen track renders its render. Written as a
// float32 WAV at the project rate from the first frame the track sounds to
// the end of its tail, or exactly `range` (project frames, [first, last)).
// Worker only; progress gets (rendered, total) project frames every few
// blocks, and false cancels without publishing anything.
struct TrackRenderOptions {
  std::filesystem::path target;
  project::TrackId track;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> range = std::nullopt;
  std::function<bool(std::uint64_t, std::uint64_t)> progress = {};
};
struct TrackRender {
  PublishOutcome outcome;
  // The project frame the file's frame 0 sounds at.
  std::uint64_t first_frame = 0;
};
[[nodiscard]] foundation::Result<TrackRender> render_track(const project::Snapshot& snapshot,
                                                           const persistence::ProjectStore& store,
                                                           const TrackRenderOptions& options);

// Mix Check's single pass (ADR-0074): the song renders once, through the
// routed mixer as a master render with tracks does, and hands over every
// audio and instrument track's output as it feeds the mix, and the master as
// a WAV render would write it. Nothing is written to disk.
struct TrackTap {
  project::TrackId track;
  std::span<const float> left;
  std::span<const float> right;
};
struct RenderTaps {
  // Once per render block: every audio and instrument track's block after
  // its inserts, fader, pan, gain automation and mute (the routed mixer's
  // post-fader node output), in snapshot order, all starting at project
  // frame `first`. A latent chain's own delay is taken out, so the tracks
  // line up with each other and with the master.
  std::function<void(std::span<const TrackTap> tracks, std::uint64_t first)> tracks = {};
  // The master after its chain and level, from project frame `first`, as
  // render_wav_master writes it (with trim_silent_tail, trimmed the same way).
  std::function<void(std::span<const float> left, std::span<const float> right,
                     std::uint64_t first)>
      master = {};
  // (rendered, total) project frames every few blocks; false cancels.
  std::function<bool(std::uint64_t, std::uint64_t)> progress = {};
  bool trim_silent_tail = true;
};
// Worker only. The stopped authority's lease is held for the render.
[[nodiscard]] foundation::Result<void> render_master_taps(const project::Snapshot& snapshot,
                                                          const persistence::ProjectStore& store,
                                                          StoppedExportAuthority authority,
                                                          const RenderTaps& taps);

} // namespace oscillate::exporting
