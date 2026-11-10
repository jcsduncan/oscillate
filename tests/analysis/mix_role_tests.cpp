// Hearing a recording's part for the master rebalance (ADR-0123): drums,
// bass, a voice and other music, synthesised so each rule is exercised.

#include "oscillate/analysis/mix_role.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace {
namespace analysis = oscillate::analysis;
using oscillate::project::MixRole;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double rate_hz = oscillate::test::sample_rate_48k_hz;
constexpr double seconds = 8.0;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::uint64_t seed = 7;

// Drums: a kick on each beat and a hat on each eighth at 120 BPM.
constexpr double beat_seconds = 0.5;
constexpr double eighth_seconds = 0.25;
constexpr double kick_hz = 55.0;
constexpr double kick_decay_seconds = 0.04;
constexpr double hat_decay_seconds = 0.015;
constexpr double kick_level = 0.8;
constexpr double hat_level = 0.3;
// Bass: a three-harmonic tone through a low line, half a second a note.
constexpr std::array<double, 4> bass_hz{55.0, 73.42, 41.2, 61.74};
constexpr double note_seconds = 0.5;
constexpr std::size_t bass_harmonics = 3;
constexpr double tone_level = 0.3;
// A voice: A3 with 5.5 Hz vibrato 40 cents deep, phrases of two seconds
// with half a second's breath between.
constexpr double voice_hz = 220.0;
constexpr double vibrato_hz = 5.5;
constexpr double vibrato_cents = 40.0;
constexpr double phrase_seconds = 2.0;
constexpr double breath_seconds = 0.5;
constexpr std::size_t voice_harmonics = 8;
constexpr double cents_per_octave = 1'200.0;
// Music: an arpeggio on equal-tempered notes, a quarter of a second each.
constexpr std::array<double, 4> arpeggio_hz{261.63, 329.63, 392.0, 523.25};
constexpr double pluck_seconds = 0.25;
constexpr double pluck_decay_seconds = 0.3;
constexpr std::size_t pluck_harmonics = 5;
// Too short to tell.
constexpr double short_seconds = 1.0;

std::size_t frames_of(double length) { return static_cast<std::size_t>(length * rate_hz); }

std::vector<float> drums() {
  const auto noise = oscillate::test::white_noise<float>(frames_of(seconds), seed);
  std::vector<float> out(frames_of(seconds));
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const double since_beat = std::fmod(t, beat_seconds);
    const double since_eighth = std::fmod(t, eighth_seconds);
    const double kick = kick_level * std::exp(-since_beat / kick_decay_seconds) *
                        std::sin(two_pi * kick_hz * since_beat);
    const double hat = hat_level * std::exp(-since_eighth / hat_decay_seconds) * noise[i];
    out[i] = static_cast<float>(kick + hat);
  }
  return out;
}

// `count` harmonics falling as 1/n, at `phase` of the fundamental.
float partials(double phase, std::size_t count) {
  double sum = 0.0;
  for (std::size_t h = 1; h <= count; ++h)
    sum += std::sin(static_cast<double>(h) * phase) / static_cast<double>(h);
  return static_cast<float>(sum);
}

std::vector<float> bass() {
  std::vector<float> out(frames_of(seconds));
  double phase = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const auto note = static_cast<std::size_t>(t / note_seconds) % bass_hz.size();
    phase += two_pi * bass_hz[note] / rate_hz;
    out[i] = static_cast<float>(tone_level) * partials(phase, bass_harmonics);
  }
  return out;
}

std::vector<float> voice() {
  std::vector<float> out(frames_of(seconds));
  double phase = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const double cents = vibrato_cents * std::sin(two_pi * vibrato_hz * t);
    phase += two_pi * voice_hz * std::exp2(cents / cents_per_octave) / rate_hz;
    const bool singing = std::fmod(t, phrase_seconds + breath_seconds) < phrase_seconds;
    out[i] = singing ? static_cast<float>(tone_level) * partials(phase, voice_harmonics) : 0.0F;
  }
  return out;
}

std::vector<float> arpeggio() {
  std::vector<float> out(frames_of(seconds));
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const auto note = static_cast<std::size_t>(t / pluck_seconds) % arpeggio_hz.size();
    const double since = std::fmod(t, pluck_seconds);
    const double phase = two_pi * arpeggio_hz[note] * since;
    out[i] = static_cast<float>(tone_level * std::exp(-since / pluck_decay_seconds)) *
             partials(phase, pluck_harmonics);
  }
  return out;
}

void heard(const std::vector<float>& audio, MixRole expected, const char* what) {
  const auto result = analysis::hear_role(audio, rate);
  const auto& f = result.features;
  if (result.role != expected) {
    throw oscillate::test::TestFailure(
        std::string(what) + " was heard wrong: centroid " + std::to_string(f.centroid_hz) +
        " low " + std::to_string(f.low_share) + " onsets " + std::to_string(f.onsets_per_second) +
        " voiced " + std::to_string(f.voiced_share) + " pitch " +
        std::to_string(f.median_pitch_hz) + " off " + std::to_string(f.off_note_cents));
  }
}

void drums_are_heard() { heard(drums(), MixRole::drums, "drums"); }
void bass_is_heard() { heard(bass(), MixRole::bass, "a bass line"); }
void a_voice_is_heard() { heard(voice(), MixRole::vocals, "a voice"); }
void other_music_is_music() { heard(arpeggio(), MixRole::music, "an arpeggio"); }

void too_little_is_music() {
  auto audio = bass();
  audio.resize(frames_of(short_seconds));
  REQUIRE_EQ(analysis::hear_role(audio, rate).role, MixRole::music, "a second tells nothing");
  REQUIRE_EQ(analysis::hear_role(std::vector<float>(frames_of(seconds)), rate).role, MixRole::music,
             "silence tells nothing");
}

void the_same_audio_is_heard_the_same() {
  const auto first = analysis::hear_role(voice(), rate).features;
  const auto second = analysis::hear_role(voice(), rate).features;
  REQUIRE_EQ(first.median_pitch_hz, second.median_pitch_hz, "hearing is deterministic");
  REQUIRE_EQ(first.onsets_per_second, second.onsets_per_second, "hearing is deterministic");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "mix role hearing passed",
      {{"drums_are_heard", drums_are_heard},
       {"bass_is_heard", bass_is_heard},
       {"a_voice_is_heard", a_voice_is_heard},
       {"other_music_is_music", other_music_is_music},
       {"too_little_is_music", too_little_is_music},
       {"the_same_audio_is_heard_the_same", the_same_audio_is_heard_the_same}});
}
