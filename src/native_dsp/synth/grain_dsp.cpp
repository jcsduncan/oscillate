#include "oscillate/native_dsp/synth/grain_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace oscillate::native_dsp::synth {
namespace {
constexpr double pi = std::numbers::pi;

// Random grain intervals in semitones, per set; one is drawn uniformly.
constexpr std::array<double, 3> octaves{-12.0, 12.0, 24.0};
constexpr std::array<double, 3> fifths{7.0, 12.0, -5.0};
constexpr std::array<double, 5> pentatonic{2.0, 4.0, 7.0, 9.0, 12.0};

double positive_mod(double value, double span) noexcept {
  const double r = value - span * std::floor(value / span);
  return r >= span ? 0.0 : r;
}
} // namespace

// ---- Source buffer ---------------------------------------------------------------

void GrainBuffer::prepare(std::size_t frames) {
  // Every level spans the same time; the coarsest still has guard samples.
  const std::size_t size = std::bit_ceil(frames + (std::size_t{8} << (levels - 1)));
  for (std::size_t level = 0; level < levels; ++level) {
    data_[level].assign(size >> level, 0.0F);
    masks_[level] = (size >> level) - 1;
  }
  length_ = size - (std::size_t{8} << (levels - 1));
  reset();
}

void GrainBuffer::reset() noexcept {
  count_ = 0;
  counts_.fill(0);
  holding_.fill(false);
  for (auto& decimator : decimators_) {
    decimator.reset();
  }
}

// ---- Grain cloud ------------------------------------------------------------------

void GrainCloud::reset(std::uint32_t seed) noexcept {
  random_.reset(seed);
  for (auto& grain : grains_) {
    grain.active = false;
  }
  elapsed_ = 0;
  next_spawn_ = 0;
  spawned_ = 0;
  base_frame_ = 0;
  base_index_ = 0.0;
  drift_ = 0.0;
}

double GrainCloud::density_gain(const GrainSettings& settings) noexcept {
  const double spacing = settings.synced ? settings.frames_per_division : settings.interval_frames;
  const double overlap = settings.size_frames / std::max(1.0, spacing);
  return std::sqrt(2.0 / std::max(2.0, overlap));
}

void GrainCloud::set(const GrainSettings& settings) noexcept {
  if (settings.synced && settings.frames_per_division != division_ && division_ > 0.0 &&
      settings_.synced) {
    // Rebase: keep the musical position at the current frame.
    base_index_ += static_cast<double>(elapsed_ - base_frame_) / division_;
    base_frame_ = elapsed_;
  }
  if (settings.synced != settings_.synced) {
    base_frame_ = elapsed_;
    base_index_ = static_cast<double>(spawned_);
  }
  const bool retimed =
      settings.synced && (settings.frames_per_division != division_ || !settings_.synced);
  division_ = settings.frames_per_division;
  settings_ = settings;
  gain_ = density_gain(settings);
  if (retimed && division_ > 0.0 && elapsed_ > 0) {
    // The pending onset moves onto the new grid.
    const double steps = static_cast<double>(spawned_) - base_index_;
    next_spawn_ =
        base_frame_ + static_cast<std::uint64_t>(std::ceil(std::max(0.0, steps * division_)));
    next_spawn_ = std::max(next_spawn_, elapsed_);
  }
}

std::size_t GrainCloud::active() const noexcept {
  std::size_t n = 0;
  for (const auto& grain : grains_) {
    n += grain.active ? 1U : 0U;
  }
  return n;
}

double GrainCloud::window(GrainShape shape, double x) noexcept {
  switch (shape) {
  case GrainShape::hann:
    return native_dsp::dsp::hann(x, 1.0);
  case GrainShape::tukey: {
    constexpr double taper = 0.25;
    if (x < taper) {
      return 0.5 - 0.5 * std::cos(pi * x / taper);
    }
    if (x > 1.0 - taper) {
      return 0.5 - 0.5 * std::cos(pi * (1.0 - x) / taper);
    }
    return 1.0;
  }
  case GrainShape::decay: {
    if (x < 0.05) {
      return x / 0.05;
    }
    const double y = (1.0 - x) / 0.95;
    return y * y;
  }
  case GrainShape::swell: {
    if (x > 0.95) {
      return (1.0 - x) / 0.05;
    }
    const double y = x / 0.95;
    return y * y;
  }
  }
  return 0.0;
}

double GrainCloud::next_window(Grain& grain) const noexcept {
  const std::uint32_t i = grain.age;
  switch (settings_.shape) {
  case GrainShape::hann: {
    // cos(2 pi i / n) by the recursion c[i+1] = 2 cos(w) c[i] - c[i-1].
    const double value = 0.5 - 0.5 * grain.cos_now;
    const double next = grain.cos_coefficient * grain.cos_now - grain.cos_previous;
    grain.cos_previous = grain.cos_now;
    grain.cos_now = next;
    return value;
  }
  case GrainShape::tukey: {
    const std::uint32_t taper = grain.taper;
    if (i < taper || i >= grain.length - taper) {
      // Mirror the rising taper on the way out.
      const std::uint32_t k = i < taper ? i : grain.length - i;
      return 0.5 - 0.5 * std::cos(pi * static_cast<double>(k) / static_cast<double>(taper));
    }
    return 1.0;
  }
  case GrainShape::decay:
  case GrainShape::swell:
    return window(settings_.shape, static_cast<double>(i) / static_cast<double>(grain.length));
  }
  return 0.0;
}

void GrainCloud::schedule_next() noexcept {
  ++spawned_;
  if (settings_.synced && division_ > 0.0) {
    const double steps = static_cast<double>(spawned_) - base_index_;
    next_spawn_ =
        base_frame_ + static_cast<std::uint64_t>(std::ceil(std::max(0.0, steps * division_)));
    if (next_spawn_ <= elapsed_) {
      next_spawn_ = elapsed_ + 1;
    }
    return;
  }
  const double spacing = settings_.interval_frames * (1.0 + settings_.chaos * uniform());
  next_spawn_ = elapsed_ + static_cast<std::uint64_t>(std::max(1.0, std::round(spacing)));
}

void GrainCloud::spawn(const GrainBuffer& buffer, bool frozen, std::uint64_t written) noexcept {
  // Every draw happens for every grain, so the sequence never depends on
  // which features are enabled.
  const double u_interval = uniform();
  const double u_choice = uniform();
  const double u_jitter = uniform();
  const double u_reverse = uniform();
  const double u_spray = uniform();
  const double u_pan = uniform();

  Grain* slot = nullptr;
  for (auto& grain : grains_) {
    if (!grain.active) {
      slot = &grain;
      break;
    }
  }
  const auto length = static_cast<std::uint32_t>(std::max(2.0, std::round(settings_.size_frames)));
  if (slot == nullptr || written < 8) {
    return;
  }

  double semitones = 0.0;
  if (settings_.intervals != GrainIntervals::off &&
      0.5 * (u_interval + 1.0) < settings_.interval_probability) {
    const double pick = 0.5 * (u_choice + 1.0);
    switch (settings_.intervals) {
    case GrainIntervals::octaves:
      semitones = octaves[std::min<std::size_t>(2, static_cast<std::size_t>(pick * 3.0))];
      break;
    case GrainIntervals::fifths:
      semitones = fifths[std::min<std::size_t>(2, static_cast<std::size_t>(pick * 3.0))];
      break;
    case GrainIntervals::pentatonic:
      semitones = pentatonic[std::min<std::size_t>(4, static_cast<std::size_t>(pick * 5.0))];
      break;
    case GrainIntervals::off:
      break;
    }
  }
  const double rate =
      settings_.rate * std::exp2((semitones + settings_.jitter_cents * u_jitter / 100.0) / 12.0);
  bool reverse = 0.5 * (u_reverse + 1.0) < settings_.reverse_probability;
  const double n = static_cast<double>(length);
  const double w = static_cast<double>(written);
  const double history = std::min(w, static_cast<double>(buffer.length()));
  const double lag = positive_mod(settings_.lag_frames + drift_, std::max(1.0, history));
  double start = w - lag - 0.5 * rate * n + settings_.spray_frames * u_spray;

  // Keep every read inside the written, not-yet-overwritten range.
  const double span = rate * n;
  const double length_history = static_cast<double>(buffer.length());
  // Pitched-up grains read a decimated level; its reads need guard samples
  // (and one pending decimator input) measured in that level's frames.
  const std::size_t level = GrainBuffer::level_for(rate);
  const double scale = static_cast<double>(std::size_t{1} << level);
  const double margin = 4.0 * scale;
  const double forward_high = frozen ? w - margin - span : w - margin + std::min(0.0, n - span);
  const double reverse_high = w - margin - span;
  double low = frozen ? w - length_history + scale + settings_.reserved_frames
                      : w - length_history + writer_margin + scale + n;
  low = std::max(scale, low);
  // A reversed grain needs a whole span of history; early in a note it plays
  // forwards instead, so notes speak at once.
  if (reverse && reverse_high < low) {
    reverse = false;
  }
  const double high = reverse ? reverse_high : forward_high;
  if (high < low) {
    return; // Not enough history yet for this grain.
  }
  // A frozen moment loops: grains past either end wrap round it instead of
  // piling onto the edge. Live, the newest audio is the natural edge.
  start = frozen && high > low ? low + positive_mod(start - low, high - low)
                               : std::clamp(start, low, high);

  auto& grain = *slot;
  grain.active = true;
  grain.age = 0;
  grain.length = length;
  grain.level = level;
  grain.position = (reverse ? start + span : start) / scale;
  grain.step = (reverse ? -rate : rate) / scale;
  const double pan = std::clamp(settings_.spread * u_pan, -1.0, 1.0);
  grain.left = gain_ * std::cos((pan + 1.0) * pi / 4.0) * std::numbers::sqrt2;
  grain.right = gain_ * std::sin((pan + 1.0) * pi / 4.0) * std::numbers::sqrt2;
  const double omega = 2.0 * pi / n;
  grain.cos_now = 1.0;
  grain.cos_previous = std::cos(omega); // cos(-w)
  grain.cos_coefficient = 2.0 * std::cos(omega);
  grain.taper = std::max<std::uint32_t>(1, length / 4);
}

void GrainCloud::render(const GrainBuffer& buffer, bool frozen, std::uint64_t count_before,
                        double* left, double* right, std::size_t frames) noexcept {
  const double writer_speed = frozen ? 0.0 : 1.0;
  for (std::size_t i = 0; i < frames; ++i) {
    const std::uint64_t written = frozen ? count_before : count_before + i + 1;
    while (elapsed_ >= next_spawn_) {
      spawn(buffer, frozen, written);
      schedule_next();
    }
    double l = 0.0;
    double r = 0.0;
    for (auto& grain : grains_) {
      if (!grain.active) {
        continue;
      }
      const double value = buffer.read(grain.level, grain.position) * next_window(grain);
      l += value * grain.left;
      r += value * grain.right;
      grain.position += grain.step;
      if (++grain.age >= grain.length) {
        grain.active = false;
      }
    }
    left[i] += l;
    right[i] += r;
    drift_ += writer_speed - settings_.scan;
    ++elapsed_;
  }
}

// ---- Partial bank ---------------------------------------------------------------

void PartialBank::reset() noexcept {
  cos_.fill(1.0);
  sin_.fill(0.0);
  increment_ = -1.0;
  stretch_ = -1.0;
  tilt_ = -1.0;
  odd_ = -2.0;
}

void PartialBank::set(double increment, double tilt, double odd, double stretch) noexcept {
  // Keep every rotation on the unit circle.
  for (std::size_t n = 0; n < count; ++n) {
    const double radius = std::sqrt(cos_[n] * cos_[n] + sin_[n] * sin_[n]);
    cos_[n] /= radius;
    sin_[n] /= radius;
  }
  if (increment == increment_ && stretch == stretch_ && tilt == tilt_ && odd == odd_) {
    return;
  }
  increment_ = increment;
  stretch_ = stretch;
  tilt_ = tilt;
  odd_ = odd;
  double power = 0.0;
  for (std::size_t n = 1; n <= count; ++n) {
    const double inc = increment * ratio(n, stretch);
    double amplitude = 0.0;
    if (inc < native_dsp::dsp::nyquist_guard) {
      // Odd partials gain and even ones lose with odd > 0 (hollow, square-like).
      const double weight = n == 1 ? 1.0 : (n % 2 == 1 ? 1.0 + odd : 1.0 - odd);
      amplitude = std::pow(static_cast<double>(n), -tilt) * std::max(0.0, weight);
    }
    rotate_cos_[n - 1] = std::cos(2.0 * pi * inc);
    rotate_sin_[n - 1] = std::sin(2.0 * pi * inc);
    amplitudes_[n - 1] = amplitude;
    power += amplitude * amplitude;
  }
  const double norm = power > 0.0 ? 0.7 / std::sqrt(power) : 0.0;
  for (auto& amplitude : amplitudes_) {
    amplitude *= norm;
  }
}

// ---- Shimmer reverb ---------------------------------------------------------------

void OctaveSweep::prepare(double rate) noexcept {
  window_ = std::round(0.05 * rate);
  reset();
}

OctaveSweep::Taps OctaveSweep::taps() const noexcept {
  // The delay falls by one frame per frame, so each tap reads at 2x speed.
  const double second = phase_ + 0.5 >= 1.0 ? phase_ - 0.5 : phase_ + 0.5;
  const double fade = std::sin(pi * phase_);
  return {1.0 + window_ * (1.0 - phase_), 1.0 + window_ * (1.0 - second), fade * fade};
}

void OctaveSweep::advance() noexcept {
  phase_ += 1.0 / window_;
  if (phase_ >= 1.0) {
    phase_ -= 1.0;
  }
}

void OctaveShifter::prepare(double window) {
  // 8th-order Butterworth: the four section Qs.
  constexpr std::array<double, 4> q{0.5097956, 0.6013449, 0.8999762, 2.5629154};
  const double g = prewarp(0.2);
  for (std::size_t i = 0; i < q.size(); ++i) {
    band_coefficients_[i] = SvfCoefficients::make(g, 1.0 / q[i]);
  }
  line_.prepare(static_cast<std::size_t>(window) + 8);
  reset();
}

void OctaveShifter::reset() noexcept {
  for (auto& section : band_limit_) {
    section.reset();
  }
  line_.reset();
}

double OctaveShifter::process(double x, const OctaveSweep::Taps& taps) noexcept {
  for (std::size_t i = 0; i < band_limit_.size(); ++i) {
    x = band_limit_[i].process(x, band_coefficients_[i]).low;
  }
  line_.write(x);
  const double a = line_.read(taps.first_delay);
  const double b = line_.read(taps.second_delay);
  return taps.first_weight * a + (1.0 - taps.first_weight) * b;
}

namespace {
constexpr std::array<double, ShimmerReverb::lines> base_ms{29.7, 37.1, 41.1, 43.7,
                                                           47.9, 53.3, 59.9, 67.1};
constexpr double max_scale = 2.0;
} // namespace

void ShimmerReverb::prepare(double rate) {
  rate_ = rate;
  sweep_.prepare(rate);
  for (std::size_t i = 0; i < lines; ++i) {
    delays_[i].prepare(static_cast<std::size_t>(base_ms[i] * 1e-3 * rate * max_scale) + 64);
    shifters_[i].prepare(sweep_.window());
    lfo_increment_[i] = (0.11 + 0.07 * static_cast<double>(i)) / rate;
  }
  depth_ = 0.0003 * rate;
  reset();
}

void ShimmerReverb::reset() noexcept {
  sweep_.reset();
  for (std::size_t i = 0; i < lines; ++i) {
    delays_[i].reset();
    shifters_[i].reset();
    low_[i] = 0.0;
    lfo_phase_[i] = 0.125 * static_cast<double>(i);
  }
}

void ShimmerReverb::set(double size, double decay_seconds, double damping, double shimmer,
                        double mix) noexcept {
  const double scale = 0.5 + 1.5 * std::clamp(size, 0.0, 1.0);
  const double decay = std::max(0.05, decay_seconds);
  for (std::size_t i = 0; i < lines; ++i) {
    length_[i] = base_ms[i] * 1e-3 * rate_ * scale;
    gain_[i] = std::pow(10.0, -3.0 * length_[i] / (decay * rate_));
  }
  damping_ = 1.0 - 0.85 * std::clamp(damping, 0.0, 1.0);
  shimmer_ = std::clamp(shimmer, 0.0, 1.0);
  mix_ = std::clamp(mix, 0.0, 1.0);
}

StereoOut ShimmerReverb::process(double left, double right) noexcept {
  if (mix_ <= 0.0) {
    return {left, right};
  }
  std::array<double, lines> m{};
  double sum = 0.0;
  for (std::size_t i = 0; i < lines; ++i) {
    const double modulation = depth_ * sin_cycle(lfo_phase_[i]);
    lfo_phase_[i] += lfo_increment_[i];
    lfo_phase_[i] -= fast_floor(lfo_phase_[i]);
    const double out = delays_[i].read(length_[i] + depth_ + modulation);
    low_[i] += (out - low_[i]) * damping_;
    m[i] = low_[i];
    sum += m[i];
  }
  const double householder = 2.0 / static_cast<double>(lines) * sum;
  double wet_left = 0.0;
  double wet_right = 0.0;
  const bool shifting = shimmer_ > 0.0;
  const auto taps = shifting ? sweep_.taps() : OctaveSweep::Taps{};
  if (shifting) {
    sweep_.advance();
  }
  for (std::size_t i = 0; i < lines; ++i) {
    const double mixed = m[i] - householder;
    const double shifted = shifting ? shifters_[i].process(mixed, taps) : 0.0;
    const double feedback = gain_[i] * ((1.0 - shimmer_) * mixed + shimmer_ * shifted);
    const double input = (i % 2 == 0 ? left : right) * 0.5;
    delays_[i].write(feedback + input);
    (i % 2 == 0 ? wet_left : wet_right) += m[i];
  }
  constexpr double wet_scale = 0.5;
  return {left + mix_ * (wet_scale * wet_left - left),
          right + mix_ * (wet_scale * wet_right - right)};
}

} // namespace oscillate::native_dsp::synth
