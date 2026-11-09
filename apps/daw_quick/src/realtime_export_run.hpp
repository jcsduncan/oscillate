#pragma once

#include "routed_playback.hpp"

#include "oscillate/export/jack_realtime_export.hpp"

#include <atomic>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace oscillate::quick {

// One revision-pinned JACK run. Its graph, source bank, plugin instances and
// cursor are private to the export. Destruction always quiesces JACK before
// retiring those owners.
//
// Tail policy: the file covers project samples [0, end_sample + tail_frames).
// Audio source slots are silent from end_sample. Prepared native synths and
// qualified VST3 instruments receive a note-off at that exact
// frame, suppress later note-ons and render their bounded releases. Processor
// histories keep rendering. The
// prepared graph's route tail is capped at max_tail_seconds.
class RealtimeExportRun final {
public:
  static constexpr std::uint64_t max_tail_seconds = 10;

  // Conservative saved-native diagnostic retained for focused parity tests.
  // begin() instead checks exact prepared runtime tails and route topology.
  // This diagnostic rejects VST3 effects and instruments, unbounded MIDI releases
  // and a sum above max_tail_seconds.
  [[nodiscard]] static foundation::Result<std::uint64_t>
  bounded_tail_frames(const project::Snapshot& snapshot);

  [[nodiscard]] static foundation::Result<std::unique_ptr<RealtimeExportRun>>
  begin(const std::filesystem::path& root, project::Snapshot snapshot,
        application::ControlTransportGate::PlaybackToken token, const std::filesystem::path& target,
        std::uint64_t end_sample, std::uint32_t buffer_frames,
        Vst3LiveQualification qualification = {}, const audio_io::JackApi* api = nullptr);

  RealtimeExportRun(const RealtimeExportRun&) = delete;
  RealtimeExportRun& operator=(const RealtimeExportRun&) = delete;
  ~RealtimeExportRun();

  [[nodiscard]] bool interval_captured() const noexcept;
  [[nodiscard]] std::uint64_t frames_captured() const noexcept;
  // end_sample + tail_frames once begin() succeeds.
  [[nodiscard]] std::uint64_t expected_frames() const noexcept { return expected_frames_; }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept { return tail_frames_; }
  // Prepared route and terminal master latency skipped before the capture.
  [[nodiscard]] std::uint32_t output_latency_frames() const noexcept { return latency_frames_; }
  [[nodiscard]] audio_io::JackOutputStatus device_status() const noexcept;
  // Called only after the run's revision has been checked against the session.
  [[nodiscard]] foundation::Result<exporting::PublishOutcome> finish();
  // Waits on an export worker; it never publishes the file.
  [[nodiscard]] foundation::Result<void> wait_until_ready(const std::atomic<bool>& cancelled,
                                                          std::stop_token stop);
  // Only the serialized Quick control thread calls this, so its revision
  // observation and publication cannot race a project command on that thread.
  [[nodiscard]] foundation::Result<exporting::PublishOutcome>
  finish_if_current(std::uint64_t expected_revision, std::uint64_t current_revision,
                    bool cancelled);

private:
  struct SourceCut {
    std::size_t index; // topological source slot
    bool retain_instrument_release = false;
    std::vector<engine::StereoFrame> frames;
  };

  explicit RealtimeExportRun(std::unique_ptr<RoutedPlayback> playback) noexcept;
  static exporting::JackRealtimeWavExport::HealthCounters health(void* context) noexcept;
  // The export's own master render: RoutedPlayback's callback with each
  // source slot cut to silence at content_end_.
  static bool render(void* context, std::span<engine::StereoFrame> output,
                     engine::BlockContext block) noexcept;
  static bool freeze_cursor(void* context) noexcept;
  [[nodiscard]] bool
  cut_sources(engine::BlockContext block, std::size_t frames,
              std::span<const std::span<const engine::StereoFrame>>& selected) noexcept;

  std::unique_ptr<RoutedPlayback> playback_;
  std::unique_ptr<exporting::JackRealtimeWavExport> capture_;
  std::uint64_t content_end_ = 0;
  std::uint64_t tail_frames_ = 0;
  std::uint64_t expected_frames_ = 0;
  std::uint32_t latency_frames_ = 0;
  // Preallocated before the device starts; callback-owned afterwards.
  std::vector<SourceCut> cuts_;
  std::vector<engine::StereoFrame> silence_;
  std::vector<std::span<const engine::StereoFrame>> spans_;
  bool callback_frozen_ = false;
};

} // namespace oscillate::quick
