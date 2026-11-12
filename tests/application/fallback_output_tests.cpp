// A backend that cannot have its device falls back (ADR-0130): the direct
// ALSA output whose card reservation is refused plays through PipeWire.
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include "oscillate/application/fallback_output.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace {
using namespace oscillate;
using oscillate::test::require;

constexpr std::uint32_t rate = test::sample_rate_48k;
constexpr auto frames = static_cast<std::uint32_t>(test::block_128);
constexpr const char* refusal = "Cannot take hw:CARD=USB,DEV=0: the sound server keeps card 1";

struct Device {
  bool refuse = false;
  int starts = 0;
  int stops = 0;
  bool running = false;
  std::optional<std::uint64_t> serial;
  audio_io::OutputState state = audio_io::OutputState::streaming;
};

class FakeOutput final : public application::PlaybackOutput {
public:
  explicit FakeOutput(Device& device) : device_(device) {}
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): PlaybackOutput's signature.
  foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                 std::optional<std::uint64_t> serial) override {
    ++device_.starts;
    device_.serial = serial;
    if (device_.refuse)
      return foundation::Result<void>::failure({foundation::ErrorCode::conflict, refusal});
    device_.running = true;
    return foundation::Result<void>::success();
  }
  void stop() noexcept override {
    ++device_.stops;
    device_.running = false;
  }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override {
    audio_io::OutputStatus status;
    status.state = device_.state;
    return status;
  }

private:
  Device& device_;
};

application::PlaybackOutputFactory factory_for(Device& device) {
  return [&device](audio_io::StereoRenderSource, audio_io::TransportCursor&) {
    return std::make_unique<FakeOutput>(device);
  };
}

void a_refused_card_plays_through_the_fallback() {
  Device alsa;
  Device pipewire;
  alsa.refuse = true;
  pipewire.state = audio_io::OutputState::paused;
  auto report = std::make_shared<application::OutputFallbackReport>();
  const auto factory =
      application::make_fallback_output_factory(factory_for(alsa), factory_for(pipewire), report);
  audio_io::TransportCursor cursor(0);
  auto output = factory({}, cursor);
  REQUIRE_OK(output->start(rate, frames, std::nullopt), "playback starts on the fallback");
  REQUIRE_EQ(alsa.starts, 1, "the primary was tried first");
  require(pipewire.running, "the fallback plays");
  REQUIRE_EQ(output->status().state, audio_io::OutputState::paused,
             "status is the fallback's while it plays");
  REQUIRE_EQ(report->reason(), std::optional<std::string>(refusal),
             "the refusal is reported for the settings to show");
  output->stop();
  REQUIRE_EQ(pipewire.stops, 1, "stop reaches the output that plays");
  REQUIRE_EQ(alsa.stops, 0, "the primary never started, so it is not stopped");

  alsa.refuse = false;
  REQUIRE_OK(output->start(rate, frames, std::nullopt), "the next start tries the primary again");
  require(alsa.running, "the primary plays once its device is free");
  REQUIRE_EQ(pipewire.starts, 1, "the fallback was not started again");
  REQUIRE_EQ(report->reason(), std::nullopt, "a successful primary start clears the report");
  output->stop();
  REQUIRE_EQ(alsa.stops, 1, "stop reaches the primary");
}

void both_failing_reports_both() {
  Device alsa;
  Device pipewire;
  alsa.refuse = true;
  pipewire.refuse = true;
  auto report = std::make_shared<application::OutputFallbackReport>();
  const auto factory =
      application::make_fallback_output_factory(factory_for(alsa), factory_for(pipewire), report);
  audio_io::TransportCursor cursor(0);
  auto output = factory({}, cursor);
  const auto started = output->start(rate, frames, std::nullopt);
  require(!started, "no output started");
  require(started.error().message.find(refusal) != std::string::npos,
          "the primary's failure is in the message");
  REQUIRE_EQ(report->reason(), std::nullopt, "nothing played, so nothing fell back");
}

void the_fallback_plays_to_its_default_device() {
  Device alsa;
  Device pipewire;
  alsa.refuse = true;
  constexpr std::uint64_t serial = 42;
  const auto factory =
      application::make_fallback_output_factory(factory_for(alsa), factory_for(pipewire), nullptr);
  audio_io::TransportCursor cursor(0);
  auto output = factory({}, cursor);
  REQUIRE_OK(output->start(rate, frames, serial), "starts without a report");
  REQUIRE_EQ(alsa.serial, std::optional<std::uint64_t>(serial), "the primary gets the target");
  REQUIRE_EQ(pipewire.serial, std::nullopt, "the fallback plays to its default device");
}

} // namespace

int main(int argc, char** argv) {
  return test::run_cases(
      argc, argv, "fallback output contract passed",
      {{"a_refused_card_plays_through_the_fallback", a_refused_card_plays_through_the_fallback},
       {"both_failing_reports_both", both_failing_reports_both},
       {"the_fallback_plays_to_its_default_device", the_fallback_plays_to_its_default_device}});
}
