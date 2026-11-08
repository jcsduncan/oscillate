#include "native_effect_visuals.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/contour.hpp"
#include "oscillate/native_dsp/effects/edge.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/effects/strata.hpp"
#include "oscillate/native_dsp/eq.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <vector>

namespace oscillate::quick {

std::vector<double> contour_response(const native_dsp::synth::EngineDescriptor& engine,
                                     std::size_t points, std::span<const double> values,
                                     double cutoff_hz) {
  namespace synth = native_dsp::synth;
  namespace effects = native_dsp::effects;
  const auto& contour = effects::contour::engine();
  if (engine.id != contour.descriptor->id || values.size() != engine.parameters.size() ||
      points < 2) {
    return {};
  }
  std::vector<double> patch(values.begin(), values.end());
  const auto set = [&](std::string_view id, double value) {
    if (const auto index = synth::find_parameter(engine, id)) {
      const auto& p = engine.parameters[*index];
      patch[*index] = std::clamp(value, p.minimum, p.maximum);
    }
  };
  set("contour.lfo.depth", 0.0);
  set("contour.env.amount", 0.0);
  set("contour.mix", 1.0);
  set("contour.output", 0.0);
  if (cutoff_hz > 0.0) {
    set("contour.cutoff", cutoff_hz);
  }
  constexpr double rate = 48'000.0;
  constexpr std::size_t length = 16'384;
  // Small enough that the driven models stay in their linear range.
  constexpr double impulse = 1e-3;
  auto runtime = effects::NativeEffectRuntime::create(contour, patch, rate);
  if (!runtime.has_value()) {
    return {};
  }
  std::vector<std::complex<double>> response(length);
  std::vector<engine::StereoFrame> block(512);
  const synth::SynthTransport transport{0.0, 0.0, 0.0, 4.0};
  for (std::size_t start = 0; start < length; start += block.size()) {
    std::ranges::fill(block, engine::StereoFrame{0.0F, 0.0F});
    if (start == 0) {
      block[0] = {static_cast<float>(impulse), static_cast<float>(impulse)};
    }
    if (runtime.value()->process(block, {}, start, 1, transport, {}) !=
        effects::EffectProcessStatus::rendered) {
      return {};
    }
    for (std::size_t i = 0; i < block.size(); ++i) {
      response[start + i] = block[i].left / impulse;
    }
  }
  // In-place radix-2 FFT.
  for (std::size_t i = 1, j = 0; i < length; ++i) {
    std::size_t bit = length >> 1U;
    for (; (j & bit) != 0; bit >>= 1U) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(response[i], response[j]);
    }
  }
  for (std::size_t size = 2; size <= length; size <<= 1U) {
    const double angle = -2.0 * std::numbers::pi / static_cast<double>(size);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (std::size_t start = 0; start < length; start += size) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < size / 2; ++k) {
        const auto even = response[start + k];
        const auto odd = response[start + k + size / 2] * w;
        response[start + k] = even + odd;
        response[start + k + size / 2] = even - odd;
        w *= root;
      }
    }
  }
  const double bin_hz = rate / static_cast<double>(length);
  const double octaves = std::log2(20'000.0 / 20.0);
  const double half_slice = octaves / static_cast<double>(points - 1) / 2.0;
  // Combs keep each slice's loudest bin, since their peaks are narrower than
  // a slice; every other model reads its exact frequency, so notches show
  // their depth.
  const auto model = synth::find_parameter(engine, "contour.model");
  const bool comb = model && patch[*model] >= 9.0;
  std::vector<double> out(points);
  for (std::size_t p = 0; p < points; ++p) {
    const double position = octaves * static_cast<double>(p) / static_cast<double>(points - 1);
    const double hz = 20.0 * std::exp2(position);
    double magnitude = 0.0;
    if (comb) {
      const auto low =
          static_cast<std::size_t>(std::max(1.0, hz * std::exp2(-half_slice) / bin_hz));
      const auto high =
          std::max(low, std::min(length / 2 - 1,
                                 static_cast<std::size_t>(hz * std::exp2(half_slice) / bin_hz)));
      for (std::size_t k = low; k <= high; ++k) {
        magnitude = std::max(magnitude, std::abs(response[k]));
      }
    } else {
      const double bin = hz / bin_hz;
      const auto k = std::min(length / 2 - 2, static_cast<std::size_t>(bin));
      const double t = bin - static_cast<double>(k);
      magnitude = std::abs(response[k]) * (1.0 - t) + std::abs(response[k + 1]) * t;
    }
    out[p] = 20.0 * std::log10(std::max(magnitude, 1e-6));
  }
  return out;
}

std::vector<double> shape_response(const native_dsp::synth::EngineDescriptor& engine,
                                   std::size_t points, std::span<const double> values, int band) {
  namespace shape = native_dsp::effects::shape;
  if (engine.id != shape::engine().descriptor->id || values.size() != engine.parameters.size() ||
      points < 2 || band >= static_cast<int>(shape::bands)) {
    return {};
  }
  constexpr std::uint32_t rate = 48'000;
  std::vector<native_dsp::EqBandSettings> on;
  for (std::size_t b = 0; b < shape::bands; ++b) {
    if (band >= 0 && b != static_cast<std::size_t>(band)) {
      continue;
    }
    const auto stages = shape::band_stages(values, b, rate);
    on.insert(on.end(), stages.stage.begin(),
              stages.stage.begin() + static_cast<std::ptrdiff_t>(stages.size));
  }
  if (band >= 0 && on.empty()) {
    return {};
  }
  const auto prepared = native_dsp::prepare_eq(on, rate);
  if (!prepared.has_value()) {
    return {};
  }
  std::vector<double> response;
  response.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double hz =
        20.0 * std::pow(1'000.0, static_cast<double>(i) / static_cast<double>(points - 1));
    response.push_back(native_dsp::eq_magnitude_db(prepared.value(), hz).value_or(0.0));
  }
  return response;
}

std::vector<double> press_transfer(const native_dsp::synth::EngineDescriptor& engine,
                                   double floor_db, std::span<const double> values,
                                   std::size_t points) {
  namespace press = native_dsp::effects::press;
  if (engine.id != press::engine().descriptor->id || values.size() != engine.parameters.size() ||
      points < 2 || !(floor_db < 0.0)) {
    return {};
  }
  const auto settings = press::settings_for(values);
  std::vector<double> curve;
  curve.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double input =
        floor_db * (1.0 - static_cast<double>(i) / static_cast<double>(points - 1));
    curve.push_back(input - native_dsp::compressor_reduction_db(input, settings) +
                    settings.makeup_db);
  }
  return curve;
}

namespace {

[[nodiscard]] bool is_edge(const native_dsp::synth::EngineDescriptor& engine,
                           std::span<const double> values) {
  return engine.id == native_dsp::effects::edge::engine().descriptor->id &&
         values.size() == engine.parameters.size();
}

} // namespace

std::vector<double> edge_transfer(const native_dsp::synth::EngineDescriptor& engine,
                                  std::span<const double> values, double from_db, double to_db,
                                  std::size_t points) {
  const auto shape = native_dsp::synth::find_parameter(engine, "edge.shape");
  if (!is_edge(engine, values) || !shape || points < 2 || !(to_db > from_db)) {
    return {};
  }
  std::vector<double> curve;
  curve.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double db =
        from_db + (to_db - from_db) * static_cast<double>(i) / static_cast<double>(points - 1);
    curve.push_back(native_dsp::dsp::gain_to_db(
        native_dsp::effects::edge::clip(native_dsp::dsp::db_to_gain(db), values[*shape])));
  }
  return curve;
}

double edge_latency_ms(const native_dsp::synth::EngineDescriptor& engine,
                       std::span<const double> values, double sample_rate) {
  constexpr double milliseconds_per_second = 1'000.0;
  if (!is_edge(engine, values) || !(sample_rate > 0.0)) {
    return 0.0;
  }
  const auto frames = native_dsp::effects::edge::engine().latency_frames(values, sample_rate);
  return static_cast<double>(frames) / sample_rate * milliseconds_per_second;
}

namespace {
// The Strata sketches' frequency span: 20 Hz to 20 kHz.
constexpr double sketch_low_hz = 20.0;
constexpr double sketch_span = 1'000.0;

[[nodiscard]] bool is_strata(const native_dsp::synth::EngineDescriptor& engine,
                             std::span<const double> values) {
  return engine.id == native_dsp::effects::strata::engine().descriptor->id &&
         values.size() == engine.parameters.size();
}
} // namespace

std::vector<double> strata_transfer(const native_dsp::synth::EngineDescriptor& engine,
                                    std::span<const double> values, int band, double floor_db,
                                    std::size_t points) {
  namespace strata = native_dsp::effects::strata;
  if (!is_strata(engine, values) || points < 2 || !(floor_db < 0.0) || band < 0 ||
      band >= static_cast<int>(strata::max_bands)) {
    return {};
  }
  const auto curve = strata::band_for(values, static_cast<std::size_t>(band)).curve;
  std::vector<double> out;
  out.reserve(points);
  for (std::size_t i = 0; i < points; ++i) {
    const double input =
        floor_db * (1.0 - static_cast<double>(i) / static_cast<double>(points - 1));
    out.push_back(strata::output_db(input, curve));
  }
  return out;
}

std::vector<double> strata_crossovers(const native_dsp::synth::EngineDescriptor& engine,
                                      std::span<const double> values, double sample_rate) {
  if (!is_strata(engine, values) || !(sample_rate > 0.0)) {
    return {};
  }
  const auto layout = native_dsp::effects::strata::layout_for(values, sample_rate);
  return {layout.crossovers.begin(),
          layout.crossovers.begin() + static_cast<std::ptrdiff_t>(layout.bands - 1)};
}

std::vector<std::vector<double>>
strata_band_shapes(const native_dsp::synth::EngineDescriptor& engine,
                   std::span<const double> values, double sample_rate, std::size_t points) {
  namespace strata = native_dsp::effects::strata;
  if (!is_strata(engine, values) || !(sample_rate > 0.0) || points < 2) {
    return {};
  }
  const auto layout = strata::layout_for(values, sample_rate);
  std::vector<std::vector<double>> out(layout.bands);
  for (std::size_t b = 0; b < layout.bands; ++b) {
    out[b].reserve(points);
    for (std::size_t i = 0; i < points; ++i) {
      const double hz = sketch_low_hz * std::pow(sketch_span, static_cast<double>(i) /
                                                                  static_cast<double>(points - 1));
      out[b].push_back(strata::band_magnitude(layout, b, hz, sample_rate));
    }
  }
  return out;
}

std::vector<native_dsp::synth::NamedValue>
strata_ott_values(const native_dsp::synth::EngineDescriptor& engine,
                  std::span<const double> values) {
  if (!is_strata(engine, values)) {
    return {};
  }
  return native_dsp::effects::strata::ott_values(values);
}

} // namespace oscillate::quick
