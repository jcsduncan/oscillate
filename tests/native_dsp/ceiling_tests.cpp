#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Ceiling (ADR-0117) against measured oracles: the true peak never over the
// ceiling at any setting, rate or preset, nor while it glides; steady tones
// and intermodulation far cleaner than the classic limiter's; onsets kept
// by Transients; an alias-free clipper; stereo link; the reported latency;
// block splits, export against live and a seek; hostile input; restrained
// presets; an allocation-free callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace ceiling = oscillate::native_dsp::effects::ceiling;
namespace native = oscillate::native_dsp;

const effects::EffectEngine& engine() { return ceiling::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::array rates{oscillate::test::sample_rate_44k_hz, oscillate::test::sample_rate_48k_hz,
                           oscillate::test::sample_rate_88k_hz,
                           oscillate::test::sample_rate_96k_hz};
constexpr double one_second = rate;
constexpr double milliseconds_per_second = 1'000.0;
constexpr std::size_t block = 128;

// Levels and ceilings.
constexpr double default_ceiling_db = -1.0;
constexpr std::array ceilings_db{-6.0, -1.0, -0.1};
constexpr double hot_db = 12.0;
// Float output may round a hair over a ceiling the guard holds 0.01 dB
// under.
constexpr double true_peak_slack_db = 0.001;

// Tones: the classic limiter's weak spots (a low note and intermodulation)
// driven 6 dB into the ceiling, read once settled.
constexpr double tone_over_db = 6.0;
constexpr double low_hz = 50.0;
constexpr double mid_hz = 1'000.0;
constexpr double smpte_low_hz = 60.0;
constexpr double smpte_high_hz = 7'000.0;
constexpr double smpte_ratio = 4.0;
constexpr int smpte_sidebands = 3;
constexpr double tone_seconds = 3.0;
constexpr double settle_seconds = 1.5;
// Measured: about -150 dB THD+N and -110 dB SMPTE IMD (ADR-0117). The
// classic limiter reads -60 and -65 dB with its defaults.
constexpr double clean_thd_n_db = -120.0;
constexpr double clean_smpte_db = -100.0;
constexpr double better_than_classic_db = 30.0;

// Onsets.
constexpr double kick_period_seconds = 0.5;
constexpr double onset_seconds = 0.005;
constexpr double body_from_seconds = 0.03;
constexpr double body_to_seconds = 0.15;

// Aliasing: a 9 kHz tone pushed 6 dB into the clipper's room, hard knee.
constexpr double alias_hz = 9'000.0;
constexpr double alias_floor_db = -90.0;
constexpr double audible_hz = 20'000.0;
constexpr double harmonic_guard_hz = 50.0;

constexpr double decibels_per_bel = 10.0;

double gain(double decibels) { return native::dsp::db_to_gain(decibels); }

std::vector<double> defaults() { return preset(table(), "Default"); }

std::vector<double> with(std::vector<double> v, std::string_view id, double value) {
  v[at(id)] = value;
  return v;
}

Stereo scaled(Stereo s, double factor) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    s.left[i] *= factor;
    s.right[i] *= factor;
  }
  return s;
}

// A kick every half second: a falling sine with a fast decay.
Stereo kicks(std::size_t frames, double level) {
  constexpr double start_hz = 150.0;
  constexpr double end_hz = 50.0;
  constexpr double sweep_seconds = 0.03;
  constexpr double decay_seconds = 0.15;
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  const auto period = static_cast<std::size_t>(kick_period_seconds * one_second);
  double phase = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i % period) / one_second;
    if (i % period == 0) {
      phase = 0.0;
    }
    phase += 2.0 * std::numbers::pi *
             (end_hz + (start_hz - end_hz) * std::exp(-t / sweep_seconds)) / one_second;
    out.left[i] = out.right[i] = level * std::sin(phase) * std::exp(-t / decay_seconds);
  }
  return out;
}

// Kicks with hats on the eighths, hats left and right in turn.
Stereo loop(std::size_t frames, double level) {
  constexpr double hat_level = 0.4;
  constexpr double hat_decay_frames = 400.0;
  auto out = kicks(frames, 1.0);
  const auto hiss = noise(1.0, frames);
  const auto period = static_cast<std::size_t>(kick_period_seconds * one_second / 2.0);
  for (std::size_t i = 0; i < frames; ++i) {
    const double envelope =
        hat_level * std::exp(-static_cast<double>(i % period) / hat_decay_frames);
    const bool left = (i / period) % 2 == 0;
    out.left[i] += left ? hiss.left[i] * envelope : 0.0;
    out.right[i] += left ? 0.0 : hiss.right[i] * envelope;
  }
  return scaled(out, level / std::max(peak(out.left), peak(out.right)));
}

Stereo render(const std::vector<double>& v, const Stereo& input, double r = rate) {
  auto runtime = make_effect(engine(), v, r);
  return process(*runtime, input, {.rate = r});
}

double true_peak_db(const Stereo& s, double r = rate) {
  native::StereoTruePeakMeter meter;
  REQUIRE_TRUE(meter.prepare(static_cast<std::uint32_t>(r)), "the meter prepares");
  std::vector<oscillate::engine::StereoFrame> frames;
  for (std::size_t i = 0; i < s.size(); ++i) {
    frames.push_back({static_cast<float>(s.left[i]), static_cast<float>(s.right[i])});
    if (frames.size() == native::StereoTruePeakMeter::max_block_frames) {
      REQUIRE_TRUE(meter.process(frames), "the meter reads");
      frames.clear();
    }
  }
  REQUIRE_TRUE(meter.process(frames) && meter.finish(), "the meter reads");
  const auto reading = meter.reading();
  return db(std::max(reading.left_true_peak, reading.right_true_peak));
}

double lufs(const Stereo& s) {
  native::IntegratedLoudnessAnalyzer analyser(static_cast<std::uint32_t>(rate),
                                              native::LoudnessLayout::stereo);
  std::vector<float> interleaved;
  for (std::size_t i = 0; i < s.size(); ++i) {
    interleaved.push_back(static_cast<float>(s.left[i]));
    interleaved.push_back(static_cast<float>(s.right[i]));
  }
  REQUIRE_TRUE(analyser.append(interleaved) == native::LoudnessStatus::ok, "loudness reads");
  return analyser.finish().integrated_lkfs.value_or(-std::numeric_limits<double>::infinity());
}

std::span<const double> settled(const std::vector<double>& x) {
  return std::span(x).subspan(static_cast<std::size_t>(settle_seconds * one_second),
                              static_cast<std::size_t>(one_second));
}

// THD+N: the residual once a least-squares sine at `hz` and DC are taken
// out, over the fitted sine.
double thd_n_db(std::span<const double> x, double hz) {
  std::array<std::array<double, 4>, 3> m{};
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    const std::array<double, 3> basis{std::sin(w), std::cos(w), 1.0};
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        m[r][c] += basis[r] * basis[c];
      }
      m[r][3] += basis[r] * x[i];
    }
  }
  for (std::size_t p = 0; p < 3; ++p) {
    for (std::size_t r = p + 1; r < 3; ++r) {
      const double f = m[r][p] / m[p][p];
      for (std::size_t c = p; c < 4; ++c) {
        m[r][c] -= f * m[p][c];
      }
    }
  }
  std::array<double, 3> s{};
  for (std::size_t r = 3; r-- > 0;) {
    double v = m[r][3];
    for (std::size_t c = r + 1; c < 3; ++c) {
      v -= m[r][c] * s[c];
    }
    s[r] = v / m[r][r];
  }
  double residual = 0.0;
  double fitted = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
    const double y = s[0] * std::sin(w) + s[1] * std::cos(w);
    fitted += y * y;
    residual += (x[i] - y - s[2]) * (x[i] - y - s[2]);
  }
  return decibels_per_bel * std::log10(residual / fitted);
}

// The amplitude at `hz` under a four-term Blackman-Harris window.
double amplitude(std::span<const double> x, double hz) {
  constexpr std::array<double, 4> bh{0.35875, 0.48829, 0.14128, 0.01168};
  std::complex<double> sum{};
  const auto n = static_cast<double>(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / n;
    const double w =
        bh[0] - bh[1] * std::cos(t) + bh[2] * std::cos(2.0 * t) - bh[3] * std::cos(3.0 * t);
    sum += w * x[i] * std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
  }
  return std::abs(sum);
}

double smpte_db(std::span<const double> x) {
  double sidebands = 0.0;
  for (int k = 1; k <= smpte_sidebands; ++k) {
    for (const double sign : {-1.0, 1.0}) {
      const double a = amplitude(x, smpte_high_hz + sign * k * smpte_low_hz);
      sidebands += a * a;
    }
  }
  return db(std::sqrt(sidebands) / amplitude(x, smpte_high_hz));
}

Stereo tone(double hz, double level) {
  return sine(hz, level, static_cast<std::size_t>(tone_seconds * one_second));
}

Stereo smpte(double level) {
  auto out = sine(smpte_low_hz, 1.0, static_cast<std::size_t>(tone_seconds * one_second));
  const auto high = sine(smpte_high_hz, 1.0, out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    out.left[i] = out.right[i] =
        level * (smpte_ratio * out.left[i] + high.left[i]) / (smpte_ratio + 1.0);
  }
  return out;
}

// The classic limiter (ADR-0047's original) with its defaults.
Stereo classic(const Stereo& input) {
  auto prepared = native::prepare_limiter({}, static_cast<std::uint32_t>(rate));
  REQUIRE_OK(prepared, "the classic limiter prepares");
  native::LimiterRuntime limiter(prepared.value());
  Stereo out = input;
  std::vector<oscillate::engine::StereoFrame> buffer;
  for (std::size_t from = 0; from < input.size(); from += block) {
    const auto n = std::min(block, input.size() - from);
    buffer.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      buffer[i] = {static_cast<float>(input.left[from + i]),
                   static_cast<float>(input.right[from + i])};
    }
    limiter.process(buffer, oscillate::engine::BlockContext{0, 1});
    for (std::size_t i = 0; i < n; ++i) {
      out.left[from + i] = buffer[i].left;
      out.right[from + i] = buffer[i].right;
    }
  }
  return out;
}

// ---- Cases ---------------------------------------------------------------------------------

void descriptor_registry_and_presets() {
  REQUIRE_TRUE(effects::find_engine("ceiling") == &engine(), "Ceiling is registered");
  REQUIRE_OK(native::validate_engine(table()), "the table is valid");
  REQUIRE_GE(table().presets.size(), std::size_t{6}, "the factory presets");
  for (const auto& entry : table().presets) {
    REQUIRE_OK(native::preset_values(table(), entry), std::string(entry.name) + " resolves");
  }
  REQUIRE_TRUE(!table().parameters[at("ceiling.lookahead")].automatable,
               "the lookahead is the latency, not automated");
}

void true_peak_never_exceeds_the_ceiling() {
  const auto frames = static_cast<std::size_t>(2.0 * one_second);
  const auto programmes = std::array{loop(frames, gain(hot_db)), noise(gain(hot_db), frames),
                                     sine(alias_hz, gain(hot_db), frames)};
  for (const double ceiling_db : ceilings_db) {
    for (const auto& entry : table().presets) {
      const auto v = with(preset(table(), entry.name), "ceiling.ceiling", ceiling_db);
      for (const auto& input : programmes) {
        REQUIRE_LE(true_peak_db(render(v, input)), ceiling_db + true_peak_slack_db,
                   std::string(entry.name) + ": the true peak stays under the ceiling");
      }
    }
  }
  for (const double r : rates) {
    const auto v = with(defaults(), "ceiling.clip", 3.0);
    const auto input = loop(static_cast<std::size_t>(r), gain(hot_db));
    REQUIRE_LE(true_peak_db(render(v, input, r), r), default_ceiling_db + true_peak_slack_db,
               "at every rate");
  }
}

void true_peak_holds_while_the_ceiling_glides() {
  constexpr double low_db = -9.0;
  const auto frames = static_cast<std::size_t>(2.0 * one_second);
  const auto input = loop(frames, gain(hot_db));
  auto runtime = make_effect(engine(), defaults(), rate);
  std::uint64_t revision = 0;
  RenderOptions options;
  for (std::uint64_t at_frame = block; at_frame < frames; at_frame += block * 8) {
    options.at.push_back(at_frame);
  }
  options.before = [&](std::uint64_t frame) {
    const double phase = static_cast<double>(frame) / static_cast<double>(frames);
    const double ceiling_db =
        default_ceiling_db +
        (low_db - default_ceiling_db) * (0.5 - 0.5 * std::cos(4.0 * std::numbers::pi * phase));
    REQUIRE_TRUE(runtime->offer_values(with(defaults(), "ceiling.ceiling", ceiling_db), ++revision),
                 "the ceiling moves");
  };
  const auto out = process(*runtime, input, options);
  REQUIRE_LE(true_peak_db(out), default_ceiling_db + true_peak_slack_db,
             "never over the highest ceiling it passed through");
}

void steady_tones_stay_clean() {
  const double over = gain(default_ceiling_db + tone_over_db);
  for (const auto* name : {"Default", "Transparent", "Streaming -14", "Punchy", "Gentle Bus"}) {
    const auto v = preset(table(), name);
    REQUIRE_LE(thd_n_db(settled(render(v, tone(low_hz, over)).left), low_hz), clean_thd_n_db,
               std::string(name) + ": a 50 Hz tone stays clean");
    REQUIRE_LE(thd_n_db(settled(render(v, tone(mid_hz, over)).left), mid_hz), clean_thd_n_db,
               std::string(name) + ": a 1 kHz tone stays clean");
    REQUIRE_LE(smpte_db(settled(render(v, smpte(over)).left)), clean_smpte_db,
               std::string(name) + ": SMPTE intermodulation stays low");
  }
}

void distortion_beats_the_classic_limiter() {
  const double over = gain(default_ceiling_db + tone_over_db);
  const auto low = tone(low_hz, over);
  const double ours = thd_n_db(settled(render(defaults(), low).left), low_hz);
  const double theirs = thd_n_db(settled(classic(low).left), low_hz);
  REQUIRE_LE(ours, theirs - better_than_classic_db, "THD+N far under the classic limiter's");
  const auto two = smpte(over);
  REQUIRE_LE(smpte_db(settled(render(defaults(), two).left)),
             smpte_db(settled(classic(two).left)) - better_than_classic_db,
             "SMPTE IMD far under the classic limiter's");
  // The classic limiter reads sample peaks; its output goes over the ceiling.
  const auto hot = loop(static_cast<std::size_t>(one_second), gain(hot_db));
  REQUIRE_GT(true_peak_db(classic(hot)), default_ceiling_db, "the classic limiter overshoots");
  REQUIRE_LE(true_peak_db(render(defaults(), hot)), default_ceiling_db + true_peak_slack_db,
             "Ceiling does not");
}

// The attack (first 5 ms) of every kick over its body (30-150 ms on), in
// dB, mean over the kicks after the first; `latency` frames late.
double onset_contrast_db(const Stereo& s, std::size_t latency = 0) {
  const auto period = static_cast<std::size_t>(kick_period_seconds * one_second);
  const auto onset = static_cast<std::size_t>(onset_seconds * one_second);
  const auto from = static_cast<std::size_t>(body_from_seconds * one_second);
  const auto to = static_cast<std::size_t>(body_to_seconds * one_second);
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t start = period + latency; start + period <= s.size(); start += period) {
    const double attack = rms(s.left, start, start + onset);
    const double body = rms(s.left, start + from, start + to);
    sum += db(attack) - db(body);
    ++count;
  }
  return sum / static_cast<double>(count);
}

void transients_keep_the_onsets() {
  const auto frames = static_cast<std::size_t>(4.0 * one_second);
  const auto input = kicks(frames, gain(hot_db));
  const auto none = render(with(defaults(), "ceiling.transients", 0.0), input);
  const auto full = render(with(defaults(), "ceiling.transients", 1.0), input);
  const auto latency = engine().latency_frames(defaults(), rate);
  const double original = onset_contrast_db(input);
  const double kept = onset_contrast_db(full, latency);
  const double lost = onset_contrast_db(none, latency);
  constexpr double kept_db = 1.0;
  REQUIRE_GT(kept, lost + kept_db,
             "Transients lets the onsets through: " + std::to_string(kept) + " vs " +
                 std::to_string(lost) + " of " + std::to_string(original));
  REQUIRE_LE(true_peak_db(full), default_ceiling_db + true_peak_slack_db, "under the ceiling");
  // At the same drive it is about as loud: the gain the onsets keep is
  // spent on the sustain.
  constexpr double loudness_tolerance_lu = 0.1;
  REQUIRE_NEAR(lufs(full), lufs(none), loudness_tolerance_lu, "about as loud");
}

void the_clipper_does_not_alias() {
  constexpr double room_db = 6.0;
  auto v = with(defaults(), "ceiling.clip", room_db);
  v = with(v, "ceiling.transients", 0.0);
  v = with(v, "ceiling.shape", 0.0);
  const auto frames = static_cast<std::size_t>(one_second);
  const auto out = render(v, sine(alias_hz, gain(default_ceiling_db + room_db), frames));
  const auto x = std::span(out.left).subspan(frames / 2, frames / 4);
  const double reference = amplitude(x, alias_hz);
  double worst = -std::numeric_limits<double>::infinity();
  constexpr double resolution_hz = 50.0;
  for (double hz = resolution_hz; hz < audible_hz; hz += resolution_hz) {
    const double harmonic = std::fmod(hz, alias_hz);
    if (harmonic < harmonic_guard_hz || alias_hz - harmonic < harmonic_guard_hz) {
      continue; // the tone and its harmonics
    }
    worst = std::max(worst, db(amplitude(x, hz) / reference));
  }
  REQUIRE_LE(worst, alias_floor_db, "clipping does not alias: " + std::to_string(worst));
}

void stereo_link_shares_the_gain() {
  const auto frames = static_cast<std::size_t>(one_second);
  auto input = sine(low_hz, gain(hot_db), frames);
  const double quiet = gain(-hot_db);
  for (double& x : input.right) {
    x *= quiet / gain(hot_db);
  }
  const auto rest = static_cast<std::size_t>(one_second / 2.0);
  const auto right_gain = [&](double link) {
    auto v = with(defaults(), "ceiling.link", link);
    v = with(v, "ceiling.transient_link", link);
    const auto out = render(v, input);
    return rms(out.right, rest, frames) / rms(input.right, rest, frames);
  };
  constexpr double untouched = 0.99;
  REQUIRE_GT(right_gain(0.0), untouched, "unlinked, the quiet side is untouched");
  REQUIRE_LT(db(right_gain(1.0)), -hot_db, "linked, it takes the loud side's reduction");
}

void latency_is_reported_and_exact() {
  constexpr double quiet = 0.25;
  constexpr double ripple = 1e-4;
  const auto input = sine(mid_hz, quiet, static_cast<std::size_t>(one_second / 4.0));
  for (const double lookahead : {0.5, 4.0, 10.0}) {
    const auto v = with(defaults(), "ceiling.lookahead", lookahead);
    for (const double r : rates) {
      REQUIRE_EQ(engine().latency_frames(v, r), ceiling::latency_frames(lookahead, r),
                 "the engine reports it");
    }
    const auto latency = engine().latency_frames(v, rate);
    const auto out = render(v, input);
    double worst = 0.0;
    for (std::size_t i = 2 * latency + 1; i < input.size(); ++i) {
      worst = std::max(worst, std::fabs(out.left[i] - input.left[i - latency]));
    }
    REQUIRE_LT(worst, ripple, "delayed exactly by the latency: " + std::to_string(worst));
  }
  // 4 ms at 48 kHz: the fine peak's reach (16), the averaging (192 - 1),
  // the 8x clipper's round trip and the guard (24 + 31).
  constexpr std::uint32_t default_48k = 16 + 191 + 92 + 55;
  REQUIRE_EQ(ceiling::latency_frames(4.0, rate), default_48k, "4 ms of lookahead at 48 kHz");
}

// With no room to clip (Transients and Clip at 0) and nothing to limit,
// Ceiling is an exact delay: the oversampler sleeps.
void without_room_it_is_an_exact_delay() {
  constexpr double quiet = 0.25;
  auto v = with(defaults(), "ceiling.transients", 0.0);
  v = with(v, "ceiling.clip", 0.0);
  v = with(v, "ceiling.ceiling", 0.0);
  const auto input = noise(quiet, static_cast<std::size_t>(one_second / 4.0));
  const auto latency = engine().latency_frames(v, rate);
  const auto out = render(v, input);
  for (std::size_t i = latency; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i - latency])),
               "an exact delay");
  }
}

// Opening the clipper's room mid-stream wakes the oversampler and fades to
// it without a click; closing it fades back.
void opening_the_room_never_clicks() {
  constexpr double quiet = 0.25;
  constexpr double steepest = 2.0 * std::numbers::pi * mid_hz / rate * quiet;
  constexpr double click_margin = 1.05;
  auto closed = with(defaults(), "ceiling.transients", 0.0);
  closed = with(closed, "ceiling.clip", 0.0);
  const auto open = with(closed, "ceiling.transients", 1.0);
  const auto frames = static_cast<std::size_t>(one_second);
  const auto input = sine(mid_hz, quiet, frames);
  auto runtime = make_effect(engine(), closed, rate);
  RenderOptions options;
  options.at = {frames / 4, frames / 2};
  std::uint64_t revision = 0;
  options.before = [&](std::uint64_t frame) {
    REQUIRE_TRUE(runtime->offer_values(frame == frames / 4 ? open : closed, ++revision),
                 "the room moves");
  };
  const auto out = process(*runtime, input, options);
  const auto latency = engine().latency_frames(closed, rate);
  double worst = 0.0;
  for (std::size_t i = latency + 1; i < frames; ++i) {
    worst = std::max(worst, std::fabs(out.left[i] - out.left[i - 1]));
  }
  REQUIRE_LE(worst, steepest * click_margin, "no step: " + std::to_string(worst));
}

void any_block_split_renders_the_same() {
  const auto input = loop(static_cast<std::size_t>(one_second), gain(hot_db));
  const auto v = preset(table(), "Loud Club");
  auto a = make_effect(engine(), v, rate);
  auto b = make_effect(engine(), v, rate);
  const auto even = process(*a, input, {.block = block});
  const auto odd = process(*b, input, {.block = 0});
  require(even.left == odd.left && even.right == odd.right, "block splits do not change a bit");
}

void export_equals_live() {
  constexpr std::size_t export_block = 4'096;
  constexpr double live_bpm = 124.0;
  const auto input = loop(static_cast<std::size_t>(one_second), gain(hot_db));
  for (const auto& entry : table().presets) {
    const auto v = preset(table(), entry.name);
    auto live = make_effect(engine(), v, rate);
    const auto played = process(*live, input, {.block = block, .bpm = live_bpm});
    auto offline = make_effect(engine(), v, rate);
    const auto exported =
        process(*offline, input, {.block = export_block, .generation = 2, .bpm = 0.0});
    require(played.left == exported.left && played.right == exported.right,
            std::string(entry.name) + ": export equals live");
  }
}

void a_seek_starts_clean() {
  const auto frames = static_cast<std::size_t>(one_second);
  const auto input = loop(frames, gain(hot_db));
  auto fresh = make_effect(engine(), defaults(), rate);
  const auto expected = process(*fresh, input);
  auto seeking = make_effect(engine(), defaults(), rate);
  (void)process(*seeking, noise(gain(hot_db), frames));
  const auto after = process(*seeking, input, {.generation = 2});
  require(after.left == expected.left && after.right == expected.right,
          "a new transport generation starts from silence");
}

void hostile_input_stays_finite() {
  constexpr std::size_t frames = 9'600;
  Stereo input = noise(0.5, frames);
  input.left[100] = std::numeric_limits<double>::quiet_NaN();
  input.right[200] = std::numeric_limits<double>::infinity();
  input.left[300] = 1e30;
  for (std::size_t i = frames / 2; i < frames; ++i) {
    input.left[i] = input.right[i] = 1e-30;
  }
  for (const auto& entry : table().presets) {
    const auto out = render(preset(table(), entry.name), input);
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), "finite output");
      REQUIRE_LE(std::fabs(out.left[i]), 1.0, "under full scale");
    }
  }
}

void presets_stay_restrained() {
  // On a loop at -14 LUFS, no preset takes more than this off the loudest
  // peak, and none comes out louder than its own target plus its drive's
  // worth.
  constexpr double programme_lufs = -14.0;
  constexpr double most_reduction_db = 10.0;
  const auto frames = static_cast<std::size_t>(4.0 * one_second);
  auto input = loop(frames, 1.0);
  input = scaled(input, gain(programme_lufs - lufs(input)));
  for (const auto& entry : table().presets) {
    const auto v = preset(table(), entry.name);
    auto runtime = make_effect(engine(), v, rate);
    double most = 0.0;
    RenderOptions options;
    for (std::uint64_t f = block; f < frames; f += block) {
      options.at.push_back(f);
    }
    std::vector<float> frame;
    options.before = [&](std::uint64_t) {
      if (runtime->read_telemetry(frame)) {
        most = std::max(most, static_cast<double>(frame[ceiling::telemetry::reduction]));
      }
    };
    const auto out = process(*runtime, input, options);
    REQUIRE_LE(most, most_reduction_db, std::string(entry.name) + " stays restrained");
    REQUIRE_LE(true_peak_db(out), v[at("ceiling.ceiling")] + true_peak_slack_db,
               std::string(entry.name) + " holds its ceiling");
  }
}

void the_callback_never_allocates() {
  const auto values = preset(table(), "Loud Club");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, noise(0.3, 4'096));
  auto changed = with(values, "ceiling.transients", 0.0);
  changed = with(changed, "ceiling.clip", 0.0);
  changed = with(changed, "ceiling.ceiling", -6.0);
  changed = with(changed, "ceiling.link", 0.0);
  require(runtime->offer_values(changed, 1), "new values are offered");
  std::vector<oscillate::engine::StereoFrame> buffer(block, {0.9F, -0.9F});
  constexpr std::uint64_t blocks = 2'000;
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    const Transport transport{0.0, 0.0, 0.0, 4.0};
    (void)runtime->process(buffer, {}, 4'096 + b * buffer.size(), b < blocks / 2 ? 1U : 2U,
                           transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  // Read just after a kick, while the held peaks are fresh.
  constexpr double kicks_played = 12.0;
  constexpr double after_kick_seconds = 0.01;
  const auto frames = static_cast<std::size_t>(
      (kicks_played * kick_period_seconds + after_kick_seconds) * one_second);
  const auto input = loop(frames, gain(hot_db));
  auto runtime = make_effect(engine(), defaults(), rate);
  const auto out = process(*runtime, input);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), ceiling::telemetry::size, "the layout's size");
  namespace t = ceiling::telemetry;
  constexpr double reading_tolerance_db = 1.0;
  constexpr double loudness_tolerance_lu = 0.2;
  REQUIRE_GT(frame[t::input_peak], static_cast<float>(hot_db - reading_tolerance_db),
             "the input's peak");
  REQUIRE_LE(frame[t::output_peak], static_cast<float>(default_ceiling_db + true_peak_slack_db),
             "the output's true peak");
  REQUIRE_GT(frame[t::reduction], static_cast<float>(hot_db / 2.0), "the gain reduction");
  REQUIRE_GE(frame[t::clipped], 0.0F, "what the clipper took");
  REQUIRE_GE(frame[t::guarded], 0.0F, "what the guard took");
  REQUIRE_NEAR(static_cast<double>(frame[t::integrated]), lufs(out), loudness_tolerance_lu,
               "the integrated loudness of the output");
  REQUIRE_TRUE(frame[t::short_term] > ceiling::no_loudness &&
                   frame[t::momentary] > ceiling::no_loudness,
               "short-term and momentary readings");
  REQUIRE_NEAR(static_cast<double>(frame[t::latency_ms]),
               static_cast<double>(engine().latency_frames(defaults(), rate)) / rate *
                   milliseconds_per_second,
               reading_tolerance_db, "the latency");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native ceiling contract passed",
      {
          {"descriptor_registry_and_presets", descriptor_registry_and_presets},
          {"true_peak_never_exceeds_the_ceiling", true_peak_never_exceeds_the_ceiling},
          {"true_peak_holds_while_the_ceiling_glides", true_peak_holds_while_the_ceiling_glides},
          {"steady_tones_stay_clean", steady_tones_stay_clean},
          {"distortion_beats_the_classic_limiter", distortion_beats_the_classic_limiter},
          {"transients_keep_the_onsets", transients_keep_the_onsets},
          {"the_clipper_does_not_alias", the_clipper_does_not_alias},
          {"stereo_link_shares_the_gain", stereo_link_shares_the_gain},
          {"latency_is_reported_and_exact", latency_is_reported_and_exact},
          {"without_room_it_is_an_exact_delay", without_room_it_is_an_exact_delay},
          {"opening_the_room_never_clicks", opening_the_room_never_clicks},
          {"any_block_split_renders_the_same", any_block_split_renders_the_same},
          {"export_equals_live", export_equals_live},
          {"a_seek_starts_clean", a_seek_starts_clean},
          {"hostile_input_stays_finite", hostile_input_stays_finite},
          {"presets_stay_restrained", presets_stay_restrained},
          {"the_callback_never_allocates", the_callback_never_allocates},
          {"telemetry_feeds_the_panel", telemetry_feeds_the_panel},
      });
}
