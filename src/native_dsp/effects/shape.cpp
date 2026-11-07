#include "oscillate/native_dsp/effects/shape.hpp"

#include "dsp/fft.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "shape_presets.hpp"
#include "shape_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::effects::shape {
namespace {

using shape_table::per_band;

constexpr double peak_hold_seconds = 1.0;
constexpr double max_tail_seconds = 10.0;

// Holds the classic cascade open once every band is switched off live: its
// runtime cannot remove the last section. A 0 dB peak is unity (b = a
// exactly), so once its history has rung out its output is the input exactly.
constexpr EqBandSettings unity_band{EqBandKind::peaking, 1'000.0, 0.707, 0.0};
constexpr int unity = -1;

using Bands = std::array<BandStages, bands>;

// One section of the classic cascade: stage `stage` of table band `band`
// (`unity` for the placeholder), and the settings it runs.
struct Section {
  int band = unity;
  std::size_t stage = 0;
  EqBandSettings settings = unity_band;
};

struct Cascade {
  std::array<Section, eq_section_limit> section{};
  std::size_t size = 0;
};

[[nodiscard]] bool same(const EqBandSettings& a, const EqBandSettings& b) noexcept {
  return a.kind == b.kind && a.frequency_hz == b.frequency_hz && a.shape == b.shape &&
         a.gain_db == b.gain_db;
}

[[nodiscard]] Bands bands_for(std::span<const double> values, double sample_rate) noexcept {
  Bands out;
  for (std::size_t b = 0; b < bands; ++b) {
    out[b] = band_stages(values, b, sample_rate);
  }
  return out;
}

// The stages of the bands that are on, in band order: the classic EQ's own
// order.
[[nodiscard]] Cascade canonical(const Bands& on) noexcept {
  Cascade cascade;
  for (std::size_t b = 0; b < bands; ++b) {
    for (std::size_t s = 0; s < on[b].size; ++s) {
      cascade.section[cascade.size++] = {static_cast<int>(b), s, on[b].stage[s]};
    }
  }
  return cascade;
}

// The table's ranges, brought into the classic EQ's by band_stages, are
// stable at every admitted rate, so this fails only for a rate the classic
// EQ refuses.
[[nodiscard]] foundation::Result<PreparedEq> prepare(const Cascade& cascade, std::uint32_t rate) {
  std::array<EqBandSettings, eq_section_limit> list{};
  for (std::size_t i = 0; i < cascade.size; ++i) {
    list[i] = cascade.section[i].settings;
  }
  return prepare_eq(std::span<const EqBandSettings>(list).first(cascade.size), rate);
}

using native_dsp::dsp::gain_to_db;

// A display spectrum of a stereo signal, as an EQ draws behind its curve:
// the mono sum through a 4-term Blackman-Harris window (longer at higher
// rates, so the low end keeps its resolution), every quarter window, as
// power per log band. A band narrower than a bin reads the nearest bin.
// Powers are averaged over about 250 ms. Allocates only on construction.
class DisplaySpectrum {
public:
  explicit DisplaySpectrum(double rate)
      : rate_(rate), size_(rate > 100'000.0  ? 16'384
                           : rate > 50'000.0 ? 8'192
                                             : 4'096),
        hop_(size_ / 4), fft_(size_), window_(size_), ring_(size_, 0.0F), data_(size_) {
    double sum = 0.0;
    for (std::size_t i = 0; i < size_; ++i) {
      const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size_);
      window_[i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) -
                   0.01168 * std::cos(3.0 * x);
      sum += window_[i];
    }
    // A full-scale sine reads 0 dBFS at its bin.
    scale_ = 2.0 / sum;
    const double seconds_per_hop = static_cast<double>(hop_) / rate_;
    alpha_ = 1.0 - std::exp(-seconds_per_hop / 0.25);
    for (std::size_t b = 0; b <= spectrum_bands; ++b)
      edges_[b] = 20.0 * std::pow(1'000.0, static_cast<double>(b) / spectrum_bands);
    power_.fill(0.0);
  }

  void push(std::span<const engine::StereoFrame> audio) noexcept {
    for (const auto& frame : audio) {
      ring_[write_] = 0.5F * (frame.left + frame.right);
      write_ = (write_ + 1) % size_;
      if (++since_ == hop_) {
        since_ = 0;
        analyse();
      }
    }
  }
  void reset() noexcept {
    std::ranges::fill(ring_, 0.0F);
    power_.fill(0.0);
    since_ = 0;
  }
  [[nodiscard]] double db(std::size_t band) const noexcept {
    return power_[band] > 1e-20 ? 10.0 * std::log10(power_[band]) : -200.0;
  }

private:
  void analyse() noexcept {
    for (std::size_t i = 0; i < size_; ++i)
      data_[i] = {ring_[(write_ + i) % size_] * window_[i], 0.0};
    fft_.forward(data_);
    const double bin_hz = rate_ / static_cast<double>(size_);
    const auto bin_power = [&](std::size_t k) {
      return std::norm(data_[std::min(k, size_ / 2)]) * scale_ * scale_;
    };
    for (std::size_t b = 0; b < spectrum_bands; ++b) {
      const auto low = static_cast<std::size_t>(std::ceil(edges_[b] / bin_hz));
      const auto high = static_cast<std::size_t>(std::floor(edges_[b + 1] / bin_hz));
      double power = 0.0;
      if (high >= low) {
        for (std::size_t k = low; k <= std::min(high, size_ / 2); ++k)
          power = std::max(power, bin_power(k));
      } else {
        const double centre = std::sqrt(edges_[b] * edges_[b + 1]) / bin_hz;
        power = bin_power(static_cast<std::size_t>(std::lround(centre)));
      }
      power_[b] += alpha_ * (power - power_[b]);
    }
  }

  double rate_;
  std::size_t size_;
  std::size_t hop_;
  native_dsp::dsp::Fft fft_;
  std::vector<double> window_;
  std::vector<float> ring_;
  std::vector<std::complex<double>> data_;
  std::array<double, spectrum_bands + 1> edges_{};
  std::array<double, spectrum_bands> power_{};
  double scale_ = 1.0;
  double alpha_ = 1.0;
  std::size_t write_ = 0;
  std::size_t since_ = 0;
};

class ShapeKernel final : public EffectKernel {
public:
  ShapeKernel(double sample_rate, const Bands& initial, PreparedEq prepared)
      : sample_rate_(sample_rate), rate_(dsp::classic_rate(sample_rate)),
        hold_(std::exp(std::log(1e-3) / (peak_hold_seconds * sample_rate))),
        eq_(std::in_place, prepared), cascade_(canonical(initial)), wanted_(initial),
        input_spectrum_(sample_rate), output_spectrum_(sample_rate) {}

  void control(std::span<const double> v, std::uint32_t, const SynthTransport&) noexcept override {
    for (std::size_t b = 0; b < bands; ++b) {
      wanted_[b] = band_stages(v, b, sample_rate_);
    }
    if (!primed_) {
      // Before the first block (a lane's first value may differ from the
      // creation values) there is no history: build as if created with them.
      if (!settled()) {
        rebuild();
      }
      return;
    }
    advance();
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    primed_ = true;
    meter(input_, audio);
    input_spectrum_.push(audio);
    // The classic runtime reads only the state generation from its context.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): eq_ is engaged from construction.
    eq_->process(audio, engine::BlockContext{0, generation_});
    meter(output_, audio);
    output_spectrum_.push(audio);
  }

  void reset() noexcept override {
    // History is cleared anyway: rebuild the cascade in band order for the
    // wanted bands, which also ends a live reordering or the placeholder.
    rebuild();
    input_ = {};
    output_ = {};
    input_spectrum_.reset();
    output_spectrum_.reset();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] = static_cast<float>(gain_to_db(input_[c]));
      out[telemetry::output_peak + c] = static_cast<float>(gain_to_db(output_[c]));
    }
    for (std::size_t b = 0; b < spectrum_bands; ++b) {
      out[telemetry::input_spectrum + b] = static_cast<float>(input_spectrum_.db(b));
      out[telemetry::output_spectrum + b] = static_cast<float>(output_spectrum_.db(b));
    }
    return telemetry::size;
  }

private:
  // Whether a section runs (or is transitioning to) a wanted stage.
  [[nodiscard]] bool wanted(const Section& section) const noexcept {
    return section.band != unity &&
           section.stage < wanted_[static_cast<std::size_t>(section.band)].size;
  }

  [[nodiscard]] bool present(std::size_t band, std::size_t stage) const noexcept {
    for (std::size_t i = 0; i < cascade_.size; ++i) {
      if (cascade_.section[i].band == static_cast<int>(band) &&
          cascade_.section[i].stage == stage) {
        return true;
      }
    }
    return false;
  }

  // The cascade runs exactly the wanted stages, at their wanted settings.
  [[nodiscard]] bool settled() const noexcept {
    std::size_t total = 0;
    for (const auto& band : wanted_) {
      total += band.size;
    }
    std::size_t running = 0;
    for (std::size_t i = 0; i < cascade_.size; ++i) {
      const auto& section = cascade_.section[i];
      if (section.band == unity) {
        continue;
      }
      if (!wanted(section) ||
          !same(section.settings,
                wanted_[static_cast<std::size_t>(section.band)].stage[section.stage])) {
        return false;
      }
      ++running;
    }
    return running == total;
  }

  // A fresh classic runtime for the wanted stages in band order, with no
  // history.
  // NOLINTNEXTLINE(bugprone-exception-escape): only bad_alloc can escape preparing; fatal here.
  void rebuild() noexcept {
    const Cascade fresh = canonical(wanted_);
    auto prepared = prepare(fresh, rate_);
    if (prepared) {
      eq_.emplace(prepared.value());
      cascade_ = fresh;
      revision_ = 0;
    }
  }

  // One step towards the wanted stages per control tick, through the classic
  // runtime's mailbox: a stage no longer wanted (its band switched off, its
  // slope lowered) fades out of the cascade, a stage newly wanted is appended
  // to it (a cascade of biquads is order-free), and otherwise the
  // coefficients glide. A refused offer (the previous change still
  // transitioning) is retried at the next tick.
  // NOLINTNEXTLINE(bugprone-exception-escape): only bad_alloc can escape preparing; fatal here.
  void advance() noexcept {
    std::optional<std::size_t> removed; // a cascade position
    for (std::size_t i = 0; i < cascade_.size && !removed; ++i) {
      if (cascade_.section[i].band != unity && !wanted(cascade_.section[i])) {
        removed = i;
      }
    }
    std::optional<Section> added;
    for (std::size_t b = 0; b < bands && !removed && !added; ++b) {
      for (std::size_t s = 0; s < wanted_[b].size && !added; ++s) {
        if (!present(b, s)) {
          added = Section{static_cast<int>(b), s, wanted_[b].stage[s]};
        }
      }
    }
    Cascade next = cascade_;
    std::optional<std::size_t> changed; // the position of a structural change
    if (removed) {
      if (cascade_.size == 1) {
        next.section[0] = Section{};
      } else {
        std::copy(next.section.begin() + static_cast<std::ptrdiff_t>(*removed) + 1,
                  next.section.begin() + static_cast<std::ptrdiff_t>(next.size),
                  next.section.begin() + static_cast<std::ptrdiff_t>(*removed));
        --next.size;
        changed = removed;
      }
    } else if (added) {
      if (cascade_.size == 1 && cascade_.section[0].band == unity) {
        next.section[0] = *added;
      } else {
        changed = next.size;
        next.section[next.size++] = *added;
      }
    } else if (settled()) {
      return;
    }
    // The stages that stay move to their wanted settings in the same change.
    for (std::size_t i = 0; i < next.size; ++i) {
      auto& section = next.section[i];
      if (wanted(section)) {
        section.settings = wanted_[static_cast<std::size_t>(section.band)].stage[section.stage];
      }
    }
    auto prepared = prepare(next, rate_);
    if (!prepared) {
      return;
    }
    const std::uint64_t revision = revision_ + 1;
    // NOLINTBEGIN(bugprone-unchecked-optional-access): eq_ is engaged from construction.
    const bool offered = changed ? eq_->offer_band_change(prepared.value(), *changed, revision)
                                 : eq_->offer_prepared(prepared.value(), revision);
    // NOLINTEND(bugprone-unchecked-optional-access)
    if (!offered) {
      return;
    }
    revision_ = revision;
    cascade_ = next;
  }

  void meter(std::array<double, 2>& peak,
             std::span<const engine::StereoFrame> audio) const noexcept {
    for (const auto& frame : audio) {
      // Non-finite input never sticks: max keeps the held value over NaN.
      peak[0] = std::max(peak[0] * hold_, std::fabs(static_cast<double>(frame.left)));
      peak[1] = std::max(peak[1] * hold_, std::fabs(static_cast<double>(frame.right)));
    }
  }

  double sample_rate_;
  std::uint32_t rate_;
  double hold_;
  std::optional<EqRuntime> eq_;
  // What the classic runtime runs (or is transitioning to), per section.
  Cascade cascade_;
  Bands wanted_;
  std::uint64_t revision_ = 0;
  std::uint64_t generation_ = 1;
  bool primed_ = false;
  std::array<double, 2> input_{};
  std::array<double, 2> output_{};
  DisplaySpectrum input_spectrum_;
  DisplaySpectrum output_spectrum_;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  const Bands initial = bands_for(values, sample_rate);
  auto prepared = prepare(canonical(initial), dsp::classic_rate(sample_rate));
  if (!prepared) {
    return nullptr;
  }
  return std::make_unique<ShapeKernel>(sample_rate, initial, prepared.value());
}

// The classic EQ's own estimate (about -180 dB per section). It refuses to
// bound a tail past ten seconds; an engine always reports a bound, so such a
// setting reports that ten-second budget.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const Bands on = bands_for(values, sample_rate);
  auto prepared = prepare(canonical(on), dsp::classic_rate(sample_rate));
  if (!prepared) {
    return 0;
  }
  const auto tail = estimate_eq_tail_frames(prepared.value());
  return tail ? tail.value()
              : static_cast<std::uint64_t>(std::ceil(max_tail_seconds * sample_rate));
}
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"shape", "Shape", 1, shape_table::parameters,
                                         shape_presets::presets};
const EffectEngine shape_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

BandStages band_stages(std::span<const double> values, std::size_t band,
                       double sample_rate) noexcept {
  const std::size_t base = band * per_band;
  BandStages out;
  if (band >= bands || values.size() < base + per_band || values[base + shape_table::on] == 0.0) {
    return out;
  }
  const auto kind =
      static_cast<EqBandKind>(std::clamp(static_cast<int>(values[base + shape_table::kind]), 0,
                                         static_cast<int>(EqBandKind::peaking)));
  const bool shelf = kind == EqBandKind::low_shelf || kind == EqBandKind::high_shelf;
  const bool pass = kind == EqBandKind::low_pass || kind == EqBandKind::high_pass;
  const double top = std::min(20'000.0, native_dsp::dsp::nyquist_guard * sample_rate);
  const double frequency = std::min(std::max(values[base + shape_table::freq], 10.0), top);
  const double q = values[base + shape_table::q];
  if (pass) {
    const auto slope =
        static_cast<std::size_t>(std::clamp(static_cast<int>(values[base + shape_table::slope]), 0,
                                            static_cast<int>(shape_table::slopes.size()) - 1));
    out.size = eq_pass_stages(kind, frequency, q, shape_table::slope_stages[slope], out.stage);
    return out;
  }
  out.stage[0] = EqBandSettings{kind, frequency, shelf ? std::min(q, 1.0) : q,
                                values[base + shape_table::gain]};
  out.size = 1;
  return out;
}

const EffectEngine& engine() { return shape_engine; }

} // namespace oscillate::native_dsp::effects::shape
