#pragma once

#include <oscillate/audio_io/jack_output.hpp>
#include <oscillate/export/realtime_capture.hpp>
#include <oscillate/export/realtime_tail_plan.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace oscillate::exporting {

// Owns a dedicated JACK output and captures exactly the final stereo frames
// rendered for that output. The caller supplies a revision-pinned render source
// and keeps its runtime alive until this object is destroyed. This is a
// control/worker API: begin(), finish() and destruction never run in an audio
// callback. Only one export may own a given source and transport cursor.
//
// The transport starts at first_sample. A master that emits project sample N
// at stream frame N + output_latency_frames is captured from stream frame
// first_sample + output_latency_frames, so the file starts at project sample
// first_sample. The device hears silence after the last captured frame.
class JackRealtimeWavExport {
public:
  struct TailPreflight {
    const engine::RoutingLatencyPlan& route;
    engine::RoutingNodeId master;
    std::span<const RealtimeNodeTail> node_tails;
    std::uint64_t content_frames;
    std::uint64_t max_tail_frames;
  };
  struct HealthCounters {
    // Cumulative, same-source counter sampled before start and after stop.
    // Unknown is deliberately not interpreted as zero.
    std::optional<std::uint64_t> source_underruns;
  };
  struct HealthProbe {
    void* context = nullptr;
    HealthCounters (*snapshot)(void*) noexcept = nullptr; // control thread only
  };

  [[nodiscard]] static foundation::Result<std::unique_ptr<JackRealtimeWavExport>>
  begin(const std::filesystem::path& target, WavSpec spec, std::uint64_t first_sample,
        std::uint32_t output_latency_frames, std::size_t ring_frames,
        audio_io::StereoRenderSource final_master, audio_io::TransportCursor& transport,
        audio_io::JackStartConfig device, HealthProbe health,
        const audio_io::JackApi* api = nullptr);

  // Verifies that expected_frames covers the complete prepared route tail
  // before opening a capture file or JACK client. The caller is responsible
  // for cutting source input after content_frames while draining processors.
  [[nodiscard]] static foundation::Result<std::unique_ptr<JackRealtimeWavExport>>
  begin_with_tail_plan(const std::filesystem::path& target, WavSpec spec, TailPreflight preflight,
                       std::uint64_t first_sample, std::uint32_t output_latency_frames,
                       std::size_t ring_frames, audio_io::StereoRenderSource final_master,
                       audio_io::TransportCursor& transport, audio_io::JackStartConfig device,
                       HealthProbe health, const audio_io::JackApi* api = nullptr);

  JackRealtimeWavExport(const JackRealtimeWavExport&) = delete;
  JackRealtimeWavExport& operator=(const JackRealtimeWavExport&) = delete;
  ~JackRealtimeWavExport();

  [[nodiscard]] bool interval_captured() const noexcept;
  // True once the capture or its drain has failed: the interval can never be
  // captured, so a caller waiting for it should stop and call finish().
  // Playback is no longer permitted from then on. Lock-free.
  [[nodiscard]] bool capture_failed() const noexcept;
  [[nodiscard]] std::uint64_t frames_captured() const noexcept;
  [[nodiscard]] audio_io::JackOutputStatus device_status() const noexcept;

  // Quiesces the JACK callback and joins the WAV drain worker before checking
  // evidence and atomically publishing. A short interval, unknown counters,
  // JACK client xrun notification, callback overrun, or device failure leaves
  // the target intact. The callback timing histogram is checked only after
  // the JACK process callback has quiesced.
  [[nodiscard]] foundation::Result<PublishOutcome> finish();

private:
  JackRealtimeWavExport(std::unique_ptr<RealtimeWavCapture> capture,
                        audio_io::StereoRenderSource final_master,
                        audio_io::TransportCursor& transport, HealthProbe health,
                        HealthCounters baseline, std::uint64_t capture_start,
                        std::uint64_t expected_frames, const audio_io::JackApi* api);
  static bool render(void* context, std::span<engine::StereoFrame> frames,
                     engine::BlockContext block) noexcept;
  static bool freeze_cursor(void* context) noexcept;
  static bool permitted(const void* context) noexcept;
  void drain_loop(std::stop_token token) noexcept;
  void stop_and_join() noexcept;

  std::unique_ptr<RealtimeWavCapture> capture_;
  audio_io::StereoRenderSource final_master_;
  HealthProbe health_;
  HealthCounters baseline_;
  std::uint64_t capture_start_; // stream frame of the first captured sample
  std::uint64_t expected_frames_;
  std::unique_ptr<audio_io::JackOutput> output_;
  std::jthread drain_worker_;
  std::atomic<bool> drain_failed_{false};
  bool pending_rendered_ = false;                       // callback-owned
  std::span<const engine::StereoFrame> pending_frames_; // callback-owned
  engine::BlockContext pending_block_{};                // callback-owned
  bool finished_ = false;                               // control-owned
};

} // namespace oscillate::exporting
