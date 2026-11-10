// Limiter audit: measures a limiter engine's factory presets at equal
// integrated loudness. For each preset and each target (-14, -9 and -6
// LUFS) it finds the drive that reaches the target on every programme, then
// reports distortion (THD+N on 50 Hz and 1 kHz tones, SMPTE and CCIF IMD),
// transient loss (short-term crest factor and onset contrast in three bands),
// pumping (gain modulation of the top band), the true peak at 4x and 16x
// against the ceiling, latency and CPU. The ceiling is the same (-1 dBTP) for
// every preset, so the presets compare like for like.
//
// Programmes: a synthetic drum-heavy loop that is always included, and any
// raw 48 kHz stereo float32 files on the command line (ffmpeg -f f32le -ac 2
// -ar 48000), each normalised to -20 LUFS before the limiter.
//
// Usage: oscillate_limiter_audit [--engine ID] [--preset NAME]... [--set ID=VALUE]...
//        [--target LUFS] [--classic] [--no-loop] [--per-programme] [FILE.f32]...
// --no-loop leaves the synthetic loop out.
// --target measures at one loudness instead of the three standard ones.
// --classic measures the classic sample-peak limiter Ceiling replaced, with
// Ceiling's former presets: the baseline.
// --set overrides a parameter on every preset measured.
#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/true_peak.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace dsp = oscillate::native_dsp;
namespace effects = oscillate::native_dsp::effects;
using oscillate::engine::StereoFrame;

constexpr double rate = 48'000.0;
constexpr std::uint32_t rate_hz = 48'000;
constexpr std::size_t block_frames = 128;
constexpr double pi = std::numbers::pi;

// The comparison's fixed points.
constexpr double ceiling_db = -1.0;
constexpr std::array targets_lufs{-14.0, -9.0, -6.0};
constexpr double programme_lufs = -20.0;
constexpr double lufs_tolerance = 0.05;
// The drive is applied to the programme before the engine (its own gain at
// 0 dB), so the search is not bounded by the engine's gain range.
constexpr double gain_low_db = -12.0;
constexpr double gain_high_db = 36.0;
constexpr int gain_search_steps = 18;
// How far over the ceiling the test tones peak, per target, in order.
constexpr std::array tone_over_db{3.0, 6.0, 9.0};

// The synthetic loop.
constexpr double loop_bpm = 124.0;
constexpr double beats_per_bar = 4.0;
constexpr std::size_t steps_per_beat = 4;
constexpr std::size_t open_hat_step = 2;
constexpr double loop_seconds = 20.0;
constexpr std::uint64_t loop_seed = 0x5eed'1234'abcd'0001ULL;

// Measurement windows and bands.
constexpr double crest_window_seconds = 0.1;
constexpr double crest_floor_db = -40.0;
constexpr double onset_hop_seconds = 0.005;
constexpr std::size_t onset_history_hops = 10;
constexpr double onset_rise_db = 6.0;
constexpr double onset_floor_db = -45.0;
constexpr std::size_t onset_attack_hops = 3;
constexpr std::size_t onset_body_first_hop = 6;
constexpr std::size_t onset_body_last_hop = 30;
constexpr double onset_spacing_seconds = 0.1;
constexpr double low_split_hz = 150.0;
constexpr double high_split_hz = 2'500.0;
constexpr double pump_band_hz = 5'000.0;
constexpr double pump_frame_seconds = 0.01;
constexpr double pump_trend_seconds = 1.0;
constexpr double pump_floor_db = -60.0;
constexpr double butterworth_q = 0.7071067811865476;

// Tones.
constexpr double tone_seconds = 3.0;
constexpr double tone_settle_seconds = 1.5;
constexpr double thd_low_hz = 50.0;
constexpr double thd_high_hz = 1'000.0;
constexpr double smpte_low_hz = 60.0;
constexpr double smpte_high_hz = 7'000.0;
constexpr double smpte_ratio = 4.0;
constexpr int smpte_sidebands = 3;
constexpr double ccif_first_hz = 19'000.0;
constexpr double ccif_second_hz = 20'000.0;

// The 16x true-peak check: a Kaiser-windowed sinc.
constexpr std::size_t fine_factor = 16;
constexpr std::size_t fine_radius = 48;
constexpr double fine_beta = 10.0;

constexpr double floor_power = 1e-30;
constexpr double milliseconds_per_second = 1'000.0;

[[nodiscard]] double to_db(double gain) { return 20.0 * std::log10(gain + floor_power); }
[[nodiscard]] double power_db(double power) { return 10.0 * std::log10(power + floor_power); }
[[nodiscard]] double from_db(double db) { return std::pow(10.0, db / 20.0); }

struct Audio {
  std::vector<double> left;
  std::vector<double> right;
  [[nodiscard]] std::size_t size() const noexcept { return left.size(); }
};

struct Programme {
  std::string name;
  Audio audio;
};

// ---- Programmes ----------------------------------------------------------------------------

class Random {
public:
  explicit Random(std::uint64_t seed) : generator_(seed) {}
  double bipolar() { return 2.0 * generator_.unit() - 1.0; }

private:
  oscillate::foundation::SplitMix64 generator_;
};

struct OnePole {
  double a = 0.0;
  double y = 0.0;
  void set(double hz) { a = std::exp(-2.0 * pi * hz / rate); }
  double low(double x) { return y = x + a * (y - x); }
  double high(double x) { return x - low(x); }
};

// A drum-heavy loop: kick on every beat, clap on 2 and 4, sixteenth hats
// panned, an off-beat open hat, a rolling bass and a wide pad.
Audio drum_loop() {
  const auto frames = static_cast<std::size_t>(loop_seconds * rate);
  Audio out{std::vector<double>(frames), std::vector<double>(frames)};
  Random random(loop_seed);
  const double beat = 60.0 / loop_bpm * rate;
  const double sixteenth = beat / static_cast<double>(steps_per_beat);

  // Each voice renders this many decay times before it is cut.
  constexpr double decays_rendered = 6.0;
  constexpr double clap_band_ratio = 4.0;
  constexpr double clap_noise = 2.0;
  constexpr double clap_tone = 0.4;
  constexpr double kick_level = 0.9;
  constexpr double kick_start_hz = 160.0;
  constexpr double kick_end_hz = 48.0;
  constexpr double kick_sweep_seconds = 0.03;
  constexpr double kick_decay_seconds = 0.18;
  constexpr double kick_click_seconds = 0.002;
  constexpr double kick_click_level = 0.35;
  constexpr double clap_level = 0.45;
  constexpr double clap_decay_seconds = 0.06;
  constexpr double clap_tone_hz = 210.0;
  constexpr double clap_band_hz = 1'200.0;
  constexpr double hat_level = 0.18;
  constexpr double hat_decay_seconds = 0.012;
  constexpr double open_hat_decay_seconds = 0.09;
  constexpr double hat_band_hz = 7'000.0;
  constexpr double hat_pan = 0.35;
  constexpr double bass_level = 0.22;
  constexpr double bass_hz = 55.0;
  constexpr double bass_decay_seconds = 0.09;
  constexpr double bass_cutoff_hz = 400.0;
  constexpr int bass_partials = 8;
  constexpr double pad_level = 0.05;
  constexpr std::array pad_hz{220.0, 261.63, 329.63, 392.0};
  constexpr double pad_detune = 1.004;

  const auto every = [&](double step, double offset, auto&& voice) {
    for (double start = offset; start < static_cast<double>(frames); start += step) {
      voice(static_cast<std::size_t>(start));
    }
  };
  every(beat, 0.0, [&](std::size_t at) {
    double phase = 0.0;
    const auto length = static_cast<std::size_t>(kick_decay_seconds * decays_rendered * rate);
    for (std::size_t i = 0; i < length && at + i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double hz =
          kick_end_hz + (kick_start_hz - kick_end_hz) * std::exp(-t / kick_sweep_seconds);
      phase += 2.0 * pi * hz / rate;
      const double body = std::sin(phase) * std::exp(-t / kick_decay_seconds);
      const double click = random.bipolar() * kick_click_level * std::exp(-t / kick_click_seconds);
      const double x = kick_level * (body + click);
      out.left[at + i] += x;
      out.right[at + i] += x;
    }
  });
  every(2.0 * beat, beat, [&](std::size_t at) {
    OnePole low;
    OnePole high;
    low.set(clap_band_hz * clap_band_ratio);
    high.set(clap_band_hz);
    const auto length = static_cast<std::size_t>(clap_decay_seconds * decays_rendered * rate);
    for (std::size_t i = 0; i < length && at + i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double noise = high.high(low.low(random.bipolar()));
      const double x = clap_level * std::exp(-t / clap_decay_seconds) *
                       (clap_noise * noise + clap_tone * std::sin(2.0 * pi * clap_tone_hz * t));
      out.left[at + i] += x;
      out.right[at + i] += x;
    }
  });
  std::size_t hat_index = 0;
  every(sixteenth, 0.0, [&](std::size_t at) {
    const bool open = hat_index % steps_per_beat == open_hat_step;
    const double decay = open ? open_hat_decay_seconds : hat_decay_seconds;
    const double pan = (hat_index % 2 == 0) ? -hat_pan : hat_pan;
    ++hat_index;
    OnePole high;
    high.set(hat_band_hz);
    const auto length = static_cast<std::size_t>(decay * decays_rendered * rate);
    for (std::size_t i = 0; i < length && at + i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double x = hat_level * std::exp(-t / decay) * high.high(random.bipolar());
      out.left[at + i] += x * (1.0 - pan);
      out.right[at + i] += x * (1.0 + pan);
    }
  });
  every(sixteenth, 0.0, [&](std::size_t at) {
    const std::size_t step =
        static_cast<std::size_t>(std::round(static_cast<double>(at) / sixteenth));
    if (step % steps_per_beat == 0) {
      return; // the kick's sixteenth
    }
    const auto length = static_cast<std::size_t>(sixteenth);
    for (std::size_t i = 0; i < length && at + i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      double saw = 0.0;
      for (int k = 1; k <= bass_partials; ++k) {
        const double hz = bass_hz * k;
        const double roll = 1.0 / std::sqrt(1.0 + std::pow(hz / bass_cutoff_hz, 4.0));
        saw += roll * std::sin(2.0 * pi * hz * t) / k;
      }
      const double x = bass_level * saw * std::exp(-t / bass_decay_seconds);
      out.left[at + i] += x;
      out.right[at + i] += x;
    }
  });
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    double l = 0.0;
    double r = 0.0;
    for (const double hz : pad_hz) {
      l += std::sin(2.0 * pi * hz * t);
      r += std::sin(2.0 * pi * hz * pad_detune * t);
    }
    out.left[i] += pad_level * l;
    out.right[i] += pad_level * r;
  }
  return out;
}

std::optional<Audio> read_f32(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::vector<float> samples;
  float pair[2];
  while (in.read(reinterpret_cast<char*>(pair), sizeof(pair))) {
    samples.push_back(pair[0]);
    samples.push_back(pair[1]);
  }
  Audio out{std::vector<double>(samples.size() / 2), std::vector<double>(samples.size() / 2)};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out.left[i] = samples[2 * i];
    out.right[i] = samples[2 * i + 1];
  }
  return out;
}

// ---- Loudness and peaks --------------------------------------------------------------------

double lufs(const Audio& audio) {
  dsp::IntegratedLoudnessAnalyzer analyser(rate_hz, dsp::LoudnessLayout::stereo);
  std::vector<float> interleaved(2 * audio.size());
  for (std::size_t i = 0; i < audio.size(); ++i) {
    interleaved[2 * i] = static_cast<float>(audio.left[i]);
    interleaved[2 * i + 1] = static_cast<float>(audio.right[i]);
  }
  (void)analyser.append(interleaved);
  const auto result = analyser.finish();
  return result.integrated_lkfs.value_or(-HUGE_VAL);
}

void scale(Audio& audio, double gain) {
  for (std::size_t i = 0; i < audio.size(); ++i) {
    audio.left[i] *= gain;
    audio.right[i] *= gain;
  }
}

double true_peak_4x(const Audio& audio) {
  dsp::StereoTruePeakMeter meter;
  (void)meter.prepare(rate_hz);
  std::vector<StereoFrame> chunk;
  for (std::size_t at = 0; at < audio.size(); at += dsp::StereoTruePeakMeter::max_block_frames) {
    const auto n = std::min(dsp::StereoTruePeakMeter::max_block_frames, audio.size() - at);
    chunk.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      chunk[i] = {static_cast<float>(audio.left[at + i]), static_cast<float>(audio.right[at + i])};
    }
    (void)meter.process(chunk);
  }
  (void)meter.finish();
  const auto r = meter.reading();
  return to_db(std::max(r.left_true_peak, r.right_true_peak));
}

double bessel_i0(double x) {
  constexpr int terms = 40;
  double sum = 1.0;
  double term = 1.0;
  for (int k = 1; k < terms; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

// Each frame's largest magnitude over both channels, the sample and the
// 16x-interpolated points after it, from the float samples a file would
// hold.
std::vector<double> fine_peaks(const Audio& audio) {
  std::vector<std::array<double, 2 * fine_radius>> phases(fine_factor);
  const double radius = static_cast<double>(fine_radius);
  for (std::size_t p = 0; p < fine_factor; ++p) {
    const double fraction = static_cast<double>(p) / static_cast<double>(fine_factor);
    double sum = 0.0;
    for (std::size_t k = 0; k < 2 * fine_radius; ++k) {
      const double offset = static_cast<double>(k) - radius + 1.0 - fraction;
      const double sinc = offset == 0.0 ? 1.0 : std::sin(pi * offset) / (pi * offset);
      const double ratio = offset / radius;
      const double window =
          std::fabs(ratio) >= 1.0
              ? 0.0
              : bessel_i0(fine_beta * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(fine_beta);
      phases[p][k] = sinc * window;
      sum += phases[p][k];
    }
    for (auto& tap : phases[p]) {
      tap /= sum;
    }
  }
  std::vector<double> out(audio.size(), 0.0);
  for (const auto* channel : {&audio.left, &audio.right}) {
    std::vector<double> x(channel->size());
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] = static_cast<float>((*channel)[i]);
      out[i] = std::max(out[i], std::fabs(x[i]));
    }
    for (std::size_t i = fine_radius; i + fine_radius < x.size(); ++i) {
      for (std::size_t p = 1; p < fine_factor; ++p) {
        double y = 0.0;
        for (std::size_t k = 0; k < 2 * fine_radius; ++k) {
          y += phases[p][k] * x[i + k + 1 - fine_radius];
        }
        out[i] = std::max(out[i], std::fabs(y));
      }
    }
  }
  return out;
}

double true_peak_16x(const std::vector<double>& peaks) {
  return to_db(*std::max_element(peaks.begin(), peaks.end()));
}

// ---- Rendering -----------------------------------------------------------------------------

struct Render {
  Audio audio;
  double seconds = 0.0;
  std::uint32_t latency = 0;
};

// Renders in fixed blocks, flushes the latency with silence and drops it,
// so the output lines up with the input.
Render render(const effects::EffectEngine& engine, const std::vector<double>& values,
              const Audio& input) {
  auto created = effects::NativeEffectRuntime::create(engine, values, rate);
  if (!created) {
    std::fprintf(stderr, "cannot create %s\n", std::string(engine.descriptor->id).c_str());
    std::exit(1);
  }
  auto& runtime = *created.value();
  const auto latency = runtime.latency_frames();
  const std::size_t total = input.size() + latency;
  Render out{{std::vector<double>(input.size()), std::vector<double>(input.size())}, 0.0, latency};
  std::vector<StereoFrame> buffer(block_frames);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t at = 0; at < total; at += block_frames) {
    const std::size_t n = std::min(block_frames, total - at);
    buffer.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = at + i;
      buffer[i] = k < input.size() ? StereoFrame{static_cast<float>(input.left[k]),
                                                 static_cast<float>(input.right[k])}
                                   : StereoFrame{0.0F, 0.0F};
    }
    const double beats_per_frame = loop_bpm / 60.0 / rate;
    const double beat = static_cast<double>(at) * beats_per_frame;
    const dsp::Transport transport{beat, beats_per_frame, beat / beats_per_bar, beats_per_bar};
    (void)runtime.process(buffer, {}, at, 1, transport, {});
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = at + i;
      if (k >= latency && k - latency < input.size()) {
        out.audio.left[k - latency] = buffer[i].left;
        out.audio.right[k - latency] = buffer[i].right;
      }
    }
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  out.latency = latency;
  return out;
}

// The classic sample-peak limiter (the one Ceiling replaced, ADR-0047),
// for the baseline: `--classic` measures it with Ceiling's former presets.
Render render_classic(dsp::LimiterSettings settings, const Audio& input) {
  auto prepared = dsp::prepare_limiter(settings, rate_hz);
  if (!prepared) {
    std::fprintf(stderr, "cannot prepare the classic limiter\n");
    std::exit(1);
  }
  dsp::LimiterRuntime limiter(prepared.value());
  const auto latency = limiter.latency_frames();
  const std::size_t total = input.size() + latency;
  Render out{{std::vector<double>(input.size()), std::vector<double>(input.size())}, 0.0, latency};
  std::vector<StereoFrame> buffer(block_frames);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t at = 0; at < total; at += block_frames) {
    const std::size_t n = std::min(block_frames, total - at);
    buffer.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = at + i;
      buffer[i] = k < input.size() ? StereoFrame{static_cast<float>(input.left[k]),
                                                 static_cast<float>(input.right[k])}
                                   : StereoFrame{0.0F, 0.0F};
    }
    limiter.process(buffer, oscillate::engine::BlockContext{0, 1});
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t k = at + i;
      if (k >= latency && k - latency < input.size()) {
        out.audio.left[k - latency] = buffer[i].left;
        out.audio.right[k - latency] = buffer[i].right;
      }
    }
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return out;
}

struct ClassicPreset {
  std::string_view name;
  double lookahead_ms = 0.0;
  double release_ms = 0.0;
};
// Ceiling's presets before ADR-0117, at the audit's ceiling.
constexpr std::array classic_presets{
    ClassicPreset{"Default", 5.0, 100.0}, ClassicPreset{"Transparent", 5.0, 250.0},
    ClassicPreset{"Loud", 2.0, 40.0}, ClassicPreset{"Safety", 1.0, 50.0},
    ClassicPreset{"Broadcast", 8.0, 400.0}};

// One configuration to measure: renders a programme, aligned.
struct Device {
  std::string name;
  std::function<Render(const Audio&)> render;
  // A device that lets true peaks over the ceiling is trimmed after it until
  // they sit at the ceiling, as it would have to be set to deliver one: the
  // comparison is at equal loudness under an equal true-peak ceiling. The
  // overshoot is still reported.
  bool trim_overs = false;
};

// The drive that brings the output to `target` LUFS, and the true peak
// the device itself let through (before any trim), over the ceiling.
struct Driven {
  double gain_db = 0.0;
  double lufs = 0.0;
  Render render;
  double overshoot_db = 0.0;
  double trim_db = 0.0;
};

Driven drive_to(const Device& device, const Audio& input, double target) {
  double low = gain_low_db;
  double high = gain_high_db;
  Driven best;
  double best_error = HUGE_VAL;
  for (int step = 0; step < gain_search_steps; ++step) {
    const double gain = 0.5 * (low + high);
    Audio driven = input;
    scale(driven, from_db(gain));
    auto out = device.render(driven);
    const double overshoot = true_peak_4x(out.audio) - ceiling_db;
    const double trim = device.trim_overs ? std::max(0.0, overshoot) : 0.0;
    scale(out.audio, from_db(-trim));
    const double level = lufs(out.audio);
    if (std::fabs(level - target) < best_error) {
      best_error = std::fabs(level - target);
      best = {gain, level, std::move(out), overshoot, trim};
    }
    if (best_error < lufs_tolerance) {
      break;
    }
    (level < target ? low : high) = gain;
  }
  return best;
}

// ---- Band filters --------------------------------------------------------------------------

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;
  double process(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Biquad butterworth(double hz, bool high) {
  const double w = 2.0 * pi * hz / rate;
  const double alpha = std::sin(w) / (2.0 * butterworth_q);
  const double c = std::cos(w);
  const double a0 = 1.0 + alpha;
  Biquad f;
  if (high) {
    f.b0 = (1.0 + c) / (2.0 * a0);
    f.b1 = -(1.0 + c) / a0;
  } else {
    f.b0 = (1.0 - c) / (2.0 * a0);
    f.b1 = (1.0 - c) / a0;
  }
  f.b2 = f.b0;
  f.a1 = -2.0 * c / a0;
  f.a2 = (1.0 - alpha) / a0;
  return f;
}

// The mono sum through a fourth-order low or high pass (two Butterworth
// sections).
std::vector<double> band(const Audio& audio, double hz, bool high) {
  std::array<Biquad, 2> f{butterworth(hz, high), butterworth(hz, high)};
  std::vector<double> out(audio.size());
  for (std::size_t i = 0; i < audio.size(); ++i) {
    out[i] = f[1].process(f[0].process(0.5 * (audio.left[i] + audio.right[i])));
  }
  return out;
}

std::array<std::vector<double>, 3> bands(const Audio& audio) {
  auto low = band(audio, low_split_hz, false);
  auto above = band(audio, low_split_hz, true);
  Audio mono{above, above};
  auto mid = band(mono, high_split_hz, false);
  auto high = band(mono, high_split_hz, true);
  return {std::move(low), std::move(mid), std::move(high)};
}

// ---- Transient loss ------------------------------------------------------------------------

// The mean short-term crest factor (16x true peak over RMS, dB) of the
// windows above the floor. The true peak, not the sample peak: a limiter
// that lets intersample peaks through would otherwise read as keeping
// transients it only failed to catch.
double crest_db(const Audio& audio, const std::vector<double>& peaks) {
  const auto window = static_cast<std::size_t>(crest_window_seconds * rate);
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t at = 0; at + window <= audio.size(); at += window) {
    double peak = 0.0;
    double power = 0.0;
    for (std::size_t i = at; i < at + window; ++i) {
      peak = std::max(peak, peaks[i]);
      power += 0.5 * (audio.left[i] * audio.left[i] + audio.right[i] * audio.right[i]);
    }
    const double rms = std::sqrt(power / static_cast<double>(window));
    if (to_db(rms) > crest_floor_db) {
      sum += to_db(peak) - to_db(rms);
      ++count;
    }
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

std::vector<double> hop_energy(const std::vector<double>& x) {
  const auto hop = static_cast<std::size_t>(onset_hop_seconds * rate);
  std::vector<double> out(x.size() / hop);
  for (std::size_t h = 0; h < out.size(); ++h) {
    double sum = 0.0;
    for (std::size_t i = h * hop; i < (h + 1) * hop; ++i) {
      sum += x[i] * x[i];
    }
    out[h] = sum / static_cast<double>(hop);
  }
  return out;
}

// Onsets in the input band: a hop 6 dB over the mean of the ten before it,
// within 45 dB of the band's loudest hop, at least 100 ms apart.
std::vector<std::size_t> onsets(const std::vector<double>& energy) {
  std::vector<std::size_t> out;
  const double loudest = *std::max_element(energy.begin(), energy.end());
  const auto spacing = static_cast<std::size_t>(onset_spacing_seconds / onset_hop_seconds);
  for (std::size_t h = onset_history_hops; h + onset_body_last_hop < energy.size(); ++h) {
    double before = 0.0;
    for (std::size_t k = h - onset_history_hops; k < h; ++k) {
      before += energy[k];
    }
    before /= static_cast<double>(onset_history_hops);
    if (power_db(energy[h]) - power_db(before) > onset_rise_db &&
        power_db(energy[h]) - power_db(loudest) > onset_floor_db &&
        (out.empty() || h - out.back() >= spacing)) {
      out.push_back(h);
    }
  }
  return out;
}

// An onset's attack (its first 15 ms) over its body (30-150 ms on), in dB.
double contrast_db(const std::vector<double>& energy, std::size_t at) {
  double attack = 0.0;
  for (std::size_t k = 0; k < onset_attack_hops; ++k) {
    attack += energy[at + k];
  }
  double body = 0.0;
  for (std::size_t k = onset_body_first_hop; k < onset_body_last_hop; ++k) {
    body += energy[at + k];
  }
  return power_db(attack / static_cast<double>(onset_attack_hops)) -
         power_db(body / static_cast<double>(onset_body_last_hop - onset_body_first_hop));
}

// Per band: the mean loss of onset contrast, input to output, in dB.
std::array<double, 3> onset_loss(const Audio& input, const Audio& output) {
  const auto in = bands(input);
  const auto out = bands(output);
  std::array<double, 3> loss{};
  for (std::size_t b = 0; b < 3; ++b) {
    const auto ein = hop_energy(in[b]);
    const auto eout = hop_energy(out[b]);
    const auto found = onsets(ein);
    double sum = 0.0;
    for (const auto at : found) {
      sum += contrast_db(ein, at) - contrast_db(eout, at);
    }
    loss[b] = found.empty() ? 0.0 : sum / static_cast<double>(found.size());
  }
  return loss;
}

// The RMS (dB) of the top band's gain trajectory once its one-second trend
// is removed: how much the limiter moves what it should leave alone.
double pumping_db(const Audio& input, const Audio& output) {
  const auto in = hop_energy(band(input, pump_band_hz, true));
  const auto out = hop_energy(band(output, pump_band_hz, true));
  const auto frame = static_cast<std::size_t>(pump_frame_seconds / onset_hop_seconds);
  std::vector<double> gain;
  std::vector<bool> valid;
  const double loudest = *std::max_element(in.begin(), in.end());
  for (std::size_t h = 0; h + frame <= in.size(); h += frame) {
    double ein = 0.0;
    double eout = 0.0;
    for (std::size_t k = h; k < h + frame; ++k) {
      ein += in[k];
      eout += out[k];
    }
    gain.push_back(power_db(eout) - power_db(ein));
    valid.push_back(power_db(ein / static_cast<double>(frame)) - power_db(loudest) > pump_floor_db);
  }
  const auto half = static_cast<std::size_t>(pump_trend_seconds / pump_frame_seconds / 2.0);
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = half; i + half < gain.size(); ++i) {
    if (!valid[i]) {
      continue;
    }
    double trend = 0.0;
    std::size_t n = 0;
    for (std::size_t k = i - half; k <= i + half; ++k) {
      if (valid[k]) {
        trend += gain[k];
        ++n;
      }
    }
    const double d = gain[i] - trend / static_cast<double>(n);
    sum += d * d;
    ++count;
  }
  return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

// ---- Tones ---------------------------------------------------------------------------------

Audio tones(std::span<const double> hz, std::span<const double> weight, double peak) {
  const auto frames = static_cast<std::size_t>(tone_seconds * rate);
  double total = 0.0;
  for (const double w : weight) {
    total += w;
  }
  Audio out{std::vector<double>(frames), std::vector<double>(frames)};
  for (std::size_t i = 0; i < frames; ++i) {
    double x = 0.0;
    for (std::size_t k = 0; k < hz.size(); ++k) {
      x += weight[k] / total * std::sin(2.0 * pi * hz[k] * static_cast<double>(i) / rate);
    }
    out.left[i] = peak * x;
    out.right[i] = peak * x;
  }
  return out;
}

std::span<const double> settled(const Audio& audio) {
  const auto from = static_cast<std::size_t>(tone_settle_seconds * rate);
  return std::span<const double>(audio.left).subspan(from, static_cast<std::size_t>(rate));
}

// THD+N: the residual once a least-squares sine at `hz` (and DC) is taken
// out, over the fitted sine, in dB.
double thd_n_db(std::span<const double> x, double hz) {
  // Normal equations for a sin + b cos + c.
  std::array<std::array<double, 4>, 3> m{};
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double w = 2.0 * pi * hz * static_cast<double>(i) / rate;
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
    const double w = 2.0 * pi * hz * static_cast<double>(i) / rate;
    const double y = s[0] * std::sin(w) + s[1] * std::cos(w);
    fitted += y * y;
    residual += (x[i] - y - s[2]) * (x[i] - y - s[2]);
  }
  return power_db(residual) - power_db(fitted);
}

// The amplitude at `hz` under a four-term Blackman-Harris window.
double amplitude(std::span<const double> x, double hz) {
  constexpr std::array<double, 4> bh{0.35875, 0.48829, 0.14128, 0.01168};
  std::complex<double> sum{};
  const double n = static_cast<double>(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double t = 2.0 * pi * static_cast<double>(i) / n;
    const double w =
        bh[0] - bh[1] * std::cos(t) + bh[2] * std::cos(2.0 * t) - bh[3] * std::cos(3.0 * t);
    sum += w * x[i] * std::polar(1.0, -2.0 * pi * hz * static_cast<double>(i) / rate);
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
  return to_db(std::sqrt(sidebands) / amplitude(x, smpte_high_hz));
}

double ccif_db(std::span<const double> x) {
  const double d2 = amplitude(x, ccif_second_hz - ccif_first_hz);
  const double d3a = amplitude(x, 2.0 * ccif_first_hz - ccif_second_hz);
  const double d3b = amplitude(x, 2.0 * ccif_second_hz - ccif_first_hz);
  return to_db(std::sqrt(d2 * d2 + d3a * d3a + d3b * d3b) / amplitude(x, ccif_first_hz));
}

// ---- The audit -----------------------------------------------------------------------------

struct Row {
  double lufs = 0.0;
  double gain_db = 0.0;
  double tp4_over = -HUGE_VAL;
  double tp16_over = -HUGE_VAL;
  double crest_loss = 0.0;
  std::array<double, 3> onset{};
  double pump = 0.0;
  double thd_low = 0.0;
  double thd_high = 0.0;
  double smpte = 0.0;
  double ccif = 0.0;
  double cpu = 0.0;
  double latency_ms = 0.0;
};

void print_header() {
  std::printf("| Preset | Target | LUFS | Drive dB | TP4 over | TP16 over | Crest loss | "
              "Onset loss L/M/H | Pump | THD+N 50 Hz | THD+N 1 kHz | SMPTE | CCIF | CPU | "
              "Latency |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
}

void print_row(std::string_view preset, double target, const Row& r) {
  std::printf("| %s | %.0f | %.2f | %.1f | %+.2f | %+.2f | %.2f | %.2f/%.2f/%.2f | %.2f | %.1f | "
              "%.1f | %.1f | %.1f | %.2f%% | %.2f ms |\n",
              std::string(preset).c_str(), target, r.lufs, r.gain_db, r.tp4_over, r.tp16_over,
              r.crest_loss, r.onset[0], r.onset[1], r.onset[2], r.pump, r.thd_low, r.thd_high,
              r.smpte, r.ccif, 100.0 * r.cpu, r.latency_ms);
}

} // namespace

int main(int argc, char** argv) {
  std::string engine_id = "ceiling";
  std::vector<std::string> only;
  std::vector<std::pair<std::string, double>> overrides;
  std::vector<Programme> programmes;
  bool loop = true;
  bool per_programme = false;
  bool classic = false;
  std::optional<double> only_target;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--engine" && i + 1 < argc) {
      engine_id = argv[++i];
    } else if (arg == "--preset" && i + 1 < argc) {
      only.emplace_back(argv[++i]);
    } else if (arg == "--set" && i + 1 < argc) {
      const std::string assignment = argv[++i];
      const auto equals = assignment.find('=');
      if (equals == std::string::npos) {
        std::fprintf(stderr, "--set takes ID=VALUE\n");
        return 1;
      }
      overrides.emplace_back(assignment.substr(0, equals),
                             std::stod(assignment.substr(equals + 1)));
    } else if (arg == "--target" && i + 1 < argc) {
      only_target = std::stod(argv[++i]);
    } else if (arg == "--no-loop") {
      loop = false;
    } else if (arg == "--classic") {
      classic = true;
    } else if (arg == "--per-programme") {
      per_programme = true;
    } else {
      auto audio = read_f32(std::string(arg));
      if (!audio || audio->size() == 0) {
        std::fprintf(stderr, "cannot read %s\n", argv[i]);
        return 1;
      }
      programmes.push_back({std::string(arg), std::move(*audio)});
    }
  }
  if (loop) {
    programmes.insert(programmes.begin(), {"loop", drum_loop()});
  }
  if (programmes.empty()) {
    std::fprintf(stderr, "nothing to measure\n");
    return 1;
  }
  for (auto& programme : programmes) {
    scale(programme.audio, from_db(programme_lufs - lufs(programme.audio)));
  }
  std::vector<Device> devices;
  if (classic) {
    for (const auto& entry : classic_presets) {
      const dsp::LimiterSettings settings{ceiling_db, entry.lookahead_ms, entry.release_ms};
      devices.push_back({std::string(entry.name),
                         [settings](const Audio& input) { return render_classic(settings, input); },
                         true});
    }
  } else {
    const auto* engine = effects::find_engine(engine_id);
    if (engine == nullptr) {
      std::fprintf(stderr, "no engine %s\n", engine_id.c_str());
      return 1;
    }
    const auto& table = *engine->descriptor;
    const auto gain = dsp::find_parameter(table, engine_id + ".gain");
    const auto ceiling = dsp::find_parameter(table, engine_id + ".ceiling");
    if (!gain || !ceiling) {
      std::fprintf(stderr, "%s has no gain or ceiling\n", engine_id.c_str());
      return 1;
    }
    for (const auto& preset : table.presets) {
      auto values = dsp::preset_values(table, preset);
      if (!values) {
        continue;
      }
      // The audit drives the programme itself; the engine's own gain stays
      // at 0 dB.
      auto v = std::move(values).value();
      v[*gain] = 0.0;
      v[*ceiling] = ceiling_db;
      for (const auto& [id, value] : overrides) {
        const auto index = dsp::find_parameter(table, id);
        if (!index) {
          std::fprintf(stderr, "no parameter %s\n", id.c_str());
          return 1;
        }
        v[*index] = value;
      }
      devices.push_back({std::string(preset.name),
                         [engine, v](const Audio& input) { return render(*engine, v, input); }});
    }
  }
  // The standard targets, or one given (its tones driven as the middle
  // target's are).
  struct Target {
    double lufs = 0.0;
    double tone_over_db = 0.0;
  };
  std::vector<Target> targets;
  if (only_target) {
    targets.push_back({*only_target, tone_over_db[1]});
  } else {
    for (std::size_t t = 0; t < targets_lufs.size(); ++t) {
      targets.push_back({targets_lufs[t], tone_over_db[t]});
    }
  }
  print_header();
  for (const auto& device : devices) {
    if (!only.empty() && std::ranges::find(only, device.name) == only.end()) {
      continue;
    }
    for (std::size_t t = 0; t < targets.size(); ++t) {
      const double target = targets[t].lufs;
      Row mean;
      double cpu_seconds = 0.0;
      double audio_seconds = 0.0;
      for (const auto& programme : programmes) {
        auto driven = drive_to(device, programme.audio, target);
        Row r;
        r.lufs = driven.lufs;
        r.gain_db = driven.gain_db;
        r.tp4_over = driven.overshoot_db;
        const auto out_peaks = fine_peaks(driven.render.audio);
        r.tp16_over = true_peak_16x(out_peaks) - ceiling_db + driven.trim_db;
        r.crest_loss = crest_db(programme.audio, fine_peaks(programme.audio)) -
                       crest_db(driven.render.audio, out_peaks);
        r.onset = onset_loss(programme.audio, driven.render.audio);
        r.pump = pumping_db(programme.audio, driven.render.audio);
        r.latency_ms = milliseconds_per_second * driven.render.latency / rate;
        cpu_seconds += driven.render.seconds;
        audio_seconds += static_cast<double>(programme.audio.size()) / rate;
        if (per_programme) {
          print_row(device.name + " · " + programme.name, target, r);
        }
        const double n = static_cast<double>(programmes.size());
        mean.lufs += r.lufs / n;
        mean.gain_db += r.gain_db / n;
        mean.tp4_over = std::max(mean.tp4_over, r.tp4_over);
        mean.tp16_over = std::max(mean.tp16_over, r.tp16_over);
        mean.crest_loss += r.crest_loss / n;
        for (std::size_t b = 0; b < 3; ++b) {
          mean.onset[b] += r.onset[b] / n;
        }
        mean.pump += r.pump / n;
        mean.latency_ms = r.latency_ms;
      }
      // The tones peak tone_over_db over the ceiling, undriven.
      const double peak = from_db(ceiling_db + targets[t].tone_over_db);
      const std::array one{1.0};
      const auto run = [&](std::span<const double> hz, std::span<const double> weight) {
        return device.render(tones(hz, weight, peak)).audio;
      };
      const std::array low{thd_low_hz};
      const std::array high{thd_high_hz};
      mean.thd_low = thd_n_db(settled(run(low, one)), thd_low_hz);
      mean.thd_high = thd_n_db(settled(run(high, one)), thd_high_hz);
      const std::array smpte_hz{smpte_low_hz, smpte_high_hz};
      const std::array smpte_weight{smpte_ratio, 1.0};
      mean.smpte = smpte_db(settled(run(smpte_hz, smpte_weight)));
      const std::array ccif_hz{ccif_first_hz, ccif_second_hz};
      const std::array ccif_weight{1.0, 1.0};
      mean.ccif = ccif_db(settled(run(ccif_hz, ccif_weight)));
      mean.cpu = cpu_seconds / audio_seconds;
      print_row(device.name, target, mean);
      std::fflush(stdout);
    }
  }
  return 0;
}
