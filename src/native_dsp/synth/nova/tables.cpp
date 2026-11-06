#include "oscillate/native_dsp/synth/nova/tables.hpp"

#include "dsp/fft.hpp"

#include <algorithm>
#include <bit>
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

// The table builder (spec 08 §3, NOVA-OSC-02). Every frame becomes a
// spectrum of complex harmonic amplitudes first; levels are then rendered
// from it by inverse transforms with the harmonics above each level's limit
// left out, so the band limits are exact rather than filtered.
namespace oscillate::native_dsp::synth::nova {
namespace {

using Complex = std::complex<double>;
using foundation::Error;
using foundation::ErrorCode;
using TableResult = foundation::Result<std::shared_ptr<const Wavetable>>;

constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * pi;
constexpr std::size_t max_harmonic = table_frame_size / 2;
// A frame's spectrum: c[h] for h = 1..max_harmonic; the cycle is
// sum Re(c[h] e^(i 2 pi h x)). Index 0 (DC) is always zero.
constexpr std::size_t spectrum_size = max_harmonic + 1;
// The longest single frame build_wavetable analyses (about 22 s at 48 kHz).
constexpr std::size_t max_frame_length = std::size_t{1} << 20U;
// The level reads stay below this fraction of the sample rate.
constexpr double band_edge = 0.45;
// Neighbouring frames whose best alignment correlates less than this morph
// in place instead of travelling.
constexpr double min_shift_correlation = 0.5;

[[nodiscard]] TableResult failure(ErrorCode code, std::string message) {
  return TableResult::failure(Error{code, std::move(message)});
}

[[nodiscard]] std::uint32_t level_harmonics(std::size_t level) noexcept {
  return static_cast<std::uint32_t>(max_harmonic >> level);
}

// Four samples per cycle of the top harmonic, so a cubic read between them
// stays clean, but never under 256 or over a full frame.
[[nodiscard]] std::uint32_t level_length(std::uint32_t harmonics) noexcept {
  return std::clamp<std::uint32_t>(std::bit_ceil(4U * harmonics), 256U,
                                   static_cast<std::uint32_t>(table_frame_size));
}

// The inverse transforms of every level length (256..2048), shared by every
// build. dsp::Fft is immutable after construction, so concurrent builds may
// use it.
class LevelTransforms {
public:
  LevelTransforms() {
    for (std::size_t length = 256; length <= table_frame_size; length <<= 1U) {
      transforms_.emplace_back(length);
    }
  }
  [[nodiscard]] const dsp::Fft& of(std::uint32_t length) const noexcept {
    return transforms_[static_cast<std::size_t>(std::countr_zero(length)) - 8];
  }

private:
  std::vector<dsp::Fft> transforms_;
};

const LevelTransforms& level_transforms() {
  static const LevelTransforms transforms;
  return transforms;
}

const dsp::Fft& frame_transform() {
  static const dsp::Fft transform(table_frame_size);
  return transform;
}

void wrap_guards(float* frame, std::size_t length) noexcept {
  for (std::size_t g = 1; g <= table_guard; ++g) {
    frame[-static_cast<std::ptrdiff_t>(g)] = frame[length - g];
    frame[length + g - 1] = frame[g - 1];
  }
}

// Every level of every frame at unit gain. Two frames share each inverse
// transform: both spectra are Hermitian, so frame a lands in the real part
// and frame b in the imaginary part.
std::vector<TableLevel> render_levels(std::span<const Complex> spectra, std::size_t frames) {
  const auto& transforms = level_transforms();
  std::vector<TableLevel> levels(table_levels);
  std::vector<Complex> bins(table_frame_size);
  for (std::size_t index = 0; index < table_levels; ++index) {
    auto& level = levels[index];
    level.harmonics = level_harmonics(index);
    level.length = level_length(level.harmonics);
    level.stride = level.length + 2 * table_guard;
    level.data.assign(frames * level.stride, 0.0F);
    const std::size_t length = level.length;
    const double half = static_cast<double>(length) / 2.0;
    const std::size_t top = std::min<std::size_t>(level.harmonics, length / 2);
    const auto& transform = transforms.of(level.length);
    for (std::size_t frame = 0; frame < frames; frame += 2) {
      const bool pair = frame + 1 < frames;
      const Complex* a = spectra.data() + frame * spectrum_size;
      const Complex* b = pair ? a + spectrum_size : nullptr;
      std::fill_n(bins.begin(), length, Complex{});
      for (std::size_t h = 1; h <= top; ++h) {
        const Complex x = a[h];
        const Complex y = pair ? b[h] : Complex{};
        if (2 * h == length) {
          // The Nyquist bin holds only the cosine part.
          bins[h] = Complex(x.real(), y.real()) * static_cast<double>(length);
          continue;
        }
        const Complex iy(-y.imag(), y.real());
        const Complex iy_conj(y.imag(), y.real());
        bins[h] = (x + iy) * half;
        bins[length - h] = (std::conj(x) + iy_conj) * half;
      }
      transform.inverse(std::span(bins.data(), length));
      float* first = level.data.data() + frame * level.stride + table_guard;
      for (std::size_t n = 0; n < length; ++n) {
        first[n] = static_cast<float>(bins[n].real());
      }
      wrap_guards(first, length);
      if (pair) {
        float* second = first + level.stride;
        for (std::size_t n = 0; n < length; ++n) {
          second[n] = static_cast<float>(bins[n].imag());
        }
        wrap_guards(second, length);
      }
    }
  }
  return levels;
}

// The circular shift d (in cycles) that best aligns frame b with frame a,
// b(x) ~ a(x - d), from the peak of their cross-correlation; 0 when the
// frames are too unlike for a shift to mean anything.
double best_shift(const Complex* a, const Complex* b, std::vector<Complex>& bins) {
  constexpr std::size_t length = table_frame_size;
  std::fill(bins.begin(), bins.end(), Complex{});
  double energy_a = 0.0;
  double energy_b = 0.0;
  for (std::size_t h = 1; h <= max_harmonic; ++h) {
    energy_a += std::norm(a[h]);
    energy_b += std::norm(b[h]);
    const Complex cross = b[h] * std::conj(a[h]);
    if (2 * h == length) {
      bins[h] = Complex(cross.real(), 0.0);
      continue;
    }
    bins[h] = cross;
    bins[length - h] = std::conj(cross);
  }
  if (energy_a <= 0.0 || energy_b <= 0.0) {
    return 0.0;
  }
  frame_transform().inverse(bins);
  std::size_t best = 0;
  for (std::size_t n = 1; n < length; ++n) {
    if (bins[n].real() > bins[best].real()) {
      best = n;
    }
  }
  // r[n] = (2 / N) sum Re(cross e^(i 2 pi h n / N)), bounded by
  // (2 / N) sqrt(Ea Eb).
  const double correlation =
      bins[best].real() * static_cast<double>(length) / 2.0 / std::sqrt(energy_a * energy_b);
  if (best == 0 || correlation < min_shift_correlation ||
      bins[best].real() <= bins[0].real() * (1.0 + 1e-9)) {
    return 0.0;
  }
  const auto signed_best = best > length / 2
                               ? static_cast<double>(best) - static_cast<double>(length)
                               : static_cast<double>(best);
  return signed_best / static_cast<double>(length);
}

// One source pair's morph: per harmonic, the two magnitudes, the start phase
// and the total phase travel from a to b. The travel is the pair's common
// shift plus each harmonic's shortest residual, so a feature that moves
// between frames moves in the morph rather than fading out in one place and
// in at another (which is what smears and combs a plain crossfade).
struct PairMorph {
  std::vector<double> from_magnitude;
  std::vector<double> to_magnitude;
  std::vector<double> from_phase;
  std::vector<double> travel;

  PairMorph()
      : from_magnitude(spectrum_size), to_magnitude(spectrum_size), from_phase(spectrum_size),
        travel(spectrum_size) {}

  void prepare(const Complex* a, const Complex* b, double shift) {
    for (std::size_t h = 1; h <= max_harmonic; ++h) {
      const double magnitude_a = std::abs(a[h]);
      const double magnitude_b = std::abs(b[h]);
      const double expected = -two_pi * static_cast<double>(h) * shift;
      // A silent end has no phase of its own: it borrows the other end's,
      // moved by the common shift, so the audible end never spins.
      double phase_a = std::arg(a[h]);
      double phase_b = std::arg(b[h]);
      const double floor = 1e-12 * (magnitude_a + magnitude_b);
      if (magnitude_a <= floor) {
        phase_a = phase_b - expected;
      } else if (magnitude_b <= floor) {
        phase_b = phase_a + expected;
      }
      from_magnitude[h] = magnitude_a;
      to_magnitude[h] = magnitude_b;
      from_phase[h] = phase_a;
      travel[h] = expected + std::remainder(phase_b - phase_a - expected, two_pi);
    }
  }

  void at(double mix, Complex* out) const noexcept {
    out[0] = Complex{};
    for (std::size_t h = 1; h <= max_harmonic; ++h) {
      const double magnitude = from_magnitude[h] + mix * (to_magnitude[h] - from_magnitude[h]);
      const double phase = from_phase[h] + mix * travel[h];
      out[h] = Complex(magnitude * std::cos(phase), magnitude * std::sin(phase));
    }
  }
};

// max_table_frames spectra morphed across the source frames: spectral frame
// j sits at source position j (frames - 1) / (max_table_frames - 1).
std::vector<Complex> morph_spectra(std::span<const Complex> spectra, std::size_t frames) {
  std::vector<Complex> out(max_table_frames * spectrum_size);
  std::vector<Complex> bins(table_frame_size);
  PairMorph pair;
  std::size_t prepared = frames;
  for (std::size_t j = 0; j < max_table_frames; ++j) {
    const double position = static_cast<double>(j) * static_cast<double>(frames - 1) /
                            static_cast<double>(max_table_frames - 1);
    const auto from = std::min(frames - 2, static_cast<std::size_t>(position));
    const double mix = std::clamp(position - static_cast<double>(from), 0.0, 1.0);
    if (from != prepared) {
      const Complex* a = spectra.data() + from * spectrum_size;
      const Complex* b = a + spectrum_size;
      pair.prepare(a, b, best_shift(a, b, bins));
      prepared = from;
    }
    pair.at(mix, out.data() + j * spectrum_size);
  }
  return out;
}

// Normalises, renders and assembles a table from frame spectra (DC already
// zero).
TableResult assemble(std::vector<Complex> spectra, std::size_t frames, std::string name) {
  auto levels = render_levels(spectra, frames);
  float peak = 0.0F;
  for (const float value : levels.front().data) {
    peak = std::max(peak, std::fabs(value));
  }
  if (!std::isfinite(peak)) {
    return failure(ErrorCode::corrupt_data, "wavetable '" + name + "' is not finite");
  }
  if (peak <= 1e-9F) {
    return failure(ErrorCode::invalid_argument, "wavetable '" + name + "' is silent");
  }
  const float gain = 1.0F / peak;
  for (auto& level : levels) {
    for (auto& value : level.data) {
      value *= gain;
    }
  }
  std::unique_ptr<const Wavetable> spectral;
  if (frames == max_table_frames) {
    // Already at full resolution: the morph would land on the frames.
    spectral = std::make_unique<const Wavetable>(name, frames, levels, nullptr);
  } else if (frames > 1) {
    for (auto& value : spectra) {
      value *= static_cast<double>(gain);
    }
    auto morphed = morph_spectra(spectra, frames);
    spectral = std::make_unique<const Wavetable>(name, max_table_frames,
                                                 render_levels(morphed, max_table_frames), nullptr);
  }
  return TableResult::success(std::make_shared<const Wavetable>(
      std::move(name), frames, std::move(levels), std::move(spectral)));
}

// The harmonics of one frame of any length: a plain FFT for powers of two,
// Bluestein's chirp transform otherwise. Built once per source.
class FrameAnalyser {
public:
  explicit FrameAnalyser(std::size_t length)
      : length_(length), direct_(std::has_single_bit(length)),
        transform_(direct_ ? length : std::bit_ceil(2 * length - 1)), work_(transform_.size()) {
    if (direct_) {
      return;
    }
    // chirp[n] = e^(i pi n^2 / L); n^2 is reduced mod 2L to keep the angle
    // exact for long frames.
    chirp_.resize(length);
    const auto period = static_cast<std::uint64_t>(2 * length);
    for (std::size_t n = 0; n < length; ++n) {
      const auto square = (static_cast<std::uint64_t>(n) * n) % period;
      chirp_[n] = std::polar(1.0, pi * static_cast<double>(square) / static_cast<double>(length));
    }
    kernel_.assign(transform_.size(), Complex{});
    kernel_[0] = chirp_[0];
    for (std::size_t n = 1; n < length; ++n) {
      kernel_[n] = chirp_[n];
      kernel_[transform_.size() - n] = chirp_[n];
    }
    transform_.forward(kernel_);
  }

  // spectrum gets spectrum_size entries.
  void analyse(std::span<const float> frame, Complex* spectrum) {
    std::fill(work_.begin(), work_.end(), Complex{});
    if (direct_) {
      for (std::size_t n = 0; n < length_; ++n) {
        work_[n] = frame[n];
      }
      transform_.forward(work_);
    } else {
      for (std::size_t n = 0; n < length_; ++n) {
        work_[n] = static_cast<double>(frame[n]) * std::conj(chirp_[n]);
      }
      transform_.forward(work_);
      for (std::size_t k = 0; k < work_.size(); ++k) {
        work_[k] *= kernel_[k];
      }
      transform_.inverse(work_);
      for (std::size_t k = 0; k <= std::min(max_harmonic, length_ / 2); ++k) {
        work_[k] *= std::conj(chirp_[k]);
      }
    }
    const auto length = static_cast<double>(length_);
    const std::size_t top = std::min(max_harmonic, length_ / 2);
    spectrum[0] = Complex{};
    for (std::size_t h = 1; h < spectrum_size; ++h) {
      if (h > top) {
        spectrum[h] = Complex{};
      } else if (2 * h == length_) {
        spectrum[h] = Complex(work_[h].real() / length, 0.0);
      } else {
        spectrum[h] = 2.0 * work_[h] / length;
      }
    }
  }

private:
  std::size_t length_;
  bool direct_;
  dsp::Fft transform_;
  std::vector<Complex> work_;
  std::vector<Complex> chirp_;
  std::vector<Complex> kernel_;
};

} // namespace

std::size_t Wavetable::level_for(double cycles_per_sample) const noexcept {
  if (levels_.empty()) {
    return 0;
  }
  const double rate = std::fabs(cycles_per_sample);
  if (std::isnan(rate)) {
    return levels_.size() - 1;
  }
  for (std::size_t index = 0; index + 1 < levels_.size(); ++index) {
    if (static_cast<double>(levels_[index].harmonics) * rate < band_edge) {
      return index;
    }
  }
  return levels_.size() - 1;
}

foundation::Result<std::shared_ptr<const Wavetable>>
build_wavetable(std::span<const float> mono, std::size_t frame_length, std::string name) {
  if (mono.empty()) {
    return failure(ErrorCode::invalid_argument, "wavetable '" + name + "' has no audio");
  }
  const std::size_t length = frame_length == 0 ? mono.size() : frame_length;
  if (length < 4) {
    return failure(ErrorCode::invalid_argument,
                   "wavetable '" + name + "' frames must be at least 4 samples");
  }
  if (length > max_frame_length) {
    return failure(ErrorCode::invalid_argument,
                   "wavetable '" + name + "' frame is longer than 2^20 samples");
  }
  const std::size_t available = mono.size() / length;
  if (available == 0) {
    return failure(ErrorCode::invalid_argument,
                   "wavetable '" + name + "' audio is shorter than one frame");
  }
  const std::size_t frames = std::min(available, max_table_frames);
  const auto used = mono.first(available * length);
  if (!std::all_of(used.begin(), used.end(), [](float value) { return std::isfinite(value); })) {
    return failure(ErrorCode::corrupt_data, "wavetable '" + name + "' audio is not finite");
  }
  FrameAnalyser analyser(length);
  std::vector<Complex> spectra(frames * spectrum_size);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    // More frames than fit are thinned evenly, keeping the first and last.
    const std::size_t source =
        available <= max_table_frames
            ? frame
            : (frame * (available - 1) + (max_table_frames - 1) / 2) / (max_table_frames - 1);
    analyser.analyse(used.subspan(source * length, length), spectra.data() + frame * spectrum_size);
  }
  return assemble(std::move(spectra), frames, std::move(name));
}

// A spectra frame is sum m[h] sin(2 pi h x + phase[h]): phase 0 is a sine
// starting at zero. In the cosine form used inside, c = m e^(i (phase - pi/2)).
foundation::Result<std::shared_ptr<const Wavetable>>
build_wavetable_from_spectra(std::span<const std::vector<float>> magnitudes,
                             std::span<const std::vector<float>> phases, std::string name) {
  if (magnitudes.empty() || magnitudes.size() != phases.size()) {
    return failure(ErrorCode::invalid_argument,
                   "wavetable '" + name + "' needs one magnitude and phase set per frame");
  }
  if (magnitudes.size() > max_table_frames) {
    return failure(ErrorCode::invalid_argument,
                   "wavetable '" + name + "' has more than 256 frames");
  }
  const std::size_t frames = magnitudes.size();
  std::vector<Complex> spectra(frames * spectrum_size);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto& m = magnitudes[frame];
    const auto& p = phases[frame];
    if (m.size() != p.size() || m.size() > max_harmonic) {
      return failure(ErrorCode::invalid_argument,
                     "wavetable '" + name + "' frame " + std::to_string(frame) +
                         " needs matching magnitudes and phases, at most 1024");
    }
    Complex* spectrum = spectra.data() + frame * spectrum_size;
    for (std::size_t index = 0; index < m.size(); ++index) {
      const double magnitude = m[index];
      const double phase = p[index];
      if (!std::isfinite(magnitude) || !std::isfinite(phase)) {
        return failure(ErrorCode::corrupt_data, "wavetable '" + name + "' spectra are not finite");
      }
      spectrum[index + 1] = Complex(magnitude * std::sin(phase), -magnitude * std::cos(phase));
    }
  }
  return assemble(std::move(spectra), frames, std::move(name));
}

} // namespace oscillate::native_dsp::synth::nova
