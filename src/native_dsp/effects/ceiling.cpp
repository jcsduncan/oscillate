#include "oscillate/native_dsp/effects/ceiling.hpp"

#include "ceiling_presets.hpp"
#include "ceiling_table.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "reduction_analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oscillate::native_dsp::effects::ceiling {
namespace {

using ceiling_table::index_of;

namespace p {
constexpr std::size_t gain = index_of("ceiling.gain");
constexpr std::size_t ceiling = index_of("ceiling.ceiling");
constexpr std::size_t lookahead = index_of("ceiling.lookahead");
constexpr std::size_t release = index_of("ceiling.release");
} // namespace p

constexpr double hold_seconds = 1.0;

using native_dsp::dsp::gain_to_db;

class CeilingKernel final : public EffectKernel {
public:
  // Control only: the classic runtime allocates its lookahead delay here.
  CeilingKernel(double sample_rate, PreparedLimiter prepared)
      : rate_(dsp::classic_rate(sample_rate)),
        hold_(std::exp(std::log(1e-3) / (hold_seconds * sample_rate))), limiter_(prepared),
        offered_(prepared.settings()) {
    bands_.prepare(sample_rate);
  }

  // Only a change of ceiling or release reaches the classic runtime, which
  // adopts an offer outright at its next block (so before the first block it
  // is as if created with it). The lookahead stays the instance's (its
  // latency): the classic runtime refuses any other.
  // NOLINTNEXTLINE(bugprone-exception-escape): only bad_alloc can escape preparing; fatal here.
  void control(std::span<const double> v, std::uint32_t, const SynthTransport&) noexcept override {
    gain_ = dsp::db_to_gain(v[p::gain]);
    LimiterSettings wanted = settings_for(v);
    wanted.lookahead_ms = offered_.lookahead_ms;
    if (wanted.ceiling_db == offered_.ceiling_db && wanted.release_ms == offered_.release_ms) {
      return;
    }
    auto prepared = prepare_limiter(wanted, rate_);
    if (prepared && limiter_.offer_prepared(prepared.value(), revision_ + 1)) {
      ++revision_;
      offered_ = wanted;
    }
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    meter(input_, audio);
    drive(audio);
    // The limiter's detector reads the driven mix.
    bands_.analyse(audio);
    // The classic runtime reads only the state generation from its context,
    // and silences generation 0.
    limiter_.process(audio, engine::BlockContext{0, generation_});
    meter(output_, audio);
    reduction_ = std::max(reduction_ * std::pow(hold_, static_cast<double>(audio.size())),
                          static_cast<double>(limiter_.latest_gain_reduction_db()));
    bands_.settle(limiter_.latest_gain_reduction_db(), audio.size());
  }

  // A new generation clears the delay line, peak window and gain at the
  // next block, as a seek does for the classic limiter.
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
  // Gain into the limiter, ramped across the block from the last block's
  // gain so a turned knob never clicks. IN meters the mix before it.
  void drive(std::span<engine::StereoFrame> audio) noexcept {
    if (gain_ == applied_gain_ && gain_ == 1.0)
      return;
    const double step =
        audio.empty() ? 0.0 : (gain_ - applied_gain_) / static_cast<double>(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const auto g = static_cast<float>(applied_gain_ + step * static_cast<double>(i + 1));
      audio[i].left *= g;
      audio[i].right *= g;
    }
    applied_gain_ = gain_;
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
  LimiterRuntime limiter_;
  LimiterSettings offered_;
  double gain_ = 1.0;
  double applied_gain_ = 1.0;
  std::uint64_t revision_ = 0;
  std::uint64_t generation_ = 1;
  std::array<double, 2> input_{};
  std::array<double, 2> output_{};
  double reduction_ = 0.0;
  dsp::ReductionBands bands_;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  auto prepared = prepare_limiter(settings_for(values), dsp::classic_rate(sample_rate));
  if (!prepared) {
    return nullptr;
  }
  return std::make_unique<CeilingKernel>(sample_rate, prepared.value());
}

// The classic limiter's: the delayed frames are its latency, which the host
// accounts separately, and its output stops with them.
std::uint64_t tail_frames(std::span<const double>, double) { return 0; }

// The lookahead, rounded as the classic limiter rounds it.
std::uint32_t latency_frames(std::span<const double> values, double sample_rate) {
  const auto prepared = prepare_limiter(settings_for(values), dsp::classic_rate(sample_rate));
  return prepared ? prepared.value().latency_frames() : 0;
}

const synth::EngineDescriptor descriptor{"ceiling", "Ceiling", 1, ceiling_table::parameters,
                                         ceiling_presets::presets};
const EffectEngine ceiling_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames,
                                  false};

} // namespace

LimiterSettings settings_for(std::span<const double> v) noexcept {
  return {v[p::ceiling], v[p::lookahead], v[p::release]};
}

const EffectEngine& engine() { return ceiling_engine; }

} // namespace oscillate::native_dsp::effects::ceiling
