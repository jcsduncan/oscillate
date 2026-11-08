#include "oscillate/native_dsp/effects/press.hpp"

#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "press_presets.hpp"
#include "press_table.hpp"
#include "reduction_analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::native_dsp::effects::press {
namespace {

using press_table::index_of;

namespace p {
constexpr std::size_t threshold = index_of("press.threshold");
constexpr std::size_t ratio = index_of("press.ratio");
constexpr std::size_t knee = index_of("press.knee");
constexpr std::size_t attack = index_of("press.attack");
constexpr std::size_t release = index_of("press.release");
constexpr std::size_t makeup = index_of("press.makeup");
constexpr std::size_t detector_hp = index_of("press.detector_hp");
constexpr std::size_t detector_freq = index_of("press.detector_freq");
} // namespace p

constexpr double hold_seconds = 1.0;

[[nodiscard]] bool same(const CompressorSettings& a, const CompressorSettings& b) noexcept {
  return a.threshold_db == b.threshold_db && a.ratio == b.ratio && a.knee_db == b.knee_db &&
         a.attack_ms == b.attack_ms && a.release_ms == b.release_ms && a.makeup_db == b.makeup_db &&
         a.sidechain_highpass_hz == b.sidechain_highpass_hz;
}

using native_dsp::dsp::gain_to_db;

class PressKernel final : public EffectKernel {
public:
  PressKernel(double sample_rate, PreparedCompressor prepared)
      : rate_(dsp::classic_rate(sample_rate)),
        hold_(std::exp(std::log(1e-3) / (hold_seconds * sample_rate))), compressor_(prepared),
        offered_(prepared.settings()) {
    bands_.prepare(sample_rate);
    weigh(offered_);
  }

  // Only a change of the classic settings reaches the classic runtime. Before
  // the first block (a lane's first value may differ from the creation
  // values) they are adopted outright, as if created with them; later ones
  // are offered, and the runtime crossfades its gain over 64 frames. A
  // refused offer (its mailbox full) is retried next tick.
  // NOLINTNEXTLINE(bugprone-exception-escape): only bad_alloc can escape preparing; fatal here.
  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    const CompressorSettings wanted = settings_for(v);
    if (same(wanted, offered_)) {
      return;
    }
    // In range by construction (the table is the classic's), at a rate the
    // classic compressor admitted at creation.
    auto prepared = prepare_compressor(wanted, rate_);
    if (!prepared) {
      return;
    }
    if (!primed_) {
      compressor_.replace_prepared_while_held(prepared.value());
      offered_ = wanted;
    } else if (compressor_.offer_prepared(prepared.value(), revision_ + 1)) {
      ++revision_;
      offered_ = wanted;
    }
    weigh(offered_);
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    primed_ = true;
    meter(input_, audio);
    // What the detector hears: the key when there is one.
    bands_.analyse(sidechain.empty() ? std::span<const engine::StereoFrame>(audio) : sidechain);
    // The classic runtime reads only the state generation from its context.
    const engine::BlockContext context{0, generation_};
    if (sidechain.empty()) {
      compressor_.process(audio, context);
    } else {
      compressor_.process_with_sidechain(audio, sidechain, context);
    }
    meter(output_, audio);
    reduction_ = std::max(reduction_ * std::pow(hold_, static_cast<double>(audio.size())),
                          static_cast<double>(compressor_.latest_gain_reduction_db()));
    bands_.settle(compressor_.latest_gain_reduction_db(), audio.size());
  }

  // A new generation clears the detector envelope and filter at the next
  // block, as a seek does for the classic compressor.
  void reset() noexcept override {
    ++generation_;
    input_ = {};
    output_ = {};
    reduction_ = 0.0;
    bands_.reset();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] = static_cast<float>(gain_to_db(input_[c]));
      out[telemetry::output_peak + c] = static_cast<float>(gain_to_db(output_[c]));
    }
    out[telemetry::reduction] = static_cast<float>(reduction_);
    bands_.write(out.subspan(telemetry::reduction_bands, reduction_bands::count));
    return telemetry::size;
  }

private:
  // The detector high-pass (first order) passes (f/fc)^2 / (1 + (f/fc)^2)
  // of the power at f: each band is weighed at its centre.
  void weigh(const CompressorSettings& settings) noexcept {
    std::array<double, reduction_bands::count> weights{};
    weights.fill(1.0);
    if (settings.sidechain_highpass_hz != 0.0) {
      for (std::size_t band = 0; band < weights.size(); ++band) {
        const double ratio = reduction_bands::centre_hz(band) / settings.sidechain_highpass_hz;
        weights[band] = ratio * ratio / (1.0 + ratio * ratio);
      }
    }
    bands_.set_weights(weights);
  }

  void meter(std::array<double, 2>& peak,
             std::span<const engine::StereoFrame> audio) const noexcept {
    for (const auto& frame : audio) {
      // Non-finite input never sticks: max keeps the held value over NaN.
      peak[0] = std::max(peak[0] * hold_, std::fabs(static_cast<double>(frame.left)));
      peak[1] = std::max(peak[1] * hold_, std::fabs(static_cast<double>(frame.right)));
    }
  }

  std::uint32_t rate_;
  double hold_;
  CompressorRuntime compressor_;
  CompressorSettings offered_;
  std::uint64_t revision_ = 0;
  std::uint64_t generation_ = 1;
  std::array<double, 2> input_{};
  std::array<double, 2> output_{};
  double reduction_ = 0.0;
  bool primed_ = false;
  dsp::ReductionBands bands_;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  auto prepared = prepare_compressor(settings_for(values), dsp::classic_rate(sample_rate));
  if (!prepared) {
    return nullptr;
  }
  return std::make_unique<PressKernel>(sample_rate, prepared.value());
}

// The classic compressor's: its output stops with its input.
std::uint64_t tail_frames(std::span<const double>, double) { return 0; }
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"press", "Press", 1, press_table::parameters,
                                         press_presets::presets};
const EffectEngine press_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

CompressorSettings settings_for(std::span<const double> v) noexcept {
  return {v[p::threshold],
          v[p::ratio],
          v[p::knee],
          v[p::attack],
          v[p::release],
          v[p::makeup],
          v[p::detector_hp] != 0.0 ? v[p::detector_freq] : 0.0};
}

const EffectEngine& engine() { return press_engine; }

} // namespace oscillate::native_dsp::effects::press
