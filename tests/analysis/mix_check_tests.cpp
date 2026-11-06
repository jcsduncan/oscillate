// Mix Check's measurements (ADR-0074) on synthetic signals with planted
// problems, each against a hand-derived expectation.
#include "oscillate/analysis/mix_check.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace mix = oscillate::analysis::mix;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double two_pi = 2.0 * std::numbers::pi;

using oscillate::test::require;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
  explicit Stereo(double seconds)
      : left(static_cast<std::size_t>(seconds * rate)), right(left.size()) {}
  void add_sine(double hz, double amplitude, double phase = 0.0) {
    for (std::size_t i = 0; i < left.size(); ++i) {
      const auto x = static_cast<float>(
          amplitude * std::sin(two_pi * hz * static_cast<double>(i) / rate + phase));
      left[i] += x;
      right[i] += x;
    }
  }
  [[nodiscard]] mix::Profile profile(mix::ProfileOptions options = {}) const {
    return mix::profile(left, right, rate, options);
  }
  // Decaying partials struck every `period` seconds: (Hz, amplitude, decay
  // time constant in seconds) each.
  void add_struck(double period, std::span<const std::array<double, 3>> partials) {
    const auto every = static_cast<std::size_t>(period * rate);
    for (std::size_t i = 0; i < left.size(); ++i) {
      const double local = static_cast<double>(i % every) / rate;
      double x = 0.0;
      for (const auto& [hz, amplitude, tau] : partials)
        x += amplitude * std::sin(two_pi * hz * local) * std::exp(-local / tau);
      left[i] += static_cast<float>(x);
      right[i] += static_cast<float>(x);
    }
  }
};

// A 30 Hz rumble 6 dB under a 55 Hz bass is found at 30 Hz; the bass alone
// is clean.
void sub_rumble_under_the_bass() {
  Stereo bass(6.0);
  bass.add_sine(55.0, 0.5);
  require(!mix::find_sub_rumble(bass.profile(), 55.0), "a clean bass has no rumble");
  bass.add_sine(30.0, 0.25);
  const auto rumble = mix::find_sub_rumble(bass.profile(), 55.0);
  REQUIRE_OK(rumble, "the rumble is found");
  REQUIRE_LE(rumble->low_hz, 30.0, "its band holds 30 Hz");
  REQUIRE_GE(rumble->high_hz, 30.0, "its band holds 30 Hz");
  // Sub power -15 dB against the bass's -9 dB: 9 dB over the 15 dB allowance,
  // plus up to 2 dB of the 55 Hz bass's window skirt below the sub's edge.
  require(rumble->excess_db > 8.0 && rumble->excess_db < 11.0,
          "the excess is about 9 dB, not " + std::to_string(rumble->excess_db));
  const auto peak = mix::strongest_low_peak_hz(bass.profile());
  REQUIRE_TRUE(peak, "the bass's fundamental is its strongest peak");
  REQUIRE_LT(std::fabs(*peak - 55.0), 1.0, "the bass's fundamental is its strongest peak");
}

// A bass playing C2 with a sub an octave down: its spectrum confirms the
// sub's C1 under the played note; a lone note confirms only itself.
void sub_confirmed_by_the_spectrum() {
  constexpr double c1 = 32.703;
  Stereo bass(6.0);
  bass.add_sine(2.0 * c1, 0.4);
  bass.add_sine(c1, 0.4);
  const std::vector<double> played{2.0 * c1, c1, c1 / 2.0};
  const auto lowest = mix::lowest_matching_peak_hz(bass.profile(), played);
  require(lowest && std::fabs(*lowest - c1) < 1.0,
          "the sub's fundamental is confirmed: " + std::to_string(lowest.value_or(0.0)));
  const std::vector<double> note_only{2.0 * c1};
  const auto note = mix::lowest_matching_peak_hz(bass.profile(), note_only);
  REQUIRE_TRUE(note, "without the sub candidate, the note");
  REQUIRE_LT(std::fabs(*note - 2.0 * c1), 1.0, "without the sub candidate, the note");
}

// Bands whose sides are anticorrelated lose in mono; a mono signal loses
// nothing; the low side ratio sees a wide sub.
void mono_fold_loss() {
  Stereo pad(6.0);
  for (const double hz : {300.0, 450.0, 1'000.0})
    pad.add_sine(hz, 0.1);
  require(!mix::find_mono_loss(pad.profile()), "a mono pad keeps everything");
  // Side partials 1.2x the mid's at 550-650 Hz: -10 log10(1 / 2.44) = 3.9 dB.
  for (std::size_t i = 0; i < pad.left.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const auto s = static_cast<float>(0.12 * std::sin(two_pi * 600.0 * t));
    pad.left[i] += s;
    pad.right[i] -= s;
  }
  pad.add_sine(600.0, 0.1);
  const auto loss = mix::find_mono_loss(pad.profile());
  REQUIRE_OK(loss, "the anticorrelated band is found");
  REQUIRE_LE(mix::band_low(loss->first_band), 600.0, "the span holds 600 Hz");
  REQUIRE_GE(mix::band_high(loss->last_band), 600.0, "the span holds 600 Hz");
  REQUIRE_GT(loss->worst_db, 3.0, "about 3.9 dB lost");
  REQUIRE_LT(loss->worst_db, 5.0, "about 3.9 dB lost");
  Stereo wide(4.0);
  for (std::size_t i = 0; i < wide.left.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    wide.left[i] = static_cast<float>(0.3 * std::sin(two_pi * 50.0 * t));
    wide.right[i] = static_cast<float>(0.3 * std::sin(two_pi * 50.0 * t + 2.0));
  }
  REQUIRE_GT(mix::low_side_db(wide.profile()), mix::low_side_threshold_db, "wide lows are seen");
}

// A hat: a noise burst every 500 ms over six partials that die in 15 ms
// and three modes (the user's Knock hat) that ring for 150 ms.
constexpr std::array<double, 3> hat_modes{9'680.0, 10'320.0, 11'010.0};
Stereo ringing_hat(double seconds) {
  Stereo hat(seconds);
  std::uint32_t state = 0x9E3779B9U;
  const auto period = static_cast<std::size_t>(0.5 * rate);
  for (std::size_t i = 0; i < hat.left.size(); ++i) {
    const auto local = static_cast<double>(i % period) / rate;
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    const double noise = static_cast<double>(state) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
    double x = 0.6 * noise * std::exp(-local / 0.004);
    for (const double hz : {4'100.0, 5'300.0, 6'700.0, 7'900.0, 12'400.0, 13'600.0})
      x += 0.1 * std::sin(two_pi * hz * local) * std::exp(-local / 0.015);
    for (const double hz : hat_modes)
      x += 0.04 * std::sin(two_pi * hz * local) * std::exp(-local / 0.15);
    hat.left[i] = static_cast<float>(x);
    hat.right[i] = static_cast<float>(x);
  }
  return hat;
}

// The modes that outlast the rest of the hat ring; the partials that die
// with it do not; notes the track plays are not resonances.
void resonances_in_the_decay() {
  const auto profile = ringing_hat(8.0).profile();
  const auto found = mix::find_resonances(profile, {}, mix::resonance_max_peaks);
  REQUIRE_EQ(found.size(), hat_modes.size(), "three modes ring: " + std::to_string(found.size()));
  for (const double hz : hat_modes)
    require(std::ranges::any_of(found,
                                [&](const mix::Resonance& r) {
                                  return std::fabs(r.hz - hz) / hz < 0.01 &&
                                         r.decay_ratio >= mix::resonance_decay_ratio;
                                }),
            "a mode at " + std::to_string(hz) + " Hz");
  const std::vector<double> played{hat_modes.front() / 2.0};
  const auto pitched = mix::find_resonances(profile, played, mix::resonance_max_peaks);
  require(std::ranges::none_of(pitched,
                               [](const mix::Resonance& r) {
                                 return std::fabs(r.hz - hat_modes.front()) / hat_modes.front() <
                                        0.01;
                               }),
          "a played note's harmonic is not a resonance");
}

// Modal percussion (Knock's way): inharmonic partials that all decay alike
// are the sound, not resonances. One that rings four times longer than the
// rest is, and only it.
constexpr std::array<std::array<double, 3>, 5> modal_partials{{{420.0, 0.2, 0.12},
                                                               {1'130.0, 0.2, 0.12},
                                                               {2'210.0, 0.2, 0.12},
                                                               {3'580.0, 0.2, 0.12},
                                                               {5'250.0, 0.2, 0.12}}};
void modal_partials_are_the_sound() {
  Stereo modal(12.0);
  modal.add_struck(1.5, modal_partials);
  require(mix::find_resonances(modal.profile(), {}, mix::resonance_max_peaks).empty(),
          "partials decaying alike do not ring");
  auto slower = modal_partials;
  slower[2][2] *= 4.0;
  Stereo ringing(12.0);
  ringing.add_struck(1.5, slower);
  const auto found = mix::find_resonances(ringing.profile(), {}, mix::resonance_max_peaks);
  REQUIRE_EQ(found.size(), 1, "exactly one partial rings: " + std::to_string(found.size()));
  REQUIRE_LT(std::fabs(found.front().hz - slower[2][0]) / slower[2][0], 0.01,
             "the slow one, at " + std::to_string(found.front().hz) + " Hz");
  REQUIRE_GT(found.front().decay_ratio, mix::resonance_decay_ratio,
             "about four times the rest's decay: " + std::to_string(found.front().decay_ratio));
}

// A clip starting at a sine's peak after silence clicks; a faded start and
// a continuing wave do not.
void clicks_at_edges() {
  constexpr std::size_t edge = 4'800;
  std::vector<float> left(9'600, 0.0F);
  for (std::size_t i = edge; i < left.size(); ++i)
    left[i] =
        static_cast<float>(0.5 * std::cos(two_pi * 200.0 * static_cast<double>(i - edge) / rate));
  REQUIRE_OK(mix::edge_click(left, left, edge, rate), "a start mid-wave clicks");
  auto faded = left;
  const auto fade = static_cast<std::size_t>(0.004 * rate);
  for (std::size_t i = 0; i < fade; ++i)
    faded[edge + i] *= static_cast<float>(static_cast<double>(i) / fade);
  require(!mix::edge_click(faded, faded, edge, rate), "a faded start does not");
  std::vector<float> wave(9'600);
  for (std::size_t i = 0; i < wave.size(); ++i)
    wave[i] = static_cast<float>(0.5 * std::sin(two_pi * 200.0 * static_cast<double>(i) / rate));
  require(!mix::edge_click(wave, wave, edge, rate), "a continuing wave does not");
}

// A -20 dBFS 1 kHz sine on both channels reads -20 LUFS: K-weighting's
// +0.69 dB at 1 kHz cancels the -0.691 offset. No loudness range.
void loudness_of_a_sine() {
  Stereo tone(10.0);
  tone.add_sine(1'000.0, 0.1);
  const auto measured = mix::loudness(tone.profile());
  require(measured.integrated && std::fabs(*measured.integrated + 20.0) < 0.2,
          "integrated loudness: " + std::to_string(measured.integrated.value_or(0.0)));
  REQUIRE_TRUE(measured.range, "a steady tone has no range");
  REQUIRE_LT(*measured.range, 0.5, "a steady tone has no range");
  REQUIRE_LT(std::fabs(measured.true_peak_db + 20.0), 0.1, "true peak -20 dBTP");
  REQUIRE_LT(std::fabs(mix::crest_db(0.1, 0.005) - 3.01), 0.01, "a sine's crest is 3 dB");
}

// A kick and bass in antiphase cancel; in phase they add. Streamed in
// blocks, as the render hands them over.
std::optional<mix::Cancellation> streamed_cancellation(double bass_gain) {
  constexpr std::size_t frames = 4 * rate;
  constexpr std::size_t block = 1'000;
  std::vector<float> kick(frames);
  std::vector<float> bass(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double x = std::sin(two_pi * 50.0 * static_cast<double>(i) / rate);
    kick[i] = static_cast<float>(x);
    bass[i] = static_cast<float>(bass_gain * x);
  }
  mix::CancellationMeter meter(rate, {rate / 2, 3 * rate / 2, 5 * rate / 2});
  for (std::size_t first = 0; first < frames; first += block)
    meter.push(std::span(kick).subspan(first, block), std::span(bass).subspan(first, block), first);
  return meter.finish();
}

void kick_bass_cancellation() {
  const auto cancel = streamed_cancellation(-0.9);
  // (1 + 0.81) / 0.01: 22.6 dB in every window of every hit.
  require(cancel && std::fabs(cancel->deepest_db - 22.6) < 0.5 && cancel->hits == 3 &&
              std::fabs(cancel->worst_db - cancel->deepest_db) < 0.01,
          "antiphase cancels by about 22.6 dB");
  const auto plot_steps = static_cast<std::size_t>(std::lround(
      (mix::dip_plot_before_seconds + mix::dip_plot_after_seconds) / mix::dip_plot_step_seconds));
  REQUIRE_EQ(cancel->envelope.size(), plot_steps, "the worst hit's plot is complete");
  const auto adds = streamed_cancellation(0.9);
  // (1 + 0.81) / 3.61: -3 dB.
  require(adds && adds->deepest_db < 0.0, "in phase they add");
  // The lows the two share: 50 Hz passes, 1 kHz does not.
  mix::LowBand low(rate);
  Stereo tones(1.0);
  tones.add_sine(50.0, 0.5);
  tones.add_sine(1'000.0, 0.5);
  std::vector<float> out(tones.left.size());
  low.process(tones.left, tones.right, out);
  double kept = 0.0;
  double fifty = 0.0;
  for (std::size_t i = out.size() / 2; i < out.size(); ++i) {
    const double reference = 0.5 * std::sin(two_pi * 50.0 * static_cast<double>(i) / rate);
    kept += static_cast<double>(out[i]) * out[i];
    fifty += reference * reference;
  }
  REQUIRE_LT(std::fabs(10.0 * std::log10(kept / fifty)), 0.1, "only the 50 Hz tone is left");
}

// The drums' crest on their sum: a click train alone, then with an audio
// candidate counted in, each against a direct computation.
void drum_crest_streamed() {
  constexpr std::size_t frames = 4'800;
  constexpr std::size_t every = 480;
  constexpr std::size_t block = 1'000;
  std::vector<float> clicks(frames, 0.0F);
  for (std::size_t i = 0; i < frames; i += every)
    clicks[i] = 1.0F;
  const std::vector<float> loop(frames, 0.5F);
  mix::DrumCrest crest(1);
  for (std::size_t first = 0; first < frames; first += block) {
    const auto count = std::min(block, frames - first);
    const std::array<std::span<const float>, 1> candidates{std::span(loop).subspan(first, count)};
    crest.push(std::span(clicks).subspan(first, count), candidates);
  }
  const auto direct = [&](bool with_loop) {
    double peak = 0.0;
    double squares = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
      const double x = clicks[i] + (with_loop ? loop[i] : 0.0F);
      peak = std::max(peak, std::fabs(x));
      squares += x * x;
    }
    return mix::crest_db(peak, squares / static_cast<double>(frames));
  };
  const auto alone = crest.crest_db({false});
  const auto with_loop = crest.crest_db({true});
  REQUIRE_TRUE(alone, "the clicks' crest");
  REQUIRE_LT(std::fabs(*alone - direct(false)), 0.01, "the clicks' crest");
  REQUIRE_TRUE(with_loop, "with the loop counted in");
  REQUIRE_LT(std::fabs(*with_loop - direct(true)), 0.01, "with the loop counted in");
}

// A second drop 1 LU under the first is flagged; markers' names count.
void section_energy() {
  std::vector<double> bars;
  for (const auto [lufs, count] : {std::pair{-14.0, 16}, {-8.0, 32}, {-13.0, 16}, {-9.0, 32}})
    bars.insert(bars.end(), static_cast<std::size_t>(count), lufs);
  const auto sections = mix::split_sections(bars);
  REQUIRE_EQ(sections.size(), 4, "four sections");
  REQUIRE_EQ(sections[1].name, "Drop 1", "named by energy");
  REQUIRE_EQ(sections[2].name, "Break", "named by energy");
  REQUIRE_EQ(sections[3].name, "Drop 2", "named by energy");
  const auto issue = mix::find_energy_issue(sections);
  REQUIRE_TRUE(issue, "the second drop is 1 LU quieter");
  REQUIRE_TRUE(!issue->break_louder, "the second drop is 1 LU quieter");
  REQUIRE_LT(std::fabs(issue->lu - 1.0), 0.05, "the second drop is 1 LU quieter");
  std::vector<mix::Section> named{{"Drop", 0, 15, -9.0}, {"Breakdown", 16, 31, -8.0}};
  const auto louder = mix::find_energy_issue(named);
  REQUIRE_TRUE(louder, "a break louder than a drop is flagged");
  REQUIRE_TRUE(louder->break_louder, "a break louder than a drop is flagged");
}

// A louder pad over the bass's harmonics masks them while both play.
void masking_pair() {
  Stereo bass(8.0);
  for (const double hz : {220.0, 330.0})
    bass.add_sine(hz, 0.1);
  Stereo pad(8.0);
  for (const double hz : {221.0, 331.0})
    pad.add_sine(hz, 0.3);
  const auto found = mix::find_masking(bass.profile(), 1.0, pad.profile(), 1.0);
  REQUIRE_OK(found, "the pad masks the bass");
  // 10 log10(1 + 9): 10 dB.
  require(found->loss_db > 8.0 && found->loss_db < 11.0, "by about 10 dB");
  require(!mix::find_masking(pad.profile(), 1.0, bass.profile(), 0.1),
          "a quiet bass does not mask the pad");
}

// Matching loudness, identical spectra differ by nothing; air added shows.
void reference_bands() {
  Stereo mix_signal(6.0);
  for (const double hz : {50.0, 120.0, 400.0, 1'000.0, 3'000.0, 9'000.0})
    mix_signal.add_sine(hz, 0.05);
  const auto a = mix_signal.profile();
  const double lufs = mix::loudness(a).integrated.value_or(0.0);
  for (const auto& band : mix::compare_reference(a, lufs, a, lufs))
    REQUIRE_LT(std::fabs(band.diff_db), 0.01, "identical tracks match in " + band.band);
  auto airy = mix_signal;
  airy.add_sine(12'000.0, 0.2);
  const auto b = airy.profile();
  const auto diff = mix::compare_reference(b, mix::loudness(b).integrated.value_or(0.0), a, lufs);
  const auto air = std::ranges::find(diff, std::string("Air"), &mix::ReferenceDifference::band);
  REQUIRE_NE(air, diff.end(), "more air than the reference");
  REQUIRE_GT(air->diff_db, 3.0, "more air than the reference");
  REQUIRE_EQ(diff.size(), mix::reference_bands.size() + 1, "six bands and Width");
  REQUIRE_EQ(diff.back().band, "Width", "six bands and Width");
}

} // namespace

int main() {
  try {
    sub_rumble_under_the_bass();
    sub_confirmed_by_the_spectrum();
    mono_fold_loss();
    resonances_in_the_decay();
    modal_partials_are_the_sound();
    clicks_at_edges();
    loudness_of_a_sine();
    kick_bass_cancellation();
    drum_crest_streamed();
    section_energy();
    masking_pair();
    reference_bands();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mix check analysis contract passed\n";
  return 0;
}
