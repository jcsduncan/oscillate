#include "alsa_playback_output.hpp"

#include "oscillate/audio_io/alsa_output.hpp"

#include <algorithm>
#include <utility>

namespace oscillate::quick {

void AlsaOutputChoice::set(Value value) {
  const std::lock_guard lock(mutex_);
  value_ = std::move(value);
}

AlsaOutputChoice::Value AlsaOutputChoice::get() const {
  const std::lock_guard lock(mutex_);
  return value_;
}

namespace {

class AlsaPlaybackOutput final : public application::PlaybackOutput {
public:
  AlsaPlaybackOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor,
                     std::shared_ptr<const AlsaOutputChoice> choice)
      : output_(source, cursor), choice_(std::move(choice)) {}

  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  [[nodiscard]] foundation::Result<void>
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  start(std::uint32_t rate, std::uint32_t requested_frames,
        std::optional<std::uint64_t> target_serial) override {
    if (target_serial) {
      return foundation::Result<void>::failure({foundation::ErrorCode::invalid_argument,
                                                "ALSA does not select PipeWire device serials"});
    }
    const auto chosen = choice_->get();
    if (chosen.device.empty())
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::not_found, "No ALSA device is chosen"});
    audio_io::AlsaStartConfig config;
    config.device = chosen.device;
    config.project_rate = rate;
    config.period_frames = requested_frames;
    config.periods =
        std::clamp(chosen.periods, audio_io::alsa_min_periods, audio_io::alsa_max_periods);
    return output_.start(std::move(config));
  }

  void stop() noexcept override { output_.stop(); }

  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    audio_io::OutputStatus result{};
    try {
      const auto alsa = output_.status();
      result.state = alsa.state;
      if (alsa.rate != 0) {
        result.requested_client_rate = alsa.rate;
        result.negotiated_client_rate = alsa.rate;
        result.observed_graph_rate = alsa.rate;
      }
      if (alsa.render_frames != 0)
        result.observed_block_frames = alsa.render_frames;
      result.render_failures = alsa.render_failures;
      result.measured_callback_blocks = alsa.measured_callback_blocks;
      result.callback_over_period_blocks = alsa.callback_over_period_blocks;
      result.max_callback_duration_ns = alsa.max_callback_duration_ns;
      result.callback_timing_distribution = alsa.callback_timing_distribution;
      result.callback_load = alsa.callback_load;
      // Underruns the device itself reported, each recovered.
      result.xruns = alsa.xruns;
      result.realtime_workers = alsa.realtime_workers;
      result.output_latency_frames = alsa.delay_frames;
    } catch (...) {
      result.state = audio_io::OutputState::error;
    }
    return result;
  }

private:
  audio_io::AlsaOutput output_;
  std::shared_ptr<const AlsaOutputChoice> choice_;
};

} // namespace

application::PlaybackOutputFactory
make_alsa_playback_output_factory(std::shared_ptr<const AlsaOutputChoice> choice) {
  return [choice = std::move(choice)](
             audio_io::StereoRenderSource source,
             audio_io::TransportCursor& cursor) -> std::unique_ptr<application::PlaybackOutput> {
    return std::make_unique<AlsaPlaybackOutput>(source, cursor, choice);
  };
}

} // namespace oscillate::quick
