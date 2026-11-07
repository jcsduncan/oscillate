#include "jack_playback_output.hpp"

#if defined(OSCILLATE_HAVE_JACK) && OSCILLATE_HAVE_JACK
#include "oscillate/audio_io/jack_output.hpp"

namespace oscillate::quick {
namespace {

class JackPlaybackOutput final : public application::PlaybackOutput {
public:
  JackPlaybackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor)
      : output_(source, cursor) {}

  [[nodiscard]] foundation::Result<void>
  start(std::uint32_t rate, std::uint32_t requested_frames,
        std::optional<std::uint64_t> target_serial) override {
    if (target_serial) {
      return foundation::Result<void>::failure({foundation::ErrorCode::invalid_argument,
                                                "JACK does not select PipeWire device serials"});
    }
    return output_.start({rate, requested_frames, audio_io::JackBufferPolicy::flexible});
  }

  void stop() noexcept override { output_.stop(); }

  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    const auto jack = output_.status();
    audio_io::OutputStatus result{};
    result.state = jack.state;
    result.requested_client_rate = jack.requested_project_rate;
    result.negotiated_client_rate = jack.server_rate;
    result.client_rate_mismatch = jack.rate_mismatch;
    result.observed_graph_rate = jack.server_rate;
    result.observed_block_frames = jack.server_buffer_frames;
    result.render_failures = jack.render_failures;
    result.oversized_blocks = jack.oversized_blocks;
    result.invalid_buffers = jack.invalid_buffers;
    result.measured_callback_blocks = jack.measured_callback_blocks;
    result.callback_over_period_blocks = jack.callback_over_period_blocks;
    result.max_callback_duration_ns = jack.max_callback_duration_ns;
    result.callback_timing_distribution = jack.callback_timing_distribution;
    result.callback_load = jack.callback_load;
    // JACK reports client xrun notifications, not a per-output device count.
    result.client_xrun_notifications = jack.xrun_notifications;
    result.xruns = jack.xrun_notifications;
    result.realtime_workers = jack.realtime_workers;
    return result;
  }

private:
  audio_io::JackOutput output_;
};

} // namespace

application::PlaybackOutputFactory make_jack_playback_output_factory() {
  return [](audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor) {
    return std::make_unique<JackPlaybackOutput>(source, cursor);
  };
}

bool jack_server_available() noexcept { return audio_io::jack_server_available(); }

} // namespace oscillate::quick
#else
namespace oscillate::quick {

application::PlaybackOutputFactory make_jack_playback_output_factory() { return {}; }
bool jack_server_available() noexcept { return false; }

} // namespace oscillate::quick
#endif
