#include "pocket_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// Pocket (ADR-0041), tested two ways.
//
// Exact oracles on controlled stimuli, where every envelope is known:
//  - bit-exact bypass;
//  - the Unmask and Duck laws on steady tones at band centres;
//  - cut timing against each band's analysis delay, and lookahead that
//    removes it;
//  - focus;
//  - release and Follow Kick;
//  - lookahead as an exact latency;
//  - the mono sub leaving the mid untouched and following the
//    Linkwitz-Riley slope on the side;
//  - no zipper distortion;
//  - click-free toggles.
//
// Outcome oracles on real kicks, from the Pulse drum synth: every kit, with
// the root note swept from -12 to +12, over a sub, a saw and a moving
// bassline:
//  - the kick comes out of the bass;
//  - bass before the kick and above the focus is untouched;
//  - kick transients survive in the sum;
//  - no new phase cancellation;
//  - a fast pattern stays steady.
namespace {
using namespace pocket_test;
namespace synth = oscillate::native_dsp::synth;

const EngineDescriptor& table() { return *pocket::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

// Clean Low End without its mono sub and lookahead, for exact timing.
std::vector<double> clean() {
  auto values = preset(table(), "Clean Low End");
  values[at("pocket.mono_sub")] = 0.0;
  values[at("pocket.lookahead")] = 0.0;
  return values;
}

// Unmask's law, as specified in ADR-0041 and pocket.cpp.
double unmask_law(double kick_db, double bass_db, double depth, double margin) {
  if (kick_db <= -60.0) {
    return 0.0;
  }
  const double deficit = bass_db + margin - kick_db;
  if (deficit > depth) {
    return depth * std::clamp(1.0 - (deficit - depth) / 12.0, 0.0, 1.0);
  }
  return std::max(0.0, deficit);
}

// Level of a steady sine over the last `frames` of a render, in dBFS peak.
double tone_db(const std::vector<double>& x, std::size_t frames) {
  return db(std::sqrt(2.0) * rms(x, x.size() - frames, x.size()));
}

void bypass_is_bit_exact() {
  const auto b = bass(Bass::saw, second);
  const Stereo quiet{std::vector<double>(second), std::vector<double>(second)};
  for (const bool sidechain : {false, true}) {
    auto runtime = make_effect(pocket::engine(), clean());
    const auto out = process(*runtime, b, {.sidechain = sidechain ? &quiet : nullptr});
    for (std::size_t i = 0; i < b.size(); ++i) {
      REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(b.left[i])),
                 "with no kick the bass passes bit-exact");
    }
  }
  const auto kick = kicks("909 House", four_on_the_floor(2), second);
  const auto out = pocket_bass(preset(table(), "Init"), b, kick);
  for (std::size_t i = 0; i < b.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(b.left[i])),
               "with no depth the bass passes bit-exact");
  }
  auto mono = clean();
  mono[at("pocket.mono_sub")] = 1.0;
  mono[at("pocket.depth")] = 0.0;
  const auto mono_out = pocket_bass(mono, b, kick);
  for (std::size_t i = 0; i < b.size(); ++i) {
    REQUIRE_LT(std::fabs(mono_out.left[i] - b.left[i]), tolerance::float_round_trip,
               "the mono sub leaves a mono bass untouched");
  }
}

// Steady kick and bass tones at one band centre: the cut settles on the law.
void the_laws_hold_on_steady_tones() {
  for (const std::size_t band : {2UL, 3UL, 5UL, 7UL}) {
    const double hz = pocket::band_hz(band);
    for (const double kick_db : {-40.0, -30.0, -24.0, -18.0, -12.0, -6.0}) {
      const double bass_db = -15.0;
      const auto kick = sine(hz, oscillate::native_dsp::dsp::db_to_gain(kick_db), second);
      const auto b = sine(hz, oscillate::native_dsp::dsp::db_to_gain(bass_db), second);
      const auto out = pocket_bass(clean(), b, kick);
      const double cut = bass_db - tone_db(out.left, second / 4);
      const double expected = unmask_law(kick_db, bass_db, 9.0, 3.0);
      REQUIRE_LT(std::fabs(cut - expected), 0.6,
                 "Unmask at " + std::to_string(hz) + " Hz, kick " + std::to_string(kick_db) +
                     " dB: cut " + std::to_string(cut) + ", law " + std::to_string(expected));
    }
    auto duck = clean();
    duck[at("pocket.mode")] = 1.0;
    const auto kick = sine(hz, 0.5, second);
    const auto b = sine(hz, 0.1, second);
    const double cut = db(0.1) - tone_db(pocket_bass(duck, b, kick).left, second / 4);
    REQUIRE_LT(std::fabs(cut - 9.0), 0.6,
               "Duck takes the full depth under a steady kick: " + std::to_string(cut));
  }
  // Pump: one cut across the whole focus, wherever the kick's energy is.
  auto pump = clean();
  pump[at("pocket.mode")] = 2.0;
  pump[at("pocket.focus_high")] = 2'000.0;
  const auto kick = sine(pocket::band_hz(3), 0.5, second);
  for (const double hz : {60.0, 400.0}) {
    const auto b = sine(hz, 0.1, second);
    const double cut = db(0.1) - tone_db(pocket_bass(pump, b, kick).left, second / 4);
    REQUIRE_LT(std::fabs(cut - 9.0), 0.6,
               "Pump ducks the whole focus at " + std::to_string(hz) +
                   " Hz: " + std::to_string(cut));
  }
}

// A decaying tone-burst kick at a band centre over a steady bass: the cut
// follows the law on the burst's analytic envelope, late by the band's
// analysis delay without lookahead and on time with enough of it.
void the_cut_lands_on_time_with_lookahead() {
  const std::size_t band = 3;
  const double hz = pocket::band_hz(band);
  const double bass_db = -15.0;
  const std::size_t onset = second / 2;
  const double decay_seconds = 0.12;
  Stereo kick{std::vector<double>(second), std::vector<double>(second)};
  std::vector<double> envelope_db(second, -300.0);
  for (std::size_t i = onset; i < second; ++i) {
    const double t = static_cast<double>(i - onset) / rate;
    const double amplitude = 0.7 * std::exp(-t / decay_seconds);
    kick.left[i] = amplitude * std::sin(2.0 * std::numbers::pi * hz * t);
    kick.right[i] = kick.left[i];
    envelope_db[i] = db(amplitude);
  }
  const auto b = sine(hz, oscillate::native_dsp::dsp::db_to_gain(bass_db), second);
  for (const double choice : {0.0, 4.0}) {
    auto values = clean();
    values[at("pocket.lookahead")] = choice;
    const std::size_t lag = choice == 0.0 ? 0 : 1'920;
    const auto out = pocket_bass(values, b, kick);
    const auto period = static_cast<std::size_t>(rate / hz);
    double best_offset = 0.0;
    double best_error = 1e9;
    for (int offset_ms = -10; offset_ms <= 60; ++offset_ms) {
      double error = 0.0;
      std::size_t count = 0;
      for (std::size_t i = onset + 4'800; i + lag + period < second && i < onset + 19'200;
           i += period) {
        const auto shifted =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) - offset_ms * 48);
        const double law = unmask_law(envelope_db[shifted], bass_db, 9.0, 3.0);
        const double cut = bass_db - db(std::sqrt(2.0) * rms(out.left, i + lag, i + lag + period));
        error += (cut - law) * (cut - law);
        ++count;
      }
      error = std::sqrt(error / static_cast<double>(count));
      if (error < best_error) {
        best_error = error;
        best_offset = offset_ms;
      }
    }
    const double expected_ms = choice == 0.0 ? pocket::band_delay_seconds(band) * 1e3 : 0.0;
    std::cout << "  timing: lookahead " << (choice == 0.0 ? 0 : 40) << " ms, cut trails the law by "
              << best_offset << " ms (rms " << best_error << " dB)\n";
    REQUIRE_LE(std::fabs(best_offset - expected_ms), 6.0,
               "the cut trails the law by " + std::to_string(best_offset) + " ms, expected " +
                   std::to_string(expected_ms));
    REQUIRE_LT(best_error, 1.0, "and follows its shape: rms " + std::to_string(best_error) + " dB");
  }
}

void focus_bounds_the_cut() {
  for (const double hz : {pocket::band_hz(12), pocket::band_hz(16)}) {
    const auto kick = sine(hz, 0.5, second);
    const auto b = sine(hz, 0.3, second);
    const double cut = db(0.3) - tone_db(pocket_bass(clean(), b, kick).left, second / 4);
    REQUIRE_LT(std::fabs(cut), 0.05, "nothing is cut outside the focus: " + std::to_string(cut));
  }
}

void release_and_follow_shape_the_recovery() {
  const double hz = pocket::band_hz(3);
  Stereo kick{std::vector<double>(second), std::vector<double>(second)};
  for (std::size_t i = 0; i < second / 2; ++i) {
    kick.left[i] = 0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
    kick.right[i] = kick.left[i];
  }
  const auto b = sine(hz, 0.3, second);
  std::array<double, 2> recovered{};
  for (const bool follow : {true, false}) {
    auto values = clean();
    values[at("pocket.mode")] = 1.0; // Duck: a full-depth cut to recover from.
    values[at("pocket.follow")] = follow ? 1.0 : 0.0;
    values[at("pocket.release")] = 200.0;
    const auto out = pocket_bass(values, b, kick);
    const auto period = static_cast<std::size_t>(rate / hz);
    double ms = -1.0;
    for (std::size_t i = second / 2; i + period < second; i += period / 4) {
      if (db(0.3) - db(std::sqrt(2.0) * rms(out.left, i, i + period)) < 3.0) {
        ms = static_cast<double>(i - second / 2) / 48.0;
        break;
      }
    }
    recovered[follow ? 0 : 1] = ms;
  }
  std::cout << "  recovery to -3 dB: Follow Kick " << recovered[0] << " ms, 200 ms release "
            << recovered[1] << " ms\n";
  require(recovered[0] > 0.0 && recovered[0] < 80.0,
          "Follow Kick lets go as the kick does: " + std::to_string(recovered[0]) + " ms");
  // A one-pole in dB from 9 to 3 dB takes ln(3) time constants, after the
  // analysis has seen the kick go.
  const double expected = 200.0 * std::log(3.0);
  require(recovered[1] > expected * 0.8 && recovered[1] < expected * 1.25 + 60.0,
          "Release sets the recovery: " + std::to_string(recovered[1]) + " ms");
}

void lookahead_is_an_exact_latency() {
  const auto b = bass(Bass::saw, second / 2);
  const std::array<std::size_t, 5> frames{0, 240, 480, 960, 1'920};
  for (std::size_t choice = 1; choice < 5; ++choice) {
    auto values = clean();
    values[at("pocket.lookahead")] = static_cast<double>(choice);
    const auto latency = pocket::engine().latency_frames(values, rate);
    REQUIRE_EQ(latency, frames[choice], "lookahead reports its latency");
    auto runtime = make_effect(pocket::engine(), values);
    REQUIRE_EQ(runtime->latency_frames(), latency, "and the runtime carries it");
    const auto out = process(*runtime, b);
    for (std::size_t i = latency; i < b.size(); ++i) {
      REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(b.left[i - latency])),
                 "with no kick the output is the input, exactly delayed");
    }
  }
}

void the_mono_sub_spares_the_mid() {
  auto values = clean();
  values[at("pocket.mono_sub")] = 1.0;
  values[at("pocket.mono_freq")] = 120.0;
  values[at("pocket.depth")] = 0.0;
  const auto stereo = noise(0.3, second);
  const Stereo quiet{std::vector<double>(second), std::vector<double>(second)};
  const auto out = pocket_bass(values, stereo, quiet);
  for (std::size_t i = 0; i < second; ++i) {
    REQUIRE_LT(std::fabs((out.left[i] + out.right[i]) - (stereo.left[i] + stereo.right[i])),
               tolerance::float_round_trip, "the mid (what sums with the kick) is never filtered");
  }
  for (const double x : {0.5, 2.0}) {
    const auto tone = sine(120.0 * x, 0.3, second);
    Stereo side{tone.left, std::vector<double>(second)};
    for (std::size_t i = 0; i < second; ++i) {
      side.right[i] = -tone.left[i];
    }
    const auto s = pocket_bass(values, side, quiet);
    std::vector<double> diff(second);
    for (std::size_t i = 0; i < second; ++i) {
      diff[i] = 0.5 * (s.left[i] - s.right[i]);
    }
    const double gain = db(rms(diff, second / 2, second) / rms(tone.left, second / 2, second));
    const double expected = db(std::pow(x, 4) / (1.0 + std::pow(x, 4)));
    REQUIRE_LT(std::fabs(gain - expected), 0.3,
               "mono sub side slope at " + std::to_string(x) + "x: " + std::to_string(gain));
  }
}

void steady_cuts_add_no_distortion() {
  const double kick_hz = pocket::band_hz(3);
  const double bass_hz = 82.41;
  const auto kick = sine(kick_hz, 0.5, 2 * second);
  const auto b = sine(bass_hz, 0.3, 2 * second);
  auto values = clean();
  values[at("pocket.mode")] = 1.0;
  const auto out = pocket_bass(values, b, kick);
  // Everything but the tone, through a 4-term Blackman-Harris window
  // (sidelobes below -92 dB), over the last 32768 frames.
  const auto residue = [&](const std::vector<double>& x) {
    std::vector<std::complex<double>> w(32'768);
    const double n = static_cast<double>(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
      const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / n;
      const double window = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) -
                            0.01168 * std::cos(3.0 * t);
      w[i] = x[x.size() - w.size() + i] * window;
    }
    fft(w);
    const auto tone_bin = static_cast<std::size_t>(std::lround(bass_hz * n / rate));
    double tone = 0.0;
    double rest = 0.0;
    for (std::size_t k = 1; k < w.size() / 2; ++k) {
      (k + 6 >= tone_bin && k <= tone_bin + 6 ? tone : rest) += std::norm(w[k]);
    }
    return 10.0 * std::log10(rest / tone);
  };
  const double floor = residue(b.left);
  const double distortion_db = residue(out.left);
  std::cout << "  steady cut distortion " << distortion_db << " dB (input " << floor << " dB)\n";
  REQUIRE_LT(distortion_db, -80.0,
             "a steady cut is a clean gain: " + std::to_string(distortion_db) + " dB");
}

void toggles_never_click() {
  const auto kick = kicks("909 House", four_on_the_floor(2), second);
  auto b = bass(Bass::line, second);
  for (std::size_t i = 0; i < second; ++i) {
    b.right[i] = 0.8 * b.left[i] +
                 0.05 * std::sin(2.0 * std::numbers::pi * 60.0 * static_cast<double>(i) / rate);
  }
  for (const auto& [id, to] : std::vector<std::pair<std::string_view, double>>{
           {"pocket.mono_sub", 1.0}, {"pocket.mode", 1.0}, {"pocket.follow", 0.0}}) {
    auto values = clean();
    auto runtime = make_effect(pocket::engine(), values);
    auto next = values;
    next[at(id)] = to;
    const auto out = process(
        *runtime, b, {.sidechain = &kick, .at = {second / 2 + 4'800}, .before = [&](std::uint64_t) {
                        require(runtime->offer_values(next, 1), "offered");
                      }});
    const auto reference = pocket_bass(values, b, kick);
    double steady = 0.0;
    for (std::size_t i = 1; i < second; ++i) {
      steady = std::max(steady, std::fabs(reference.right[i] - reference.right[i - 1]));
    }
    double across = 0.0;
    for (std::size_t i = second / 2 + 4'800; i < second / 2 + 6'000; ++i) {
      across = std::max(across, std::fabs(out.right[i] - out.right[i - 1]));
    }
    REQUIRE_LT(across, steady * 1.2 + 1e-4, std::string(id) + " switches smoothly");
  }
}

// --- Real kicks -----------------------------------------------------------

struct Kit {
  std::string_view name;
  double tune;
};

std::vector<Kit> kit_variations() {
  std::vector<Kit> out;
  for (const auto kit : {"808 Classic", "808 Boom", "909 House", "909 Techno", "Electro", "Lo-Fi",
                         "Trap", "Industrial", "Minimal", "Dub", "Breaks"}) {
    for (const double tune : {-12.0, -5.0, 0.0, 5.0, 12.0}) {
      out.push_back({kit, tune});
    }
  }
  return out;
}

Stereo tuned_kicks(const Kit& kit, const std::vector<std::size_t>& onsets, std::size_t frames) {
  const auto& engine = synth::pulse::engine();
  auto values = preset(*engine.descriptor, kit.name);
  const auto tune = index(*engine.descriptor, "pulse.kick.tune");
  values[tune] = std::clamp(values[tune] + kit.tune, -24.0, 24.0);
  auto runtime = make_runtime(engine, values);
  std::vector<Event> events;
  std::uint64_t id = 1;
  for (const auto onset : onsets) {
    auto hit = note(onset, 2'400, 36, id++, 110);
    events.insert(events.end(), hit.begin(), hit.end());
  }
  const auto out = render(*runtime, events, frames);
  return {out.left, out.right};
}

// Band powers per 256-frame hop at Pocket's band centres within the focus,
// from the zero-lag Hilbert envelope of an ideal third-octave: exact in
// frequency (an STFT short enough for a kick's timing smears 30-100 Hz
// together) and with no delay. Bands outside 35-220 Hz stay zero.
std::vector<std::array<double, pocket::bands>> pocket_bands(const std::vector<double>& x) {
  constexpr std::size_t hop = 256;
  std::vector<std::array<double, pocket::bands>> out(x.size() / hop);
  for (std::size_t b = 0; b < pocket::bands; ++b) {
    const double hz = pocket::band_hz(b);
    if (hz < 35.0 || hz > 220.0) {
      continue;
    }
    // Edges at the geometric midpoints between Pocket's centres, so the
    // bands tile with no gaps (its spacing is a little over a third).
    const double lo = b == 0 ? hz / 1.13 : std::sqrt(hz * pocket::band_hz(b - 1));
    const double hi = std::sqrt(hz * pocket::band_hz(b + 1));
    const auto envelope = band_envelope(x, hz, lo, hi);
    for (std::size_t f = 0; f < out.size(); ++f) {
      double power = 0.0;
      for (std::size_t i = f * hop; i < (f + 1) * hop; ++i) {
        power += envelope[i] * envelope[i];
      }
      out[f][b] = power / static_cast<double>(hop);
    }
  }
  return out;
}

// Energy in [from, to) after a fourth-order high-pass run over the whole
// signal, so no window leaks the bass into it.
double highpass_energy(const std::vector<double>& x, std::size_t from, std::size_t to, double hz) {
  const auto c =
      synth::make_filter_coefficients(synth::FilterMode::highpass_24, hz / rate, 0.0, 1.0);
  synth::MultimodeFilter filter;
  double e = 0.0;
  for (std::size_t i = 0; i < to; ++i) {
    const double y = filter.process(x[i], c);
    if (i >= from) {
      e += y * y;
    }
  }
  return e;
}

double lowpass_energy(const std::vector<double>& x, std::size_t from, std::size_t to) {
  const auto c =
      synth::make_filter_coefficients(synth::FilterMode::lowpass_24, 250.0 / rate, 0.0, 1.0);
  synth::MultimodeFilter filter;
  double e = 0.0;
  for (std::size_t i = 0; i < to; ++i) {
    const double y = filter.process(x[i], c);
    if (i >= from) {
      e += y * y;
    }
  }
  return e;
}

double lowpass_correlation(const std::vector<double>& a, const std::vector<double>& b,
                           std::size_t from, std::size_t to) {
  const auto c =
      synth::make_filter_coefficients(synth::FilterMode::lowpass_24, 250.0 / rate, 0.0, 1.0);
  synth::MultimodeFilter fa;
  synth::MultimodeFilter fb;
  std::vector<double> la(to);
  std::vector<double> lb(to);
  for (std::size_t i = 0; i < to; ++i) {
    la[i] = fa.process(a[i], c);
    lb[i] = fb.process(b[i], c);
  }
  return correlation(la, lb, from, to);
}

// One kick and bass pair's measurements. Pairs are measured on worker
// threads; every judgement is made afterwards, in the original order, so
// the parallelism changes nothing a check sees.
struct PairResult {
  struct Hit {
    double change = 0.0;
    double rho_before = 0.0;
    double rho_after = 0.0;
    double thinning = 0.0;
  };
  std::string label;
  Bass kind = Bass::sub;
  double asked = 0.0;
  double gained = 0.0;
  double untouched = 0.0;
  double early = 0.0;
  double high = 0.0;
  std::vector<Hit> hits;
};

void real_kicks_come_out_of_the_bass(double lookahead_choice = 0.0, bool survey = false) {
  const std::size_t frames = 2 * second;
  const std::array<std::size_t, 5> lags{0, 240, 480, 960, 1'920};
  const std::size_t lag = lags[static_cast<std::size_t>(lookahead_choice)];
  std::vector<double> clarity;
  const std::vector<std::size_t> onsets{24'000, 48'000};
  double worst_gain = 1e9;
  double worst_untouched = 0.0;
  double worst_transient = 0.0;
  double worst_phase = 0.0;
  double worst_thinning = -1e9;
  std::size_t hits = 0;
  std::size_t thin_hits = 0;
  double worst_high = 0.0;
  std::size_t cases = 0;
  // Every signal truncated alike: the shifted output ends `lag` early, and
  // the reference's circular FFT would carry a different ending round to
  // the start.
  const auto truncated = [&](std::vector<double> x) {
    std::fill(x.end() - static_cast<std::ptrdiff_t>(lag), x.end(), 0.0);
    return x;
  };
  // The basses and their bands are the same for every kit: computed once.
  const std::array<Bass, 3> kinds{Bass::sub, Bass::saw, Bass::line};
  std::array<Stereo, 3> basses;
  std::array<std::vector<std::array<double, pocket::bands>>, 3> bass_bands;
  for (std::size_t k = 0; k < kinds.size(); ++k) {
    basses[k] = bass(kinds[k], frames);
    bass_bands[k] = pocket_bands(truncated(basses[k].left));
  }
  const auto measure = [&](const Kit& kit) {
    std::vector<PairResult> results;
    const auto kick = tuned_kicks(kit, onsets, frames);
    const auto K = pocket_bands(truncated(kick.left));
    std::array<double, pocket::bands> peak{};
    for (const auto& f : K) {
      for (std::size_t j = 0; j < pocket::bands; ++j) {
        peak[j] = std::max(peak[j], f[j]);
      }
    }
    for (std::size_t k = 0; k < kinds.size(); ++k) {
      const Bass kind = kinds[k];
      const auto& b = basses[k];
      const auto& B = bass_bands[k];
      auto values = clean();
      values[at("pocket.lookahead")] = lookahead_choice;
      auto out = pocket_bass(values, b, kick);
      // Line the output up with its input again.
      for (auto* channel : {&out.left, &out.right}) {
        channel->erase(channel->begin(), channel->begin() + static_cast<std::ptrdiff_t>(lag));
        channel->resize(frames, 0.0);
      }
      PairResult r;
      r.kind = kind;
      r.label = std::string(kit.name) + " tune " + std::to_string(kit.tune) +
                (kind == Bass::sub   ? " sub"
                 : kind == Bass::saw ? " saw"
                                     : " line");
      const auto P = pocket_bands(truncated(out.left));
      // Clarity: where the law asks for a cut in the kick's body (the kick
      // within 12 dB of its peak in the band), the kick gains at least
      // half of it on average. Untouched: before the first kick, nothing.
      const std::size_t usable = std::min(K.size(), (frames - lag) / 256);
      for (std::size_t f = 0; f < usable; ++f) {
        double loudest = 0.0;
        for (std::size_t j = 0; j < pocket::bands; ++j) {
          if (pocket::band_hz(j) >= 30.0 && pocket::band_hz(j) <= 250.0) {
            loudest = std::max(loudest, B[f][j]);
          }
        }
        for (std::size_t j = 0; j < pocket::bands; ++j) {
          const double hz = pocket::band_hz(j);
          if (hz < 35.0 || hz > 220.0 || B[f][j] < loudest * 0.03) {
            continue;
          }
          const double k_db = 10.0 * std::log10(K[f][j] + 1e-30);
          const double b_db = 10.0 * std::log10(B[f][j] + 1e-30);
          const double cut = b_db - 10.0 * std::log10(P[f][j] + 1e-30);
          // Each frame earns its cut up to what the law asked there, never
          // below zero: a bass note ending moves its envelope faster than a
          // filtered copy can follow, which is not a cut.
          const double law = unmask_law(k_db, b_db, 9.0, 3.0);
          if (k_db > 10.0 * std::log10(peak[j] + 1e-30) - 12.0 && law >= 1.0) {
            r.asked += law;
            r.gained += std::clamp(cut, 0.0, law);
          }
        }
      }
      if (!survey) {
        for (std::size_t i = 0; i + 960 < onsets[0]; ++i) {
          r.untouched = std::max(r.untouched, std::fabs(out.left[i] - b.left[i]));
        }
        r.early = db(rms(b.left, onsets[0] - 960, onsets[0] - 480) /
                     rms(out.left, onsets[0] - 960, onsets[0] - 480));
        if (kind == Bass::saw) {
          r.high = 10.0 * std::log10(highpass_energy(out.left, second / 4, frames, 1'000.0) /
                                     highpass_energy(b.left, second / 4, frames, 1'000.0));
        }
        const auto before = sum(kick, b);
        const auto after = sum(kick, out);
        for (const auto onset : onsets) {
          PairResult::Hit hit;
          hit.change = 10.0 * std::log10(highpass_energy(after.left, onset, onset + 480, 1'000.0) /
                                         highpass_energy(before.left, onset, onset + 480, 1'000.0));
          hit.rho_before = lowpass_correlation(kick.left, b.left, onset, onset + 9'600);
          hit.rho_after = lowpass_correlation(kick.left, out.left, onset, onset + 9'600);
          const double kick_low = lowpass_energy(kick.left, onset, onset + 9'600);
          const double pair_low = lowpass_energy(before.left, onset, onset + 9'600);
          const double after_low = lowpass_energy(after.left, onset, onset + 9'600);
          hit.thinning = 10.0 * std::log10(std::min(kick_low, pair_low) / after_low);
          r.hits.push_back(hit);
        }
      }
      results.push_back(std::move(r));
    }
    return results;
  };
  const auto kits = kit_variations();
  std::vector<std::vector<PairResult>> measured(kits.size());
  std::vector<std::exception_ptr> failures(kits.size());
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> workers;
  const std::size_t threads =
      std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kits.size());
  for (std::size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (std::size_t i = next++; i < kits.size(); i = next++) {
        try {
          measured[i] = measure(kits[i]);
        } catch (...) {
          failures[i] = std::current_exception();
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  for (std::size_t i = 0; i < kits.size(); ++i) {
    if (failures[i]) {
      std::rethrow_exception(failures[i]);
    }
    for (const auto& r : measured[i]) {
      ++cases;
      if (r.asked > 1.0) {
        worst_gain = std::min(worst_gain, r.gained / r.asked);
        clarity.push_back(r.gained / r.asked);
        // A substantial conflict always gets real help (measured worst: 0.18
        // of the law, a saw whose partials the two analyses split
        // differently).
        if (!survey && r.asked >= 100.0) {
          REQUIRE_GE(r.gained, 0.15 * r.asked,
                     r.label + ": a real conflict is resolved (" +
                         std::to_string(r.gained / r.asked) + " of the law)");
        }
      }
      if (survey) {
        continue;
      }
      // Before the first kick the bass is exactly what came in, in the time
      // domain (the ideal bands above ring both ways in time, so they cannot
      // judge this). Lookahead ramps the cut in just before the kick lands,
      // on purpose: each band's cut is centred on the kick (half-way at the
      // hit), so a band's rise starts up to its analysis delay early, within
      // the lookahead. Exact until 20 ms before; a pre-dip under 2 dB in the
      // 10 ms ending 10 ms before (a saw's low partials dip about 1.2 dB).
      worst_untouched = std::max(worst_untouched, r.untouched);
      REQUIRE_LT(r.untouched, 1e-6, r.label + ": bass before the kick is untouched");
      REQUIRE_LT(r.early, 2.0,
                 r.label + ": the lookahead ramp starts gently (" + std::to_string(r.early) +
                     " dB 10-20 ms early)");
      if (r.kind == Bass::saw) {
        worst_high = std::max(worst_high, std::fabs(r.high));
        REQUIRE_LT(std::fabs(r.high), 0.1, r.label + ": the bass above the focus is untouched");
      }
      for (const auto& hit : r.hits) {
        // The kick's click, above 1 kHz in the first 10 ms of each hit.
        worst_transient = std::max(worst_transient, std::fabs(hit.change));
        REQUIRE_LT(std::fabs(hit.change), 0.2,
                   r.label + ": the kick's click survives (" + std::to_string(hit.change) + " dB)");
        // The bass is never turned against the kick: below 250 Hz, over the
        // hit, the kick plus the processed bass keeps at least the energy
        // of the kick alone or of the original pair, whichever is less,
        // within 0.5 dB. (Correlation alone would mislead: removing the
        // overlapping bass removes what was correlated with the kick.)
        worst_phase = std::min(worst_phase, hit.rho_after - hit.rho_before);
        worst_thinning = std::max(worst_thinning, hit.thinning);
        // The cuts are minimum phase, so the bass left under the kick turns
        // a little; on a few pairs that leans against the kick (measured:
        // 4 of 330 hits over 0.3 dB, worst 0.71). Pocket's Align (ADR-0041
        // phase 2) is the cure; this holds phase 1 to what it does.
        if (hit.thinning > 0.3) {
          ++thin_hits;
        }
        ++hits;
        REQUIRE_LT(hit.thinning, 0.75,
                   r.label + ": the low end never gets thinner (" + std::to_string(hit.thinning) +
                       " dB)");
      }
    }
  }
  std::sort(clarity.begin(), clarity.end());
  if (survey) {
    std::cout << "  lookahead choice " << lookahead_choice << ": clarity min " << clarity.front()
              << ", 5% " << clarity[clarity.size() / 20] << ", median "
              << clarity[clarity.size() / 2] << '\n';
    return;
  }
  // Analyses differ by a few dB on a gliding kick, and a tone right between
  // two bands is split differently: judged over the population.
  const double median = clarity[clarity.size() / 2];
  const auto below = static_cast<double>(
      std::count_if(clarity.begin(), clarity.end(), [](double c) { return c < 0.3; }));
  REQUIRE_GE(median, 0.55, "the median pair gets most of the law: " + std::to_string(median));
  REQUIRE_LE(static_cast<double>(thin_hits), 0.02 * static_cast<double>(hits),
             "the low end thins by over 0.3 dB on at most 2% of hits");
  REQUIRE_LE(below, 0.1 * static_cast<double>(clarity.size()),
             "nine pairs in ten get at least 0.3 of it");
  std::cout << "  " << cases << " real kick and bass pairs: median clarity " << median
            << " of the law, worst " << worst_gain << "; before the kick within " << worst_untouched
            << " dB; above focus within " << worst_high << " dB; transients within "
            << worst_transient << " dB; correlation change >= " << worst_phase
            << "; low end thinner by at most " << worst_thinning << " dB\n";
}

void a_fast_pattern_stays_steady() {
  const auto onsets = four_on_the_floor(16, 174.0 * 2.0); // Eighths at 174.
  const std::size_t frames = onsets.back() + second / 2;
  const auto kick = tuned_kicks({"909 Techno", 0.0}, onsets, frames);
  const auto b = bass(Bass::sub, frames);
  const auto out = pocket_bass(clean(), b, kick);
  std::vector<double> cuts;
  for (std::size_t h = 4; h < onsets.size(); ++h) {
    cuts.push_back(db(rms(b.left, onsets[h], onsets[h] + 4'000) /
                      rms(out.left, onsets[h], onsets[h] + 4'000)));
  }
  const auto [low, high] = std::minmax_element(cuts.begin(), cuts.end());
  REQUIRE_LT(*high - *low, 1.0,
             "every hit of a fast pattern gets the same cut: " + std::to_string(*high - *low) +
                 " dB spread");
  REQUIRE_GT(*low, 0.5, "and each one is cut");
}

// The panel's telemetry: band levels read true for steady tones, the
// overlap falls when Pocket cuts, and the waveforms carry the signals.
void telemetry_feeds_the_panel() {
  const std::size_t band = 3;
  const double hz = pocket::band_hz(band);
  // A kick under a louder bass: the law asks 7.5 dB.
  const auto kick = sine(hz, 0.3, second);
  const auto b = sine(hz, 0.5, second);
  auto runtime = make_effect(pocket::engine(), clean());
  (void)process(*runtime, b, {.sidechain = &kick});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), pocket::telemetry::size, "telemetry");
  require(std::fabs(t[pocket::telemetry::kick_db + band] - db(0.3)) < 0.5 &&
              std::fabs(t[pocket::telemetry::bass_db + band] - db(0.5)) < 0.5,
          "band levels read true: " + std::to_string(t[pocket::telemetry::kick_db + band]) + ", " +
              std::to_string(t[pocket::telemetry::bass_db + band]));
  REQUIRE_LT(std::fabs(-t[pocket::telemetry::reduction_db + band] - 7.5), 0.6,
             "the target cut reads as the law: " +
                 std::to_string(-t[pocket::telemetry::reduction_db + band]));
  REQUIRE_LT(t[pocket::telemetry::overlap_after_db], t[pocket::telemetry::overlap_before_db] - 2.0,
             "the overlap meter shows the cut");
  // The last waveform columns: kick and bass in peak at their levels, the
  // bass out lower by the cut.
  const auto column_peak = [&](std::size_t offset) {
    double top = 0.0;
    for (std::size_t i = pocket::wave_points - 8; i < pocket::wave_points; ++i) {
      top = std::max(top, static_cast<double>(t[offset + 2 * i + 1]));
    }
    return top;
  };
  REQUIRE_LT(std::fabs(column_peak(pocket::telemetry::wave_kick) - 0.3), 0.02,
             "the waveforms carry the kick and the bass");
  REQUIRE_LT(std::fabs(column_peak(pocket::telemetry::wave_bass_in) - 0.5), 0.02,
             "the waveforms carry the kick and the bass");
  REQUIRE_LT(std::fabs(db(column_peak(pocket::telemetry::wave_bass_out) / 0.5) + 7.5), 0.8,
             "and the bass out shows the cut");
}

struct Golden {
  std::string_view name;
  double level_db;
  double low_db;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 7> golden{{
      {"Clean Low End", -12.0538, 37.4001},
      {"Sub Only", -12.6648, 36.7499},
      {"Punchy House", -11.6249, 37.8629},
      {"Gentle Rock", -11.3314, 38.1872},
      {"808 And Kick", -16.0735, 33.634},
      {"Classic Pump", -20.902, 28.9748},
      {"Init", -10.5845, 39.0934},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  const auto kick = kicks("909 House", four_on_the_floor(4), 2 * second);
  const auto b = bass(Bass::line, 2 * second);
  bool ok = true;
  for (const auto& entry : golden) {
    const auto out = pocket_bass(preset(table(), entry.name), b, kick);
    const double level = db(rms(out.left, second / 4, 2 * second));
    const double low = 10.0 * std::log10(highpass_energy(out.left, 0, 2 * second, 20.0) -
                                         highpass_energy(out.left, 0, 2 * second, 120.0) + 1e-30);
    if (std::fabs(level - entry.level_db) > 0.05 || std::fabs(low - entry.low_db) > 0.05) {
      std::cerr << "{\"" << entry.name << "\", " << level << ", " << low << "},\n";
      ok = false;
    }
  }
  require(ok, "presets match their golden fingerprints");
}

} // namespace

int main(int argc, char** argv) {
  if (std::getenv("SURVEY") != nullptr) {
    for (const double choice : {0.0, 3.0}) {
      real_kicks_come_out_of_the_bass(choice, true);
    }
    return 0;
  }
  return oscillate::test::run_cases(
      argc, argv, "native pocket contract passed",
      {
          {"bypass_is_bit_exact", [] { bypass_is_bit_exact(); }},
          {"the_laws_hold_on_steady_tones", [] { the_laws_hold_on_steady_tones(); }},
          {"the_cut_lands_on_time_with_lookahead", [] { the_cut_lands_on_time_with_lookahead(); }},
          {"focus_bounds_the_cut", [] { focus_bounds_the_cut(); }},
          {"release_and_follow_shape_the_recovery",
           [] { release_and_follow_shape_the_recovery(); }},
          {"lookahead_is_an_exact_latency", [] { lookahead_is_an_exact_latency(); }},
          {"the_mono_sub_spares_the_mid", [] { the_mono_sub_spares_the_mid(); }},
          {"steady_cuts_add_no_distortion", [] { steady_cuts_add_no_distortion(); }},
          {"toggles_never_click", [] { toggles_never_click(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
          // The presets' 20 ms lookahead.
          {"real_kicks_come_out_of_the_bass", [] { real_kicks_come_out_of_the_bass(3.0); }},
          {"a_fast_pattern_stays_steady", [] { a_fast_pattern_stays_steady(); }},
          {"presets_match_golden_fingerprints", [] { presets_match_golden_fingerprints(); }},
      });
}
