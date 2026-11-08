#include "fx_modules.hpp"

#include "dsp/fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Nova's Convolve module (08a §5): the managed IR (media slot fx.<uid>.ir)
// resampled to the engine's rate on control, then a zero-latency uniformly
// partitioned convolution: the first partition runs directly in time, the
// rest through a frequency-domain delay line, both channels sharing one
// complex FFT per block. The pinned PFFFT core (native_dsp::Convolution) is
// an optional build and accepts only four rates and 128-frame calls, so the
// module carries its own engine on the shared dsp::Fft.
namespace oscillate::native_dsp::synth::nova::fx_detail {
namespace {

using Complex = std::complex<double>;

constexpr double max_ir_seconds = 3.0;
constexpr double max_predelay_seconds = 0.25;
constexpr std::uint32_t coefficient_interval = 16;
constexpr double sinc_half_width = 16.0;

// Windowed-sinc resampling of one channel, on control.
[[nodiscard]] std::vector<double> resample(const std::vector<double>& in, double from, double to,
                                           std::size_t limit) {
  if (from == to) {
    std::vector<double> out(in.begin(),
                            in.begin() + static_cast<std::ptrdiff_t>(std::min(in.size(), limit)));
    return out;
  }
  const double ratio = from / to;
  const double cutoff = std::min(1.0, to / from);
  const double half = sinc_half_width / cutoff;
  const auto length =
      std::min(limit, static_cast<std::size_t>(std::floor(static_cast<double>(in.size()) / ratio)));
  std::vector<double> out(length, 0.0);
  for (std::size_t n = 0; n < length; ++n) {
    const double t = static_cast<double>(n) * ratio;
    const auto first = static_cast<std::ptrdiff_t>(std::ceil(t - half));
    const auto last = static_cast<std::ptrdiff_t>(std::floor(t + half));
    double sum = 0.0;
    for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, first);
         k <= last && k < static_cast<std::ptrdiff_t>(in.size()); ++k) {
      const double d = t - static_cast<double>(k);
      const double x = pi * cutoff * d;
      const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
      // Blackman window over the kernel's span.
      const double w = 0.42 + 0.5 * std::cos(pi * d / half) + 0.08 * std::cos(two_pi * d / half);
      sum += in[static_cast<std::size_t>(k)] * cutoff * sinc * w;
    }
    out[n] = sum;
  }
  return out;
}

class ConvolveModule final : public FxModule {
public:
  ConvolveModule(double base_rate, std::size_t max_frames,
                 std::shared_ptr<const SynthMediaAsset> ir, std::string identity)
      : base_(base_rate), identity_(std::move(identity)), asset_(std::move(ir)),
        block_(base_rate <= 50'000.0 ? 64 : (base_rate <= 100'000.0 ? 128 : 256)),
        fft_(2 * block_) {
    scratch_.assign(std::max<std::size_t>(1, max_frames), {0.0F, 0.0F});
    for (auto& line : predelay_) {
      line.prepare(static_cast<std::size_t>(std::ceil(max_predelay_seconds * base_rate)) + 8);
    }
    load();
    const std::size_t n = 2 * block_;
    work_.assign(n, Complex{});
    for (std::size_t c = 0; c < 2; ++c) {
      history_[c].assign(block_, 0.0);
      previous_[c].assign(block_, 0.0);
      current_[c].assign(block_, 0.0);
      output_[c].assign(block_, 0.0);
    }
    meter_.prepare(base_rate);
    set(fx_defaults(FxKind::convolve).p, {2.0, base_rate}, 0);
  }

  [[nodiscard]] std::string_view identity() const noexcept { return identity_; }

  [[nodiscard]] FxKind kind() const noexcept override { return FxKind::convolve; }
  void reset() noexcept override {
    resampler_.reset();
    for (std::size_t c = 0; c < 2; ++c) {
      predelay_[c].reset();
      std::fill(history_[c].begin(), history_[c].end(), 0.0);
      std::fill(previous_[c].begin(), previous_[c].end(), 0.0);
      std::fill(current_[c].begin(), current_[c].end(), 0.0);
      std::fill(output_[c].begin(), output_[c].end(), 0.0);
      low_cut_[c].reset();
      high_cut_[c].reset();
    }
    std::ranges::fill(spectra_left_, Complex{});
    std::ranges::fill(spectra_right_, Complex{});
    position_ = 0;
    history_position_ = 0;
    newest_ = 0;
    meter_.reset();
  }
  void set(const FxParams& p, const FxContext& context, std::uint32_t frames) noexcept override {
    const std::uint32_t factor = factor_for(context.sample_rate, base_);
    if (factor != resampler_.factor()) {
      resampler_.set_factor(factor);
      meter_.prepare(base_ * factor);
    }
    const std::uint32_t steps = frames == 0 ? 0U : std::max<std::uint32_t>(1, frames / factor);
    predelay_frames_.to(max_predelay_seconds * 1'000.0 * clamp01(p[0]) * 1e-3 * base_, steps);
    low_octave_.to(std::log2(log_map(p[1], 20.0, 2'000.0)), steps);
    high_octave_.to(std::log2(log_map(p[2], 1'000.0, 20'000.0)), steps);
  }
  void process(Frames io) noexcept override {
    const std::uint32_t factor = resampler_.factor();
    if (factor == 1U) {
      run(io);
    } else {
      const std::size_t n = std::min(io.size() / factor, scratch_.size());
      resampler_.down(io.data(), n, scratch_.data());
      run(Frames(scratch_.data(), n));
      resampler_.up(scratch_.data(), n, io.data());
      std::fill(io.begin() + static_cast<std::ptrdiff_t>(n * factor), io.end(),
                engine::StereoFrame{0.0F, 0.0F});
    }
    meter_.feed(io);
  }
  [[nodiscard]] float telemetry() const noexcept override { return meter_.db(); }
  [[nodiscard]] std::uint64_t tail_frames() const noexcept override {
    if (length_ == 0) {
      return 0;
    }
    const double frames =
        static_cast<double>(length_ + block_) + predelay_frames_.target + 0.05 * base_;
    return static_cast<std::uint64_t>(std::ceil(frames)) * resampler_.factor();
  }

private:
  // Control only: the IR at the engine's rate, unit energy, partitioned.
  void load() {
    if (!asset_ || asset_->frames.empty() || asset_->source_rate == 0) {
      return;
    }
    const auto limit = static_cast<std::size_t>(max_ir_seconds * base_);
    // Only the source frames the limited output reads: its span at the
    // source rate plus the resampler's (widened) kernel half-width.
    const double from = asset_->source_rate;
    const double half = sinc_half_width / std::min(1.0, base_ / from);
    const auto needed =
        static_cast<std::size_t>(std::ceil(static_cast<double>(limit) * from / base_ + half)) + 1;
    const std::size_t used = std::min(asset_->frames.size(), needed);
    std::array<std::vector<double>, 2> raw;
    for (auto& channel : raw) {
      channel.reserve(used);
    }
    for (std::size_t i = 0; i < used; ++i) {
      const auto& frame = asset_->frames[i];
      raw[0].push_back(std::isfinite(frame.left) ? frame.left : 0.0);
      raw[1].push_back(std::isfinite(frame.right) ? frame.right : 0.0);
    }
    std::array<std::vector<double>, 2> ir{resample(raw[0], asset_->source_rate, base_, limit),
                                          resample(raw[1], asset_->source_rate, base_, limit)};
    double energy = 0.0;
    for (const auto& channel : ir) {
      for (const double v : channel) {
        energy += v * v;
      }
    }
    energy *= 0.5;
    if (!(energy > 1e-12) || ir[0].empty()) {
      return;
    }
    const double scale = 1.0 / std::sqrt(energy);
    length_ = ir[0].size();
    const std::size_t n = 2 * block_;
    for (std::size_t c = 0; c < 2; ++c) {
      head_[c].assign(block_, 0.0);
      for (std::size_t t = 0; t < std::min(block_, length_); ++t) {
        head_[c][t] = ir[c][t] * scale;
      }
    }
    partitions_ = length_ > block_ ? (length_ - block_ + block_ - 1) / block_ : 0;
    filter_left_.assign(partitions_ * n, Complex{});
    filter_right_.assign(partitions_ * n, Complex{});
    spectra_left_.assign(partitions_ * n, Complex{});
    spectra_right_.assign(partitions_ * n, Complex{});
    std::vector<Complex> buffer(n);
    for (std::size_t p = 0; p < partitions_; ++p) {
      const std::size_t offset = (p + 1) * block_;
      std::ranges::fill(buffer, Complex{});
      for (std::size_t t = 0; t < block_ && offset + t < length_; ++t) {
        buffer[t] = Complex{ir[0][offset + t] * scale, ir[1][offset + t] * scale};
      }
      fft_.forward(buffer);
      for (std::size_t k = 0; k < n; ++k) {
        const Complex mirror = std::conj(buffer[(n - k) % n]);
        filter_left_[p * n + k] = 0.5 * (buffer[k] + mirror);
        filter_right_[p * n + k] = Complex{0.0, -0.5} * (buffer[k] - mirror);
      }
    }
  }

  void update_filters(double low_octave, double high_octave) noexcept {
    const double top_hz = native_dsp::dsp::nyquist_guard * base_;
    low_c_ = SvfCoefficients::make(prewarp(std::min(std::exp2(low_octave), top_hz) / base_),
                                   std::numbers::sqrt2);
    high_c_ = SvfCoefficients::make(prewarp(std::min(std::exp2(high_octave), top_hz) / base_),
                                    std::numbers::sqrt2);
  }

  // At the engine's rate.
  void run(Frames io) noexcept {
    for (auto& frame : io) {
      const double delay = predelay_frames_.next();
      const double low_octave = low_octave_.next();
      const double high_octave = high_octave_.next();
      if ((tick_++ % coefficient_interval) == 0) {
        update_filters(low_octave, high_octave);
      }
      std::array<double, 2> x{frame.left, frame.right};
      for (std::size_t c = 0; c < 2; ++c) {
        predelay_[c].write(x[c]);
        x[c] = delay < 1.0 ? x[c] : predelay_[c].read(delay);
      }
      std::array<double, 2> y{};
      if (length_ > 0) {
        const std::size_t mask = block_ - 1;
        for (std::size_t c = 0; c < 2; ++c) {
          history_[c][history_position_] = x[c];
          double sum = output_[c][position_];
          const auto& head = head_[c];
          const auto& history = history_[c];
          for (std::size_t t = 0; t < block_; ++t) {
            sum += head[t] * history[(history_position_ - t) & mask];
          }
          y[c] = sum;
          current_[c][position_] = x[c];
        }
        history_position_ = (history_position_ + 1) & mask;
        if (++position_ == block_) {
          block();
          position_ = 0;
        }
      }
      for (std::size_t c = 0; c < 2; ++c) {
        y[c] = high_cut_[c].process(low_cut_[c].process(y[c], low_c_).high, high_c_).low;
      }
      frame.left = std::isfinite(y[0]) ? static_cast<float>(y[0]) : 0.0F;
      frame.right = std::isfinite(y[1]) ? static_cast<float>(y[1]) : 0.0F;
    }
  }

  // A block of input is complete: transform it into the delay line and
  // render the partitions' sum for the next block.
  void block() noexcept {
    const std::size_t n = 2 * block_;
    if (partitions_ == 0) {
      for (std::size_t c = 0; c < 2; ++c) {
        std::copy(current_[c].begin(), current_[c].end(), previous_[c].begin());
      }
      return;
    }
    for (std::size_t i = 0; i < block_; ++i) {
      work_[i] = Complex{previous_[0][i], previous_[1][i]};
      work_[block_ + i] = Complex{current_[0][i], current_[1][i]};
    }
    fft_.forward(work_);
    newest_ = (newest_ + partitions_ - 1) % partitions_;
    Complex* left = &spectra_left_[newest_ * n];
    Complex* right = &spectra_right_[newest_ * n];
    for (std::size_t k = 0; k < n; ++k) {
      const Complex mirror = std::conj(work_[(n - k) % n]);
      left[k] = 0.5 * (work_[k] + mirror);
      right[k] = Complex{0.0, -0.5} * (work_[k] - mirror);
    }
    std::ranges::fill(work_, Complex{});
    for (std::size_t p = 0; p < partitions_; ++p) {
      const std::size_t slot = (newest_ + p) % partitions_;
      const Complex* xl = &spectra_left_[slot * n];
      const Complex* xr = &spectra_right_[slot * n];
      const Complex* hl = &filter_left_[p * n];
      const Complex* hr = &filter_right_[p * n];
      for (std::size_t k = 0; k < n; ++k) {
        // Y = YL + i YR: both outputs are real, so one inverse carries both.
        work_[k] += xl[k] * hl[k] + Complex{0.0, 1.0} * (xr[k] * hr[k]);
      }
    }
    fft_.inverse(work_);
    for (std::size_t i = 0; i < block_; ++i) {
      output_[0][i] = work_[block_ + i].real();
      output_[1][i] = work_[block_ + i].imag();
    }
    for (std::size_t c = 0; c < 2; ++c) {
      std::copy(current_[c].begin(), current_[c].end(), previous_[c].begin());
    }
  }

  double base_;
  std::string identity_;
  std::shared_ptr<const SynthMediaAsset> asset_;
  std::size_t block_;
  native_dsp::dsp::Fft fft_;
  std::size_t length_ = 0;
  std::size_t partitions_ = 0;
  std::array<std::vector<double>, 2> head_;
  std::array<std::vector<double>, 2> history_;
  std::array<std::vector<double>, 2> previous_;
  std::array<std::vector<double>, 2> current_;
  std::array<std::vector<double>, 2> output_;
  std::vector<Complex> filter_left_;
  std::vector<Complex> filter_right_;
  std::vector<Complex> spectra_left_;
  std::vector<Complex> spectra_right_;
  std::vector<Complex> work_;
  std::size_t position_ = 0;
  std::size_t history_position_ = 0;
  std::size_t newest_ = 0;
  std::array<DelayLine, 2> predelay_{};
  std::array<ZdfSvf, 2> low_cut_{};
  std::array<ZdfSvf, 2> high_cut_{};
  SvfCoefficients low_c_{};
  SvfCoefficients high_c_{};
  Glide predelay_frames_;
  Glide low_octave_;
  Glide high_octave_;
  std::uint32_t tick_ = 0;
  std::vector<engine::StereoFrame> scratch_;
  Resampler resampler_;
  PeakMeter meter_;
};

} // namespace

std::unique_ptr<FxModule> make_convolve(double base_rate, std::size_t max_frames,
                                        std::shared_ptr<const SynthMediaAsset> ir,
                                        std::string sha256) {
  return std::make_unique<ConvolveModule>(base_rate, max_frames, std::move(ir), std::move(sha256));
}

std::string_view convolve_identity(const FxModule& module) noexcept {
  const auto* convolve = dynamic_cast<const ConvolveModule*>(&module);
  return convolve != nullptr ? convolve->identity() : std::string_view{};
}

} // namespace oscillate::native_dsp::synth::nova::fx_detail
