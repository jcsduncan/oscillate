#include "../../../apps/daw_quick/src/native_synth_audition.hpp"

#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wait.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace audio = oscillate::audio_io;
namespace engine = oscillate::engine;
namespace synth = oscillate::native_dsp::synth;
namespace quick = oscillate::quick;

class PreviewKernel final : public synth::SynthKernel {
public:
  void control(std::span<const double>, std::uint32_t,
               const synth::SynthTransport&) noexcept override {}
  void note_on(const synth::NoteEvent& event) noexcept override {
    level_ += static_cast<float>(event.velocity) / 127.0F;
  }
  void note_off(const synth::NoteEvent&) noexcept override { level_ = 0.0F; }
  void render(std::span<engine::StereoFrame> output) noexcept override {
    for (auto& frame : output) {
      frame.left += level_;
      frame.right += level_;
    }
  }
  void reset() noexcept override { level_ = 0.0F; }
  [[nodiscard]] synth::KernelStatus status() const noexcept override { return {}; }

private:
  float level_ = 0.0F;
};

const synth::SynthEngine& preview_engine() {
  static const synth::EngineDescriptor descriptor{"preview-test", "Preview Test", 1, {}, {}};
  static const synth::SynthEngine selected{
      &descriptor,
      [](double) -> std::unique_ptr<synth::SynthKernel> {
        return std::make_unique<PreviewKernel>();
      },
      [](std::span<const double>, double) -> std::uint64_t { return 0; }, false};
  return selected;
}

struct ProbeState {
  audio::StereoRenderSource source;
  audio::TransportCursor* cursor = nullptr;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{false};
};

class ProbeOutput final : public app::PlaybackOutput {
public:
  explicit ProbeOutput(std::shared_ptr<ProbeState> probe) : probe_(std::move(probe)) {}

  [[nodiscard]] oscillate::foundation::Result<void> start(std::uint32_t, std::uint32_t,
                                                          std::optional<std::uint64_t>) override {
    probe_->started.store(true, std::memory_order_release);
    return oscillate::foundation::Result<void>::success();
  }
  void stop() noexcept override { probe_->stopped.store(true, std::memory_order_release); }
  [[nodiscard]] audio::OutputStatus status() const noexcept override {
    audio::OutputStatus result;
    result.state = probe_->stopped.load(std::memory_order_acquire) ? audio::OutputState::stopped
                                                                   : audio::OutputState::streaming;
    return result;
  }

private:
  std::shared_ptr<ProbeState> probe_;
};

using oscillate::test::require;

void pulse_pad_produces_audio_without_a_real_device() {
  auto probe = std::make_shared<ProbeState>();
  auto output_factory =
      [probe](audio::StereoRenderSource source,
              audio::TransportCursor& cursor) -> std::unique_ptr<app::PlaybackOutput> {
    probe->source = source;
    probe->cursor = &cursor;
    return std::make_unique<ProbeOutput>(probe);
  };
  const auto& pulse = synth::pulse::engine();
  const auto values = synth::default_values(*pulse.descriptor);
  {
    quick::NativeSynthAudition audition;
    require(audition.trigger(pulse, values, 48'000, 128, synth::pulse::first_key, 100, std::nullopt,
                             output_factory),
            "Pulse pad preview request failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    oscillate::test::wait_until([&] { return probe->started.load(std::memory_order_acquire); },
                                "the preview device started");
    require(probe->started.load(std::memory_order_acquire), "preview device did not start");

    std::array<engine::StereoFrame, 128> scratch{};
    std::array<float, 256> output{};
    bool sounded = false;
    while (!sounded && std::chrono::steady_clock::now() < deadline) {
      require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
                  audio::BlockResult::rendered,
              "Pulse preview render failed");
      for (const auto sample : output)
        sounded |= std::fabs(sample) > 0.0001F;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(sounded, "clicking a Pulse pad rendered silence");
    audition.stop();
  }
  require(probe->stopped.load(std::memory_order_acquire), "preview device stayed open");
}

void audition_uses_velocity_and_releases_the_note() {
  auto probe = std::make_shared<ProbeState>();
  auto output_factory =
      [probe](audio::StereoRenderSource source,
              audio::TransportCursor& cursor) -> std::unique_ptr<app::PlaybackOutput> {
    probe->source = source;
    probe->cursor = &cursor;
    return std::make_unique<ProbeOutput>(probe);
  };
  quick::NativeSynthAudition audition;
  require(audition.trigger(preview_engine(), {}, 48'000, 128, 60, 40, std::nullopt, output_factory),
          "pitched-note preview request failed");
  oscillate::test::wait_until([&] { return probe->started.load(std::memory_order_acquire); },
                              "the preview device started");
  require(probe->started.load(std::memory_order_acquire), "pitched preview did not start");

  std::array<engine::StereoFrame, 128> scratch{};
  std::array<float, 256> output{};
  require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
              audio::BlockResult::rendered,
          "pitched preview failed to render");
  require(std::fabs(output[0] - 0.5F * 40.0F / 127.0F) < 0.001F,
          "pitched preview ignored the requested velocity");
  require(audition.trigger(preview_engine(), {}, 48'000, 128, 60, 80, std::nullopt, output_factory,
                           true),
          "replacement preview request failed");
  bool replaced = false;
  for (int block = 0; block < 192; ++block) {
    require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
                audio::BlockResult::rendered,
            "pitched preview stopped rendering before release");
    if (std::fabs(output[0] - 0.5F * 80.0F / 127.0F) < 0.001F) {
      replaced = true;
      break;
    }
    std::this_thread::yield();
  }
  require(replaced, "replacement preview stacked or failed to adopt the new velocity");
  for (int block = 0; block < 192; ++block) {
    require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
                audio::BlockResult::rendered,
            "pitched preview stopped rendering before release");
  }
  require(std::fabs(output[0]) < 0.0001F, "pitched preview left its note held");
}
// Live playing: a held note sounds until its key is released, however long
// that is, and chords hold together.
void live_notes_hold_until_released() {
  auto probe = std::make_shared<ProbeState>();
  auto output_factory =
      [probe](audio::StereoRenderSource source,
              audio::TransportCursor& cursor) -> std::unique_ptr<app::PlaybackOutput> {
    probe->source = source;
    probe->cursor = &cursor;
    return std::make_unique<ProbeOutput>(probe);
  };
  quick::NativeSynthAudition audition;
  const auto sound = [] {
    return quick::NativeSynthAudition::Sound{&preview_engine(), {}, {}, {}};
  };
  require(audition.note_on(sound(), 48'000, 128, 60, 127, std::nullopt, output_factory),
          "live note request failed");
  oscillate::test::wait_until([&] { return probe->started.load(std::memory_order_acquire); },
                              "the preview device started");
  require(probe->started.load(std::memory_order_acquire), "live preview did not start");
  std::array<engine::StereoFrame, 128> scratch{};
  std::array<float, 256> output{};
  const auto render = [&] {
    require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
                audio::BlockResult::rendered,
            "live preview failed to render");
    return output[0];
  };
  (void)render();
  // Far longer than a triggered preview's half second: still sounding.
  for (int block = 0; block < 600; ++block)
    (void)render();
  require(std::fabs(render() - 0.5F) < 0.001F, "a held note stopped before its key was released");
  require(audition.note_off(60), "release request failed");
  float level = 1.0F;
  for (int block = 0; block < 400 && level > 0.0001F; ++block) {
    level = std::fabs(render());
    std::this_thread::yield();
  }
  require(level < 0.0001F, "a released note kept sounding");
}
} // namespace

// A preset preview's phrase: each note starts on its own frame, whatever
// the block size, counted from the block that took the first note.
void phrase_notes_land_on_their_frames() {
  auto probe = std::make_shared<ProbeState>();
  auto output_factory =
      [probe](audio::StereoRenderSource source,
              audio::TransportCursor& cursor) -> std::unique_ptr<app::PlaybackOutput> {
    probe->source = source;
    probe->cursor = &cursor;
    return std::make_unique<ProbeOutput>(probe);
  };
  quick::NativeSynthAudition audition;
  // The second note joins 300 frames in: the kernel's level doubles there.
  require(audition.phrase(preview_engine(), {}, 48'000, 128,
                          {{0, 2'000, 60, 127}, {300, 2'000, 64, 127}}, std::nullopt,
                          output_factory),
          "phrase request failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  oscillate::test::wait_until([&] { return probe->started.load(std::memory_order_acquire); },
                              "the preview device started");
  require(probe->started.load(std::memory_order_acquire), "phrase preview did not start");
  std::array<engine::StereoFrame, 128> scratch{};
  std::array<float, 256> output{};
  std::vector<float> heard;
  while (std::chrono::steady_clock::now() < deadline && heard.size() < 1'024) {
    require(audio::render_transport_block(probe->source, *probe->cursor, true, scratch, output) ==
                audio::BlockResult::rendered,
            "phrase preview failed to render");
    // Wait for the block that took the first note; then keep every frame.
    if (heard.empty() && std::fabs(output[0]) < 0.0001F) {
      std::this_thread::yield();
      continue;
    }
    for (std::size_t frame = 0; frame < 128; ++frame)
      heard.push_back(output[frame * 2]);
  }
  require(heard.size() >= 1'024, "the phrase never sounded");
  require(std::fabs(heard[0] - 0.5F) < 0.001F && std::fabs(heard[299] - 0.5F) < 0.001F,
          "the first note sounds alone until frame 300");
  require(std::fabs(heard[300] - 1.0F) < 0.001F, "the second note starts exactly on frame 300");
}

int main() {
  try {
    pulse_pad_produces_audio_without_a_real_device();
    audition_uses_velocity_and_releases_the_note();
    live_notes_hold_until_released();
    phrase_notes_land_on_their_frames();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
