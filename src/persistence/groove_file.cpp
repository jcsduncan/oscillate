#include "oscillate/persistence/groove_file.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::persistence {
namespace {

using Json = nlohmann::json;
using foundation::ErrorCode;

constexpr std::string_view groove_kind = "oscillate.groove";
constexpr std::uint32_t groove_version = 1;

[[nodiscard]] bool supported_steps_per_quarter(std::uint32_t steps) noexcept {
  return steps == 1 || steps == 2 || steps == 3 || steps == 4 || steps == 6 || steps == 8;
}

[[nodiscard]] bool exactly(const Json& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object() || object.size() != names.size())
    return false;
  for (const auto name : names) {
    if (!object.contains(std::string(name)))
      return false;
  }
  return true;
}

} // namespace

foundation::Result<void> validate_groove(const GrooveTemplate& groove) {
  using R = foundation::Result<void>;
  if (groove.name.empty() || groove.name.size() > max_groove_name_bytes)
    return R::failure({ErrorCode::invalid_argument, "A groove needs a name of at most 128 bytes"});
  if (!supported_steps_per_quarter(groove.steps_per_quarter))
    return R::failure({ErrorCode::invalid_argument, "Unsupported groove grid"});
  if (groove.steps.empty() || groove.steps.size() > max_groove_steps)
    return R::failure({ErrorCode::invalid_argument, "A groove has 1 to 64 steps"});
  for (const auto& step : groove.steps) {
    if (!std::isfinite(step.timing) || std::abs(step.timing) > max_groove_timing ||
        !std::isfinite(step.velocity) || step.velocity < 0.0 || step.velocity > max_groove_velocity)
      return R::failure({ErrorCode::invalid_argument, "A groove step is out of range"});
  }
  return R::success();
}

std::string encode_groove(const GrooveTemplate& groove) {
  Json steps = Json::array();
  for (const auto& step : groove.steps)
    steps.push_back({{"timing", step.timing}, {"velocity", step.velocity}});
  return Json{{"kind", std::string(groove_kind)},
              {"version", groove_version},
              {"name", groove.name},
              {"steps_per_quarter", groove.steps_per_quarter},
              {"steps", std::move(steps)}}
             .dump(2) +
         '\n';
}

foundation::Result<GrooveTemplate> decode_groove(std::string_view text) {
  using R = foundation::Result<GrooveTemplate>;
  if (text.size() > max_groove_file_bytes)
    return R::failure({ErrorCode::resource_exhausted, "Groove file too large"});
  const auto corrupt = [](std::string message) {
    return R::failure({ErrorCode::corrupt_data, std::move(message)});
  };
  // Duplicate keys are refused, as in the project codec: one key set per
  // open object.
  bool duplicate = false;
  std::vector<std::set<std::string>> open;
  Json root;
  try {
    root = Json::parse(text, [&](int, Json::parse_event_t event, Json& parsed) {
      if (event == Json::parse_event_t::object_start)
        open.emplace_back();
      else if (event == Json::parse_event_t::object_end && !open.empty())
        open.pop_back();
      else if (event == Json::parse_event_t::key && !open.empty() &&
               !open.back().insert(parsed.get<std::string>()).second)
        duplicate = true;
      return true;
    });
  } catch (const Json::exception& error) {
    return corrupt(error.what());
  }
  if (duplicate)
    return corrupt("duplicate JSON field");
  if (!exactly(root, {"kind", "version", "name", "steps_per_quarter", "steps"}))
    return corrupt("not a groove file");
  if (!root["kind"].is_string() || root["kind"].get<std::string>() != groove_kind ||
      !root["version"].is_number_unsigned() ||
      root["version"].get<std::uint64_t>() != groove_version)
    return R::failure({ErrorCode::unsupported, "not a groove file"});
  if (!root["name"].is_string() || !root["steps_per_quarter"].is_number_unsigned() ||
      !root["steps"].is_array() || root["steps"].size() > max_groove_steps)
    return corrupt("malformed groove file");
  GrooveTemplate groove;
  groove.name = root["name"].get<std::string>();
  const auto steps_per_quarter = root["steps_per_quarter"].get<std::uint64_t>();
  if (steps_per_quarter > 64)
    return corrupt("malformed groove grid");
  groove.steps_per_quarter = static_cast<std::uint32_t>(steps_per_quarter);
  for (const auto& entry : root["steps"]) {
    if (!exactly(entry, {"timing", "velocity"}) || !entry["timing"].is_number() ||
        !entry["velocity"].is_number())
      return corrupt("malformed groove step");
    groove.steps.push_back({entry["timing"].get<double>(), entry["velocity"].get<double>()});
  }
  if (auto valid = validate_groove(groove); !valid)
    return R::failure({ErrorCode::corrupt_data, valid.error().message});
  return R::success(std::move(groove));
}

} // namespace oscillate::persistence
