#pragma once

#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace test_insert {
namespace project = oscillate::project;

inline project::InsertSlotId next_slot(const project::Snapshot& snapshot) {
  std::uint64_t low = 10'000;
  for (const auto& slot : snapshot.master_effects)
    low = std::max(low, slot.id.bits().low + 1);
  for (const auto& track : snapshot.tracks)
    for (const auto& slot : track.effects)
      low = std::max(low, slot.id.bits().low + 1);
  auto result = project::InsertSlotId::from_bits({0, low});
  if (!result)
    throw std::runtime_error(result.error().message);
  return result.value();
}

template <typename T> T& master(project::Snapshot& snapshot) {
  const auto found = std::ranges::find_if(snapshot.master_effects, [](const auto& slot) {
    return std::holds_alternative<T>(slot.processor);
  });
  if (found == snapshot.master_effects.end())
    throw std::runtime_error("expected master effect is absent");
  return std::get<T>(found->processor);
}

template <typename T> void set_master(project::Snapshot& snapshot, T value) {
  const auto found = std::ranges::find_if(snapshot.master_effects, [](const auto& slot) {
    return std::holds_alternative<T>(slot.processor);
  });
  if (found != snapshot.master_effects.end()) {
    found->processor = std::move(value);
    return;
  }
  snapshot.master_effects.push_back({next_slot(snapshot), true, std::move(value)});
  std::ranges::sort(snapshot.master_effects, [](const auto& left, const auto& right) {
    return left.processor.index() < right.processor.index();
  });
}

// Built-in effect fixtures (ADR-0047). Shape, Press and Ceiling run the
// native_dsp EQ, compressor and limiter bit for bit, so those runtimes are
// the oracles; Expanse is the reverb.
namespace native_dsp = oscillate::native_dsp;

inline project::NativeEffectPatch
effect_patch(std::string_view engine_id, std::span<const std::pair<std::string, double>> values) {
  const auto* engine = native_dsp::effects::find_engine(engine_id);
  if (engine == nullptr)
    throw std::runtime_error("built-in effect is not registered");
  auto dense = native_dsp::synth::default_values(*engine->descriptor);
  for (const auto& [parameter, value] : values) {
    const auto index = native_dsp::synth::find_parameter(*engine->descriptor, parameter);
    if (!index)
      throw std::runtime_error("unknown built-in effect parameter " + parameter);
    dense[*index] = value;
  }
  auto patch = oscillate::native_inserts::make_native_effect_patch(*engine, dense);
  if (!patch)
    throw std::runtime_error(patch.error().message);
  return std::move(patch).value();
}

inline project::NativeEffectPatch shape(std::span<const native_dsp::EqBandSettings> bands) {
  std::vector<std::pair<std::string, double>> values;
  for (std::size_t index = 0; index < bands.size(); ++index) {
    const auto band = std::to_string(index + 1);
    values.push_back({"shape.on_" + band, 1.0});
    values.push_back({"shape.kind_" + band, static_cast<double>(bands[index].kind)});
    values.push_back({"shape.freq_" + band, bands[index].frequency_hz});
    values.push_back({"shape.q_" + band, bands[index].shape});
    values.push_back({"shape.gain_" + band, bands[index].gain_db});
  }
  return effect_patch("shape", values);
}
inline project::NativeEffectPatch shape(native_dsp::EqBandSettings band) {
  return shape(std::span(&band, 1));
}

// Shape with every band off: a valid stage that passes its input.
inline project::NativeEffectPatch flat_shape() {
  return shape(std::span<const native_dsp::EqBandSettings>{});
}

inline project::NativeEffectPatch press(const native_dsp::CompressorSettings& settings) {
  std::vector<std::pair<std::string, double>> values{
      {"press.threshold", settings.threshold_db}, {"press.ratio", settings.ratio},
      {"press.knee", settings.knee_db},           {"press.attack", settings.attack_ms},
      {"press.release", settings.release_ms},     {"press.makeup", settings.makeup_db}};
  if (settings.sidechain_highpass_hz != 0.0) {
    values.push_back({"press.detector_hp", 1.0});
    values.push_back({"press.detector_freq", settings.sidechain_highpass_hz});
  }
  return effect_patch("press", values);
}

inline project::NativeEffectPatch ceiling(const native_dsp::LimiterSettings& settings) {
  const std::array<std::pair<std::string, double>, 3> values{
      {{"ceiling.ceiling", settings.ceiling_db},
       {"ceiling.lookahead", settings.lookahead_ms},
       {"ceiling.release", settings.release_ms}}};
  return effect_patch("ceiling", values);
}

// Expanse at a decay (seconds) and mix (0..1).
inline project::NativeEffectPatch expanse(double decay_seconds, double mix) {
  const std::array<std::pair<std::string, double>, 2> values{
      {{"expanse.decay", decay_seconds}, {"expanse.mix", mix}}};
  return effect_patch("expanse", values);
}

// A saved value written straight into the patch, unvalidated, for tests of
// how an engine refuses a value outside its range.
inline project::NativeEffectPatch with_raw_value(project::NativeEffectPatch patch,
                                                 std::string_view parameter, double value) {
  const auto found =
      std::ranges::find(patch.parameters, parameter, &project::NativeInstrumentParameter::id);
  if (found != patch.parameters.end())
    found->value = value;
  else
    patch.parameters.push_back({std::string(parameter), value});
  return patch;
}

// The frames a built-in effect keeps sounding after its input stops: what an
// export adds after the last source frame.
inline std::uint64_t tail_frames(const project::NativeEffectPatch& patch, double sample_rate) {
  auto resolved = oscillate::native_inserts::resolve_native_effect(patch);
  if (!resolved)
    throw std::runtime_error(resolved.error().message);
  return resolved.value().engine->tail_frames(resolved.value().values, sample_rate);
}

// The master slot running `engine_id`, replaced in place, or appended.
inline void set_master(project::Snapshot& snapshot, project::NativeEffectPatch patch) {
  const auto found = std::ranges::find_if(snapshot.master_effects, [&](const auto& slot) {
    const auto* saved = std::get_if<project::NativeEffectPatch>(&slot.processor);
    return saved != nullptr && saved->engine == patch.engine;
  });
  if (found != snapshot.master_effects.end()) {
    found->processor = std::move(patch);
    return;
  }
  snapshot.master_effects.push_back({next_slot(snapshot), true, std::move(patch)});
}

inline project::Vst3PluginInstance& vst3_source(project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}
inline const project::Vst3PluginInstance& vst3_source(const project::Track& track) {
  return std::get<project::Vst3PluginInstance>(track.instrument_source);
}
inline void append_effect(project::Snapshot& snapshot, project::Track& track,
                          project::Vst3PluginInstance plugin) {
  track.effects.emplace_back(next_slot(snapshot), true, std::move(plugin));
}
} // namespace test_insert
