#pragma once

// A playback output with no device behind it, for session and controller
// tests: start() records what was asked for and reports streaming, and the
// test pulls blocks through the render source with pump() instead of waiting
// for an audio thread. Link oscillate::application to use it.
//
//   auto device = std::make_shared<oscillate::test::FakePlaybackDevice>();
//   auto session = ProjectSession::open(root, fake_output_factory(device));
//   session->play();
//   device->pump();
//   REQUIRE_GT(device->peak, 0.0F);

#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace oscillate::test {

struct FakePlaybackDevice {
  // One pump renders this many frames.
  static constexpr std::size_t pump_frames = default_block;

  audio_io::StereoRenderSource source;
  audio_io::TransportCursor* transport = nullptr;
  audio_io::OutputStatus status;
  // The largest absolute sample any pump has rendered.
  float peak = 0.0F;
  std::uint32_t requested_buffer_frames = 0;

  // Renders one block as the audio callback would and folds it into `peak`.
  void pump() {
    std::array<engine::StereoFrame, pump_frames> scratch{};
    std::array<float, pump_frames * stereo_channels> output{};
    require(audio_io::render_transport_block(source, *transport, true, scratch, output) ==
                audio_io::BlockResult::rendered,
            "fake output could not render the graph");
    for (const auto sample : output) {
      peak = std::max(peak, std::abs(sample));
    }
  }
};

class FakePlaybackOutput final : public application::PlaybackOutput {
public:
  explicit FakePlaybackOutput(std::shared_ptr<FakePlaybackDevice> device)
      : device_(std::move(device)) {}
  // PlaybackOutput fixes the adjacent rate and requested-frame argument order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  foundation::Result<void> start(std::uint32_t rate, std::uint32_t frames,
                                 std::optional<std::uint64_t> serial) override {
    device_->requested_buffer_frames = frames;
    device_->status.state = audio_io::OutputState::streaming;
    device_->status.requested_client_rate = rate;
    device_->status.selected_target_serial = serial;
    return foundation::Result<void>::success();
  }
  void stop() noexcept override { device_->status.state = audio_io::OutputState::stopped; }
  audio_io::OutputStatus status() const noexcept override { return device_->status; }

private:
  std::shared_ptr<FakePlaybackDevice> device_;
};

// A factory that hands the session's render source and transport to `device`.
[[nodiscard]] inline application::PlaybackOutputFactory
fake_output_factory(const std::shared_ptr<FakePlaybackDevice>& device) {
  return [device](audio_io::StereoRenderSource source, audio_io::TransportCursor& transport) {
    device->source = source;
    device->transport = &transport;
    return std::make_unique<FakePlaybackOutput>(device);
  };
}

} // namespace oscillate::test
