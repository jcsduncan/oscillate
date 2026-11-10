#include "ceiling_dsp.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::ceiling_dsp {
namespace {

// The onset detector: two followers of the frame's power that release
// alike but attack at different speeds. An onset is the fast one leading
// the slow one, counted from onset_start_db and fully from onset_full_db;
// it lasts about as long as the slow attack takes to catch up.
constexpr double onset_fast_attack_seconds = 0.0005;
constexpr double onset_slow_attack_seconds = 0.008;
constexpr double onset_release_seconds = 0.1;
constexpr double onset_start_db = 2.0;
constexpr double onset_full_db = 8.0;
constexpr double decibels_per_bel = 10.0;

// A peak this small asks for no gain reduction.
constexpr double silent_peak = 1e-12;

// The fine peak's window: Kaiser, about 90 dB of sidelobe rejection.
constexpr double fine_peak_beta = 9.0;

} // namespace

FinePeak::FinePeak() noexcept {
  const auto half_width = static_cast<double>(radius + 1);
  for (std::size_t phase = 1; phase < factor; ++phase) {
    const double fraction = static_cast<double>(phase) / static_cast<double>(factor);
    std::array<double, taps> kernel{};
    double sum = 0.0;
    for (std::size_t tap = 0; tap < taps; ++tap) {
      const double x = static_cast<double>(tap) - static_cast<double>(radius) - fraction;
      const double sinc = std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
      kernel[tap] = sinc * native_dsp::dsp::kaiser(x, half_width, fine_peak_beta);
      sum += kernel[tap];
    }
    for (std::size_t tap = 0; tap < taps; ++tap) {
      kernels_[tap][phase - 1] = kernel[tap] / sum;
    }
  }
}

void FinePeak::reset() noexcept {
  history_.fill(0.0);
  write_ = 0;
}

double FinePeak::push(double x) noexcept {
  // Kept twice, `taps` apart, so the window from oldest to newest is
  // contiguous.
  history_[write_] = x;
  history_[write_ + taps] = x;
  if (++write_ == taps) {
    write_ = 0;
  }
  const double* window = history_.data() + write_;
  std::array<double, factor> y{};
  for (std::size_t tap = 0; tap < taps; ++tap) {
    const double x = window[tap];
    for (std::size_t phase = 0; phase < factor; ++phase) {
      y[phase] += kernels_[tap][phase] * x;
    }
  }
  double top = 0.0;
  for (std::size_t phase = 0; phase + 1 < factor; ++phase) {
    top = std::max(top, std::fabs(y[phase]));
  }
  return top;
}

void MinHold::prepare(std::size_t length) {
  length_ = std::max<std::size_t>(length, 1);
  queue_.assign(length_ + 1, Entry{});
  reset();
}

void MinHold::reset() noexcept {
  head_ = 0;
  count_ = 0;
  frame_ = 0;
}

double MinHold::push(double value) noexcept {
  const std::size_t capacity = queue_.size();
  while (count_ > 0) {
    std::size_t back = head_ + count_ - 1;
    if (back >= capacity) {
      back -= capacity;
    }
    if (queue_[back].value < value) {
      break;
    }
    --count_;
  }
  std::size_t tail = head_ + count_;
  if (tail >= capacity) {
    tail -= capacity;
  }
  queue_[tail] = {frame_, value};
  ++count_;
  while (frame_ - queue_[head_].frame >= length_) {
    if (++head_ == capacity) {
      head_ = 0;
    }
    --count_;
  }
  ++frame_;
  return queue_[head_].value;
}

void Boxcar::prepare(std::size_t length) {
  values_.assign(std::max<std::size_t>(length, 1), 0.0);
  reset(0.0);
}

void Boxcar::reset(double value) noexcept {
  std::ranges::fill(values_, value);
  position_ = 0;
  sum_ = value * static_cast<double>(values_.size());
}

double Boxcar::push(double value) noexcept {
  sum_ += value - values_[position_];
  values_[position_] = value;
  if (++position_ == values_.size()) {
    position_ = 0;
    sum_ = 0.0;
    for (const double v : values_) {
      sum_ += v;
    }
  }
  return sum_ / static_cast<double>(values_.size());
}

void FrameDelay::prepare(std::size_t frames) {
  line_.assign(frames + 1, {0.0, 0.0});
  reset();
}

void FrameDelay::reset() noexcept {
  std::ranges::fill(line_, std::array<double, 2>{0.0, 0.0});
  position_ = 0;
}

std::array<double, 2> FrameDelay::push(std::array<double, 2> frame) noexcept {
  line_[position_] = frame;
  if (++position_ == line_.size()) {
    position_ = 0;
  }
  return line_[position_];
}

void GainStage::prepare(std::size_t attack, std::size_t hold, double sample_rate) {
  attack = std::max<std::size_t>(attack, 1);
  // Two averages in cascade whose spans add to the attack: a smooth
  // S-shaped fall that reaches each peak's gain exactly at the peak.
  const std::size_t first = (attack + 1) / 2;
  const std::size_t second = attack - first + 1;
  for (std::size_t c = 0; c < 2; ++c) {
    magnitudes_[c].resize(peak_radius + 1);
    holds_[c].prepare(attack + hold);
    first_[c].prepare(first);
    second_[c].prepare(second);
  }
  fast_keep_ = std::exp(-1.0 / (onset_fast_attack_seconds * sample_rate));
  slow_keep_ = std::exp(-1.0 / (onset_slow_attack_seconds * sample_rate));
  release_keep_ = std::exp(-1.0 / (onset_release_seconds * sample_rate));
  delay_ = peak_radius + attack - 1;
  reset();
}

void GainStage::reset() noexcept {
  for (std::size_t c = 0; c < 2; ++c) {
    peaks_[c].reset();
    magnitudes_[c].reset();
    holds_[c].reset();
    first_[c].reset(0.0);
    second_[c].reset(0.0);
  }
  slow_ = {};
  fast_ = {};
  fast_power_ = 0.0;
  slow_power_ = 0.0;
  onset_ = 0.0;
}

double GainStage::detect_onset(double left, double right) noexcept {
  const double power = 0.5 * (left * left + right * right);
  fast_power_ = power + (power > fast_power_ ? fast_keep_ : release_keep_) * (fast_power_ - power);
  slow_power_ = power + (power > slow_power_ ? slow_keep_ : release_keep_) * (slow_power_ - power);
  if (fast_power_ <= slow_power_ || slow_power_ <= 0.0) {
    return 0.0;
  }
  const double rise = decibels_per_bel * std::log10(fast_power_ / slow_power_);
  return std::clamp((rise - onset_start_db) / (onset_full_db - onset_start_db), 0.0, 1.0);
}

std::array<double, 2> GainStage::process(double left, double right) noexcept {
  onset_ = detect_onset(left, right);
  const double room =
      shape_.sustain + shape_.clip_room + shape_.transients * shape_.onset_room * onset_;
  const std::array<double, 2> in{left, right};
  std::array<double, 2> held{};
  for (std::size_t c = 0; c < 2; ++c) {
    // The interval peak_radius frames back, between that frame and the next.
    magnitudes_[c].push(std::fabs(in[c]));
    const double between = peaks_[c].push(in[c]);
    const double* m = magnitudes_[c].newest();
    const double peak = std::max({between, m[peak_radius], m[peak_radius - 1]});
    const double needed = peak > silent_peak ? std::min(0.0, room - std::log(peak)) : 0.0;
    held[c] = holds_[c].push(needed);
  }
  const double linked = std::min(held[0], held[1]);
  std::array<double, 2> out{};
  for (std::size_t c = 0; c < 2; ++c) {
    const double sustain = held[c] + shape_.link * (linked - held[c]);
    const double transient = held[c] + shape_.transient_link * (linked - held[c]);
    const double keep = sustain < slow_[c] ? shape_.slow_attack : shape_.slow_release;
    slow_[c] = sustain + keep * (slow_[c] - sustain);
    // The fast envelope releases toward the slow one's level (scaled by
    // Character), and never past the gain the peaks need.
    const double floor = shape_.character * slow_[c];
    const double released =
        fast_[c] < floor ? floor + shape_.fast_release * (fast_[c] - floor) : fast_[c];
    fast_[c] = std::min(transient, released);
    out[c] = second_[c].push(first_[c].push(fast_[c]));
  }
  return out;
}

void Clipper::prepare(double sample_rate) {
  design_ = &edge_dsp::design(clip_oversampling, 0);
  for (auto& resampler : resamplers_) {
    resampler.prepare(*design_);
  }
  delay_.prepare(design_->latency);
  fade_step_ = 1.0 / (native_dsp::dsp::switch_fade_seconds * sample_rate);
  reset();
}

void Clipper::reset() noexcept {
  for (auto& resampler : resamplers_) {
    resampler.reset();
  }
  for (auto& antialiased : antialiased_) {
    antialiased.reset();
  }
  delay_.reset();
  // From silence both paths start alike: take the wanted one at once.
  running_ = active_;
  warming_ = 0;
  mix_ = active_ ? 1.0 : 0.0;
}

void Clipper::set(double knee, bool active, bool jump) noexcept {
  curve_ = active ? edge_dsp::Curve::with_knee(knee)
                  : edge_dsp::Curve{0.0, std::numeric_limits<double>::infinity()};
  active_ = active;
  if (jump) {
    reset();
  } else if (active_ && !running_) {
    for (auto& resampler : resamplers_) {
      resampler.reset();
    }
    for (auto& antialiased : antialiased_) {
      antialiased.reset();
    }
    running_ = true;
    // The round trip and the filters' reach before it.
    warming_ = 2 * design_->latency;
  }
}

std::uint32_t Clipper::latency() const noexcept { return design_->latency; }

double Clipper::process(std::span<double> left, std::span<double> right) noexcept {
  const std::size_t frames = left.size();
  std::array<std::array<double, 2>, edge_dsp::block_frames> delayed{};
  for (std::size_t i = 0; i < frames; ++i) {
    delayed[i] = delay_.push({left[i], right[i]});
  }
  if (!running_) {
    for (std::size_t i = 0; i < frames; ++i) {
      left[i] = delayed[i][0];
      right[i] = delayed[i][1];
    }
    return 1.0;
  }
  const std::array<std::span<double>, 2> channels{left, right};
  std::array<std::array<double, edge_dsp::block_frames * edge_dsp::max_factor>, 2> top{};
  const std::size_t samples = frames * design_->factor;
  double ratio = 1.0;
  for (std::size_t c = 0; c < 2; ++c) {
    resamplers_[c].up(channels[c], top[c].data());
    for (std::size_t i = 0; i < samples; ++i) {
      const double u = top[c][i];
      const double magnitude = std::fabs(u);
      if (magnitude > curve_.threshold) {
        ratio = std::max(ratio, magnitude / curve_.magnitude(magnitude));
      }
      top[c][i] = design_->antialiased ? antialiased_[c].process(u, curve_) : curve_(u);
    }
  }
  std::array<std::array<double, edge_dsp::block_frames>, 2> clipped{};
  for (std::size_t c = 0; c < 2; ++c) {
    resamplers_[c].down(top[c].data(), std::span(clipped[c]).first(frames));
  }
  for (std::size_t i = 0; i < frames; ++i) {
    if (!running_) {
      // Stopped earlier in this block: the rest is the delay alone.
      left[i] = delayed[i][0];
      right[i] = delayed[i][1];
      continue;
    }
    if (warming_ > 0) {
      --warming_;
    } else {
      mix_ = active_ ? std::min(1.0, mix_ + fade_step_) : std::max(0.0, mix_ - fade_step_);
    }
    left[i] = delayed[i][0] + mix_ * (clipped[0][i] - delayed[i][0]);
    right[i] = delayed[i][1] + mix_ * (clipped[1][i] - delayed[i][1]);
    if (!active_ && mix_ == 0.0) {
      running_ = false;
    }
  }
  return ratio;
}

} // namespace oscillate::native_dsp::effects::ceiling_dsp
