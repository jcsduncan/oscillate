#include "oscillate/native_dsp/effects/signal.hpp"

#include "effect_dsp.hpp"
#include "signal_presets.hpp"
#include "signal_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::signal {
namespace {

using signal_table::index_of;
using signal_table::Output;
using signal_table::Wave;

namespace p {
constexpr std::size_t on = index_of("signal.on");
constexpr std::size_t wave = index_of("signal.wave");
constexpr std::size_t frequency = index_of("signal.frequency");
constexpr std::size_t level = index_of("signal.level");
constexpr std::size_t sweep_seconds = index_of("signal.sweep_seconds");
constexpr std::size_t impulse_rate = index_of("signal.impulse_rate");
constexpr std::size_t mode = index_of("signal.mode");
constexpr std::size_t output = index_of("signal.output");
} // namespace p

constexpr double fade_seconds = 0.01;

// Pink: first-order corners every half octave from 2.5 Hz, alternating
// pole and zero (-6 then 0 dB per octave, -3 on average), placed by matched
// z. Near Nyquist that law bends, so the top corners are refitted per rate
// against 1/f out to Nyquist (Levenberg-Marquardt, at construction).
constexpr double pink_low_hz = 2.5;
constexpr std::size_t pink_max_corners = 48;
constexpr std::size_t pink_fitted = 6;
constexpr std::size_t pink_grid = 500;

struct Pink {
  std::array<double, pink_max_corners> radius{};
  std::size_t corners = 0;
};

// dB of |H|^2 at each grid frequency, plus 10 log f, about its mean: zero
// for exact pink.
void pink_error(const Pink& pink, std::span<const double> w, std::span<const double> log_f,
                std::span<double> error) noexcept {
  double mean = 0.0;
  for (std::size_t i = 0; i < w.size(); ++i) {
    const std::complex<double> z_inv = std::polar(1.0, -w[i]);
    std::complex<double> h{1.0, 0.0};
    for (std::size_t c = 0; c < pink.corners; ++c) {
      const std::complex<double> term = 1.0 - pink.radius[c] * z_inv;
      h = c % 2 == 0 ? h / term : h * term;
    }
    error[i] = 10.0 * std::log10(std::norm(h)) + log_f[i];
    mean += error[i];
  }
  mean /= static_cast<double>(w.size());
  for (double& e : error) {
    e -= mean;
  }
}

double squared(std::span<const double> x) noexcept {
  double sum = 0.0;
  for (const double v : x) {
    sum += v * v;
  }
  return sum;
}

// Solves the small dense system a x = b in place (Gaussian elimination with
// partial pivoting).
template <std::size_t N>
std::array<double, N> solve(std::array<std::array<double, N>, N> a, std::array<double, N> b) {
  for (std::size_t col = 0; col < N; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < N; ++row) {
      if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
        pivot = row;
      }
    }
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);
    for (std::size_t row = col + 1; row < N; ++row) {
      const double k = a[row][col] / a[col][col];
      for (std::size_t j = col; j < N; ++j) {
        a[row][j] -= k * a[col][j];
      }
      b[row] -= k * b[col];
    }
  }
  std::array<double, N> x{};
  for (std::size_t row = N; row-- > 0;) {
    double sum = b[row];
    for (std::size_t j = row + 1; j < N; ++j) {
      sum -= a[row][j] * x[j];
    }
    x[row] = sum / a[row][row];
  }
  return x;
}

Pink design_pink(double rate) {
  Pink pink;
  // Corners a half octave apart, by design.
  // NOLINTBEGIN(bugprone-float-loop-counter)
  for (double hz = pink_low_hz; hz < 0.5 * rate && pink.corners < pink_max_corners;
       hz *= std::numbers::sqrt2) {
    // NOLINTEND(bugprone-float-loop-counter)
    pink.radius[pink.corners++] = std::exp(-2.0 * std::numbers::pi * hz / rate);
  }
  std::vector<double> w(pink_grid);
  std::vector<double> log_f(pink_grid);
  for (std::size_t i = 0; i < pink_grid; ++i) {
    const double hz = 10.0 * std::pow(0.499 * rate / 10.0,
                                      static_cast<double>(i) / static_cast<double>(pink_grid - 1));
    w[i] = 2.0 * std::numbers::pi * hz / rate;
    log_f[i] = 10.0 * std::log10(hz);
  }
  // The top corners, as atanh of their radii so they stay inside the unit
  // circle (and may go negative to shape the band edge).
  const std::size_t first = pink.corners - pink_fitted;
  std::array<double, pink_fitted> x{};
  for (std::size_t j = 0; j < pink_fitted; ++j) {
    x[j] = std::atanh(pink.radius[first + j]);
  }
  const auto apply = [&](const std::array<double, pink_fitted>& at) {
    for (std::size_t j = 0; j < pink_fitted; ++j) {
      pink.radius[first + j] = std::tanh(at[j]);
    }
  };
  std::vector<double> error(pink_grid);
  std::vector<double> trial(pink_grid);
  std::vector<std::array<double, pink_fitted>> jacobian(pink_grid);
  apply(x);
  pink_error(pink, w, log_f, error);
  double cost = squared(error);
  double lambda = 1e-2;
  constexpr double delta = 1e-6;
  for (int iteration = 0; iteration < 60 && lambda < 1e8; ++iteration) {
    for (std::size_t j = 0; j < pink_fitted; ++j) {
      auto nudged = x;
      nudged[j] += delta;
      apply(nudged);
      pink_error(pink, w, log_f, trial);
      for (std::size_t i = 0; i < pink_grid; ++i) {
        jacobian[i][j] = (trial[i] - error[i]) / delta;
      }
    }
    std::array<std::array<double, pink_fitted>, pink_fitted> normal{};
    std::array<double, pink_fitted> gradient{};
    for (std::size_t i = 0; i < pink_grid; ++i) {
      for (std::size_t r = 0; r < pink_fitted; ++r) {
        gradient[r] -= jacobian[i][r] * error[i];
        for (std::size_t c = 0; c < pink_fitted; ++c) {
          normal[r][c] += jacobian[i][r] * jacobian[i][c];
        }
      }
    }
    while (lambda < 1e8) {
      auto damped = normal;
      for (std::size_t r = 0; r < pink_fitted; ++r) {
        damped[r][r] += lambda * normal[r][r] + 1e-12;
      }
      const auto step = solve(damped, gradient);
      auto next = x;
      for (std::size_t j = 0; j < pink_fitted; ++j) {
        next[j] += step[j];
      }
      apply(next);
      pink_error(pink, w, log_f, trial);
      const double next_cost = squared(trial);
      if (next_cost < cost) {
        const bool settled = next_cost > cost * (1.0 - 1e-6);
        x = next;
        error.swap(trial);
        cost = next_cost;
        lambda *= 0.3;
        if (settled) {
          lambda = 1e8; // Converged: a further step gains nothing.
        }
        break;
      }
      lambda *= 10.0;
    }
  }
  apply(x);
  return pink;
}

class SignalKernel final : public EffectKernel {
public:
  explicit SignalKernel(double sample_rate)
      : rate_(sample_rate), fade_step_(1.0 / (fade_seconds * sample_rate)),
        pink_(design_pink(sample_rate)) {
    // White noise through the filter: its power is the impulse response's
    // energy, summed until the slowest pole has decayed far below 1e-12.
    double power = 0.0;
    std::array<double, pink_max_corners> state{};
    const double slowest = pink_.radius[0];
    const auto length = static_cast<std::size_t>(30.0 / (1.0 - slowest));
    for (std::size_t n = 0; n < length; ++n) {
      const double y = filter_pink(n == 0 ? 1.0 : 0.0, state);
      power += y * y;
    }
    pink_rms_ = std::sqrt(power / 3.0); // Uniform noise has variance 1/3.
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport&) noexcept override {
    const bool jump = !primed_;
    on_target_ = v[p::on] != 0.0 ? 1.0 : 0.0;
    if (jump) {
      on_ = on_target_;
    }
    wave_ = static_cast<int>(v[p::wave]);
    frequency_ = v[p::frequency];
    level_.settle(dsp::db_to_gain(v[p::level]), std::max<double>(1.0, frames), jump);
    sweep_frames_ = static_cast<std::uint64_t>(std::lround(v[p::sweep_seconds] * rate_));
    impulse_period_ = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::lround(rate_ / v[p::impulse_rate])));
    add_ = v[p::mode] != 0.0;
    output_ = static_cast<int>(v[p::output]);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      on_ += std::clamp(on_target_ - on_, -fade_step_, fade_step_);
      const double level = level_.next();
      const double x = generate(level);
      const std::uint64_t n = frame_++;
      if (on_ == 0.0) {
        continue; // Off: the input passes untouched.
      }
      std::array<double, 2> sig{x, x};
      switch (output_) {
      case Output::left:
        sig[1] = 0.0;
        break;
      case Output::right:
        sig[0] = 0.0;
        break;
      case Output::alternate:
        sig[(n / static_cast<std::uint64_t>(rate_)) % 2 == 0 ? 1 : 0] = 0.0;
        break;
      case Output::inverted:
        sig[1] = -x;
        break;
      default:
        break;
      }
      const std::array<double, 2> in{frame.left, frame.right};
      std::array<double, 2> out{};
      for (std::size_t c = 0; c < 2; ++c) {
        out[c] = add_ ? in[c] + on_ * sig[c] : in[c] * (1.0 - on_) + on_ * sig[c];
      }
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
  }

  void reset() noexcept override {
    phase_ = 0.0;
    frame_ = 0;
    noise_ = 0x9E3779B9U;
    pink_state_.fill(0.0);
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::frequency] = static_cast<float>(now_hz_);
    out[telemetry::progress] = static_cast<float>(progress_);
    return telemetry::size;
  }

private:
  [[nodiscard]] double uniform() noexcept {
    // xorshift32: deterministic, allocation-free.
    noise_ ^= noise_ << 13U;
    noise_ ^= noise_ >> 17U;
    noise_ ^= noise_ << 5U;
    return static_cast<double>(noise_) / 2'147'483'648.0 - 1.0;
  }

  // The pink cascade: each pole (even corner) then zero (odd corner), one
  // sample of state per corner.
  [[nodiscard]] double filter_pink(double x,
                                   std::array<double, pink_max_corners>& state) const noexcept {
    for (std::size_t c = 0; c < pink_.corners; ++c) {
      const double r = pink_.radius[c];
      if (c % 2 == 0) {
        x += r * state[c];
        state[c] = x;
      } else {
        const double in = x;
        x -= r * state[c];
        state[c] = in;
      }
    }
    return x;
  }

  [[nodiscard]] double generate(double level) noexcept {
    switch (wave_) {
    case Wave::sweep: {
      // Farina: f(t) = f1 e^(t / L), phase f1 L (e^(t / L) - 1), L = T / ln(f2 / f1).
      const auto n = frame_ % std::max<std::uint64_t>(1, sweep_frames_);
      const double t = static_cast<double>(n) / rate_;
      const double span = static_cast<double>(sweep_frames_) / rate_;
      const double l = span / std::log(sweep_high_hz / sweep_low_hz);
      const double growth = std::exp(t / l);
      now_hz_ = sweep_low_hz * growth;
      progress_ = t / span;
      const double cycles = sweep_low_hz * l * (growth - 1.0);
      return level * std::sin(2.0 * std::numbers::pi * (cycles - std::floor(cycles)));
    }
    case Wave::white:
      now_hz_ = 0.0;
      return level * std::sqrt(3.0) * uniform();
    case Wave::pink: {
      now_hz_ = 0.0;
      return level * filter_pink(uniform(), pink_state_) / pink_rms_;
    }
    case Wave::impulses:
      now_hz_ = 0.0;
      return frame_ % impulse_period_ == 0 ? level : 0.0;
    default: {
      now_hz_ = frequency_;
      const double y = level * std::sin(2.0 * std::numbers::pi * phase_);
      phase_ += frequency_ / rate_;
      phase_ -= std::floor(phase_);
      return y;
    }
    }
  }

  double rate_;
  double fade_step_;
  Pink pink_;
  std::array<double, pink_max_corners> pink_state_{};
  double pink_rms_ = 1.0;
  double on_ = 1.0;
  double on_target_ = 1.0;
  int wave_ = Wave::sine;
  double frequency_ = 1'000.0;
  dsp::Ramp level_;
  std::uint64_t sweep_frames_ = 480'000;
  std::uint64_t impulse_period_ = 48'000;
  bool add_ = false;
  int output_ = Output::both;
  double phase_ = 0.0;
  std::uint64_t frame_ = 0;
  std::uint32_t noise_ = 1;
  double now_hz_ = 0.0;
  double progress_ = 0.0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<SignalKernel>(sample_rate);
}

// The fade after switching off; while on it sounds without input.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  return static_cast<std::uint64_t>(std::ceil(fade_seconds * sample_rate)) + 1;
}
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"signal", "Signal", 1, signal_table::parameters,
                                         signal_presets::presets};
const EffectEngine signal_engine{&descriptor,     &create_kernel, &tail_frames,
                                 &latency_frames, false,          true};

} // namespace

const EffectEngine& engine() { return signal_engine; }

} // namespace oscillate::native_dsp::effects::signal
