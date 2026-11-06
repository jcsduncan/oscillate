#include "oscillate/clay_dsp/analysis.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include "synth_test_support.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Clay's polyphonic oracles on recorded audio: chords played on Concert,
// the sampled Salamander grand (ADR-0071). Without the library installed
// every case passes with a note saying it was skipped.
namespace {

using namespace oscillate;
namespace clay = oscillate::clay_dsp;
namespace concert = oscillate::native_dsp::synth::concert;

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::uint8_t velocity = 90;
constexpr double held_seconds = 1.5;
constexpr double staccato_seconds = 0.15;
// Rendered past the release so the dampers' fall is in the take.
constexpr double tail_seconds = 0.6;
// Every played key is found within this many cents (a real piano's stretch
// tuning moves its upper notes a few cents) and holds over this share of its
// held time; anything else is no longer than a fragment.
constexpr double key_tolerance_cents = 30.0;
constexpr double held_share = 0.9;
constexpr double fragment_seconds = 0.05;
constexpr double cents_per_key = 100.0;

using oscillate::test::require;

[[nodiscard]] bool library_ready() {
  const auto status = concert::library::status();
  if (status.state == concert::library::State::ready)
    return true;
  std::printf("skipped: Concert has no library (%s); install it with %s\n", status.message.c_str(),
              std::string(concert::library::install_command).c_str());
  return false;
}

[[nodiscard]] std::vector<float> play(std::span<const std::uint8_t> keys, double seconds) {
  const auto& engine = concert::engine();
  auto runtime =
      synth_test::make_runtime(engine, native_dsp::synth::default_values(*engine.descriptor), rate);
  const auto length = static_cast<std::uint64_t>(std::llround(seconds * rate));
  const auto frames = static_cast<std::size_t>(std::llround((seconds + tail_seconds) * rate));
  std::vector<synth_test::Event> events;
  std::uint64_t id = 1;
  for (const auto key : keys) {
    const auto n = synth_test::note(0, length, key, id++, velocity);
    events.insert(events.end(), n.begin(), n.end());
  }
  const auto out = synth_test::render(*runtime, events, frames, 128, rate);
  std::vector<float> mono(frames);
  for (std::size_t i = 0; i < frames; ++i)
    mono[i] = static_cast<float>(0.5 * (out.left[i] + out.right[i]));
  return mono;
}

[[nodiscard]] double seconds_of(std::int64_t frames) { return static_cast<double>(frames) / rate; }

// Exactly the keys played, each one note unbroken over its held time.
void chord(std::span<const std::uint8_t> keys, double seconds, const std::string& what) {
  const auto audio = play(keys, seconds);
  clay::AnalysisOptions options;
  options.mode = clay::Mode::polyphonic;
  options.minimum_hz = 40.0;
  auto analysed = clay::analyse({audio, {}, static_cast<std::uint32_t>(rate)}, options);
  REQUIRE_OK(analysed, what + ": analysis failed");
  const auto& take = analysed.value();
  std::string listing;
  for (const auto& n : take.notes)
    listing += " " + std::to_string(std::lround(n.centre)) + "@" +
               std::to_string(seconds_of(n.start)).substr(0, 5) + "-" +
               std::to_string(seconds_of(n.end)).substr(0, 5);
  std::vector<bool> played(take.notes.size(), false);
  for (const auto key : keys) {
    const double cents = key * cents_per_key;
    int count = 0;
    for (std::size_t i = 0; i < take.notes.size(); ++i) {
      const auto& n = take.notes[i];
      if (std::abs(n.centre - cents) > key_tolerance_cents ||
          seconds_of(n.end - n.start) <= fragment_seconds)
        continue;
      ++count;
      played[i] = true;
      require(seconds_of(n.start) <= (1.0 - held_share) * seconds &&
                  seconds_of(n.end) >= held_share * seconds,
              what + ": key " + std::to_string(key) + " is not held whole:" + listing);
    }
    REQUIRE_EQ(count, 1,
               what + ": key " + std::to_string(key) + " found " + std::to_string(count) +
                   " times:" + listing);
  }
  for (std::size_t i = 0; i < take.notes.size(); ++i)
    require(played[i] || seconds_of(take.notes[i].end - take.notes[i].start) <= fragment_seconds,
            what + ": a note nobody played:" + listing);
}

constexpr std::array<std::uint8_t, 4> am7_close{57, 60, 64, 67}; // A3 C4 E4 G4
constexpr std::array<std::uint8_t, 4> am7_open{45, 52, 55, 60};  // A2 E3 G3 C4
constexpr std::array<std::uint8_t, 5> am9_close{57, 60, 64, 67, 71};
constexpr std::array<std::uint8_t, 4> am9_open{45, 55, 60, 71}; // A2 G3 C4 B4
constexpr std::array<std::uint8_t, 3> c_close{60, 64, 67};      // C4 E4 G4
constexpr std::array<std::uint8_t, 3> c_open{48, 55, 64};       // C3 G3 E4

// Not a CTest case: the polyphonic analysis time of a 30-second piano take
// (a chord every two seconds through the six voicings), printed.
constexpr double timing_seconds = 30.0;
constexpr double timing_chord_seconds = 2.0;
void time_a_take() {
  const auto& engine = concert::engine();
  auto runtime =
      synth_test::make_runtime(engine, native_dsp::synth::default_values(*engine.descriptor), rate);
  const std::array<std::span<const std::uint8_t>, 6> voicings{am7_close, am7_open, am9_close,
                                                              am9_open,  c_close,  c_open};
  std::vector<synth_test::Event> events;
  std::uint64_t id = 1;
  const auto chord_frames = static_cast<std::uint64_t>(timing_chord_seconds * rate);
  for (std::uint64_t at = 0, i = 0; static_cast<double>(at + chord_frames) <= timing_seconds * rate;
       at += chord_frames, ++i) {
    for (const auto key : voicings[i % voicings.size()]) {
      const auto n = synth_test::note(at, chord_frames * 3 / 4, key, id++, velocity);
      events.insert(events.end(), n.begin(), n.end());
    }
  }
  const auto frames = static_cast<std::size_t>(timing_seconds * rate);
  const auto out = synth_test::render(*runtime, events, frames, 128, rate);
  std::vector<float> mono(frames);
  for (std::size_t i = 0; i < frames; ++i)
    mono[i] = static_cast<float>(0.5 * (out.left[i] + out.right[i]));
  clay::AnalysisOptions options;
  options.mode = clay::Mode::polyphonic;
  options.minimum_hz = 40.0;
  const auto began = std::chrono::steady_clock::now();
  const auto take = clay::analyse({mono, {}, static_cast<std::uint32_t>(rate)}, options);
  const std::chrono::duration<double> took = std::chrono::steady_clock::now() - began;
  REQUIRE_OK(take, "timing: analysis failed");
  std::printf("30 s piano take: %zu notes in %.2f s (%.1fx real time)\n", take.value().notes.size(),
              took.count(), timing_seconds / took.count());
}

} // namespace

int main(int argc, char** argv) {
  // Without the sample library there is nothing to analyse: CTest skips.
  if (!library_ready())
    return OSCILLATE_TEST_SKIP_RETURN_CODE;
  return oscillate::test::run_cases(
      argc, argv, "clay piano passed",
      {
          {"am7_close_held", [] { chord(am7_close, held_seconds, "Am7 close, held"); }},
          {"am7_open_held", [] { chord(am7_open, held_seconds, "Am7 open, held"); }},
          {"am7_close_staccato", [] { chord(am7_close, staccato_seconds, "Am7 close, staccato"); }},
          {"am9_close_held", [] { chord(am9_close, held_seconds, "Am9 close, held"); }},
          {"am9_open_held", [] { chord(am9_open, held_seconds, "Am9 open, held"); }},
          {"am9_open_staccato", [] { chord(am9_open, staccato_seconds, "Am9 open, staccato"); }},
          {"c_close_held", [] { chord(c_close, held_seconds, "C close, held"); }},
          {"c_open_held", [] { chord(c_open, held_seconds, "C open, held"); }},
          {"c_close_staccato", [] { chord(c_close, staccato_seconds, "C close, staccato"); }},
          {"c_open_staccato", [] { chord(c_open, staccato_seconds, "C open, staccato"); }},
          {"timing", time_a_take},
      });
}
