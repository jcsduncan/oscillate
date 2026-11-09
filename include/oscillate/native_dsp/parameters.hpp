#pragma once

#include "oscillate/foundation/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::native_dsp {

// Every native engine (synth, effect or note effect) publishes one versioned
// table of these. The same table drives validation, persistence, automation
// domains, the device panel and preset upgrades, so ids are stable forever
// once released.
enum class ParameterScale : std::uint8_t {
  linear,      // Continuous, uniform knob travel.
  logarithmic, // Continuous, knob travel uniform in log(value); minimum > 0.
  stepped,     // Integer values from minimum to maximum.
  choice,      // Index into choices.
};

// live: applied smoothly to sounding notes. next_note: a sounding note keeps
// its value; new notes use the new one, and the runtime reports the deferral.
enum class ParameterTiming : std::uint8_t { live, next_note };

struct ParameterDescriptor {
  std::string_view id;    // "prism.filter.cutoff"
  std::string_view name;  // "Cutoff"
  std::string_view group; // "Filter"
  std::string_view unit;  // "Hz", "dB", "st", "ct", "s", "%" or empty.
  ParameterScale scale = ParameterScale::linear;
  double minimum = 0.0;
  double maximum = 1.0;
  double default_value = 0.0;
  std::span<const std::string_view> choices{};
  ParameterTiming timing = ParameterTiming::live;
  // How long a jump from the device panel takes to arrive, so knob moves
  // never click. Automation lanes are already continuous and bypass it.
  double glide_seconds = 0.0;
  // Continuous live parameters only: validate_engine rejects automatable
  // discrete or next-note parameters.
  bool automatable = true;
  // Knob travel shaping for linear parameters: values > 1 give finer control
  // near the minimum (or near zero for bipolar ranges).
  double skew = 1.0;
};

// The index of `id` in a parameter table, at compile time, so a misspelt id
// fails to compile.
template <std::size_t N>
consteval std::size_t parameter_index(const std::array<ParameterDescriptor, N>& table,
                                      std::string_view id) {
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i].id == id) {
      return i;
    }
  }
  throw std::invalid_argument("unknown parameter id");
}

// How long a panel jump takes to arrive on a continuous control: the
// built-in effects' and synths' common glides.
inline constexpr double effect_glide_seconds = 0.03;
inline constexpr double synth_glide_seconds = 0.02;

inline constexpr std::size_t max_engine_parameters = 2'048;

struct Preset;

struct EngineDescriptor {
  std::string_view id; // "prism"
  std::string_view name;
  std::uint32_t version = 1;
  std::span<const ParameterDescriptor> parameters;
  std::span<const Preset> presets;
};

struct NamedValue {
  std::string id;
  double value = 0.0;
};

struct Preset {
  std::string_view name;
  std::string_view category;
  // Only values that differ from the parameter defaults.
  std::span<const std::pair<std::string_view, double>> values;
  // Words a preset browser searches and filters by ("bass", "dark"), in
  // lower case; may be empty.
  std::span<const std::string_view> tags{};
};

[[nodiscard]] bool is_discrete(const ParameterDescriptor& parameter) noexcept;

// Clamps into range and snaps discrete parameters to their step. Non-finite
// input yields the default.
[[nodiscard]] double sanitise(const ParameterDescriptor& parameter, double value) noexcept;

// 0..1 knob travel <-> plain value, honouring scale and skew.
[[nodiscard]] double to_normalised(const ParameterDescriptor& parameter, double value) noexcept;
[[nodiscard]] double from_normalised(const ParameterDescriptor& parameter,
                                     double normalised) noexcept;

[[nodiscard]] std::optional<std::size_t> find_parameter(const EngineDescriptor& engine,
                                                        std::string_view id) noexcept;
[[nodiscard]] std::vector<double> default_values(const EngineDescriptor& engine);

// Human readable value through the display formatter
// (foundation/value_text.hpp), e.g. "1.20 kHz", "−6.0 dB", "Saw".
[[nodiscard]] std::string format_value(const ParameterDescriptor& parameter, double value);

// Validates a descriptor table: unique ids, finite ordered ranges, defaults in
// range, choices present for choice parameters, positive log minimum.
[[nodiscard]] foundation::Result<void> validate_engine(const EngineDescriptor& engine);

// Saved values -> dense values in table order. Missing ids take defaults;
// out-of-range values are rejected rather than silently clamped, so a corrupt
// patch is reported instead of changing sound. Unknown ids are rejected.
[[nodiscard]] foundation::Result<std::vector<double>>
resolve_values(const EngineDescriptor& engine, std::span<const NamedValue> saved);

// Dense values -> the minimal saved form: only values that differ from the
// defaults, in table order.
[[nodiscard]] std::vector<NamedValue> sparse_values(const EngineDescriptor& engine,
                                                    std::span<const double> values);

[[nodiscard]] foundation::Result<std::vector<double>> preset_values(const EngineDescriptor& engine,
                                                                    const Preset& preset);

} // namespace oscillate::native_dsp
