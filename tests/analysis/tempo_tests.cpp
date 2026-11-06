#include "oscillate/analysis/tempo.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
namespace fs = std::filesystem;
constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;

using oscillate::test::require;

using oscillate::test::take;

class WavFixture {
public:
  explicit WavFixture(const std::vector<std::int16_t>& samples) {
    std::string pattern = (fs::temp_directory_path() / "oscillate-tempo-XXXXXX").string();
    const auto fd = ::mkstemp(pattern.data());
    REQUIRE_GE(fd, 0, "fixture creation failed");
    path_ = pattern;
    ::close(fd);
    oscillate::test::write_wav_pcm16(path_, rate, oscillate::test::mono_channels, samples);
  }
  ~WavFixture() { fs::remove(path_); }
  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

media::AudioReader reader_for(const WavFixture& file) {
  return media::AudioReader::from_wav(take(media::WavReader::open(file.path())));
}

struct ClickPattern {
  double bpm;
  unsigned beats;
};

std::vector<std::int16_t> clicks(ClickPattern pattern) {
  const auto period = 60.0 * rate / pattern.bpm;
  const auto frames = static_cast<std::size_t>(rate / 4.0 + period * (pattern.beats + 1));
  std::vector<std::int16_t> samples(frames);
  for (unsigned beat = 0; beat < pattern.beats; ++beat) {
    const auto at = static_cast<std::size_t>(std::llround(rate / 4.0 + beat * period));
    for (std::size_t i = 0; i < 120; ++i) {
      samples[at + i] = 20'000;
    }
  }
  return samples;
}

void known_tempo_and_conflict(double bpm) {
  const WavFixture file(clicks({bpm, 16}));
  auto source = reader_for(file);
  const auto result = take(analysis::analyze_tempo(source, {"click-source", 7}));
  REQUIRE_EQ(result.identity.source_key, "click-source", "source identity or metadata changed");
  REQUIRE_EQ(result.identity.generation, 7, "source identity or metadata changed");
  REQUIRE_EQ(result.sample_rate, rate, "source identity or metadata changed");
  REQUIRE_GT(result.frames, 0, "source identity or metadata changed");
  REQUIRE_LT(std::abs(result.bpm - bpm), 1.0, "known click tempo or confidence wrong");
  REQUIRE_GT(result.confidence, 0.65, "known click tempo or confidence wrong");
  REQUIRE_GE(result.suggested_beat_frames.size(), 15, "beat grid phase wrong");
  REQUIRE_LT(std::abs(static_cast<double>(result.suggested_beat_frames.front()) - rate / 4.0),
             rate / 100.0, "beat grid phase wrong");
  REQUIRE_GE(result.suggested_onset_frames.size(), 15, "known click transients were not retained");
  REQUIRE_LT(std::abs(static_cast<double>(result.suggested_onset_frames.front()) - rate / 4.0),
             rate / 100.0, "known click transients were not retained");
  REQUIRE_TRUE(!result.onsets_truncated, "known click transients were not retained");
  for (std::size_t beat = 0; beat < 15; ++beat) {
    const auto expected = rate / 4.0 + static_cast<double>(beat) * 60.0 * rate / bpm;
    REQUIRE_LT(std::abs(static_cast<double>(result.suggested_beat_frames[beat]) - expected),
               rate / 50.0, "beat grid drift exceeded 20 ms");
  }
  REQUIRE_TRUE(!analysis::conflicts_with_project_tempo(result, bpm),
               "project tempo conflict policy wrong");
  REQUIRE_TRUE(analysis::conflicts_with_project_tempo(result, bpm * 1.1),
               "project tempo conflict policy wrong");
  REQUIRE_TRUE(!analysis::conflicts_with_project_tempo(result, -1.0),
               "project tempo conflict policy wrong");
}

// A house groove: a kick on each beat, louder offbeat hats (half a beat
// from the kick), then a long beatless tail of decaying noise, and a pad
// intro with no pulse. The tempo must come out exact and the grid must sit
// on the kick, not the hats.
std::vector<std::int16_t> house(double bpm, double intro_seconds, unsigned beats,
                                double tail_seconds) {
  const auto period = 60.0 * rate / bpm;
  const auto intro = static_cast<std::size_t>(intro_seconds * rate);
  const auto groove = static_cast<std::size_t>(period * beats);
  const auto tail = static_cast<std::size_t>(tail_seconds * rate);
  std::vector<double> mix(intro + groove + tail, 0.0);
  std::uint32_t noise = 12'345;
  const auto random = [&] {
    noise = noise * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(noise >> 8U) / static_cast<double>(1U << 24U) * 2.0 - 1.0;
  };
  for (std::size_t i = 0; i < intro; ++i)
    mix[i] = 0.08 * std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / rate);
  for (unsigned beat = 0; beat < beats; ++beat) {
    const auto kick = intro + static_cast<std::size_t>(std::llround(beat * period));
    for (std::size_t i = 0; i < static_cast<std::size_t>(rate / 8); ++i) {
      const auto t = static_cast<double>(i) / rate;
      const auto pitch = 50.0 + 120.0 * std::exp(-t * 40.0);
      mix[kick + i] += 0.5 * std::exp(-t * 18.0) * std::sin(2.0 * 3.141592653589793 * pitch * t);
    }
    const auto hat = kick + static_cast<std::size_t>(std::llround(period / 2));
    for (std::size_t i = 0; i < static_cast<std::size_t>(rate / 30); ++i)
      mix[hat + i] += 0.6 * std::exp(-static_cast<double>(i) / rate * 90.0) * random();
  }
  for (std::size_t i = 0; i < tail; ++i)
    mix[intro + groove + i] +=
        0.3 * std::exp(-static_cast<double>(i) / static_cast<double>(tail) * 4.0) * random();
  std::vector<std::int16_t> samples(mix.size());
  for (std::size_t i = 0; i < mix.size(); ++i)
    samples[i] = static_cast<std::int16_t>(std::clamp(mix[i], -1.0, 1.0) * 30'000.0);
  return samples;
}

void groove_with_offbeat_hats_and_tail() {
  constexpr double bpm = 124.0;
  constexpr double intro = 3.0;
  constexpr unsigned beats = 40;
  const WavFixture file(house(bpm, intro, beats, 8.0));
  auto source = reader_for(file);
  const auto result = take(analysis::analyze_tempo(source, {"house", 1}));
  REQUIRE_EQ(result.bpm, bpm, "house groove tempo was not exact");
  REQUIRE_GE(result.confidence, 0.5, "house groove tempo was not confident");
  // Every suggested beat inside the groove sits on a kick, within 5 ms.
  const auto period = 60.0 * rate / bpm;
  for (const auto beat : result.suggested_beat_frames) {
    const auto from_start = static_cast<double>(beat) - intro * rate;
    if (from_start < 0.0 || from_start > period * (beats - 1))
      continue;
    const auto offset = std::remainder(from_start, period);
    REQUIRE_LT(std::abs(offset), rate * 0.005, "beat grid sits off the kick");
  }
}

void ambiguity_and_bounds() {
  const WavFixture empty({});
  auto empty_reader = reader_for(empty);
  const auto empty_result = take(analysis::analyze_tempo(empty_reader, {"empty", 1}));
  REQUIRE_EQ(empty_result.frames, 0, "empty source should have no beat suggestions");
  REQUIRE_TRUE(empty_result.suggested_beat_frames.empty(),
               "empty source should have no beat suggestions");

  const WavFixture impulse({20'000});
  auto impulse_reader = reader_for(impulse);
  const auto impulse_result = take(analysis::analyze_tempo(impulse_reader, {"impulse", 2}));
  REQUIRE_EQ(impulse_result.bpm, 0.0, "one-frame impulse should have no beat grid");
  REQUIRE_TRUE(impulse_result.suggested_beat_frames.empty(),
               "one-frame impulse should have no beat grid");

  const WavFixture silence(std::vector<std::int16_t>(static_cast<std::size_t>(rate) * 3));
  auto silent_reader = reader_for(silence);
  const auto silent = take(analysis::analyze_tempo(silent_reader, {"silence", 1}));
  REQUIRE_EQ(silent.bpm, 0.0, "silence should not produce a tempo conflict");
  REQUIRE_EQ(silent.confidence, 0.0, "silence should not produce a tempo conflict");
  REQUIRE_TRUE(silent.suggested_beat_frames.empty(), "silence should not produce a tempo conflict");
  REQUIRE_TRUE(!analysis::conflicts_with_project_tempo(silent, 120.0),
               "silence should not produce a tempo conflict");

  auto short_signal = clicks({120.0, 3});
  const WavFixture short_file(short_signal);
  auto short_reader = reader_for(short_file);
  const auto short_result = take(analysis::analyze_tempo(short_reader, {"short", 2}));
  REQUIRE_EQ(short_result.bpm, 0.0, "three beats do not justify a confident estimate");
  REQUIRE_EQ(short_result.confidence, 0.0, "three beats do not justify a confident estimate");

  auto rapid = std::vector<std::int16_t>(static_cast<std::size_t>(rate));
  for (std::size_t onset = 0; onset < 8; ++onset) {
    const auto at = onset * static_cast<std::size_t>(rate) / 10;
    for (std::size_t i = 0; i < 120; ++i) {
      rapid[at + i] = 20'000;
    }
  }
  const WavFixture rapid_file(rapid);
  auto rapid_reader = reader_for(rapid_file);
  const auto rapid_result = take(analysis::analyze_tempo(rapid_reader, {"too-fast", 3}));
  REQUIRE_EQ(rapid_result.bpm, 0.0, "onsets outside selected BPM range should not yield a tempo");
  REQUIRE_EQ(rapid_result.confidence, 0.0,
             "onsets outside selected BPM range should not yield a tempo");
  REQUIRE_EQ(rapid_result.suggested_onset_frames.size(), 8,
             "transient suggestions must survive a low-confidence tempo");

  auto odd = std::vector<std::int16_t>(static_cast<std::size_t>(rate) * 8);
  constexpr std::array<double, 8> times{0.2, 0.53, 1.28, 1.69, 2.58, 3.21, 4.17, 4.62};
  for (const auto time : times) {
    const auto at = static_cast<std::size_t>(time * rate);
    for (std::size_t i = 0; i < 120; ++i) {
      odd[at + i] = 20'000;
    }
  }
  const WavFixture odd_file(odd);
  auto odd_reader = reader_for(odd_file);
  const auto irregular = take(analysis::analyze_tempo(odd_reader, {"irregular", 3}));
  REQUIRE_LT(irregular.confidence, 0.5,
             "irregular onsets must not prompt an automatic conform conflict");
  REQUIRE_TRUE(!analysis::conflicts_with_project_tempo(irregular, 110.0),
               "irregular onsets must not prompt an automatic conform conflict");

  auto at_zero = clicks({120.0, 16});
  for (unsigned beat = 0; beat < 16; ++beat) {
    const auto original =
        static_cast<std::size_t>(rate) / 4 + static_cast<std::size_t>(beat) * rate / 2;
    const auto shifted = static_cast<std::size_t>(beat) * rate / 2;
    for (std::size_t i = 0; i < 120; ++i) {
      at_zero[original + i] = 0;
      at_zero[shifted + i] = 20'000;
    }
  }
  const WavFixture at_zero_file(at_zero);
  auto zero_reader = reader_for(at_zero_file);
  const auto zero_result = take(analysis::analyze_tempo(zero_reader, {"zero", 4}));
  REQUIRE_GT(zero_result.bpm, 119.0, "first-bin onset or beat-grid phase lost");
  REQUIRE_LT(zero_result.bpm, 121.0, "first-bin onset or beat-grid phase lost");
  REQUIRE_TRUE(!zero_result.suggested_beat_frames.empty(),
               "first-bin onset or beat-grid phase lost");
  REQUIRE_EQ(zero_result.suggested_beat_frames.front(), 0,
             "first-bin onset or beat-grid phase lost");
}

void cancellation_validation_and_truncation() {
  const WavFixture file(clicks({120.0, 16}));
  auto source = reader_for(file);
  int calls = 0;
  auto cancelled = analysis::analyze_tempo(source, {"cancel", 1}, {}, [&] { return ++calls == 2; });
  REQUIRE_TRUE(!cancelled, "cancelled read returned a partial tempo");
  REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
             "cancelled read returned a partial tempo");
  calls = 0;
  const auto read_batches = static_cast<int>((source.frame_count() + 4'095) / 4'096);
  cancelled =
      analysis::analyze_tempo(source, {"cancel", 2}, {}, [&] { return ++calls > read_batches; });
  REQUIRE_TRUE(!cancelled, "cancelled publication returned a tempo");
  REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
             "cancelled publication returned a tempo");
  const auto bad = analysis::analyze_tempo(source, {"", 0});
  REQUIRE_TRUE(!bad, "invalid identity accepted");
  REQUIRE_EQ(bad.error().code, foundation::ErrorCode::invalid_argument,
             "invalid identity accepted");
  const auto bad_bounds = analysis::analyze_tempo(source, {"bounded", 1}, {200.0, 60.0});
  REQUIRE_TRUE(!bad_bounds, "inverted BPM bounds accepted");
  REQUIRE_EQ(bad_bounds.error().code, foundation::ErrorCode::invalid_argument,
             "inverted BPM bounds accepted");
  const auto bounded =
      take(analysis::analyze_tempo(source, {"bounded", 2}, {60.0, 200.0, 100, 3, 3}));
  REQUIRE_EQ(bounded.suggested_beat_frames.size(), 3,
             "long-source envelope and marker budgets did not bound output");
  REQUIRE_TRUE(bounded.suggestions_truncated,
               "long-source envelope and marker budgets did not bound output");
  REQUIRE_EQ(bounded.suggested_onset_frames.size(), 3,
             "long-source envelope and marker budgets did not bound output");
  REQUIRE_TRUE(bounded.onsets_truncated,
               "long-source envelope and marker budgets did not bound output");

  fs::resize_file(file.path(), 44);
  const auto changed = analysis::analyze_tempo(source, {"changed", 3});
  REQUIRE_TRUE(!changed, "changed source should fail without a partial beat grid");
  REQUIRE_EQ(changed.error().code, foundation::ErrorCode::io_failure,
             "changed source should fail without a partial beat grid");
}

} // namespace

int main(int argc, char** argv) {
  try {
    // Real-length material runs in the slow tier.
    if (argc == 2 && std::string_view(argv[1]) == "--groove") {
      groove_with_offbeat_hats_and_tail();
      std::cout << "tempo groove fixture passed\n";
      return EXIT_SUCCESS;
    }
    known_tempo_and_conflict(120.0);
    known_tempo_and_conflict(90.0);
    known_tempo_and_conflict(93.0);
    ambiguity_and_bounds();
    cancellation_validation_and_truncation();
    std::cout << "tempo analysis fixtures passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
