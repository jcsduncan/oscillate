// The direct ALSA output (ADR-0130) against ALSA's own null and file PCMs,
// so it runs without a sound card: period negotiation, sample formats and
// channel mapping, xrun recovery, device loss, clean shutdown and the card
// reservation.
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wait.hpp"

#include <oscillate/audio_io/alsa_output.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace oscillate::audio_io;
using oscillate::test::require;
using oscillate::test::wait_until;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr auto period_128 = static_cast<std::uint32_t>(oscillate::test::block_128);
constexpr auto period_64 = static_cast<std::uint32_t>(oscillate::test::block_64);
constexpr std::uint32_t three_periods = 3;
constexpr std::uint32_t four_channels = 4;
constexpr std::uint64_t start_sample = 100;
// Periods to see rendered before a check: past the prefill and a recovery.
constexpr std::uint64_t settled_periods = 8;
constexpr std::uint64_t xrun_period = 2;
constexpr std::uint64_t lost_period = 1;
constexpr std::uint64_t counted_period = 3;
// The renderer's ramp step: frame i is (i + 1) / ramp_divisor, so a
// 64-frame block stays well inside full scale.
constexpr float ramp_divisor = 256.0F;
constexpr const char* null_pcm = "null";
// A card number no machine has: it parses as a card but never opens.
constexpr const char* missing_card_pcm = "hw:31,0";
constexpr int missing_card = 31;
constexpr const char* first_card_pcm = "hw:0,0";

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a period, then its count.
AlsaStartConfig config(std::string device, std::uint32_t period, std::uint32_t periods,
                       std::uint32_t device_rate = rate) {
  AlsaStartConfig result;
  result.device = std::move(device);
  result.project_rate = device_rate;
  result.period_frames = period;
  result.periods = periods;
  return result;
}

struct Renderer {
  std::atomic<std::uint64_t> calls{0};
  static bool render(void* context, std::span<oscillate::engine::StereoFrame> output,
                     oscillate::engine::BlockContext) noexcept {
    auto& self = *static_cast<Renderer*>(context);
    for (std::size_t index = 0; index < output.size(); ++index) {
      const float value = static_cast<float>(index + 1) / ramp_divisor;
      output[index] = {value, -value};
    }
    self.calls.fetch_add(1, std::memory_order_release);
    return true;
  }
  [[nodiscard]] std::uint64_t count() const noexcept {
    return calls.load(std::memory_order_acquire);
  }
};

struct FakeReservation final : DeviceReservation {
  bool refuse = false;
  int acquires = 0;
  int releases = 0;
  std::optional<int> card;
  oscillate::foundation::Result<Outcome> acquire(int requested) override {
    ++acquires;
    card = requested;
    if (refuse)
      return oscillate::foundation::Result<Outcome>::failure(
          {oscillate::foundation::ErrorCode::conflict, "the sound server keeps it"});
    return oscillate::foundation::Result<Outcome>::success(Outcome::reserved);
  }
  void release() noexcept override { ++releases; }
};

// Injects one error before the given period's write.
struct Injector {
  std::uint64_t period = 0;
  int error = 0;
  static int before_write(void* context, std::uint64_t period) noexcept {
    const auto& self = *static_cast<const Injector*>(context);
    return period == self.period ? self.error : 0;
  }
};

// Counts the I/O thread's allocations over one whole period: from the hook
// before one write to the hook before the next.
struct AllocationProbe {
  std::atomic<bool> measured{false};
  std::atomic<std::size_t> allocations{0};
  static int before_write(void* context, std::uint64_t period) noexcept {
    auto& self = *static_cast<AllocationProbe*>(context);
    if (period == counted_period) {
      oscillate::test::counted_allocations = 0;
      oscillate::test::counting_allocations = true;
    } else if (period == counted_period + 1) {
      oscillate::test::counting_allocations = false;
      self.allocations.store(oscillate::test::counted_allocations, std::memory_order_relaxed);
      self.measured.store(true, std::memory_order_release);
    }
    return 0;
  }
};

void negotiates_periods_and_shuts_down_cleanly() {
  Renderer renderer;
  TransportCursor cursor(start_sample);
  auto reservation = std::make_shared<FakeReservation>();
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, reservation);
  REQUIRE_OK(output.start(config(null_pcm, period_128, three_periods, rate)),
             "the null PCM starts");
  require(!output.start(config(null_pcm, period_128, three_periods, rate)),
          "a running output refuses a second start");
  auto status = output.status();
  REQUIRE_EQ(status.state, OutputState::streaming, "the output streams");
  REQUIRE_EQ(status.period_frames, period_128, "the requested period is negotiated");
  REQUIRE_EQ(status.periods, three_periods, "the requested period count is negotiated");
  REQUIRE_EQ(status.buffer_frames, period_128 * three_periods, "the buffer is the periods");
  REQUIRE_EQ(status.channels, 2U, "a PCM taking any count gets stereo");
  REQUIRE_EQ(status.format, AlsaSampleFormat::s32_le, "the first preferred format is chosen");
  REQUIRE_EQ(status.rate, rate, "the project rate is the device rate");
  REQUIRE_EQ(reservation->acquires, 0, "a PCM that is not a card is not reserved");
  require(!status.reserved, "a PCM that is not a card is not reserved");
  wait_until([&] { return renderer.count() >= settled_periods; }, "the I/O thread renders");
  output.stop();
  const auto rendered = renderer.count();
  REQUIRE_EQ(output.status().state, OutputState::stopped, "stop joins and closes");
  REQUIRE_EQ(renderer.count(), rendered, "nothing renders after stop");
  REQUIRE_GT(cursor.completed_snapshot().next_sample, start_sample,
             "the transport advanced while streaming");
  REQUIRE_OK(output.start(config(null_pcm, period_64, alsa_min_periods, rate)),
             "a stopped output restarts");
  REQUIRE_EQ(output.status().period_frames, period_64, "the restart negotiates afresh");
  REQUIRE_EQ(output.status().xruns, 0U, "a restart counts xruns from zero");
  output.stop();
}

void refuses_invalid_configurations() {
  Renderer renderer;
  TransportCursor cursor(0);
  AlsaOutput output({&renderer, &Renderer::render}, cursor);
  constexpr std::uint32_t odd_rate = 12'345;
  require(!output.start(config(null_pcm, period_128, alsa_min_periods - 1, rate)), "one period");
  require(!output.start(config(null_pcm, period_128, alsa_max_periods + 1, rate)),
          "too many periods");
  require(!output.start(config(null_pcm, 0, alsa_default_periods, rate)), "a zero period");
  require(!output.start(config(null_pcm, period_128, alsa_default_periods, odd_rate)),
          "an odd rate");
  require(!output.start(config("", period_128, alsa_default_periods, rate)), "no device");
  auto mono = config(null_pcm, period_128, alsa_default_periods);
  mono.channels = 1;
  require(!output.start(mono), "fewer than two channels");
  REQUIRE_EQ(renderer.count(), 0U, "nothing rendered");
}

void converts_each_format_onto_the_first_two_channels() {
  // Full scale, its negative, a half and an overload that clips.
  constexpr std::size_t stereo_frames = 2;
  const std::array<float, stereo_frames * 2> stereo{1.0F, -1.0F, 0.5F, 2.0F};
  constexpr std::size_t widest_sample = 4;
  std::array<std::byte, stereo_frames * four_channels * widest_sample> device{};
  const auto written = [&device](AlsaSampleFormat format, std::uint32_t channels) {
    std::vector<unsigned> values;
    const auto count = stereo_frames * channels * alsa_sample_bytes(format);
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
      values.push_back(static_cast<unsigned>(device[index]));
    return values;
  };
  convert_stereo_to_device(stereo, AlsaSampleFormat::s16_le, 2, device);
  REQUIRE_EQ(written(AlsaSampleFormat::s16_le, 2),
             (std::vector<unsigned>{0xFF, 0x7F, 0x01, 0x80, 0x00, 0x40, 0xFF, 0x7F}),
             "S16: 32767, -32767, 16384 (half, rounded) and a clipped 32767");
  convert_stereo_to_device(stereo, AlsaSampleFormat::s24_3le, 2, device);
  REQUIRE_EQ(written(AlsaSampleFormat::s24_3le, 2),
             (std::vector<unsigned>{0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x80, 0x00, 0x00, 0x40, 0xFF,
                                    0xFF, 0x7F}),
             "S24_3LE: three little-endian bytes a sample");
  convert_stereo_to_device(stereo, AlsaSampleFormat::s32_le, 2, device);
  REQUIRE_EQ(written(AlsaSampleFormat::s32_le, 2),
             (std::vector<unsigned>{0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00,
                                    0x00, 0x40, 0xFF, 0xFF, 0xFF, 0x7F}),
             "S32_LE: four little-endian bytes a sample");
  device.fill(std::byte{0xAA});
  convert_stereo_to_device(stereo, AlsaSampleFormat::s16_le, four_channels, device);
  REQUIRE_EQ(written(AlsaSampleFormat::s16_le, four_channels),
             (std::vector<unsigned>{0xFF, 0x7F, 0x01, 0x80, 0, 0, 0, 0, 0x00, 0x40, 0xFF, 0x7F, 0,
                                    0, 0, 0}),
             "four channels: stereo on the first two, silence on the rest");
}

void writes_the_device_format_through_a_file_pcm() {
  const oscillate::test::TempDir directory("oscillate-alsa");
  const auto path = (directory.path() / "out.raw").string();
  Renderer renderer;
  TransportCursor cursor(0);
  AlsaOutput output({&renderer, &Renderer::render}, cursor);
  auto file = config("file:'" + path + "',raw", period_64, alsa_min_periods);
  file.format = AlsaSampleFormat::s24_3le;
  file.channels = four_channels;
  REQUIRE_OK(output.start(file), "the file PCM starts");
  REQUIRE_EQ(output.status().format, AlsaSampleFormat::s24_3le, "the forced format is used");
  REQUIRE_EQ(output.status().channels, four_channels, "the forced channel count is used");
  wait_until([&] { return renderer.count() >= settled_periods; }, "the I/O thread renders");
  output.stop();
  std::ifstream written(path, std::ios::binary);
  const std::vector<char> raw{std::istreambuf_iterator<char>(written), {}};
  const std::size_t frame_bytes = four_channels * alsa_sample_bytes(AlsaSampleFormat::s24_3le);
  const std::size_t prefill = static_cast<std::size_t>(period_64) * alsa_min_periods * frame_bytes;
  require(raw.size() >= prefill + period_64 * frame_bytes,
          "the file holds the prefill and a period");
  for (std::size_t index = 0; index < prefill; ++index)
    REQUIRE_EQ(raw[index], 0, "the stream starts with a buffer of silence");
  std::vector<float> stereo;
  for (std::size_t frame = 0; frame < period_64; ++frame) {
    const float value = static_cast<float>(frame + 1) / ramp_divisor;
    stereo.push_back(value);
    stereo.push_back(-value);
  }
  std::vector<std::byte> expected(period_64 * frame_bytes);
  convert_stereo_to_device(stereo, AlsaSampleFormat::s24_3le, four_channels, expected);
  for (std::size_t index = 0; index < expected.size(); ++index)
    REQUIRE_EQ(static_cast<std::byte>(raw[prefill + index]), expected[index],
               "the first rendered period follows the silence, converted");
}

void recovers_an_injected_xrun() {
  Renderer renderer;
  TransportCursor cursor(0);
  Injector injector{xrun_period, -EPIPE};
  const AlsaFaults faults{&injector, &Injector::before_write};
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, nullptr, &faults);
  REQUIRE_OK(output.start(config(null_pcm, period_128, alsa_default_periods, rate)), "starts");
  wait_until([&] { return renderer.count() >= settled_periods; }, "rendering goes on");
  const auto status = output.status();
  REQUIRE_EQ(status.xruns, 1U, "the underrun is counted");
  REQUIRE_EQ(status.state, OutputState::streaming, "snd_pcm_recover restarts the stream");
  output.stop();
}

void a_lost_device_stops_streaming() {
  Renderer renderer;
  TransportCursor cursor(0);
  Injector injector{lost_period, -ENODEV};
  const AlsaFaults faults{&injector, &Injector::before_write};
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, nullptr, &faults);
  REQUIRE_OK(output.start(config(null_pcm, period_128, alsa_default_periods, rate)), "starts");
  wait_until([&] { return output.status().state != OutputState::streaming; },
             "the I/O thread gives up");
  REQUIRE_EQ(output.status().state, OutputState::disconnected, "a vanished card is disconnected");
  REQUIRE_EQ(output.status().xruns, 0U, "device loss is not an xrun");
  output.stop();
  REQUIRE_EQ(output.status().state, OutputState::stopped, "stop after loss is clean");
}

void the_io_thread_never_allocates() {
  Renderer renderer;
  TransportCursor cursor(0);
  AllocationProbe probe;
  const AlsaFaults faults{&probe, &AllocationProbe::before_write};
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, nullptr, &faults);
  REQUIRE_OK(output.start(config(null_pcm, period_128, alsa_default_periods, rate)), "starts");
  wait_until([&] { return probe.measured.load(std::memory_order_acquire); },
             "a whole period was measured");
  output.stop();
  REQUIRE_EQ(probe.allocations.load(), 0U, "render, convert, write and delay do not allocate");
}

void a_refused_reservation_keeps_the_card_closed() {
  Renderer renderer;
  TransportCursor cursor(0);
  auto reservation = std::make_shared<FakeReservation>();
  reservation->refuse = true;
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, reservation);
  const auto started = output.start(config(first_card_pcm, period_128, alsa_default_periods, rate));
  require(!started, "a refused reservation does not start");
  REQUIRE_EQ(started.error().code, oscillate::foundation::ErrorCode::conflict,
             "the refusal is a conflict, for the caller to fall back");
  REQUIRE_EQ(reservation->acquires, 1, "card 0 was asked for");
  REQUIRE_EQ(reservation->card, 0, "card 0 was asked for");
  REQUIRE_EQ(reservation->releases, 0, "nothing was taken, so nothing is given back");
  REQUIRE_EQ(output.status().state, OutputState::error, "the output reports the failure");
  REQUIRE_EQ(renderer.count(), 0U, "nothing rendered");
}

void a_card_that_does_not_open_is_given_back() {
  Renderer renderer;
  TransportCursor cursor(0);
  auto reservation = std::make_shared<FakeReservation>();
  AlsaOutput output({&renderer, &Renderer::render}, cursor, {}, reservation);
  require(!output.start(config(missing_card_pcm, period_128, alsa_default_periods, rate)),
          "a missing card does not start");
  REQUIRE_EQ(reservation->card, missing_card, "its reservation was taken");
  REQUIRE_EQ(reservation->releases, 1, "and given back");
}

// The UI reads status while a transport worker starts and stops the output
// (run under TSan in CI's sanitizer sweep).
void status_reads_race_nothing_while_starting_and_stopping() {
  constexpr int restarts = 20;
  Renderer renderer;
  TransportCursor cursor(0);
  AlsaOutput output({&renderer, &Renderer::render}, cursor);
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> reads{0};
  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      const auto status = output.status();
      require(status.periods == 0 || status.periods == alsa_default_periods,
              "a status shows the whole configuration or none of it");
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int restart = 0; restart < restarts; ++restart) {
    REQUIRE_OK(output.start(config(null_pcm, period_128, alsa_default_periods)), "starts");
    output.stop();
  }
  wait_until([&] { return reads.load(std::memory_order_relaxed) > 0; }, "status was read");
  done.store(true, std::memory_order_release);
  reader.join();
  REQUIRE_EQ(output.status().periods, 0U, "a stopped output reports no device");
}

void names_cards() {
  REQUIRE_EQ(alsa_card_of("hw:2,0"), 2, "hw:N,D");
  REQUIRE_EQ(alsa_card_of("hw:3"), 3, "hw:N");
  REQUIRE_EQ(alsa_card_of("plughw:CARD=1,DEV=0"), 1, "a numeric CARD=");
  REQUIRE_EQ(alsa_card_of("hw:CARD=NoSuchCardHere,DEV=0"), std::nullopt, "an unknown card id");
  REQUIRE_EQ(alsa_card_of(null_pcm), std::nullopt, "the null PCM");
  REQUIRE_EQ(alsa_card_of("file:'/x',raw"), std::nullopt, "a file PCM");
  REQUIRE_EQ(alsa_card_of("hw:"), std::nullopt, "no card");
  const auto devices = list_alsa_playback_devices();
  REQUIRE_OK(devices, "listing cards succeeds with or without any");
  for (const auto& device : devices.value())
    REQUIRE_EQ(alsa_card_of(device.id), device.card, "each listed id names its card");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "ALSA output contract passed",
      {{"negotiates_periods_and_shuts_down_cleanly", negotiates_periods_and_shuts_down_cleanly},
       {"refuses_invalid_configurations", refuses_invalid_configurations},
       {"converts_each_format_onto_the_first_two_channels",
        converts_each_format_onto_the_first_two_channels},
       {"writes_the_device_format_through_a_file_pcm", writes_the_device_format_through_a_file_pcm},
       {"recovers_an_injected_xrun", recovers_an_injected_xrun},
       {"a_lost_device_stops_streaming", a_lost_device_stops_streaming},
       {"the_io_thread_never_allocates", the_io_thread_never_allocates},
       {"a_refused_reservation_keeps_the_card_closed", a_refused_reservation_keeps_the_card_closed},
       {"a_card_that_does_not_open_is_given_back", a_card_that_does_not_open_is_given_back},
       {"status_reads_race_nothing_while_starting_and_stopping",
        status_reads_race_nothing_while_starting_and_stopping},
       {"names_cards", names_cards}});
}
