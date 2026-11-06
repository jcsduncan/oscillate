#include "oscillate/analysis/key.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/wav.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
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
    std::string pattern = (fs::temp_directory_path() / "oscillate-key-XXXXXX").string();
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
std::vector<std::int16_t> progression(const std::vector<std::array<int, 3>>& chords,
                                      bool harmonics = false) {
  const auto chord_frames = static_cast<std::size_t>(rate) * 2;
  std::vector<std::int16_t> samples(chords.size() * chord_frames);
  for (std::size_t chord = 0; chord < chords.size(); ++chord) {
    for (std::size_t index = 0; index < chord_frames; ++index) {
      double value = 0.0;
      for (const auto note : chords[chord]) {
        const auto frequency = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        const auto phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate;
        value += std::sin(phase);
        if (harmonics) {
          value += 0.2 * std::sin(2.0 * phase);
        }
      }
      const auto attack = std::min(1.0, static_cast<double>(index) / 480.0);
      const auto release = std::min(1.0, static_cast<double>(chord_frames - index) / 480.0);
      samples[chord * chord_frames + index] =
          static_cast<std::int16_t>(value * std::min(attack, release) * 7'000.0);
    }
  }
  return samples;
}
void tonal_passages() {
  // Independent note-frequency oracle: C-F-G-C and A-Dm-E-Am progressions.
  const WavFixture major(
      progression({{{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}}}, true));
  auto major_reader = reader_for(major);
  const auto c = take(analysis::analyze_key(major_reader, {"major", 1}));
  REQUIRE_TRUE(c.key, "C major passage classification failed");
  REQUIRE_EQ(c.key->tonic, 0, "C major passage classification failed");
  REQUIRE_EQ(c.key->mode, analysis::KeyMode::major, "C major passage classification failed");
  REQUIRE_GE(c.confidence, 0.1, "C major passage classification failed");
  REQUIRE_EQ(c.compatible_keys.size(), 3, "relative key mapping wrong");
  REQUIRE_EQ(c.compatible_keys[0].relation, analysis::KeyRelation::relative,
             "relative key mapping wrong");
  REQUIRE_EQ(c.compatible_keys[0].key.tonic, 9, "relative key mapping wrong");
  REQUIRE_EQ(c.compatible_keys[0].key.mode, analysis::KeyMode::minor, "relative key mapping wrong");

  const WavFixture d_major(
      progression({{{62, 66, 69}, {67, 71, 74}, {69, 73, 76}, {62, 66, 69}}}, true));
  auto d_reader = reader_for(d_major);
  const auto d = take(analysis::analyze_key(d_reader, {"d-major", 3}));
  REQUIRE_TRUE(d.key, "polyphonic D major passage classification failed");
  REQUIRE_EQ(d.key->tonic, 2, "polyphonic D major passage classification failed");
  REQUIRE_EQ(d.key->mode, analysis::KeyMode::major,
             "polyphonic D major passage classification failed");

  const WavFixture minor(
      progression({{{57, 60, 64}, {62, 65, 69}, {64, 68, 71}, {57, 60, 64}}}, true));
  auto minor_reader = reader_for(minor);
  const auto a = take(analysis::analyze_key(minor_reader, {"minor", 2}));
  REQUIRE_TRUE(a.key, "A minor passage classification failed");
  REQUIRE_EQ(a.key->tonic, 9, "A minor passage classification failed");
  REQUIRE_EQ(a.key->mode, analysis::KeyMode::minor, "A minor passage classification failed");
  REQUIRE_GE(a.confidence, 0.1, "A minor passage classification failed");
}
void uncertainty_and_errors() {
  const WavFixture chord(progression({{{60, 64, 67}, {60, 64, 67}, {60, 64, 67}, {60, 64, 67}}}));
  auto chord_reader = reader_for(chord);
  const auto one = take(analysis::analyze_key(chord_reader, {"one-chord", 1}));
  REQUIRE_TRUE(!one.key, "one triad must not imply a key");
  REQUIRE_EQ(one.confidence, 0.0, "one triad must not imply a key");
  REQUIRE_TRUE(one.compatible_keys.empty(), "one triad must not imply a key");
  const WavFixture minor_chord(
      progression({{{57, 60, 64}, {57, 60, 64}, {57, 60, 64}, {57, 60, 64}}}));
  auto minor_chord_reader = reader_for(minor_chord);
  const auto single_minor = take(analysis::analyze_key(minor_chord_reader, {"minor-chord", 1}));
  require(!single_minor.key, "one minor triad must not imply a key");

  // Both relative centres have equally supported phrases; no tonic is safe.
  const WavFixture relative_mix(progression({{{60, 64, 67},
                                              {65, 69, 72},
                                              {67, 71, 74},
                                              {60, 64, 67},
                                              {57, 60, 64},
                                              {62, 65, 69},
                                              {64, 67, 71},
                                              {57, 60, 64}}},
                                            true));
  auto mix_reader = reader_for(relative_mix);
  const auto mixed = take(analysis::analyze_key(mix_reader, {"relative-mix", 1}));
  REQUIRE_TRUE(!mixed.key, "equally supported relative keys must remain ambiguous");
  REQUIRE_EQ(mixed.confidence, 0.0, "equally supported relative keys must remain ambiguous");

  const WavFixture exact_window(std::vector<std::int16_t>(16'384));
  auto exact_reader = reader_for(exact_window);
  require(!take(analysis::analyze_key(exact_reader, {"exact", 1})).key,
          "one exact-size window should be unknown without division by zero");
  const WavFixture silence(std::vector<std::int16_t>(static_cast<std::size_t>(rate) * 3));
  auto silent_reader = reader_for(silence);
  const auto empty = take(analysis::analyze_key(silent_reader, {"silence", 1}));
  REQUIRE_TRUE(!empty.key, "silence must be unknown");
  REQUIRE_EQ(empty.confidence, 0.0, "silence must be unknown");
  std::vector<std::int16_t> noise(static_cast<std::size_t>(rate) * 5);
  std::uint32_t state = 0x12345678U;
  for (auto& sample : noise) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    sample = static_cast<std::int16_t>(state >> 16U);
  }
  const WavFixture noisy(noise);
  auto noise_reader = reader_for(noisy);
  const auto random = take(analysis::analyze_key(noise_reader, {"noise", 1}));
  REQUIRE_TRUE(!random.key, "noise must be unknown");
  REQUIRE_EQ(random.confidence, 0.0, "noise must be unknown");
  const WavFixture truncated(
      progression({{{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}}}));
  auto truncated_reader = reader_for(truncated);
  fs::resize_file(truncated.path(), 44);
  const auto damaged = analysis::analyze_key(truncated_reader, {"truncated", 1});
  require(!damaged, "truncated decoded source must not return partial analysis");

  auto cancelled_reader = reader_for(chord);
  const auto cancelled =
      analysis::analyze_key(cancelled_reader, {"cancel", 1}, {}, [] { return true; });
  REQUIRE_TRUE(!cancelled, "cancelled key analysis must not return partial result");
  REQUIRE_EQ(cancelled.error().code, foundation::ErrorCode::cancelled,
             "cancelled key analysis must not return partial result");
  auto invalid_reader = reader_for(chord);
  REQUIRE_TRUE(!analysis::analyze_key(invalid_reader, {"", 1}),
               "invalid analysis identity or budget accepted");
  REQUIRE_TRUE(!analysis::analyze_key(invalid_reader, {"source", 0}),
               "invalid analysis identity or budget accepted");
  REQUIRE_TRUE(!analysis::analyze_key(invalid_reader, {"source", 1}, {.max_windows = 1}),
               "invalid analysis identity or budget accepted");
}
} // namespace

int main(int argc, char** argv) try {
  if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
    std::vector<std::array<int, 3>> chords;
    for (int index = 0; index < 30; ++index) {
      constexpr std::array<std::array<int, 3>, 4> phrase{
          {{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}}};
      chords.push_back(phrase[static_cast<std::size_t>(index) % phrase.size()]);
    }
    const WavFixture minute(progression(chords, true));
    auto reader = reader_for(minute);
    const auto start = std::chrono::steady_clock::now();
    const auto estimate = take(analysis::analyze_key(reader, {"bench", 1}, {.max_windows = 256}));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_OK(estimate.key, "benchmark fixture must classify");
    std::cout << "48k mono, 60 s, 256 windows: "
              << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
    return 0;
  }
  tonal_passages();
  uncertainty_and_errors();
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
