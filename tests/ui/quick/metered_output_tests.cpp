#include "../../../apps/daw_quick/src/metered_output.hpp"

#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace dsp = oscillate::native_dsp;
namespace quick = oscillate::quick;

using oscillate::test::require;

struct Source {
  float left = 0.5F;
  float right = 0.25F;
  bool frozen = false;
  bool failed = false;

  static bool render(void* opaque, std::span<engine::StereoFrame> frames,
                     engine::BlockContext) noexcept {
    const auto& source = *static_cast<Source*>(opaque);
    std::ranges::fill(frames, engine::StereoFrame{source.left, source.right});
    return !source.failed;
  }
  static bool freeze(void* opaque) noexcept { return static_cast<Source*>(opaque)->frozen; }
  [[nodiscard]] audio::StereoRenderSource callback() noexcept { return {this, render, freeze}; }
};

class ProbeOutput final : public app::PlaybackOutput {
public:
  ProbeOutput(audio::StereoRenderSource source, audio::TransportCursor& cursor)
      : source_(source), cursor_(cursor) {}

  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t rate, std::uint32_t,
                                                          std::optional<std::uint64_t>) override {
    ++start_count_;
    status_.requested_client_rate = rate;
    status_.state = audio::OutputState::streaming;
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override {
    ++stop_count_;
    status_.state = audio::OutputState::stopped;
  }
  [[nodiscard]] audio::OutputStatus status() const noexcept override { return status_; }
  [[nodiscard]] int start_count() const noexcept { return start_count_; }
  [[nodiscard]] int stop_count() const noexcept { return stop_count_; }

  [[nodiscard]] audio::BlockResult render(std::size_t frames) noexcept {
    return audio::render_transport_block(source_, cursor_, true, std::span{scratch_}.first(frames),
                                         std::span{interleaved_}.first(frames * 2));
  }
  [[nodiscard]] float first_left() const noexcept { return interleaved_[0]; }

private:
  audio::StereoRenderSource source_;
  audio::TransportCursor& cursor_;
  std::array<engine::StereoFrame, 128> scratch_{};
  std::array<float, 256> interleaved_{};
  audio::OutputStatus status_;
  int start_count_ = 0;
  int stop_count_ = 0;
};

struct Fixture {
  Source source;
  audio::TransportCursor cursor;
  std::shared_ptr<dsp::StereoMeter> meter = std::make_shared<dsp::StereoMeter>();
  std::shared_ptr<app::SpectrumMonitor> spectrum = std::make_shared<app::SpectrumMonitor>();
  std::shared_ptr<quick::LiveTruePeakMonitor> true_peak =
      std::make_shared<quick::LiveTruePeakMonitor>();
  ProbeOutput* probe = nullptr;
  std::unique_ptr<app::PlaybackOutput> output;

  Fixture() {
    auto downstream =
        [this](audio::StereoRenderSource render_source,
               audio::TransportCursor& transport) -> std::unique_ptr<app::PlaybackOutput> {
      auto device = std::make_unique<ProbeOutput>(render_source, transport);
      probe = device.get();
      return device;
    };
    output = quick::make_metered_output_factory(std::move(downstream), meter, spectrum,
                                                true_peak)(source.callback(), cursor);
    require(output && output->start(48'000, 128, std::nullopt).has_value(),
            "metered fake output did not start");
  }

  void render_window(std::size_t block_frames) {
    for (std::size_t offset = 0; offset < dsp::StereoMeter::window_frames; offset += block_frames)
      require(probe->render(block_frames) == audio::BlockResult::rendered,
              "metered fake block did not render");
  }
};

void measures_final_output_and_resets_on_seek_and_stop() {
  Fixture fixture;
  counted_allocations = 0;
  counting_allocations = true;
  fixture.render_window(64);
  counting_allocations = false;
  const auto first = fixture.meter->latest();
  require(counted_allocations == 0 && first && first->left_peak == 0.5F &&
              first->right_peak == 0.25F && first->left_rms == 0.5F && first->right_rms == 0.25F &&
              first->end_sample == 1024 && first->transport_generation == 1 && !first->clipped,
          "meter did not observe the final direct stereo output without allocations");

  require(fixture.cursor.request_seek(5000), "meter seek request failed");
  fixture.source.left = 1.25F;
  fixture.source.right = -0.5F;
  counted_allocations = 0;
  counting_allocations = true;
  fixture.render_window(128);
  counting_allocations = false;
  const auto sought = fixture.meter->latest();
  require(counted_allocations == 0 && sought && sought->left_peak == 1.25F &&
              sought->left_rms == 1.25F && sought->right_peak == 0.5F && sought->clipped &&
              sought->end_sample == 6024 && sought->transport_generation == 2,
          "seek reused an old window or lost clipping state");

  fixture.output->stop();
  const auto stopped = fixture.meter->latest();
  require(stopped && stopped->transport_generation == 0 && stopped->left_peak == 0.0F &&
              stopped->left_rms == 0.0F && !stopped->clipped &&
              fixture.output->status().state == audio::OutputState::stopped,
          "stopped output left stale level or clipping visible");
}

void spectrum_worker_observes_final_output_and_clears_on_stop() {
  Fixture fixture;
  fixture.render_window(64);
  fixture.render_window(64);
  std::optional<app::SpectrumReading> reading;
  for (int attempt = 0; attempt < 200 && !reading; ++attempt) {
    reading = fixture.spectrum->latest();
    if (!reading)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(reading && reading->end_sample == 2048 &&
              std::abs(reading->spectrum.left[0] - 0.5F) < 1e-5F &&
              std::abs(reading->spectrum.right[0] - 0.25F) < 1e-5F,
          "spectrum worker did not analyze the final stereo output");
  fixture.output->stop();
  require(!fixture.spectrum->latest(), "stopped output retained a stale master spectrum");
}

void true_peak_worker_observes_final_output_and_invalidates_on_transport_change() {
  Fixture fixture;
  fixture.render_window(64);
  std::optional<quick::LiveTruePeakReading> reading;
  for (int attempt = 0; attempt < 200 && (!reading || reading->end_sample != 1024); ++attempt) {
    reading = fixture.true_peak->latest();
    if (!reading || reading->end_sample != 1024)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(reading && reading->complete && reading->peak.frames == 1024 &&
              reading->peak.left_true_peak >= 0.5F && reading->peak.left_true_peak < 0.6F &&
              reading->peak.right_true_peak >= 0.25F && reading->peak.right_true_peak < 0.3F &&
              reading->transport_generation == 1,
          "true-peak worker did not analyze the final master output");

  require(fixture.cursor.request_seek(5000), "true-peak seek request failed");
  require(fixture.probe->render(64) == audio::BlockResult::rendered,
          "true-peak post-seek render failed");
  require(!fixture.true_peak->latest(), "seek exposed a stale true-peak reading");
  fixture.output->stop();
  require(!fixture.true_peak->latest() && !fixture.true_peak->incomplete(),
          "stop exposed a stale true-peak reading");
}

void true_peak_dropped_block_never_claims_a_complete_stream() {
  quick::LiveTruePeakMonitor monitor;
  monitor.set_sample_rate_after_quiesce(48'000);
  const std::array<engine::StereoFrame, 64> frames{{{0.5F, 0.25F}}};
  monitor.observe(frames,
                  engine::BlockContext{.project_sample_position = 0, .transport_generation = 1});
  std::array<engine::StereoFrame, quick::LiveTruePeakMonitor::max_callback_frames + 1> oversized{};
  monitor.observe(oversized,
                  engine::BlockContext{.project_sample_position = 64, .transport_generation = 1});
  require(monitor.dropped_blocks() == 1 && monitor.incomplete() && !monitor.latest(),
          "rejected callback block left a complete true-peak reading visible");
  monitor.observe(frames, engine::BlockContext{.project_sample_position = 64 + oversized.size(),
                                               .transport_generation = 1});
  std::optional<quick::LiveTruePeakReading> incomplete;
  for (int attempt = 0; attempt < 200 && !incomplete; ++attempt) {
    incomplete = monitor.latest();
    if (!incomplete)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(incomplete && !incomplete->complete && incomplete->peak.frames == 64,
          "missing master audio was reported as a complete true-peak stream");
  monitor.observe(frames,
                  engine::BlockContext{.project_sample_position = 5000, .transport_generation = 2});
  require(!monitor.latest() && !monitor.incomplete(), "seek left partial true-peak data visible");
  std::optional<quick::LiveTruePeakReading> resumed;
  for (int attempt = 0; attempt < 200 && (!resumed || resumed->transport_generation != 2);
       ++attempt) {
    resumed = monitor.latest();
    if (!resumed || resumed->transport_generation != 2)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(resumed && resumed->complete && resumed->peak.frames == 64,
          "fresh transport did not restore a complete true-peak reading");
}

void true_peak_saturated_worker_queue_does_not_publish_a_complete_peak() {
  quick::LiveTruePeakMonitor monitor;
  monitor.set_sample_rate_after_quiesce(48'000);
  std::array<engine::StereoFrame, quick::LiveTruePeakMonitor::max_callback_frames> frames{};
  std::ranges::fill(frames, engine::StereoFrame{0.5F, 0.25F});
  for (std::size_t block = 0; block < quick::LiveTruePeakMonitor::queue_capacity * 4; ++block) {
    monitor.observe(frames, engine::BlockContext{.project_sample_position = block * frames.size(),
                                                 .transport_generation = 1});
  }
  const auto reading = monitor.latest();
  require(monitor.dropped_blocks() > 0 && monitor.incomplete() && (!reading || !reading->complete),
          "saturated true-peak worker queue claimed missing audio as complete");
}

void forwards_hold_and_meters_failed_render_as_silence() {
  Fixture fixture;
  fixture.source.frozen = true;
  fixture.source.left = 0.0F;
  fixture.source.right = 0.0F;
  require(fixture.probe->render(64) == audio::BlockResult::rendered &&
              fixture.cursor.completed_snapshot().next_sample == 0,
          "meter wrapper consumed a held source block");
  fixture.source.frozen = false;
  fixture.source.left = 0.5F;
  fixture.render_window(64);
  const auto resumed = fixture.meter->latest();
  require(resumed && resumed->left_peak == 0.5F && resumed->end_sample == 1024 &&
              fixture.cursor.completed_snapshot().next_sample == 1024,
          "meter wrapper failed to reset the held window at resume");

  fixture.source.failed = true;
  fixture.source.left = 1.5F;
  require(fixture.probe->render(64) == audio::BlockResult::silent_render_failure &&
              fixture.probe->first_left() == 0.0F,
          "failed upstream render was not silent at the output");
  for (int block = 1; block < 16; ++block)
    require(fixture.probe->render(64) == audio::BlockResult::silent_render_failure,
            "failed output window changed result");
  const auto failed = fixture.meter->latest();
  require(failed && failed->left_peak == 0.0F && failed->left_rms == 0.0F && !failed->clipped,
          "meter reported partially written audio that device silenced");
}

void restarted_output_stops_the_restarted_device() {
  Fixture fixture;
  fixture.output->stop();
  require(fixture.probe->stop_count() == 1, "first stop did not quiesce the downstream output");
  require(fixture.output->start(48'000, 128, std::nullopt).has_value() &&
              fixture.probe->start_count() == 2,
          "metered output did not restart its downstream output");
  fixture.output->stop();
  require(fixture.probe->stop_count() == 2 &&
              fixture.output->status().state == audio::OutputState::stopped,
          "restart left the downstream output active after stop");
}

void concurrent_ui_reads_are_coherent() {
  Fixture fixture;
  std::atomic<bool> finished{false};
  std::atomic<bool> invalid_read{false};
  std::jthread observer([&] {
    while (!finished.load(std::memory_order_acquire)) {
      const auto latest = fixture.meter->latest();
      if (latest && (latest->left_peak != 0.0F && latest->left_peak != 0.5F))
        invalid_read.store(true, std::memory_order_relaxed);
    }
  });
  for (int block = 0; block < 16'000; ++block)
    require(fixture.probe->render(64) == audio::BlockResult::rendered,
            "concurrent meter render failed");
  finished.store(true, std::memory_order_release);
  observer.join();
  require(!invalid_read.load(std::memory_order_relaxed), "concurrent meter read was torn");
}

void rejected_output_does_not_publish_or_retain_callback_context() {
  Source source;
  audio::TransportCursor cursor;
  auto meter = std::make_shared<dsp::StereoMeter>();
  auto spectrum = std::make_shared<app::SpectrumMonitor>();
  auto true_peak = std::make_shared<quick::LiveTruePeakMonitor>();
  app::PlaybackOutputFactory unavailable =
      [](audio::StereoRenderSource,
         audio::TransportCursor&) -> std::unique_ptr<app::PlaybackOutput> { return {}; };
  auto output = quick::make_metered_output_factory(unavailable, meter, spectrum,
                                                   true_peak)(source.callback(), cursor);
  require(!output && !meter->latest(), "rejected output published stale meter state");
}

void default_output_rejects_invalid_rate_without_a_device() {
  Source source;
  audio::TransportCursor cursor;
  auto meter = std::make_shared<dsp::StereoMeter>();
  auto spectrum = std::make_shared<app::SpectrumMonitor>();
  auto true_peak = std::make_shared<quick::LiveTruePeakMonitor>();
  auto output =
      quick::make_metered_output_factory({}, meter, spectrum, true_peak)(source.callback(), cursor);
  require(output && output->status().state == audio::OutputState::stopped,
          "default metered output was not constructed");
  const auto invalid = output->start(0, 128, std::nullopt);
  require(!invalid && invalid.error().code == oscillate::foundation::ErrorCode::invalid_argument,
          "default output accepted an invalid client rate");
  output->stop();
  require(meter->latest() && meter->latest()->transport_generation == 0,
          "default output stop did not clear the meter");
}

double bench_path(bool metered, std::size_t block_frames) {
  Source source;
  audio::TransportCursor cursor;
  auto meter = std::make_shared<dsp::StereoMeter>();
  auto spectrum = std::make_shared<app::SpectrumMonitor>();
  auto true_peak = std::make_shared<quick::LiveTruePeakMonitor>();
  ProbeOutput* probe = nullptr;
  auto downstream =
      [&probe](audio::StereoRenderSource render_source,
               audio::TransportCursor& transport) -> std::unique_ptr<app::PlaybackOutput> {
    auto output = std::make_unique<ProbeOutput>(render_source, transport);
    probe = output.get();
    return output;
  };
  auto factory = metered
                     ? quick::make_metered_output_factory(downstream, meter, spectrum, true_peak)
                     : downstream;
  auto output = factory(source.callback(), cursor);
  require(output && output->start(48'000, 128, std::nullopt).has_value(),
          "benchmark output failed");
  constexpr int warm_blocks = 10'000;
  constexpr int timed_blocks = 1'000'000;
  for (int index = 0; index < warm_blocks; ++index)
    require(probe->render(block_frames) == audio::BlockResult::rendered, "benchmark warmup failed");
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < timed_blocks; ++index)
    require(probe->render(block_frames) == audio::BlockResult::rendered, "benchmark render failed");
  const auto elapsed =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
  output->stop();
  return elapsed.count() / timed_blocks;
}

// The master section's mono switch folds only what the device hears: the
// meters keep reading the stereo mix.
void mono_listen_folds_only_the_device_output() {
  Source source;
  source.left = 0.5F;
  source.right = -0.25F;
  audio::TransportCursor cursor;
  auto meter = std::make_shared<dsp::StereoMeter>();
  auto spectrum = std::make_shared<app::SpectrumMonitor>();
  auto true_peak = std::make_shared<quick::LiveTruePeakMonitor>();
  auto mono = std::make_shared<std::atomic<bool>>(false);
  ProbeOutput* probe = nullptr;
  auto downstream =
      [&probe](audio::StereoRenderSource render_source,
               audio::TransportCursor& transport) -> std::unique_ptr<app::PlaybackOutput> {
    auto device = std::make_unique<ProbeOutput>(render_source, transport);
    probe = device.get();
    return device;
  };
  auto output = quick::make_metered_output_factory(downstream, meter, spectrum, true_peak,
                                                   mono)(source.callback(), cursor);
  require(output && output->start(48'000, 128, std::nullopt).has_value(), "output did not start");
  require(probe->render(64) == audio::BlockResult::rendered && probe->first_left() == 0.5F,
          "stereo output changed with mono off");
  mono->store(true);
  for (std::size_t offset = 64; offset < dsp::StereoMeter::window_frames; offset += 64)
    require(probe->render(64) == audio::BlockResult::rendered, "mono render failed");
  require(probe->first_left() == 0.125F, "mono output is not (L + R) / 2");
  const auto reading = meter->latest();
  require(reading && reading->left_peak == 0.5F && reading->right_peak == 0.25F,
          "the meters read the folded output instead of the mix");
}

// One second of a -20 dBFS 997 Hz sine on both channels reads -20 LUFS
// (BS.1770: 0 dBFS on two channels is 0 LKFS), in phase, losing nothing in
// mono; a reset empties integrated loudness and keeps the rest.
void master_analysis_reads_loudness_and_phase() {
  quick::LiveTruePeakMonitor monitor;
  monitor.set_sample_rate_after_quiesce(48'000);
  constexpr std::size_t block = 128;
  constexpr std::size_t total = 48'000 * 3 + 9'600;
  std::array<engine::StereoFrame, block> frames{};
  std::size_t written = 0;
  while (written < total) {
    for (std::size_t burst = 0; burst < 32 && written < total; ++burst) {
      for (std::size_t n = 0; n < block; ++n) {
        const auto t =
            2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(written + n) / 48'000.0;
        const auto value = 0.1F * static_cast<float>(std::sin(t));
        frames[n] = {value, value};
      }
      monitor.observe(frames, engine::BlockContext{.project_sample_position = written,
                                                   .transport_generation = 1});
      written += block;
    }
    // Let the worker drain, so no block is dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  std::optional<quick::LiveTruePeakReading> reading;
  for (int attempt = 0; attempt < 500 && (!reading || reading->end_sample != written); ++attempt) {
    reading = monitor.latest();
    if (!reading || reading->end_sample != written)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  require(monitor.dropped_blocks() == 0 && reading && reading->end_sample == written,
          "the master analysis did not keep up");
  const auto near = [](const std::optional<double>& value, double expected, double tolerance) {
    return value && std::abs(*value - expected) <= tolerance;
  };
  require(near(reading->integrated_lufs, -20.0, 0.3) &&
              near(reading->short_term_lufs, -20.0, 0.3) &&
              near(reading->momentary_lufs, -20.0, 0.3),
          "loudness of a -20 dBFS sine pair is not -20 LUFS");
  require(near(reading->correlation, 1.0, 1e-3) && near(reading->mono_loss_db, 0.0, 0.01),
          "an identical pair did not read as mono");
  monitor.reset_loudness();
  monitor.observe(
      frames, engine::BlockContext{.project_sample_position = written, .transport_generation = 1});
  written += block;
  for (int attempt = 0; attempt < 500 && (!reading || reading->end_sample != written); ++attempt) {
    reading = monitor.latest();
    if (!reading || reading->end_sample != written)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  require(reading && reading->end_sample == written && !reading->integrated_lufs &&
              reading->correlation && reading->peak.frames > 0,
          "a loudness reset did not empty integrated loudness alone");
}

void benchmark() {
  for (const std::size_t frames : {64U, 128U}) {
    std::array<double, 5> bare{};
    std::array<double, 5> metered{};
    for (std::size_t run = 0; run < bare.size(); ++run) {
      bare[run] = bench_path(false, frames);
      metered[run] = bench_path(true, frames);
    }
    std::ranges::sort(bare);
    std::ranges::sort(metered);
    std::printf("frames=%zu bare_median_us=%.3f metered_median_us=%.3f delta_us=%.3f\n", frames,
                bare[2], metered[2], metered[2] - bare[2]);
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      benchmark();
      return 0;
    }
    measures_final_output_and_resets_on_seek_and_stop();
    spectrum_worker_observes_final_output_and_clears_on_stop();
    true_peak_worker_observes_final_output_and_invalidates_on_transport_change();
    true_peak_dropped_block_never_claims_a_complete_stream();
    true_peak_saturated_worker_queue_does_not_publish_a_complete_peak();
    forwards_hold_and_meters_failed_render_as_silence();
    restarted_output_stops_the_restarted_device();
    concurrent_ui_reads_are_coherent();
    rejected_output_does_not_publish_or_retain_callback_context();
    default_output_rejects_invalid_rate_without_a_device();
    mono_listen_folds_only_the_device_output();
    master_analysis_reads_loudness_and_phase();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
