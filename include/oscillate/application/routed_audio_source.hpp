#pragma once

#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/native_dsp/synth/player.hpp"
#include "oscillate/processing/warp_live_reader.hpp"
#include "oscillate/project/clip_fade.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace oscillate::application {

struct RoutedSourceStatus {
  std::uint64_t ready_generation = 0;
  std::uint64_t missing_frames = 0;
  bool worker_failed = false;
  bool worker_paused = false;
  // Saved processed (warped or pitched) clips in this source's revision.
  std::size_t processed_clips = 0;
  // Processed clips read from a validated Q0 render cache rather than live.
  std::size_t cache_backed_clips = 0;
  // Live-rendered clips currently below Q0, and all step-downs so far.
  std::size_t reduced_quality_clips = 0;
  std::uint64_t quality_step_downs = 0;
};

// Live state of one processed clip. It describes this prepared source, so it
// belongs to the graph/source revision that owns the source, never to a
// saved edit that has not yet been accepted for playback.
struct RoutedProcessedClipStatus {
  project::ClipId clip;
  project::TrackId track;
  processing::WarpLiveStatus live;
  // Processed frames resident in the clip's ring, ahead of the callback.
  std::size_t buffered_frames = 0;
};

// One bounded worker streams verified project audio clips into
// per-clip SPSC rings. The callback never reads a file or waits for the worker.
// An audio-only move/trim can prepare a replacement source off the callback
// and hand it to a matching graph revision at a held block boundary.
class RoutedAudioSource final {
public:
  // first_sample must match the initial transport cursor position. Each clip
  // ring is primed from the corresponding source frame before playback starts.
  // A captured candidate also pins the cursor generation used by those rings.
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedAudioSource>>
  prepare(const std::filesystem::path& root, const project::Snapshot& snapshot,
          RoutedPlaybackControl& control, std::size_t buffer_frame_budget,
          const std::atomic<bool>* worker_permit = nullptr, const std::stop_token& stop = {},
          std::uint64_t first_sample = 0);
  // The capture owns every graph input read by preparation. The worker only
  // stores control for later callback rendering; it does not inspect it.
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedAudioSource>>
  prepare(const std::filesystem::path& root, const RoutedSourcePreparationCapture& capture,
          RoutedPlaybackControl& control, std::size_t buffer_frame_budget,
          const std::atomic<bool>* worker_permit = nullptr, const std::stop_token& stop = {},
          std::uint64_t first_sample = 0, std::uint64_t first_generation = 1,
          const audio_io::TransportCursor* live_cursor = nullptr);

  RoutedAudioSource(const RoutedAudioSource&) = delete;
  RoutedAudioSource& operator=(const RoutedAudioSource&) = delete;
  ~RoutedAudioSource(); // Stop the device callback before destruction.

  // Control side, before the device starts. The device may attach workers
  // to the pool; rendering then splits slots and graph levels across them.
  [[nodiscard]] audio_io::StereoRenderSource
  render_source(engine::AudioWorkerPool* workers = nullptr) noexcept;
  [[nodiscard]] RoutedSourceStatus status() const noexcept;
  // Control side. One entry per processed clip, in preparation order.
  [[nodiscard]] std::vector<RoutedProcessedClipStatus> processed_clip_status() const;
  [[nodiscard]] std::optional<media::UnderrunEvent> pop_source_underrun() noexcept;
  [[nodiscard]] std::optional<foundation::Error> worker_failure() const;
  // Control side only, before this candidate has a callback consumer. The
  // worker may continue producing. A ready result covers every audio clip
  // overlapping the requested first block, with no stale scan or silence.
  [[nodiscard]] media::HandoffWindowStatus prime_handoff_window(std::uint64_t generation,
                                                                std::uint64_t first_sample,
                                                                std::size_t frames) noexcept;
  // Callback only. Supplies this source's preallocated node spans to the
  // matching graph revision without invoking the graph publisher itself.
  // With workers, every track's slot renders in parallel (ADR-0048).
  [[nodiscard]] bool
  fill_source_bank(engine::BlockContext block, std::size_t frames,
                   std::span<const std::span<const engine::StereoFrame>>& selected,
                   engine::AudioWorkerPool* workers = nullptr) noexcept;
  // Callback-only candidate path. A missing clip frame rejects the complete
  // source block so the publisher can render the old source in this callback.
  [[nodiscard]] bool
  fill_source_bank_exact(engine::BlockContext block, std::size_t frames,
                         std::span<const std::span<const engine::StereoFrame>>& selected,
                         engine::AudioWorkerPool* workers = nullptr) noexcept;
  void note_rendered_frames(std::size_t frames) noexcept;
  [[nodiscard]] bool streaming_hold() const noexcept { return stream_hold_; } // callback only
  // Gapless loop playback over project samples [start, end).
  //
  // Ownership: set_loop(), clear_loop() and loop_status() belong to one
  // control thread (the thread that owns this source and requests transport
  // seeks and loop wraps). Concurrent callers are unsupported.
  //
  // Each set/clear publishes a new loop configuration version. The worker
  // streams every clip overlapping an enabled loop only up to `end` in the
  // current transport generation, then pre-primes the next generation from
  // `start`. loop_status() reports readiness and misses only for the current
  // version, never for an earlier one. When ready_generation equals the
  // current transport generation plus one, the caller may request the loop
  // wrap to `start`; the callback accepts the planned wrap block without a
  // hold only if the configuration, its ready record and the block position
  // all still agree. Anything else is a seek that holds until primed.
  //
  // Missed means queued frames cannot serve this configuration and cannot be
  // withdrawn: a clip already streamed past `end`, or a rearm or clear after
  // frames of the next generation were queued for an earlier loop. The caller
  // must then seek (before the old loop end, after a clear) to discard them;
  // a seek re-arms the current configuration for its generation.
  [[nodiscard]] bool set_loop(std::uint64_t start, std::uint64_t end) noexcept;
  void clear_loop() noexcept;
  struct LoopStatus {
    std::uint64_t ready_generation = 0;
    bool missed = false;
  };
  [[nodiscard]] LoopStatus loop_status() const noexcept;
  // Control thread requests a block-boundary cut to silent source slots. The
  // published graph and its stateful master processors continue rendering.
  void request_silent_drain() noexcept;
  [[nodiscard]] std::uint64_t completed_silent_drain_frames() const noexcept;
  // The built-in synth runtime of a track, for live panel offers. Control
  // thread only; valid until the source is retired after callback quiescence.
  [[nodiscard]] native_dsp::synth::NativeSynthRuntime*
  synth_runtime(project::TrackId track) noexcept;
  // Control only. The player retains its runtime and voices while a prepared
  // native automation plan is offered to the callback.
  [[nodiscard]] native_dsp::synth::NativeSynthPlayer* synth_player(project::TrackId track) noexcept;
  // Control only: every synth player re-offers the notes its engine follows
  // (Kick & Bass Lock, ADR-0072) from `snapshot`. A player that fails keeps
  // the notes it had.
  void refresh_synth_locks(const project::Snapshot& snapshot);
  // Control only: every synth player hears the project's key (Knock's pads
  // tune to it). Lock-free; the callback adopts it at a block start.
  void follow_project_key(std::optional<project::MusicalKey> key) noexcept;
  // Control only. The declared maximum release, used to find a MIDI-silent
  // boundary before replacing a live synth player.
  [[nodiscard]] std::optional<std::uint64_t>
  synth_tail_frames(project::TrackId track) const noexcept;
  // Control only, before export starts. These are the exact source players
  // used by the prepared graph, never a second tail-probe instance.
  [[nodiscard]] foundation::Result<void> prepare_realtime_export_end(std::uint64_t end);
  [[nodiscard]] std::optional<std::uint64_t>
  prepared_synth_export_tail(project::TrackId track, std::uint64_t end) const noexcept;

private:
  // Ring tags are absolute project-rate source frames for an unprocessed
  // clip and clip-local output frames (source_start 0) for a processed one,
  // so the callback consumes both identically.
  struct Clip {
    project::ClipId id;
    project::TrackId track;
    std::uint64_t start;
    std::uint64_t end;
    std::uint64_t source_start; // first ring tag
    std::uint64_t source_end;   // one past the last ring tag
    project::PreparedClipFades fades;
    std::optional<media::SourceRateReader> reader;    // unprocessed clips
    std::unique_ptr<processing::WarpLiveReader> warp; // processed clips
    std::unique_ptr<media::PrefetchRing> ring;
    std::vector<media::StereoFrame> scratch; // callback-owned
    std::uint64_t next_source = 0;           // worker-owned after prepare
    // Worker-owned: the tag limit and generation of frames being pushed.
    // Without a loop these are source_end and the transport generation.
    std::uint64_t limit = 0;
    std::uint64_t fill_generation = 0;
    std::uint64_t loop_first = 0; // first tag pushed for a pre-primed wrap
    // Worker-owned quality step-down state for processed clips.
    bool filled_since_seek = false;
    bool behind = false;
    std::uint64_t underruns_seen = 0;
  };
  struct Slot {
    project::TrackId track;
    std::size_t index;
    std::vector<engine::StereoFrame> scratch;
  };
  struct SynthSlot {
    std::size_t slot_index;
    native_dsp::synth::NativeSynthPlayer player;
  };
  // Indices into clips_ and synths_ that render into one slot.
  struct SlotWork {
    std::vector<std::size_t> clips;
    std::vector<std::size_t> synths;
  };
  struct FillRequest {
    std::uint64_t generation;
    std::size_t frames;
  };

  RoutedAudioSource(RoutedPlaybackControl& control, std::vector<Clip> clips,
                    std::vector<Slot> slots, std::vector<SynthSlot> synths,
                    std::size_t topology_size, std::size_t max_frames,
                    std::uint64_t initial_generation, const std::atomic<bool>* worker_permit);
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedAudioSource>>
  prepare_with_layout(const std::filesystem::path& root, const project::Snapshot& snapshot,
                      RoutedPlaybackControl& control,
                      std::span<const routing::ProjectRouteSource> layout,
                      std::span<const engine::RoutingNodeId> order, std::size_t max_frames,
                      std::size_t buffer_frame_budget, const std::atomic<bool>* worker_permit,
                      const std::stop_token& stop, std::uint64_t first_sample,
                      std::uint64_t first_generation, const audio_io::TransportCursor* live_cursor);
  static bool render(void* context, std::span<engine::StereoFrame> output,
                     engine::BlockContext block) noexcept;
  static bool freeze_cursor(void* context) noexcept;
  bool render_block(std::span<engine::StereoFrame> output, engine::BlockContext block) noexcept;
  bool fill_source_bank_impl(engine::BlockContext block, std::size_t frames,
                             std::span<const std::span<const engine::StereoFrame>>& selected,
                             bool require_complete, engine::AudioWorkerPool* workers) noexcept;
  bool fill_slots(engine::BlockContext block, std::size_t frames, bool require_complete,
                  engine::AudioWorkerPool* workers) noexcept;
  // One slot's block: may run on any audio worker, never beside itself.
  bool fill_slot(std::size_t index, engine::BlockContext block, std::size_t frames,
                 bool require_complete, engine::AudioWorkerPool* workers) noexcept;
  void worker_loop() noexcept;
  [[nodiscard]] bool fill_lead(FillRequest request);
  [[nodiscard]] bool fill_one(Clip& clip, FillRequest request);
  [[nodiscard]] static foundation::Result<std::size_t> read_clip(Clip& clip, std::uint64_t first,
                                                                 std::span<media::StereoFrame> out);
  void manage_quality(Clip& clip) noexcept;
  struct LoopConfig {
    std::uint64_t version;
    bool enabled;
    std::uint64_t start;
    std::uint64_t end;
  };
  struct LoopReady {
    std::uint64_t version;
    std::uint64_t generation;
  };
  // One bounded attempt each; nullopt while a writer is mid-update.
  [[nodiscard]] std::optional<LoopConfig> read_loop_config() const noexcept;
  [[nodiscard]] std::optional<LoopReady> read_loop_ready() const noexcept;
  void publish_loop_ready(LoopReady ready) noexcept; // worker only
  void publish_loop_config(bool enabled, std::uint64_t start, std::uint64_t end) noexcept;
  void prime_loop(std::uint64_t generation) noexcept;
  void reset_clips(std::uint64_t generation, std::uint64_t sample) noexcept;

  RoutedPlaybackControl& control_;
  const std::size_t max_frames_;
  const std::uint64_t initial_generation_;
  std::vector<Clip> clips_;
  std::vector<Slot> slots_;
  std::vector<SynthSlot> synths_;
  std::vector<SlotWork> slot_work_;
  engine::AudioWorkerPool* render_workers_ = nullptr; // set before the device starts
  std::vector<std::span<const engine::StereoFrame>> spans_;
  std::thread worker_;
  const std::atomic<bool>* worker_permit_; // optional control/test gate; outlives source
  std::atomic<bool> stop_{false};
  std::atomic<bool> worker_failed_{false};
  std::atomic<bool> worker_paused_{false};
  foundation::Error failure_{foundation::ErrorCode::internal, ""};
  // Single callback writer / single worker reader. The generation release
  // publishes its matching sample. Worker acknowledgment stops old writes;
  // callback then discards old rings before permitting new writes.
  std::atomic<std::uint64_t> requested_sample_{0};
  std::atomic<std::uint64_t> requested_generation_{1};
  std::atomic<std::uint64_t> acknowledged_generation_{1};
  std::atomic<std::uint64_t> discarded_generation_{1};
  std::atomic<std::uint64_t> ready_generation_{1};
  std::atomic<std::uint64_t> missing_frames_{0};
  std::size_t next_underrun_clip_ = 0; // control-thread round-robin consumer
  // Loop configuration seqlock, written by the control thread: odd while
  // fields change, and each even value names one configuration (0: none).
  std::atomic<std::uint64_t> loop_version_{0};
  std::atomic<std::uint64_t> loop_start_{0};
  std::atomic<std::uint64_t> loop_end_{0};
  std::atomic<bool> loop_enabled_{false};
  // Ready record seqlock, written by the worker: the configuration version
  // and next generation whose loop start is fully queued (both 0: none).
  std::atomic<std::uint64_t> loop_ready_sequence_{0};
  std::atomic<std::uint64_t> loop_ready_version_{0};
  std::atomic<std::uint64_t> loop_ready_generation_{0};
  // Worker-written configuration version that missed (0: none).
  std::atomic<std::uint64_t> loop_missed_version_{0};
  // Callback-written before requested_generation_: the generation whose
  // planned wrap it accepted. The worker adopts queued frames only for it.
  std::atomic<std::uint64_t> loop_accepted_wrap_{0};
  std::atomic<bool> silent_drain_requested_{false};
  std::atomic<std::uint64_t> completed_silent_drain_frames_{0};
  std::uint64_t callback_generation_ = 1;
  std::uint64_t loop_accepted_generation_ = 0; // callback-owned
  std::uint64_t loop_version_seen_ = 0;        // worker-owned
  bool silent_drain_block_ = false;            // callback-owned
  bool frozen_ = false;                        // callback-owned
  bool stream_hold_ = false;                   // callback-owned
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace oscillate::application
