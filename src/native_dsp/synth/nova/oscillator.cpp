#include "oscillator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <emmintrin.h>

namespace oscillate::native_dsp::synth::nova {
namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;

[[nodiscard]] Waveform blep_waveform(ClassicShape shape) noexcept {
  switch (shape) {
  case ClassicShape::sine:
    return Waveform::sine;
  case ClassicShape::triangle:
    return Waveform::triangle;
  case ClassicShape::saw:
    return Waveform::saw;
  case ClassicShape::square:
  case ClassicShape::pulse:
    return Waveform::pulse;
  }
  return Waveform::saw;
}

// The Random curve's draws (ADR-0073): fixed per stack size and voice, so
// every note spreads the same way.
constexpr std::uint64_t random_curve_salt = 0x5EED'C0DE'0F0D'E7B1ULL;

// Unison position -1..1 of voice `i` of `count`, shaped by the detune curve.
// Super is a supersaw-like spread: dense near the centre, wide outside.
// Inverse mirrors Exponential: inner voices move out, the outer ones less
// than they would. Random keeps each voice on its side of the centre at a
// seeded distance, so a stack still spreads both ways.
[[nodiscard]] double unison_offset(std::size_t i, std::uint32_t count, UnisonCurve curve) noexcept {
  if (count <= 1)
    return 0.0;
  const double x = -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(count - 1);
  const double a = std::fabs(x);
  switch (curve) {
  case UnisonCurve::linear:
    return x;
  case UnisonCurve::exponential:
    return std::copysign(a * a, x);
  case UnisonCurve::super:
    return std::copysign(0.15 * a + 0.85 * std::pow(a, 1.6), x);
  case UnisonCurve::inverse:
    return std::copysign(std::sqrt(a), x);
  case UnisonCurve::random: {
    if (x == 0.0)
      return 0.0;
    const std::uint64_t key = (static_cast<std::uint64_t>(count) << 8U) | i;
    return std::copysign(hash_unit(key ^ random_curve_salt), x);
  }
  }
  return x;
}

// Semitones a stack adds to voice `i` (the first voice is never moved).
[[nodiscard]] double stack_semitones(std::size_t i, UnisonStack stack) noexcept {
  static constexpr std::array<double, 2> octave{0.0, 12.0};
  static constexpr std::array<double, 3> two_octaves{0.0, 12.0, 24.0};
  static constexpr std::array<double, 2> fifth{0.0, 7.0};
  static constexpr std::array<double, 3> fifth_octave{0.0, 7.0, 12.0};
  static constexpr std::array<double, 4> chord{0.0, 4.0, 7.0, 12.0};
  switch (stack) {
  case UnisonStack::off:
    return 0.0;
  case UnisonStack::octave:
    return octave[i % octave.size()];
  case UnisonStack::two_octaves:
    return two_octaves[i % two_octaves.size()];
  case UnisonStack::fifth:
    return fifth[i % fifth.size()];
  case UnisonStack::fifth_octave:
    return fifth_octave[i % fifth_octave.size()];
  case UnisonStack::chord:
    return chord[i % chord.size()];
  }
  return 0.0;
}

// How much faster than its pitch a warp can make the read run, so the band
// limit leaves room for it (cf. wavetable_oscillator.hpp's warp_speed).
[[nodiscard]] double warp_speed(WarpMode mode, double amount) noexcept {
  switch (mode) {
  case WarpMode::bend_plus:
  case WarpMode::bend_minus:
    return 1.0 + 0.9 * amount;
  case WarpMode::sync:
  case WarpMode::window_sync:
    return 1.0 + 7.0 * amount;
  case WarpMode::mirror:
    return amount > 0.0 ? std::numbers::pi : 1.0;
  case WarpMode::asym:
    return 1.0 + 0.3 * std::numbers::pi * amount;
  case WarpMode::pwm:
    return 1.0 / (1.0 - 0.95 * amount);
  case WarpMode::remap:
    return 1.0 + 3.0 * amount;
  case WarpMode::fm:
    return 1.0 + 8.0 * amount;
  case WarpMode::pm:
    return 1.0 + 6.0 * amount;
  default:
    return 1.0;
  }
}

[[nodiscard]] double wrap(double x) noexcept { return x - fast_floor(x); }

// Two unison voices in one SSE2 register. Each lane performs exactly the
// scalar operation (SSE2 double arithmetic, no contraction), so a paired
// voice computes the same bits as on its own.
struct Pair {
  __m128d v;
  Pair(__m128d value) noexcept : v(value) {}
  Pair(double a, double b) noexcept : v(_mm_set_pd(b, a)) {}
  explicit Pair(double both) noexcept : v(_mm_set1_pd(both)) {}
  [[nodiscard]] double a() const noexcept { return _mm_cvtsd_f64(v); }
  [[nodiscard]] double b() const noexcept { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }
};
[[nodiscard]] Pair operator+(Pair x, Pair y) noexcept { return _mm_add_pd(x.v, y.v); }
[[nodiscard]] Pair operator-(Pair x, Pair y) noexcept { return _mm_sub_pd(x.v, y.v); }
[[nodiscard]] Pair operator*(Pair x, Pair y) noexcept { return _mm_mul_pd(x.v, y.v); }
[[nodiscard]] Pair operator*(double x, Pair y) noexcept { return _mm_mul_pd(_mm_set1_pd(x), y.v); }

// wrap() per lane, for |x| < 2^31: fast_floor truncates through int64, and
// any such x truncates the same through int32.
[[nodiscard]] Pair wrap(Pair x) noexcept {
  const __m128d truncated = _mm_cvtepi32_pd(_mm_cvttpd_epi32(x.v));
  const __m128d below = _mm_and_pd(_mm_cmplt_pd(x.v, truncated), _mm_set1_pd(1.0));
  return _mm_sub_pd(x.v, _mm_sub_pd(truncated, below));
}

// One read's four taps, a frame's and the next frame's blended by `mix`:
// tap k is a[k] + mix * (b[k] - a[k]), the difference taken in float as
// the scalar read does. Returns taps 0, 1 and taps 2, 3.
void blend_taps(const float* a, const float* b, double mix, __m128d& low, __m128d& high) noexcept {
  const __m128 fa = _mm_loadu_ps(a);
  const __m128 difference = _mm_sub_ps(_mm_loadu_ps(b), fa);
  const __m128d m = _mm_set1_pd(mix);
  low = _mm_add_pd(_mm_cvtps_pd(fa), _mm_mul_pd(m, _mm_cvtps_pd(difference)));
  high = _mm_add_pd(_mm_cvtps_pd(_mm_movehl_ps(fa, fa)),
                    _mm_mul_pd(m, _mm_cvtps_pd(_mm_movehl_ps(difference, difference))));
}

// Equal-power gains for a pan of -1..1, unity in the centre.
void pan_gains(double pan, double gain, double& left, double& right) noexcept {
  // Silent (a unison slot beyond the count): both gains are +0, which is
  // what the products below give, as cos and sin are non-negative here.
  if (gain == 0.0) {
    left = right = 0.0;
    return;
  }
  const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * 0.25 * std::numbers::pi;
  left = gain * std::numbers::sqrt2 * std::cos(angle);
  right = gain * std::numbers::sqrt2 * std::sin(angle);
}

} // namespace

void Oscillator::start(const OscSettings& settings, double note_hz, const Wavetable* table,
                       double phase_cycles, double random_phase, std::uint64_t seed,
                       const UnisonAnalogue* analogue) noexcept {
  settings_ = settings;
  table_ = table;
  for (std::size_t i = 0; i < max_unison; ++i) {
    auto& voice = voices_[i];
    voice.seed = seed * 0x9E3779B97F4A7C15ULL + i;
    if (analogue == nullptr)
      voice.phase = wrap(phase_cycles + random_phase * hash_unit(voice.seed));
    else if (analogue->free_running)
      voice.phase = wrap(analogue->phase[i]);
    else
      voice.phase = wrap(phase_cycles + random_phase * hash_unit(voice.seed) + analogue->phase[i]);
    voice.master = voice.phase;
    bleps_[i].reset(voice.phase, blep_waveform(settings.shape),
                    settings.shape == ClassicShape::pulse ? settings.pw : 0.5);
    voice.presence = i < settings.voices ? 1.0 : 0.0;
    voice.drift = 0.0;
    voice.drift_target = 0.0;
    voice.drift_rate = 0.0;
    voice.increment = {};
    voice.gain_left = {};
    voice.gain_right = {};
    voice.position = {};
    voice.warp0 = {};
    voice.warp1 = {};
  }
  level_ramp_ = {};
  level_fade_ = 0;
  peak_ = 0.0F;
  last_mono_ = 0.0;
  dc_in_ = {};
  dc_out_ = {};
  set(settings, note_hz, table, remap_, 0, 0.0, analogue);
  if (table_ != nullptr)
    previous_level_ = level_;
}

void Oscillator::set(const OscSettings& settings, double note_hz, const Wavetable* table,
                     const std::array<const CurveShape*, warp_slots>& remap, std::uint32_t frames,
                     double tick_seconds, const UnisonAnalogue* analogue) noexcept {
  const bool shape_changed = settings.shape != settings_.shape;
  settings_ = settings;
  remap_ = remap;
  const Wavetable* readable = table;
  if (readable != nullptr && settings.interpolation == FrameInterpolation::spectral &&
      readable->spectral() != nullptr)
    readable = readable->spectral();
  table_ = readable;
  level_ramp_.to(settings.on ? settings.level : 0.0, frames);

  // Unison voice weights: blend 0 favours the centre, 1 makes all equal;
  // the sum of squares is one, so loudness holds as the count changes.
  std::array<double, max_unison> weight{};
  double power = 0.0;
  const std::uint32_t count = std::clamp<std::uint32_t>(settings.voices, 1, max_unison);
  for (std::size_t i = 0; i < count; ++i) {
    const double x = std::fabs(unison_offset(i, count, UnisonCurve::linear));
    weight[i] = 1.0 - (1.0 - settings.blend) * 0.9 * x;
    power += weight[i] * weight[i];
  }
  const double normalise = power > 0.0 ? 1.0 / std::sqrt(power) : 0.0;

  // The detune curve's offsets depend on the count and curve alone.
  if (count != offsets_count_ || settings.curve != offsets_curve_) {
    offsets_count_ = count;
    offsets_curve_ = settings.curve;
    for (std::size_t i = 0; i < max_unison; ++i)
      offsets_[i] = unison_offset(i, count, settings.curve);
  }
  double fastest = 0.0;
  for (std::size_t i = 0; i < max_unison; ++i) {
    auto& voice = voices_[i];
    const bool active = i < count;
    // Presence moves 5 ms per second of ticks, so count changes never click.
    const double step = tick_seconds > 0.0 ? tick_seconds / 0.005 : 1.0;
    voice.presence =
        active ? std::min(1.0, voice.presence + step) : std::max(0.0, voice.presence - step);
    // Drift: a slow, seeded random walk of up to 8 cents.
    if (tick_seconds > 0.0) {
      voice.drift_rate -= tick_seconds;
      if (voice.drift_rate <= 0.0) {
        voice.seed = voice.seed * 6364136223846793005ULL + 1442695040888963407ULL;
        voice.drift_target = 2.0 * hash_unit(voice.seed) - 1.0;
        voice.drift_rate = 0.4 + 1.2 * hash_unit(voice.seed ^ 0xABCDEFULL);
      }
      voice.drift += (voice.drift_target - voice.drift) * std::min(1.0, tick_seconds * 2.0);
    }
    const double x = offsets_[i];
    const double drift_amount = settings.type == OscType::classic
                                    ? std::max(settings.drift, settings.global_drift)
                                    : settings.global_drift;
    double cents = x * settings.detune_cents + voice.drift * 8.0 * drift_amount;
    double pan = settings.pan + x * settings.width;
    double gain = (active ? weight[i] * normalise : 0.0) * voice.presence;
    double position = settings.position + 0.5 * x * settings.position_spread;
    double warp0 = settings.warp_amount[0] + 0.5 * x * settings.warp_spread;
    double warp1 = settings.warp_amount[1] + 0.5 * x * settings.warp_spread;
    // Analogue: this unison voice's own tuning, level, place and shape.
    if (analogue != nullptr) {
      cents += analogue->cents[i];
      pan += analogue->pan[i];
      gain *= analogue->gain[i];
      position += analogue->shape[i];
      warp0 += analogue->shape[i];
      warp1 += analogue->shape[i];
    }
    const double semitones =
        settings.semitones + cents / 100.0 + (i == 0 ? 0.0 : stack_semitones(i, settings.stack));
    const double increment = note_hz * std::exp2(semitones / 12.0) / rate_;
    fastest = std::max(fastest, increment);
    voice.increment.to(increment, frames);
    double left = 0.0, right = 0.0;
    pan_gains(std::clamp(pan, -1.0, 1.0), gain, left, right);
    voice.gain_left.to(left, frames);
    voice.gain_right.to(right, frames);
    voice.position.to(std::clamp(position, 0.0, 1.0), frames);
    voice.warp0.to(std::clamp(warp0, 0.0, 1.0), frames);
    voice.warp1.to(std::clamp(warp1, 0.0, 1.0), frames);
    if (shape_changed)
      bleps_[i].set_waveform(blep_waveform(settings.shape),
                             settings.shape == ClassicShape::pulse ? settings.pw : 0.5);
  }

  if (table_ != nullptr) {
    double speed = fastest;
    for (std::size_t w = 0; w < warp_slots; ++w)
      speed *=
          warp_speed(settings.warp_mode[w], settings.warp_amount[w] + 0.5 * settings.warp_spread);
    const std::size_t level = table_->level_for(speed);
    if (frames == 0) {
      level_ = level;
      level_fade_ = 0;
    } else if (level != level_) {
      previous_level_ = level_;
      level_ = level;
      level_fade_length_ = std::max<std::uint32_t>(frames, 1);
      level_fade_ = level_fade_length_;
    }
  }
}

double Oscillator::read_table(double phase, double position, std::uint32_t fade) const noexcept {
  const auto read = [&](std::size_t level) {
    const auto& band = table_->level(level);
    const double frame_position = position * static_cast<double>(table_->frames() - 1);
    const auto first = std::min(static_cast<std::size_t>(frame_position), table_->frames() - 1);
    const std::size_t second = std::min(first + 1, table_->frames() - 1);
    const double mix = frame_position - static_cast<double>(first);
    const double x = phase * band.length;
    const auto index = std::min(static_cast<std::size_t>(x), std::size_t{band.length} - 1);
    const double t = x - static_cast<double>(index);
    const float* a = table_->frame(level, first) + index;
    const float* b = table_->frame(level, second) + index;
    const double y0 = a[-1] + mix * (b[-1] - a[-1]);
    const double y1 = a[0] + mix * (b[0] - a[0]);
    const double y2 = a[1] + mix * (b[1] - a[1]);
    const double y3 = a[2] + mix * (b[2] - a[2]);
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
  };
  const double value = read(level_);
  if (fade == 0)
    return value;
  const double share = static_cast<double>(fade) / level_fade_length_;
  return value + share * (read(previous_level_) - value);
}

bool Oscillator::pairs_apply(std::size_t count) const noexcept {
  if (settings_.type != OscType::wavetable || table_ == nullptr || level_fade_ != 0 ||
      settings_.warp_mode[0] != WarpMode::off || settings_.warp_mode[1] != WarpMode::off)
    return false;
  // Phases stay in [0, 1) and advance by an increment far below 2^31
  // (wrap(Pair)'s range) at either end of its ramp.
  constexpr double limit = 1e6;
  return std::ranges::all_of(voices_, [&](const Voice& voice) {
    const double end = voice.increment.value + static_cast<double>(count) * voice.increment.step;
    return std::fabs(voice.increment.value) < limit && std::fabs(end) < limit;
  });
}

// step() and read_table() for two voices, in their order: per sample each
// lane's arithmetic is the scalar path's, and voice a adds to the sums
// before voice b, as the voice-by-voice loop does. A lane whose gains are
// both zero adds zeros to sums that are never -0, which leaves them as
// they are, as skipping it would.
void Oscillator::render_pair(Voice& a, Voice& b, std::size_t count, double* __restrict sum_left,
                             double* __restrict sum_right,
                             double* __restrict sum_mono) const noexcept {
  const auto& band = table_->level(level_);
  const std::size_t last_frame = table_->frames() - 1;
  const double frame_scale = static_cast<double>(table_->frames() - 1);
  const double length = band.length;
  const std::size_t last_index = std::size_t{band.length} - 1;
  const float* data = band.data.data() + table_guard;
  const std::size_t stride = band.stride;
  const auto pair = [](const Ramp& x, const Ramp& y) {
    return std::array<Pair, 2>{Pair(x.value, y.value), Pair(x.step, y.step)};
  };
  auto [increment, increment_step] = pair(a.increment, b.increment);
  auto [gain_left, gain_left_step] = pair(a.gain_left, b.gain_left);
  auto [gain_right, gain_right_step] = pair(a.gain_right, b.gain_right);
  auto [position, position_step] = pair(a.position, b.position);
  auto [warp0, warp0_step] = pair(a.warp0, b.warp0);
  auto [warp1, warp1_step] = pair(a.warp1, b.warp1);
  Pair phase(a.phase, b.phase);
  // One lane's taps at its phase and position (read_table's read()).
  const auto taps = [&](double phase_lane, double position_lane, __m128d& low, __m128d& high,
                        double& t) {
    const double frame_position = position_lane * frame_scale;
    const auto first = std::min(static_cast<std::size_t>(frame_position), last_frame);
    const std::size_t second = std::min(first + 1, last_frame);
    const double mix = frame_position - static_cast<double>(first);
    const double x = phase_lane * length;
    const auto index = std::min(static_cast<std::size_t>(x), last_index);
    t = x - static_cast<double>(index);
    // Taps -1..2 around the index; the guards hold the wrapped samples.
    const float* at_first = data + first * stride + index;
    const float* at_second = data + second * stride + index;
    blend_taps(at_first - 1, at_second - 1, mix, low, high);
  };
  for (std::size_t i = 0; i < count; ++i) {
    increment = increment + increment_step;
    gain_left = gain_left + gain_left_step;
    gain_right = gain_right + gain_right_step;
    position = position + position_step;
    warp0 = warp0 + warp0_step;
    warp1 = warp1 + warp1_step;
    phase = wrap(phase + increment);
    // read_table(wrap(phase), ...): the phase is in [0, 1), where wrap
    // returns it unchanged.
    __m128d low_a, high_a, low_b, high_b;
    double t_a = 0.0, t_b = 0.0;
    taps(phase.a(), position.a(), low_a, high_a, t_a);
    taps(phase.b(), position.b(), low_b, high_b, t_b);
    const Pair y0 = _mm_unpacklo_pd(low_a, low_b);
    const Pair y1 = _mm_unpackhi_pd(low_a, low_b);
    const Pair y2 = _mm_unpacklo_pd(high_a, high_b);
    const Pair y3 = _mm_unpackhi_pd(high_a, high_b);
    const Pair t(t_a, t_b);
    const Pair c1 = 0.5 * (y2 - y0);
    const Pair c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const Pair c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    const Pair value = ((c3 * t + c2) * t + c1) * t + y1;
    const Pair left = value * gain_left;
    const Pair right = value * gain_right;
    const Pair mono = 0.5 * value * (gain_left + gain_right);
    sum_left[i] = sum_left[i] + left.a() + left.b();
    sum_right[i] = sum_right[i] + right.a() + right.b();
    sum_mono[i] = sum_mono[i] + mono.a() + mono.b();
  }
  const auto store = [](Pair x, Ramp& first, Ramp& second) {
    first.value = x.a();
    second.value = x.b();
  };
  store(increment, a.increment, b.increment);
  store(gain_left, a.gain_left, b.gain_left);
  store(gain_right, a.gain_right, b.gain_right);
  store(position, a.position, b.position);
  store(warp0, a.warp0, b.warp0);
  store(warp1, a.warp1, b.warp1);
  a.phase = phase.a();
  b.phase = phase.b();
}

double Oscillator::warp_phase(double phase, WarpMode mode, double a, double cross,
                              const CurveShape* remap) const noexcept {
  switch (mode) {
  case WarpMode::bend_plus:
    return wrap(phase - 0.9 * a / two_pi * sin_cycle(phase));
  case WarpMode::bend_minus:
    return wrap(phase + 0.9 * a / two_pi * sin_cycle(phase));
  case WarpMode::sync:
  case WarpMode::window_sync:
    return wrap(phase * (1.0 + 7.0 * a));
  case WarpMode::mirror: {
    const double mirrored = 0.5 - 0.5 * sin_cycle(phase + 0.25);
    return phase + a * (mirrored - phase);
  }
  case WarpMode::asym: {
    const double s = sin_cycle(0.5 * phase);
    return wrap(phase + 0.3 * a * s * s);
  }
  case WarpMode::pwm: {
    const double width = 1.0 - 0.95 * a;
    return phase < width ? phase / width : 0.0;
  }
  case WarpMode::remap:
    return remap == nullptr ? phase : phase + a * (std::clamp(remap->at(phase), 0.0, 1.0) - phase);
  case WarpMode::pm:
    return wrap(phase + 2.0 * a * cross);
  default:
    return phase;
  }
}

double Oscillator::warp_value(double value, WarpMode mode, double a, double cross) noexcept {
  switch (mode) {
  case WarpMode::quantise: {
    const double steps = std::exp2(8.0 - 7.0 * a);
    return std::round(value * steps) / steps;
  }
  case WarpMode::flip:
    return value; // Applied with the phase in process(), which knows it.
  case WarpMode::fold: {
    // A triangle fold: smooth, symmetric, and gentle at small amounts.
    const double x = value * (1.0 + 7.0 * a);
    const double folded = std::fabs(std::fmod(std::fabs(x) + 1.0, 4.0) - 2.0) - 1.0;
    return value + std::min(1.0, 2.0 * a) * (std::copysign(1.0, x) * -folded - value) * 1.0;
  }
  case WarpMode::am:
    return value * (1.0 - a + a * (0.5 + 0.5 * cross));
  case WarpMode::rm:
    return value * (1.0 - a + a * cross);
  default:
    return value;
  }
}

[[gnu::always_inline]] inline bool Oscillator::step(Voice& voice, ClassicBlep& blep, double cross0,
                                                    double cross1, std::uint32_t fade,
                                                    double& value, double& gain_left,
                                                    double& gain_right) const noexcept {
  double increment = voice.increment.next();
  gain_left = voice.gain_left.next();
  gain_right = voice.gain_right.next();
  const double position = voice.position.next();
  const double a0 = voice.warp0.next();
  const double a1 = voice.warp1.next();
  // Linear FM: the source moves the frequency, through zero for tables.
  if (settings_.warp_mode[0] == WarpMode::fm)
    increment *= 1.0 + 8.0 * a0 * cross0;
  if (settings_.warp_mode[1] == WarpMode::fm)
    increment *= 1.0 + 8.0 * a1 * cross1;
  if (gain_left == 0.0 && gain_right == 0.0) {
    voice.phase = wrap(voice.phase + increment);
    return false;
  }
  if (settings_.type == OscType::classic) {
    SyncPoint sync{};
    double slave = increment;
    if (settings_.sync > 1.0001) {
      voice.master += increment;
      if (voice.master >= 1.0) {
        voice.master -= std::floor(voice.master);
        sync = {true, increment > 0.0 ? voice.master / increment : 0.0};
      }
      slave = increment * settings_.sync;
    }
    const double width = settings_.shape == ClassicShape::pulse ? settings_.pw : 0.5;
    value = blep.next(std::max(0.0, slave), width, sync, nullptr);
    voice.phase = blep.phase();
  } else if (settings_.type == OscType::wavetable && table_ != nullptr) {
    voice.phase = wrap(voice.phase + increment);
    // A warp that is off returns its input: skip the call.
    double phase = voice.phase;
    if (settings_.warp_mode[0] != WarpMode::off)
      phase = warp_phase(phase, settings_.warp_mode[0], a0, cross0, remap_[0]);
    if (settings_.warp_mode[1] != WarpMode::off)
      phase = warp_phase(phase, settings_.warp_mode[1], a1, cross1, remap_[1]);
    value = read_table(wrap(phase), position, fade);
    if (settings_.warp_mode[0] == WarpMode::window_sync ||
        settings_.warp_mode[1] == WarpMode::window_sync) {
      const double a = settings_.warp_mode[0] == WarpMode::window_sync ? a0 : a1;
      value *= 1.0 - std::min(1.0, 2.0 * a) * (1.0 - std::sin(std::numbers::pi * voice.phase));
    }
    for (std::size_t w = 0; w < warp_slots; ++w) {
      if (settings_.warp_mode[w] == WarpMode::flip && voice.phase > 1.0 - (w == 0 ? a0 : a1))
        value = -value;
    }
  } else {
    voice.phase = wrap(voice.phase + increment);
    return false;
  }
  if (settings_.warp_mode[0] != WarpMode::off)
    value = warp_value(value, settings_.warp_mode[0], a0, cross0);
  if (settings_.warp_mode[1] != WarpMode::off)
    value = warp_value(value, settings_.warp_mode[1], a1, cross1);
  return true;
}

float Oscillator::process(const CrossInputs& cross, double& left, double& right) noexcept {
  const double level = level_ramp_.next();
  const auto source = [&](std::size_t w) {
    return static_cast<double>(cross.value[static_cast<std::size_t>(settings_.warp_source[w])]);
  };
  const double cross0 = source(0);
  const double cross1 = source(1);
  double sum_left = 0.0, sum_right = 0.0, mono = 0.0;
  for (std::size_t v = 0; v < max_unison; ++v) {
    double value = 0.0, gain_left = 0.0, gain_right = 0.0;
    if (!step(voices_[v], bleps_[v], cross0, cross1, level_fade_, value, gain_left, gain_right))
      continue;
    sum_left += value * gain_left;
    sum_right += value * gain_right;
    mono += value * 0.5 * (gain_left + gain_right);
  }
  if (level_fade_ > 0)
    --level_fade_;
  sum_left *= level;
  sum_right *= level;
  sum_left = dc_block(0, sum_left);
  sum_right = dc_block(1, sum_right);
  left += sum_left;
  right += sum_right;
  peak_ = std::max(peak_, static_cast<float>(std::max(std::fabs(sum_left), std::fabs(sum_right))));
  last_mono_ = mono;
  return static_cast<float>(mono);
}

bool Oscillator::reads_cross() const noexcept {
  return std::ranges::any_of(settings_.warp_mode, [](WarpMode mode) {
    return mode == WarpMode::fm || mode == WarpMode::pm || mode == WarpMode::am ||
           mode == WarpMode::rm;
  });
}

void Oscillator::render(std::size_t count, double* left, double* right, float* mono) noexcept {
  // Per sample, each voice adds to the sums in voice order, as process()
  // does, so every sum is the same. Each voice runs on a local copy, so its
  // state stays in registers instead of being reloaded after every store.
  // Switched off and faded out: silent, and it holds its state too.
  if (dormant()) {
    std::fill_n(left, count, 0.0);
    std::fill_n(right, count, 0.0);
    std::fill_n(mono, count, 0.0F);
    return;
  }
  std::array<double, max_render_samples> mono_sum; // The first `count` are zeroed.
  double* __restrict sum_left = left;
  double* __restrict sum_right = right;
  double* __restrict sum_mono = mono_sum.data();
  std::fill_n(sum_left, count, 0.0);
  std::fill_n(sum_right, count, 0.0);
  std::fill_n(sum_mono, count, 0.0);
  const bool paired = pairs_apply(count);
  Voice* waiting = nullptr; // A paired voice still looking for its partner.
  for (std::size_t v = 0; v < max_unison; ++v) {
    auto& slot = voices_[v];
    // A slot silent through the block (beyond the unison count, faded
    // out) holds its state until it is needed: an unheard phase need not
    // advance, and its ramps reach their targets from where they stopped.
    if (slot.gain_left.value == 0.0 && slot.gain_left.step == 0.0 && slot.gain_right.value == 0.0 &&
        slot.gain_right.step == 0.0)
      continue;
    if (paired) {
      if (waiting == nullptr) {
        waiting = &slot;
      } else {
        render_pair(*waiting, slot, count, sum_left, sum_right, sum_mono);
        waiting = nullptr;
      }
      continue;
    }
    Voice voice = slot;
    auto& blep = bleps_[v];
    for (std::size_t i = 0; i < count; ++i) {
      const auto fade = level_fade_ > i ? level_fade_ - static_cast<std::uint32_t>(i) : 0U;
      double value = 0.0, gain_left = 0.0, gain_right = 0.0;
      if (!step(voice, blep, 0.0, 0.0, fade, value, gain_left, gain_right))
        continue;
      sum_left[i] += value * gain_left;
      sum_right[i] += value * gain_right;
      sum_mono[i] += value * 0.5 * (gain_left + gain_right);
    }
    slot = voice;
  }
  // An odd voice out runs alone: the same lanes beside a silent copy of
  // itself, whose gains are zero so it adds nothing.
  if (waiting != nullptr) {
    Voice silent = *waiting;
    silent.gain_left = {};
    silent.gain_right = {};
    render_pair(*waiting, silent, count, sum_left, sum_right, sum_mono);
  }
  level_fade_ = level_fade_ > count ? level_fade_ - static_cast<std::uint32_t>(count) : 0U;
  for (std::size_t i = 0; i < count; ++i) {
    const double level = level_ramp_.next();
    const double out_left = dc_block(0, sum_left[i] * level);
    const double out_right = dc_block(1, sum_right[i] * level);
    // As process() adds to a voice's zeroed accumulators.
    sum_left[i] = 0.0 + out_left;
    sum_right[i] = 0.0 + out_right;
    peak_ =
        std::max(peak_, static_cast<float>(std::max(std::fabs(out_left), std::fabs(out_right))));
    mono[i] = static_cast<float>(sum_mono[i]);
  }
  if (count > 0)
    last_mono_ = sum_mono[count - 1];
}

std::uint32_t Oscillator::sounding() const noexcept {
  if (!settings_.on)
    return 0;
  std::uint32_t count = 0;
  for (const auto& voice : voices_)
    count += voice.presence > 0.0 ? 1U : 0U;
  return count;
}

void Oscillator::snapshot(std::span<float, 128> out) const noexcept {
  float peak = 0.0F;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double phase = static_cast<double>(i) / static_cast<double>(out.size());
    double value = 0.0;
    if (settings_.type == OscType::classic) {
      const double width = settings_.shape == ClassicShape::pulse ? settings_.pw : 0.5;
      value = BlepOscillator::naive(wrap(phase * std::max(1.0, settings_.sync)),
                                    blep_waveform(settings_.shape), width);
    } else if (settings_.type == OscType::wavetable && table_ != nullptr) {
      double p = phase;
      for (std::size_t w = 0; w < warp_slots; ++w)
        p = wrap(warp_phase(p, settings_.warp_mode[w], settings_.warp_amount[w], 0.0, remap_[w]));
      const auto& band = table_->level(0);
      const double frame_position = settings_.position * static_cast<double>(table_->frames() - 1);
      const auto frame =
          std::min(static_cast<std::size_t>(std::lround(frame_position)), table_->frames() - 1);
      const auto sample =
          std::min(static_cast<std::size_t>(p * band.length), std::size_t{band.length} - 1);
      value = table_->frame(0, frame)[sample];
      for (std::size_t w = 0; w < warp_slots; ++w)
        value = warp_value(value, settings_.warp_mode[w], settings_.warp_amount[w], 0.0);
    }
    out[i] = static_cast<float>(value);
    peak = std::max(peak, std::fabs(out[i]));
  }
  if (peak > 1e-6F) {
    for (auto& value : out)
      value /= peak;
  }
}

void SubOscillator::start(ClassicShape shape) noexcept {
  blep_.reset(0.0, blep_waveform(shape));
  increment_step_ = 0.0;
  gain_left_ = gain_right_ = 0.0;
  gain_left_step_ = gain_right_step_ = 0.0;
  peak_ = 0.0F;
}

void SubOscillator::set(double hz, double level, double pan, ClassicShape shape,
                        std::uint32_t frames) noexcept {
  blep_.set_waveform(blep_waveform(shape), 0.5);
  const double increment = hz / rate_;
  double left = 0.0, right = 0.0;
  pan_gains(pan, level, left, right);
  if (frames == 0) {
    increment_ = increment;
    gain_left_ = left;
    gain_right_ = right;
    increment_step_ = gain_left_step_ = gain_right_step_ = 0.0;
    return;
  }
  increment_step_ = (increment - increment_) / frames;
  gain_left_step_ = (left - gain_left_) / frames;
  gain_right_step_ = (right - gain_right_) / frames;
}

float SubOscillator::process(double& left, double& right) noexcept {
  increment_ += increment_step_;
  gain_left_ += gain_left_step_;
  gain_right_ += gain_right_step_;
  const double value = blep_.next(increment_, 0.5, {}, nullptr);
  const double l = value * gain_left_;
  const double r = value * gain_right_;
  left += l;
  right += r;
  peak_ = std::max(peak_, static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
  return static_cast<float>(value);
}

void NoiseOscillator::start(const NoiseSample* sample, std::uint64_t seed) noexcept {
  sample_ = sample;
  position_ = sample != nullptr && !sample->frames.empty()
                  ? hash_unit(seed) * static_cast<double>(sample->frames.size())
                  : 0.0;
  gain_left_ = gain_right_ = 0.0;
  gain_left_step_ = gain_right_step_ = 0.0;
  low_ = {};
  peak_ = 0.0F;
}

void NoiseOscillator::set(const NoiseSample* sample, double speed, double level, double pan,
                          double tone, bool one_shot, std::uint32_t frames) noexcept {
  if (sample != sample_) {
    sample_ = sample;
    position_ = 0.0;
  }
  speed_ = sample != nullptr ? speed * sample->sample_rate / rate_ : 0.0;
  one_shot_ = one_shot;
  tone_ = std::clamp(tone, -1.0, 1.0);
  double left = 0.0, right = 0.0;
  pan_gains(pan, level, left, right);
  if (frames == 0) {
    gain_left_ = left;
    gain_right_ = right;
    gain_left_step_ = gain_right_step_ = 0.0;
    return;
  }
  gain_left_step_ = (left - gain_left_) / frames;
  gain_right_step_ = (right - gain_right_) / frames;
}

float NoiseOscillator::process(double& left, double& right, double am) noexcept {
  gain_left_ += gain_left_step_;
  gain_right_ += gain_right_step_;
  if (sample_ == nullptr || sample_->frames.empty())
    return 0.0F;
  const auto count = sample_->frames.size();
  if (one_shot_ && position_ >= static_cast<double>(count - 1))
    return 0.0F;
  // The position is almost always inside the sample already: no division.
  const auto whole = static_cast<std::size_t>(position_);
  const auto i = whole < count ? whole : whole % count;
  const auto j = i + 1 < count ? i + 1 : 0;
  const double t = position_ - std::floor(position_);
  const auto& a = sample_->frames[i];
  const auto& b = sample_->frames[j];
  StereoSample value{a.left + t * (b.left - a.left), a.right + t * (b.right - a.right)};
  position_ += speed_;
  if (!one_shot_ && position_ >= static_cast<double>(count))
    position_ -= static_cast<double>(count);
  // Tilt: a one-pole split around 1 kHz, dark below zero, bright above.
  low_.left += tilt_ * (value.left - low_.left);
  low_.right += tilt_ * (value.right - low_.right);
  const double low_gain = 1.0 + std::max(0.0, -tone_);
  const double high_gain = 1.0 + std::max(0.0, tone_) - std::max(0.0, -tone_);
  const double l = (low_.left * low_gain + (value.left - low_.left) * high_gain) * gain_left_ * am;
  const double r =
      (low_.right * low_gain + (value.right - low_.right) * high_gain) * gain_right_ * am;
  left += l;
  right += r;
  peak_ = std::max(peak_, static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
  return static_cast<float>(0.5 * (value.left + value.right));
}

} // namespace oscillate::native_dsp::synth::nova
