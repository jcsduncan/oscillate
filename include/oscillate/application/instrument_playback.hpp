#pragma once

#include "oscillate/application/revision_status.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace oscillate::application {

// The output buffers playback may request. The small ones suit playing live;
// a dense mix needs the larger ones' headroom, so 512 frames is the default.
inline constexpr std::array<std::uint32_t, 5> playback_buffer_frames{64, 128, 256, 512, 1024};
inline constexpr std::uint32_t default_playback_buffer_frames = 512;
inline constexpr std::uint32_t max_playback_buffer_frames = playback_buffer_frames.back();

[[nodiscard]] constexpr bool valid_playback_buffer_frames(std::uint32_t frames) noexcept {
  return std::ranges::find(playback_buffer_frames, frames) != playback_buffer_frames.end();
}
// "64, 128, 256, 512 or 1024", for messages.
[[nodiscard]] std::string playback_buffer_frames_text();

class ProjectSession;
class ClipPrefetchWorker;

// Control-side output boundary. The render source remains the same real-time
// graph callback for production and deterministic device fixtures.
class PlaybackOutput {
public:
  virtual ~PlaybackOutput() = default;
  [[nodiscard]] virtual foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t latency_frames,
        std::optional<std::uint64_t> target_serial) = 0;
  virtual void stop() noexcept = 0;
  [[nodiscard]] virtual audio_io::OutputStatus status() const noexcept = 0;
};

using PlaybackOutputFactory = std::function<std::unique_ptr<PlaybackOutput>(
    audio_io::StereoRenderSource, audio_io::TransportCursor&)>;

struct PreparedAudioClip {
  project::ClipId clip;
  media::AudioReader source;
  std::uint64_t first_source_frame;
  std::uint64_t end_source_frame;
  std::uint64_t project_start_frame;
  std::uint64_t project_end_frame;
};

struct PreparedInstrumentPlayback {
  std::shared_ptr<const project::Snapshot> snapshot;
  // The first clip retains the M2 field for existing prepared-playback clients.
  std::optional<PreparedAudioClip> audio_clip;
  std::vector<PreparedAudioClip> additional_audio_clips;
};

// Captured on the project control thread. run() opens verified project audio
// on a worker; its result is installed only if the project
// revision still matches. The task never accesses the live mutable model.
struct InstrumentPlaybackPreparation {
  std::filesystem::path root;
  std::shared_ptr<const project::Snapshot> snapshot;
  [[nodiscard]] foundation::Result<PreparedInstrumentPlayback>
  run(const std::stop_token& stop = {}) &&;
};

// Control-thread owner for the narrow desktop live-playback slice. It renders
// prepared managed WAV clips through the PipeWire output; instruments, effects
// and automation play on the routed path. The
// session must outlive this object. Stop the output before retiring the source
// worker; poll_retirement() keeps its join off the UI event path.
class InstrumentPlayback {
public:
  explicit InstrumentPlayback(
      ProjectSession& session, PlaybackOutputFactory output_factory = {},
      std::uint32_t requested_buffer_frames = default_playback_buffer_frames);
  InstrumentPlayback(const InstrumentPlayback&) = delete;
  InstrumentPlayback& operator=(const InstrumentPlayback&) = delete;
  ~InstrumentPlayback();

  [[nodiscard]] foundation::Result<InstrumentPlaybackPreparation> prepare_start() const;
  [[nodiscard]] foundation::Result<void> set_requested_buffer_frames(std::uint32_t frames);
  // Capture the accepted revision while playback continues. Run the returned
  // media preparation on a worker, then install it on the serialized control
  // thread. A newer edit invalidates the prepared result without stopping audio.
  [[nodiscard]] foundation::Result<InstrumentPlaybackPreparation> prepare_live_edit() const;
  [[nodiscard]] foundation::Result<void>
  start_prepared(PreparedInstrumentPlayback prepared,
                 std::optional<std::uint64_t> target_serial = std::nullopt);
  // Stops/joins the output at a completed block, advances transport generation,
  // re-primes every audio feed from that exact sample and resumes via poll_start.
  // The caller must run this non-real-time handoff off the UI event path if the
  // device's stop/join may be slow. Failure after quiescence leaves playback
  // stopped with a visible model-versus-graph status; the edit remains accepted.
  [[nodiscard]] foundation::Result<void>
  restart_prepared_after_edit(PreparedInstrumentPlayback prepared);
  [[nodiscard]] foundation::Result<void>
  start(std::optional<std::uint64_t> target_serial = std::nullopt);
  // During audio-clip priming, returns false until enough frames are ready.
  // On failure playback is stopped and the error can be shown by the UI.
  [[nodiscard]] foundation::Result<bool> poll_start();
  void stop() noexcept;
  [[nodiscard]] bool playing() const noexcept;
  [[nodiscard]] bool retiring() const noexcept;
  [[nodiscard]] bool poll_retirement() noexcept;
  // Call after an accepted project edit. A failed graph update keeps the edit
  // and the previous audible revision; status() reports the divergence.
  [[nodiscard]] foundation::Result<void> submit_current();
  void observe();
  [[nodiscard]] RevisionStatus status() const;
  [[nodiscard]] audio_io::OutputState output_state() const noexcept;
  [[nodiscard]] audio_io::OutputStatus output_status() const noexcept;
  [[nodiscard]] std::uint64_t completed_sample() const noexcept;
  [[nodiscard]] std::uint64_t source_underruns() const noexcept;
  // Drain one bounded callback event on the control thread for per-clip status.
  [[nodiscard]] std::optional<media::UnderrunEvent> pop_source_underrun() noexcept;
  [[nodiscard]] std::optional<foundation::Error> source_failure() const;

private:
  struct Run;
  ProjectSession& session_;
  PlaybackOutputFactory output_factory_;
  std::uint32_t requested_buffer_frames_ = default_playback_buffer_frames;
  std::unique_ptr<Run> run_;
  std::size_t next_underrun_feed_ = 0; // control-thread round-robin consumer
  std::vector<std::unique_ptr<ClipPrefetchWorker>> retired_workers_;
  std::optional<RevisionStatus> status_override_;
};

} // namespace oscillate::application
