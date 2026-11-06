#include "oscillate/native_dsp/synth/parameters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>

namespace oscillate::native_dsp::synth {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

Error invalid(std::string message) { return {ErrorCode::invalid_argument, std::move(message)}; }

bool bipolar(const ParameterDescriptor& parameter) noexcept {
  return parameter.minimum < 0.0 && parameter.maximum > 0.0;
}

double clamp_unit(double value) noexcept { return value > 0.0 ? (value < 1.0 ? value : 1.0) : 0.0; }

} // namespace

bool is_discrete(const ParameterDescriptor& parameter) noexcept {
  return parameter.scale == ParameterScale::stepped || parameter.scale == ParameterScale::choice;
}

double sanitise(const ParameterDescriptor& parameter, double value) noexcept {
  if (!std::isfinite(value)) {
    return parameter.default_value;
  }
  value = std::clamp(value, parameter.minimum, parameter.maximum);
  return is_discrete(parameter) ? std::round(value) : value;
}

double to_normalised(const ParameterDescriptor& parameter, double value) noexcept {
  value = sanitise(parameter, value);
  const double span = parameter.maximum - parameter.minimum;
  if (span <= 0.0) {
    return 0.0;
  }
  if (parameter.scale == ParameterScale::logarithmic) {
    return clamp_unit(std::log(value / parameter.minimum) /
                      std::log(parameter.maximum / parameter.minimum));
  }
  if (parameter.skew != 1.0 && parameter.scale == ParameterScale::linear) {
    if (bipolar(parameter)) {
      // Symmetric skew around zero; knob centre is zero.
      const double extent = value >= 0.0 ? parameter.maximum : -parameter.minimum;
      const double shaped = std::pow(std::fabs(value) / extent, 1.0 / parameter.skew);
      return clamp_unit(0.5 + (value >= 0.0 ? 0.5 : -0.5) * shaped);
    }
    return clamp_unit(std::pow((value - parameter.minimum) / span, 1.0 / parameter.skew));
  }
  return clamp_unit((value - parameter.minimum) / span);
}

double from_normalised(const ParameterDescriptor& parameter, double normalised) noexcept {
  normalised = std::isfinite(normalised) ? clamp_unit(normalised) : 0.0;
  const double span = parameter.maximum - parameter.minimum;
  double value = parameter.minimum + normalised * span;
  if (parameter.scale == ParameterScale::logarithmic) {
    value = parameter.minimum * std::pow(parameter.maximum / parameter.minimum, normalised);
  } else if (parameter.skew != 1.0 && parameter.scale == ParameterScale::linear) {
    if (bipolar(parameter)) {
      const double offset = 2.0 * (normalised - 0.5);
      const double shaped = std::pow(std::fabs(offset), parameter.skew);
      value = offset >= 0.0 ? shaped * parameter.maximum : -shaped * -parameter.minimum;
    } else {
      value = parameter.minimum + std::pow(normalised, parameter.skew) * span;
    }
  }
  return sanitise(parameter, value);
}

std::optional<std::size_t> find_parameter(const EngineDescriptor& engine,
                                          std::string_view id) noexcept {
  for (std::size_t index = 0; index < engine.parameters.size(); ++index) {
    if (engine.parameters[index].id == id) {
      return index;
    }
  }
  return std::nullopt;
}

std::vector<double> default_values(const EngineDescriptor& engine) {
  std::vector<double> values;
  values.reserve(engine.parameters.size());
  for (const auto& parameter : engine.parameters) {
    values.push_back(parameter.default_value);
  }
  return values;
}

std::string format_value(const ParameterDescriptor& parameter, double value) {
  value = sanitise(parameter, value);
  if (parameter.scale == ParameterScale::choice) {
    const auto index = static_cast<std::size_t>(value);
    return index < parameter.choices.size() ? std::string(parameter.choices[index]) : "?";
  }
  const std::string unit(parameter.unit);
  char text[48];
  if (unit == "Hz" && value >= 1'000.0) {
    std::snprintf(text, sizeof text, "%.2f kHz", value / 1'000.0);
  } else if (unit == "s" && value < 1.0) {
    std::snprintf(text, sizeof text, value < 0.01 ? "%.1f ms" : "%.0f ms", value * 1'000.0);
  } else if (unit == "%") {
    std::snprintf(text, sizeof text, "%.0f %%", value * 100.0);
  } else if (unit == "dB" && value <= parameter.minimum && parameter.minimum <= -60.0) {
    std::snprintf(text, sizeof text, "-inf dB");
  } else if (parameter.scale == ParameterScale::stepped) {
    std::snprintf(text, sizeof text, "%+.0f%s%s", value, unit.empty() ? "" : " ", unit.c_str());
  } else {
    const double magnitude = std::fabs(value);
    const char* format =
        magnitude >= 100.0 ? "%.0f%s%s" : (magnitude >= 10.0 ? "%.1f%s%s" : "%.2f%s%s");
    std::snprintf(text, sizeof text, format, value, unit.empty() ? "" : " ", unit.c_str());
  }
  return text;
}

Result<void> validate_engine(const EngineDescriptor& engine) {
  if (engine.id.empty() || engine.version == 0 || engine.parameters.empty() ||
      engine.parameters.size() > max_engine_parameters) {
    return Result<void>::failure(invalid("engine descriptor needs an id, version and parameters"));
  }
  std::set<std::string_view> ids;
  for (const auto& parameter : engine.parameters) {
    const std::string where = "parameter '" + std::string(parameter.id) + "' ";
    if (parameter.id.empty() || !ids.insert(parameter.id).second) {
      return Result<void>::failure(invalid(where + "id is empty or duplicated"));
    }
    if (!std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
        !(parameter.minimum < parameter.maximum) ||
        !(parameter.default_value >= parameter.minimum &&
          parameter.default_value <= parameter.maximum) ||
        !(parameter.glide_seconds >= 0.0) || !(parameter.skew > 0.0)) {
      return Result<void>::failure(invalid(where + "has an invalid range, default or glide"));
    }
    if (parameter.scale == ParameterScale::logarithmic && !(parameter.minimum > 0.0)) {
      return Result<void>::failure(invalid(where + "is logarithmic with a non-positive minimum"));
    }
    if (is_discrete(parameter) &&
        (parameter.minimum != std::round(parameter.minimum) ||
         parameter.maximum != std::round(parameter.maximum) ||
         parameter.default_value != std::round(parameter.default_value))) {
      return Result<void>::failure(invalid(where + "is discrete with fractional bounds"));
    }
    if (parameter.scale == ParameterScale::choice &&
        (parameter.minimum != 0.0 ||
         parameter.choices.size() != static_cast<std::size_t>(parameter.maximum) + 1)) {
      return Result<void>::failure(invalid(where + "choices do not match its range"));
    }
  }
  for (const auto& preset : engine.presets) {
    if (auto values = preset_values(engine, preset); !values) {
      return Result<void>::failure(values.error());
    }
  }
  return Result<void>::success();
}

Result<std::vector<double>> resolve_values(const EngineDescriptor& engine,
                                           std::span<const NamedValue> saved) {
  auto values = default_values(engine);
  std::vector<bool> seen(values.size(), false);
  for (const auto& entry : saved) {
    const auto index = find_parameter(engine, entry.id);
    if (!index) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::corrupt_data,
           "unknown " + std::string(engine.id) + " parameter '" + entry.id + "'"});
    }
    const auto& parameter = engine.parameters[*index];
    if (seen[*index] || !std::isfinite(entry.value) || entry.value < parameter.minimum ||
        entry.value > parameter.maximum ||
        (is_discrete(parameter) && entry.value != std::round(entry.value))) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::corrupt_data, "invalid or duplicated value for '" + entry.id + "'"});
    }
    seen[*index] = true;
    values[*index] = entry.value;
  }
  return Result<std::vector<double>>::success(std::move(values));
}

std::vector<NamedValue> sparse_values(const EngineDescriptor& engine,
                                      std::span<const double> values) {
  std::vector<NamedValue> sparse;
  for (std::size_t index = 0; index < engine.parameters.size() && index < values.size(); ++index) {
    if (values[index] != engine.parameters[index].default_value) {
      sparse.push_back({std::string(engine.parameters[index].id), values[index]});
    }
  }
  return sparse;
}

Result<std::vector<double>> preset_values(const EngineDescriptor& engine,
                                          const SynthPreset& preset) {
  std::vector<NamedValue> saved;
  saved.reserve(preset.values.size());
  for (const auto& [id, value] : preset.values) {
    saved.push_back({std::string(id), value});
  }
  auto values = resolve_values(engine, saved);
  if (!values) {
    return Result<std::vector<double>>::failure(
        invalid("preset '" + std::string(preset.name) + "': " + values.error().message));
  }
  return values;
}

} // namespace oscillate::native_dsp::synth
