#include "private.hpp"

#include "oscillate/foundation/id.hpp"
#include "oscillate/persistence/effect_preset.hpp"
#include "oscillate/persistence/rack_file.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oscillate::persistence::detail {
// The one schema this build reads and writes (greenfield: no older layouts).
constexpr std::uint32_t schema_major = 1;
constexpr std::uint32_t schema_minor = 47;

namespace {

using Json = nlohmann::json;
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

class ParseError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// The one boundary where a decode failure becomes a Result: the recursive
// decoders below throw ParseError, and nlohmann::json throws Json::exception,
// from deep inside a document. Both mean the input is corrupt.
template <typename T, typename Decode>
[[nodiscard]] foundation::Result<T> contain_parse_errors(Decode&& decode) {
  try {
    return std::forward<Decode>(decode)();
  } catch (const ParseError& error) {
    return foundation::Result<T>::failure({foundation::ErrorCode::corrupt_data, error.what()});
  } catch (const Json::exception& error) {
    return foundation::Result<T>::failure({foundation::ErrorCode::corrupt_data, error.what()});
  }
}

// The library handles JSON grammar, UTF-8 and escaping. This callback keeps
// duplicate fields from being silently overwritten in its object container.
[[nodiscard]] Json parse_strict(std::string_view text) {
  std::vector<std::unordered_set<std::string>> object_keys;
  std::size_t nodes = 0;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > 32 || ++nodes > 500'000) {
      throw ParseError("JSON structure exceeds manifest limits");
    }
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (object_keys.empty() || !object_keys.back().insert(value.get<std::string>()).second) {
        throw ParseError("duplicate JSON field");
      }
    } else if (event == Json::parse_event_t::object_end) {
      object_keys.pop_back();
    }
    return true;
  };
  return Json::parse(text.begin(), text.end(), callback);
}

void fields(const Json& value, std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.size() != expected.size()) {
    throw ParseError("unexpected JSON object fields");
  }
  for (const auto name : expected) {
    if (!value.contains(std::string(name))) {
      throw ParseError("missing JSON field");
    }
  }
}

[[nodiscard]] const Json& field(const Json& object, std::string_view name) {
  return object.at(std::string(name));
}

void check_array(const Json& value, std::size_t limit) {
  if (!value.is_array() || value.size() > limit) {
    throw ParseError("invalid JSON array size");
  }
}

[[nodiscard]] std::string string(const Json& value, std::size_t limit = 128) {
  if (!value.is_string()) {
    throw ParseError("expected JSON string");
  }
  auto decoded = value.get<std::string>();
  if (decoded.size() > limit) {
    throw ParseError("JSON string too long");
  }
  return decoded;
}

template <typename T> [[nodiscard]] T unsigned_integer(const Json& value) {
  static_assert(std::is_unsigned_v<T>);
  if (!value.is_number_unsigned()) {
    throw ParseError("expected unsigned JSON integer");
  }
  const auto number = value.get<std::uint64_t>();
  if (number > std::numeric_limits<T>::max()) {
    throw ParseError("JSON integer out of range");
  }
  return static_cast<T>(number);
}

[[nodiscard]] std::int64_t signed_integer(const Json& value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw ParseError("JSON integer out of range");
    }
    return static_cast<std::int64_t>(number);
  }
  if (!value.is_number_integer()) {
    throw ParseError("expected signed JSON integer");
  }
  return value.get<std::int64_t>();
}

[[nodiscard]] double real(const Json& value) {
  if (!value.is_number()) {
    throw ParseError("expected JSON number");
  }
  const auto number = value.get<double>();
  if (!std::isfinite(number)) {
    throw ParseError("non-finite JSON number");
  }
  return number;
}

[[nodiscard]] Json encode_key(project::MusicalKey key) {
  return {{"tonic", key.tonic}, {"mode", key.mode == project::KeyMode::major ? "major" : "minor"}};
}

[[nodiscard]] project::MusicalKey decode_key(const Json& value) {
  fields(value, {"tonic", "mode"});
  const auto tonic = unsigned_integer<std::uint8_t>(field(value, "tonic"));
  const auto mode = string(field(value, "mode"), 8);
  if (tonic >= 12 || (mode != "major" && mode != "minor")) {
    throw ParseError("invalid musical key");
  }
  return {tonic, mode == "major" ? project::KeyMode::major : project::KeyMode::minor};
}

[[nodiscard]] std::string_view key_relation_name(project::KeyRelation relation) {
  switch (relation) {
  case project::KeyRelation::relative:
    return "relative";
  case project::KeyRelation::dominant:
    return "dominant";
  case project::KeyRelation::subdominant:
    return "subdominant";
  }
  throw ParseError("invalid compatible key relation");
}

[[nodiscard]] project::KeyRelation decode_key_relation(const Json& value) {
  const auto name = string(value, 16);
  if (name == "relative")
    return project::KeyRelation::relative;
  if (name == "dominant")
    return project::KeyRelation::dominant;
  if (name == "subdominant")
    return project::KeyRelation::subdominant;
  throw ParseError("invalid compatible key relation");
}

template <typename T> [[nodiscard]] T parse_id(const Json& value) {
  auto parsed = T::parse(string(value));
  if (!parsed) {
    throw ParseError("invalid entity ID");
  }
  return parsed.value();
}

[[nodiscard]] bool hex_name(std::string_view value, std::size_t length) {
  if (value.size() != length) {
    return false;
  }
  for (const auto ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string encode_state(const std::vector<std::uint8_t>& bytes) {
  std::string encoded(((bytes.size() + 2) / 3) * 4, '\0');
  if (!bytes.empty()) {
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), bytes.data(),
                    static_cast<int>(bytes.size()));
  }
  return encoded;
}

[[nodiscard]] std::vector<std::uint8_t> decode_state(const Json& value) {
  const auto encoded = string(value, ((project::max_vst3_state_bytes + 2) / 3) * 4);
  if (encoded.size() % 4 != 0) {
    throw ParseError("invalid VST3 state base64 length");
  }
  std::vector<std::uint8_t> decoded((encoded.size() / 4) * 3);
  if (!encoded.empty()) {
    const int length =
        EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
                        static_cast<int>(encoded.size()));
    if (length < 0) {
      throw ParseError("invalid VST3 state base64");
    }
    const auto padding =
        static_cast<std::size_t>(encoded.back() == '=') +
        static_cast<std::size_t>(encoded.size() > 1 && encoded[encoded.size() - 2] == '=');
    if (static_cast<std::size_t>(length) < padding) {
      throw ParseError("invalid VST3 state base64 padding");
    }
    decoded.resize(static_cast<std::size_t>(length) - padding);
  }
  if (decoded.size() > project::max_vst3_state_bytes || encode_state(decoded) != encoded) {
    throw ParseError("noncanonical or oversized VST3 state base64");
  }
  return decoded;
}

[[nodiscard]] Json encode_plugin(const project::Vst3PluginInstance& plugin) {
  Json parameters = Json::array();
  for (const auto& parameter : plugin.parameters) {
    parameters.push_back({{"id", parameter.id}, {"normalized", parameter.normalized_value}});
  }
  return {{"id", plugin.id.to_string()},
          {"class_id", plugin.class_id},
          {"vendor", plugin.vendor},
          {"name", plugin.name},
          {"version", plugin.version},
          {"bundle_path", plugin.bundle_path},
          {"binary_sha256", plugin.binary_sha256},
          {"component_state_base64", encode_state(plugin.component_state)},
          {"controller_state_base64",
           plugin.controller_state ? Json(encode_state(*plugin.controller_state)) : Json(nullptr)},
          {"parameters", std::move(parameters)},
          {"missing", plugin.missing}};
}

[[nodiscard]] project::Vst3PluginInstance decode_plugin(const Json& value) {
  fields(value, {"id", "class_id", "vendor", "name", "version", "bundle_path", "binary_sha256",
                 "component_state_base64", "controller_state_base64", "parameters", "missing"});
  project::Vst3PluginInstance plugin{
      parse_id<project::PluginInstanceId>(field(value, "id")),
      string(field(value, "class_id"), 32),
      string(field(value, "vendor")),
      string(field(value, "name")),
      string(field(value, "version")),
      string(field(value, "bundle_path"), project::max_vst3_bundle_path_bytes),
      string(field(value, "binary_sha256"), 64),
      decode_state(field(value, "component_state_base64")),
      {},
      false,
      std::nullopt};
  if (const auto& controller = field(value, "controller_state_base64"); !controller.is_null())
    plugin.controller_state = decode_state(controller);
  const auto& parameters = field(value, "parameters");
  check_array(parameters, project::max_vst3_parameter_references);
  for (const auto& parameter : parameters) {
    fields(parameter, {"id", "normalized"});
    plugin.parameters.push_back({unsigned_integer<std::uint32_t>(field(parameter, "id")),
                                 real(field(parameter, "normalized"))});
  }
  if (!field(value, "missing").is_boolean()) {
    throw ParseError("invalid VST3 missing flag");
  }
  plugin.missing = field(value, "missing").get<bool>();
  return plugin;
}

[[nodiscard]] std::string_view shape_name(timeline::TempoShape shape) {
  switch (shape) {
  case timeline::TempoShape::step:
    return "step";
  case timeline::TempoShape::linear_bpm:
    return "linear_bpm";
  case timeline::TempoShape::ease_in:
    return "ease_in";
  case timeline::TempoShape::ease_out:
    return "ease_out";
  case timeline::TempoShape::ease_in_out:
    return "ease_in_out";
  }
  throw std::runtime_error("unknown tempo shape");
}

[[nodiscard]] timeline::TempoShape parse_shape(std::string_view name) {
  if (name == "step")
    return timeline::TempoShape::step;
  if (name == "linear_bpm")
    return timeline::TempoShape::linear_bpm;
  if (name == "ease_in")
    return timeline::TempoShape::ease_in;
  if (name == "ease_out")
    return timeline::TempoShape::ease_out;
  if (name == "ease_in_out")
    return timeline::TempoShape::ease_in_out;
  throw ParseError("unknown tempo shape");
}

class ValidationGate final : public project::TransportGate {
public:
  Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
        {ErrorCode::unavailable, "transport unavailable during manifest validation"});
  }
};

[[nodiscard]] const char* automation_shape_name(project::AutomationShape shape) {
  switch (shape) {
  case project::AutomationShape::hold:
    return "hold";
  case project::AutomationShape::linear:
    return "linear";
  case project::AutomationShape::quadratic:
    return "quadratic";
  case project::AutomationShape::exponential:
    return "exponential";
  case project::AutomationShape::ease_in_out:
    return "ease_in_out";
  case project::AutomationShape::bezier:
    return "bezier";
  }
  return "invalid";
}

[[nodiscard]] project::AutomationShape decode_automation_shape(const Json& value) {
  const auto name = string(value);
  if (name == "hold")
    return project::AutomationShape::hold;
  if (name == "linear")
    return project::AutomationShape::linear;
  if (name == "quadratic")
    return project::AutomationShape::quadratic;
  if (name == "exponential")
    return project::AutomationShape::exponential;
  if (name == "ease_in_out")
    return project::AutomationShape::ease_in_out;
  if (name == "bezier")
    return project::AutomationShape::bezier;
  throw ParseError("unknown automation shape");
}

[[nodiscard]] Json encode_domain(const project::AutomationDomain& domain) {
  return {{"representation", domain.representation == project::AutomationRepresentation::physical
                                 ? "physical"
                                 : "vst3_normalized"},
          {"semantic_key", domain.semantic_key},
          {"unit", domain.unit},
          {"minimum", domain.minimum},
          {"maximum", domain.maximum},
          {"default", domain.default_value},
          {"discrete", domain.discrete},
          {"step_count", domain.step_count},
          {"scale", domain.scale == project::AutomationScale::decibel       ? "decibel"
                    : domain.scale == project::AutomationScale::logarithmic ? "logarithmic"
                                                                            : "linear"}};
}

[[nodiscard]] project::AutomationDomain decode_domain(const Json& domain_value) {
  fields(domain_value, {"representation", "semantic_key", "unit", "minimum", "maximum", "default",
                        "discrete", "step_count", "scale"});
  const auto representation = string(field(domain_value, "representation"));
  if (representation != "physical" && representation != "vst3_normalized") {
    throw ParseError("invalid automation representation");
  }
  const auto& discrete_value = field(domain_value, "discrete");
  if (!discrete_value.is_boolean()) {
    throw ParseError("invalid automation discrete flag");
  }
  project::AutomationDomain domain{
      representation == "physical" ? project::AutomationRepresentation::physical
                                   : project::AutomationRepresentation::vst3_normalized,
      string(field(domain_value, "semantic_key"), 128),
      string(field(domain_value, "unit"), 64),
      real(field(domain_value, "minimum")),
      real(field(domain_value, "maximum")),
      real(field(domain_value, "default")),
      discrete_value.get<bool>(),
      unsigned_integer<std::uint32_t>(field(domain_value, "step_count"))};
  const auto scale = string(field(domain_value, "scale"));
  if (scale != "linear" && scale != "logarithmic" && scale != "decibel") {
    throw ParseError("invalid automation scale");
  }
  domain.scale = scale == "decibel"       ? project::AutomationScale::decibel
                 : scale == "logarithmic" ? project::AutomationScale::logarithmic
                                          : project::AutomationScale::linear;
  return domain;
}

[[nodiscard]] Json encode_automation(const std::vector<project::AutomationLane>& lanes) {
  Json result = Json::array();
  for (const auto& lane : lanes) {
    Json target;
    if (const auto* control = std::get_if<project::TrackControlTarget>(&lane.target)) {
      const auto key = [&] {
        switch (control->key) {
        case project::TrackControlTarget::Key::channel_pan:
          return "channel_pan";
        case project::TrackControlTarget::Key::channel_mute:
          return "channel_mute";
        case project::TrackControlTarget::Key::channel_level:
          break;
        }
        return "channel_level";
      }();
      target = {{"kind", "track_control"}, {"track_id", control->track.to_string()}, {"key", key}};
    } else if (const auto* send = std::get_if<project::SendGainTarget>(&lane.target)) {
      target = {{"kind", "send_gain"}, {"send_id", send->send.to_string()}};
    } else if (const auto* native = std::get_if<project::NativeParameterTarget>(&lane.target)) {
      target = {{"kind", "native_parameter"},
                {"track_id", native->track.to_string()},
                {"parameter_id", native->parameter_id}};
    } else if (const auto* effect =
                   std::get_if<project::NativeEffectParameterTarget>(&lane.target)) {
      target = {{"kind", "native_effect_parameter"},
                {"slot_id", effect->slot.to_string()},
                {"parameter_id", effect->parameter_id}};
    } else if (const auto* macro = std::get_if<project::MacroTarget>(&lane.target)) {
      target = {{"kind", "macro"}, {"track_id", macro->track.to_string()}, {"index", macro->index}};
    } else {
      const auto& vst = std::get<project::Vst3ParameterTarget>(lane.target);
      target = {{"kind", "vst3_parameter"},
                {"plugin_id", vst.plugin.to_string()},
                {"parameter_id", vst.parameter_id}};
    }
    Json points = Json::array();
    for (const auto& point : lane.points) {
      points.push_back({{"id", point.id.to_string()},
                        {"coordinate", point.coordinate},
                        {"value", point.value},
                        {"to_next", automation_shape_name(point.to_next)},
                        {"handles",
                         {{"x1", point.handles.x1},
                          {"y1", point.handles.y1},
                          {"x2", point.handles.x2},
                          {"y2", point.handles.y2}}}});
    }
    result.push_back(
        {{"id", lane.id.to_string()},
         {"target", std::move(target)},
         {"domain", encode_domain(lane.domain)},
         {"timebase",
          lane.timebase == project::AutomationTimebase::musical ? "musical" : "elapsed"},
         {"points", std::move(points)},
         {"settings_generation", lane.settings_generation}});
  }
  return result;
}

[[nodiscard]] std::vector<project::AutomationLane> decode_automation(const Json& value,
                                                                     std::size_t limit) {
  check_array(value, limit);
  std::vector<project::AutomationLane> lanes;
  lanes.reserve(value.size());
  for (const auto& item : value) {
    fields(item, {"id", "target", "domain", "timebase", "points", "settings_generation"});
    const auto& target_value = field(item, "target");
    const auto kind = string(field(target_value, "kind"));
    project::AutomationTarget target = [&]() -> project::AutomationTarget {
      if (kind == "track_control") {
        fields(target_value, {"kind", "track_id", "key"});
        const auto key = string(field(target_value, "key"));
        using Key = project::TrackControlTarget::Key;
        Key parsed;
        if (key == "channel_level")
          parsed = Key::channel_level;
        else if (key == "channel_pan")
          parsed = Key::channel_pan;
        else if (key == "channel_mute")
          parsed = Key::channel_mute;
        else
          throw ParseError("unsupported automation control");
        return project::TrackControlTarget{
            parse_id<project::TrackId>(field(target_value, "track_id")), parsed};
      }
      if (kind == "vst3_parameter") {
        fields(target_value, {"kind", "plugin_id", "parameter_id"});
        return project::Vst3ParameterTarget{
            parse_id<project::PluginInstanceId>(field(target_value, "plugin_id")),
            unsigned_integer<std::uint32_t>(field(target_value, "parameter_id"))};
      }
      if (kind == "send_gain") {
        fields(target_value, {"kind", "send_id"});
        return project::SendGainTarget{parse_id<project::SendId>(field(target_value, "send_id"))};
      }
      if (kind == "native_parameter") {
        fields(target_value, {"kind", "track_id", "parameter_id"});
        return project::NativeParameterTarget{
            parse_id<project::TrackId>(field(target_value, "track_id")),
            string(field(target_value, "parameter_id"), project::max_native_identifier_length)};
      }
      if (kind == "native_effect_parameter") {
        fields(target_value, {"kind", "slot_id", "parameter_id"});
        return project::NativeEffectParameterTarget{
            parse_id<project::InsertSlotId>(field(target_value, "slot_id")),
            string(field(target_value, "parameter_id"), project::max_native_identifier_length)};
      }
      if (kind == "macro") {
        fields(target_value, {"kind", "track_id", "index"});
        return project::MacroTarget{parse_id<project::TrackId>(field(target_value, "track_id")),
                                    unsigned_integer<std::uint32_t>(field(target_value, "index"))};
      }
      throw ParseError("unsupported automation target");
    }();
    auto domain = decode_domain(field(item, "domain"));
    const auto timebase = string(field(item, "timebase"));
    if (timebase != "musical" && timebase != "elapsed") {
      throw ParseError("invalid automation timebase");
    }
    const auto& point_values = field(item, "points");
    check_array(point_values, project::max_automation_points_per_lane);
    std::vector<project::AutomationPoint> points;
    points.reserve(point_values.size());
    for (const auto& point : point_values) {
      fields(point, {"id", "coordinate", "value", "to_next", "handles"});
      const auto& handles = field(point, "handles");
      fields(handles, {"x1", "y1", "x2", "y2"});
      points.push_back({parse_id<project::AutomationPointId>(field(point, "id")),
                        signed_integer(field(point, "coordinate")),
                        real(field(point, "value")),
                        decode_automation_shape(field(point, "to_next")),
                        {real(field(handles, "x1")), real(field(handles, "y1")),
                         real(field(handles, "x2")), real(field(handles, "y2"))}});
    }
    lanes.push_back(
        {parse_id<project::AutomationLaneId>(field(item, "id")), target, std::move(domain),
         timebase == "musical" ? project::AutomationTimebase::musical
                               : project::AutomationTimebase::elapsed,
         std::move(points), unsigned_integer<std::uint64_t>(field(item, "settings_generation"))});
  }
  return lanes;
}

[[nodiscard]] std::string_view warp_mode_name(project::WarpMode mode) {
  switch (mode) {
  case project::WarpMode::repitch:
    return "repitch";
  case project::WarpMode::beats:
    return "beats";
  case project::WarpMode::tones:
    return "tones";
  case project::WarpMode::texture:
    return "texture";
  case project::WarpMode::complex:
    return "complex";
  case project::WarpMode::complex_pro:
    return "complex_pro";
  }
  throw ParseError("invalid clip warp mode");
}

[[nodiscard]] project::WarpMode decode_warp_mode(const Json& value) {
  const auto name = string(value, 16);
  for (const auto mode :
       {project::WarpMode::repitch, project::WarpMode::beats, project::WarpMode::tones,
        project::WarpMode::texture, project::WarpMode::complex, project::WarpMode::complex_pro}) {
    if (name == warp_mode_name(mode))
      return mode;
  }
  throw ParseError("invalid clip warp mode");
}

[[nodiscard]] std::string_view beats_loop_name(project::WarpBeatsLoop loop) {
  switch (loop) {
  case project::WarpBeatsLoop::off:
    return "off";
  case project::WarpBeatsLoop::forward:
    return "forward";
  case project::WarpBeatsLoop::back_and_forth:
    return "back_and_forth";
  }
  throw ParseError("invalid clip Beats loop mode");
}

[[nodiscard]] project::WarpBeatsLoop decode_beats_loop(const Json& value) {
  const auto name = string(value, 16);
  for (const auto loop : {project::WarpBeatsLoop::off, project::WarpBeatsLoop::forward,
                          project::WarpBeatsLoop::back_and_forth}) {
    if (name == beats_loop_name(loop))
      return loop;
  }
  throw ParseError("invalid clip Beats loop mode");
}

[[nodiscard]] Json encode_clip_warp(const project::ClipWarpSettings& warp) {
  return {
      {"mode", warp_mode_name(warp.mode)},
      {"mode_overridden", warp.mode_overridden},
      {"beats", {{"loop", beats_loop_name(warp.beats.loop)}, {"envelope", warp.beats.envelope}}},
      {"tones", {{"grain_ms", warp.tones.grain_ms}}},
      {"texture", {{"grain_ms", warp.texture.grain_ms}, {"flux", warp.texture.flux}}},
      {"complex_pro",
       {{"formants", warp.complex_pro.formants}, {"envelope", warp.complex_pro.envelope}}}};
}

// Range checks belong to project validation, which runs before publication.
[[nodiscard]] project::ClipWarpSettings decode_clip_warp(const Json& value) {
  fields(value, {"mode", "mode_overridden", "beats", "tones", "texture", "complex_pro"});
  project::ClipWarpSettings warp;
  warp.mode = decode_warp_mode(field(value, "mode"));
  const auto& overridden = field(value, "mode_overridden");
  if (!overridden.is_boolean())
    throw ParseError("invalid clip warp override setting");
  warp.mode_overridden = overridden.get<bool>();
  const auto& beats = field(value, "beats");
  fields(beats, {"loop", "envelope"});
  warp.beats = {decode_beats_loop(field(beats, "loop")), real(field(beats, "envelope"))};
  const auto& tones = field(value, "tones");
  fields(tones, {"grain_ms"});
  warp.tones = {real(field(tones, "grain_ms"))};
  const auto& texture = field(value, "texture");
  fields(texture, {"grain_ms", "flux"});
  warp.texture = {real(field(texture, "grain_ms")), real(field(texture, "flux"))};
  const auto& complex_pro = field(value, "complex_pro");
  fields(complex_pro, {"formants", "envelope"});
  warp.complex_pro = {real(field(complex_pro, "formants")), real(field(complex_pro, "envelope"))};
  return warp;
}

// A colour is null or an sRGB 0xRRGGBB integer.
[[nodiscard]] Json encode_color(const std::optional<std::uint32_t>& color) {
  return color ? Json(*color) : Json(nullptr);
}

[[nodiscard]] std::optional<std::uint32_t> decode_color(const Json& value) {
  if (value.is_null())
    return std::nullopt;
  const auto color = unsigned_integer<std::uint32_t>(value);
  if (color > project::max_presentation_color)
    throw ParseError("invalid colour");
  return color;
}

// Clay note edits (ADR-0081).
[[nodiscard]] Json encode_clay(const std::optional<project::ClayTake>& take) {
  if (!take)
    return nullptr;
  const auto mode = [](project::ClayMode value) {
    switch (value) {
    case project::ClayMode::melodic:
      return "melodic";
    case project::ClayMode::polyphonic:
      return "polyphonic";
    case project::ClayMode::percussive:
      return "percussive";
    }
    return "melodic";
  };
  Json ops = Json::array();
  for (const auto& op : take->ops)
    ops.push_back({{"kind", op.kind == project::ClayOpKind::join ? "join" : "split"},
                   {"note", op.note},
                   {"other", op.other},
                   {"frame", op.frame}});
  Json edits = Json::array();
  for (const auto& edit : take->edits) {
    Json placed = nullptr;
    if (edit.start && edit.end)
      placed = {{"start", *edit.start}, {"end", *edit.end}};
    edits.push_back({{"note", edit.note},
                     {"voice", edit.voice},
                     {"pitch_cents", edit.pitch_cents},
                     {"modulation", edit.modulation},
                     {"drift", edit.drift},
                     {"formant_cents", edit.formant_cents},
                     {"gain_db", edit.gain_db},
                     {"muted", edit.muted},
                     {"placement", std::move(placed)}});
  }
  return {{"mode", mode(take->mode)},
          {"analysis_version", take->analysis_version},
          {"ops", std::move(ops)},
          {"edits", std::move(edits)},
          {"key", take->key ? encode_key(*take->key) : Json(nullptr)}};
}

[[nodiscard]] Json encode_clip(const project::Clip& clip) {
  Json markers = Json::array();
  for (const auto& marker : clip.processing.reviewed_warp_markers) {
    markers.push_back({{"source_frame", marker.source_frame.value},
                       {"target_offset", std::visit([](const auto& offset) { return offset.value; },
                                                    marker.target_offset)}});
  }
  Json result{{"id", clip.id.to_string()},
              {"source", clip.source.to_string()},
              {"source_start", clip.source_range.start.value},
              {"source_end", clip.source_range.end.value},
              {"settings_generation", clip.settings_generation},
              {"processing",
               {{"pitch_semitones", clip.processing.pitch_semitones},
                {"preserve_formants", clip.processing.preserve_formants},
                {"reviewed_warp_markers", std::move(markers)},
                {"warp", encode_clip_warp(clip.processing.warp)}}},
              {"fades",
               {{"in_fraction", clip.fades.in_fraction},
                {"out_fraction", clip.fades.out_fraction},
                {"in_curve",
                 clip.fades.in_curve == project::FadeCurve::smoothstep ? "smoothstep" : "linear"},
                {"out_curve", clip.fades.out_curve == project::FadeCurve::smoothstep ? "smoothstep"
                                                                                     : "linear"}}}};
  result["automation"] = encode_automation(clip.automation);
  result["color"] = encode_color(clip.color);
  result["name"] = clip.name;
  result["clay"] = encode_clay(clip.clay);
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    result["anchor"] = "musical";
    result["start"] = musical->start.value;
    result["end"] = musical->end.value;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    result["anchor"] = "elapsed";
    result["start"] = elapsed.start.value;
    result["end"] = elapsed.end.value;
  }
  return result;
}

[[nodiscard]] std::string_view pedal_name(project::MidiPedal pedal) {
  switch (pedal) {
  case project::MidiPedal::sostenuto:
    return "sostenuto";
  case project::MidiPedal::soft:
    return "soft";
  default:
    return "sustain";
  }
}

[[nodiscard]] project::MidiPedal parse_pedal(std::string_view name) {
  if (name == "sustain") {
    return project::MidiPedal::sustain;
  }
  if (name == "sostenuto") {
    return project::MidiPedal::sostenuto;
  }
  if (name == "soft") {
    return project::MidiPedal::soft;
  }
  throw ParseError("invalid MIDI pedal");
}

[[nodiscard]] Json encode_midi_clip(const project::MidiClip& clip) {
  Json notes = Json::array();
  for (const auto& note : clip.notes) {
    notes.push_back({{"id", note.id.to_string()},
                     {"start", note.start.value},
                     {"end", note.end.value},
                     {"key", note.key},
                     {"velocity", note.velocity}});
  }
  Json result{{"id", clip.id.to_string()},
              {"pattern_length", clip.pattern_length.value},
              {"loop", clip.loop},
              {"notes", std::move(notes)},
              {"settings_generation", clip.settings_generation}};
  result["automation"] = encode_automation(clip.automation);
  Json pedals = Json::array();
  for (const auto& lane : clip.pedals) {
    Json points = Json::array();
    for (const auto& point : lane.points) {
      points.push_back({{"position", point.position.value}, {"value", point.value}});
    }
    pedals.push_back({{"pedal", pedal_name(lane.pedal)}, {"points", std::move(points)}});
  }
  result["pedals"] = std::move(pedals);
  result["color"] = encode_color(clip.color);
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    result["anchor"] = "musical";
    result["start"] = musical->start.value;
    result["end"] = musical->end.value;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    result["anchor"] = "elapsed";
    result["start"] = elapsed.start.value;
    result["end"] = elapsed.end.value;
  }
  return result;
}

// Sustain, sostenuto and soft pedal lanes (ADR-0035).
[[nodiscard]] std::vector<project::MidiPedalLane> decode_midi_pedals(const Json& value) {
  check_array(value, project::midi_pedal_count);
  std::vector<project::MidiPedalLane> lanes;
  for (const auto& lane_value : value) {
    fields(lane_value, {"pedal", "points"});
    project::MidiPedalLane lane;
    lane.pedal = parse_pedal(string(field(lane_value, "pedal")));
    const auto& point_values = field(lane_value, "points");
    check_array(point_values, project::max_midi_pedal_points_per_lane);
    for (const auto& point : point_values) {
      fields(point, {"position", "value"});
      lane.points.push_back(
          {{signed_integer(field(point, "position"))}, real(field(point, "value"))});
    }
    lanes.push_back(std::move(lane));
  }
  return lanes;
}

[[nodiscard]] project::MidiClip decode_midi_clip(const Json& value) {
  fields(value, {"id", "anchor", "start", "end", "pattern_length", "loop", "notes",
                 "settings_generation", "automation", "pedals", "color"});
  const auto anchor = string(field(value, "anchor"));
  const auto start = signed_integer(field(value, "start"));
  const auto end = signed_integer(field(value, "end"));
  project::Placement placement;
  if (anchor == "musical") {
    placement = project::MusicalInterval{{start}, {end}};
  } else if (anchor == "elapsed") {
    placement = project::ElapsedInterval{{start}, {end}};
  } else {
    throw ParseError("invalid MIDI clip anchor");
  }
  const auto& loop_value = field(value, "loop");
  if (!loop_value.is_boolean()) {
    throw ParseError("invalid MIDI loop flag");
  }
  std::vector<project::MidiNote> notes;
  const auto& note_values = field(value, "notes");
  check_array(note_values, project::max_midi_notes_per_clip);
  for (const auto& note : note_values) {
    fields(note, {"id", "start", "end", "key", "velocity"});
    notes.push_back({parse_id<project::NoteId>(field(note, "id")),
                     {signed_integer(field(note, "start"))},
                     {signed_integer(field(note, "end"))},
                     unsigned_integer<std::uint8_t>(field(note, "key")),
                     unsigned_integer<std::uint8_t>(field(note, "velocity"))});
  }
  project::MidiClip clip{
      parse_id<project::ClipId>(field(value, "id")),
      placement,
      {signed_integer(field(value, "pattern_length"))},
      loop_value.get<bool>(),
      std::move(notes),
      unsigned_integer<std::uint64_t>(field(value, "settings_generation")),
      decode_automation(field(value, "automation"), project::max_automation_lanes_per_clip),
      decode_midi_pedals(field(value, "pedals"))};
  clip.color = decode_color(field(value, "color"));
  return clip;
}

[[nodiscard]] project::TrackKind decode_track_kind(const Json& value) {
  const auto name = string(value);
  if (name == "audio") {
    return project::TrackKind::audio;
  }
  if (name == "instrument") {
    return project::TrackKind::instrument;
  }
  if (name == "group") {
    return project::TrackKind::group_bus;
  }
  if (name == "return") {
    return project::TrackKind::return_bus;
  }
  throw ParseError("invalid track kind");
}

[[nodiscard]] const char* track_kind_name(project::TrackKind kind) {
  switch (kind) {
  case project::TrackKind::audio:
    return "audio";
  case project::TrackKind::instrument:
    return "instrument";
  case project::TrackKind::group_bus:
    return "group";
  case project::TrackKind::return_bus:
    return "return";
  }
  return "invalid";
}

[[nodiscard]] Json encode_managed_audio(const project::ManagedAudioReference& reference) {
  return {{"source", reference.source.to_string()},   {"sha256", reference.sha256},
          {"encoded_bytes", reference.encoded_bytes}, {"sample_rate", reference.sample_rate},
          {"channels", reference.channels},           {"frames", reference.frames}};
}

[[nodiscard]] project::ManagedAudioReference decode_managed_audio(const Json& value) {
  fields(value, {"source", "sha256", "encoded_bytes", "sample_rate", "channels", "frames"});
  return {parse_id<project::SourceId>(field(value, "source")),
          string(field(value, "sha256"), 64),
          unsigned_integer<std::uint64_t>(field(value, "encoded_bytes")),
          unsigned_integer<std::uint32_t>(field(value, "sample_rate")),
          unsigned_integer<std::uint32_t>(field(value, "channels")),
          unsigned_integer<std::uint64_t>(field(value, "frames"))};
}

[[nodiscard]] bool boolean(const Json& value) {
  if (!value.is_boolean())
    throw ParseError("expected JSON boolean");
  return value.get<bool>();
}

[[nodiscard]] Json
encode_sparse_parameters(const std::vector<project::NativeInstrumentParameter>& values) {
  Json result = Json::array();
  for (const auto& parameter : values)
    result.push_back({{"id", parameter.id}, {"value", parameter.value}});
  return result;
}

[[nodiscard]] std::vector<project::NativeInstrumentParameter>
decode_sparse_parameters(const Json& entries) {
  check_array(entries, project::max_native_instrument_parameters);
  std::vector<project::NativeInstrumentParameter> values;
  values.reserve(entries.size());
  for (const auto& entry : entries) {
    fields(entry, {"id", "value"});
    values.push_back({string(field(entry, "id"), project::max_native_identifier_length),
                      real(field(entry, "value"))});
  }
  return values;
}

// ADR-0059: note effects, macros and a frozen render.
[[nodiscard]] Json encode_note_effects(const std::vector<project::NoteEffectInsert>& effects) {
  Json result = Json::array();
  for (const auto& effect : effects)
    result.push_back({{"id", effect.id.to_string()},
                      {"enabled", effect.enabled},
                      {"engine", effect.engine},
                      {"engine_version", effect.engine_version},
                      {"parameters", encode_sparse_parameters(effect.parameters)}});
  return result;
}

[[nodiscard]] std::vector<project::NoteEffectInsert> decode_note_effects(const Json& value) {
  check_array(value, project::max_note_effects_per_track);
  std::vector<project::NoteEffectInsert> effects;
  for (const auto& item : value) {
    fields(item, {"id", "enabled", "engine", "engine_version", "parameters"});
    effects.push_back({parse_id<project::InsertSlotId>(field(item, "id")),
                       boolean(field(item, "enabled")),
                       string(field(item, "engine"), project::max_native_identifier_length),
                       unsigned_integer<std::uint32_t>(field(item, "engine_version")),
                       decode_sparse_parameters(field(item, "parameters"))});
  }
  return effects;
}

[[nodiscard]] Json encode_macros(const std::vector<project::Macro>& macros) {
  Json result = Json::array();
  for (const auto& macro : macros) {
    Json mappings = Json::array();
    for (const auto& mapping : macro.mappings) {
      Json destination;
      if (const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
        destination = {{"kind", "native_parameter"},
                       {"track_id", native->track.to_string()},
                       {"parameter_id", native->parameter_id}};
      } else {
        const auto& effect = std::get<project::NativeEffectParameterTarget>(mapping.destination);
        destination = {{"kind", "native_effect_parameter"},
                       {"slot_id", effect.slot.to_string()},
                       {"parameter_id", effect.parameter_id}};
      }
      mappings.push_back({{"destination", std::move(destination)},
                          {"domain", encode_domain(mapping.domain)},
                          {"minimum", mapping.minimum},
                          {"maximum", mapping.maximum}});
    }
    result.push_back(
        {{"name", macro.name}, {"value", macro.value}, {"mappings", std::move(mappings)}});
  }
  return result;
}

[[nodiscard]] std::vector<project::Macro> decode_macros(const Json& value) {
  check_array(value, project::max_macros_per_track);
  std::vector<project::Macro> macros;
  for (const auto& item : value) {
    fields(item, {"name", "value", "mappings"});
    project::Macro macro{
        string(field(item, "name"), project::max_macro_name_bytes), real(field(item, "value")), {}};
    const auto& mappings = field(item, "mappings");
    check_array(mappings, project::max_macro_mappings);
    for (const auto& mapping : mappings) {
      fields(mapping, {"destination", "domain", "minimum", "maximum"});
      const auto& destination = field(mapping, "destination");
      const auto kind = string(field(destination, "kind"));
      project::MacroDestination parsed = [&]() -> project::MacroDestination {
        if (kind == "native_parameter") {
          fields(destination, {"kind", "track_id", "parameter_id"});
          return project::NativeParameterTarget{
              parse_id<project::TrackId>(field(destination, "track_id")),
              string(field(destination, "parameter_id"), project::max_native_identifier_length)};
        }
        if (kind == "native_effect_parameter") {
          fields(destination, {"kind", "slot_id", "parameter_id"});
          return project::NativeEffectParameterTarget{
              parse_id<project::InsertSlotId>(field(destination, "slot_id")),
              string(field(destination, "parameter_id"), project::max_native_identifier_length)};
        }
        throw ParseError("unsupported macro destination");
      }();
      macro.mappings.push_back({std::move(parsed), decode_domain(field(mapping, "domain")),
                                real(field(mapping, "minimum")), real(field(mapping, "maximum"))});
    }
    macros.push_back(std::move(macro));
  }
  return macros;
}

[[nodiscard]] Json encode_frozen(const std::optional<project::FrozenTrack>& frozen) {
  if (!frozen)
    return nullptr;
  return {{"audio", encode_managed_audio(frozen->audio)},
          {"first_frame", frozen->first_frame},
          {"rendered_revision", frozen->rendered_revision}};
}

[[nodiscard]] std::optional<project::FrozenTrack> decode_frozen(const Json& value) {
  if (value.is_null())
    return std::nullopt;
  fields(value, {"audio", "first_frame", "rendered_revision"});
  return project::FrozenTrack{decode_managed_audio(field(value, "audio")),
                              unsigned_integer<std::uint64_t>(field(value, "first_frame")),
                              unsigned_integer<std::uint64_t>(field(value, "rendered_revision"))};
}

// Structured synth state (ADR-0057).
[[nodiscard]] Json encode_native_document(const project::NativeSynthDocument& document) {
  Json routes = Json::array();
  for (const auto& route : document.routes)
    routes.push_back({{"slot", route.slot},
                      {"source", route.source},
                      {"aux", route.aux},
                      {"destination", route.destination},
                      {"bipolar", route.bipolar},
                      {"tension", route.tension},
                      {"scale", route.scale}});
  Json curves = Json::array();
  for (const auto& curve : document.curves) {
    Json points = Json::array();
    for (const auto& point : curve.points)
      points.push_back(
          {{"x", point.x},
           {"y", point.y},
           {"tension", point.tension},
           {"segment", point.segment == project::NativeCurveSegment::step ? "step" : "curve"}});
    curves.push_back({{"id", curve.id}, {"points", std::move(points)}});
  }
  Json chains = Json::array();
  for (const auto& chain : document.chains) {
    Json slots = Json::array();
    for (const auto& slot : chain.modules)
      slots.push_back({{"uid", slot.uid}, {"module", slot.module}});
    chains.push_back({{"id", chain.id}, {"modules", std::move(slots)}});
  }
  Json sequences = Json::array();
  for (const auto& sequence : document.sequences) {
    Json notes = Json::array();
    for (const auto& note : sequence.notes)
      notes.push_back({{"start", note.start.value},
                       {"length", note.length.value},
                       {"key", note.key},
                       {"velocity", note.velocity}});
    sequences.push_back(
        {{"id", sequence.id}, {"length", sequence.length.value}, {"notes", std::move(notes)}});
  }
  Json media = Json::array();
  for (const auto& slot : document.media) {
    Json entry{{"id", slot.id}, {"frame_length", slot.frame_length}};
    if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media)) {
      entry["builtin"] = builtin->name;
      entry["managed"] = nullptr;
    } else {
      entry["builtin"] = nullptr;
      entry["managed"] = encode_managed_audio(std::get<project::ManagedAudioReference>(slot.media));
    }
    media.push_back(std::move(entry));
  }
  Json labels = Json::array();
  for (const auto& label : document.labels)
    labels.push_back({{"id", label.id}, {"text", label.text}});
  Json result{{"routes", std::move(routes)}, {"curves", std::move(curves)},
              {"chains", std::move(chains)}, {"sequences", std::move(sequences)},
              {"media", std::move(media)},   {"labels", std::move(labels)}};
  // Regions are omitted when the document has none.
  if (!document.regions.empty()) {
    Json regions = Json::array();
    for (const auto& region : document.regions)
      regions.push_back({{"slot", region.slot},
                         {"media", region.media},
                         {"start_frame", region.start_frame},
                         {"tag", region.tag},
                         {"name", region.name}});
    result["regions"] = std::move(regions);
  }
  // Voice lanes (ADR-0073) are likewise omitted when empty.
  if (!document.voice_lanes.empty()) {
    Json lanes = Json::array();
    for (const auto& lane : document.voice_lanes) {
      Json values = Json::array();
      for (const auto& value : lane.values)
        values.push_back({{"voice", value.voice}, {"value", value.value}});
      lanes.push_back({{"id", lane.id}, {"values", std::move(values)}});
    }
    result["voice_lanes"] = std::move(lanes);
  }
  return result;
}

[[nodiscard]] project::NativeSynthDocument decode_native_document(const Json& value) {
  // Regions and voice lanes are optional sections, written only when used.
  const bool has_regions = value.is_object() && value.contains("regions");
  const bool has_voice_lanes = value.is_object() && value.contains("voice_lanes");
  constexpr std::array<std::string_view, 6> required{"routes",    "curves", "chains",
                                                     "sequences", "media",  "labels"};
  const std::size_t optional = (has_regions ? 1U : 0U) + (has_voice_lanes ? 1U : 0U);
  if (!value.is_object() || value.size() != required.size() + optional)
    throw ParseError("unexpected JSON object fields");
  for (const auto name : required) {
    if (!value.contains(std::string(name)))
      throw ParseError("missing JSON field");
  }
  constexpr auto id_limit = project::max_native_identifier_length;
  project::NativeSynthDocument document;
  const auto& routes = field(value, "routes");
  check_array(routes, project::max_native_synth_routes);
  for (const auto& route : routes) {
    fields(route, {"slot", "source", "aux", "destination", "bipolar", "tension", "scale"});
    document.routes.push_back(
        {unsigned_integer<std::uint32_t>(field(route, "slot")),
         string(field(route, "source"), id_limit), string(field(route, "aux"), id_limit),
         string(field(route, "destination"), id_limit), boolean(field(route, "bipolar")),
         real(field(route, "tension")), real(field(route, "scale"))});
  }
  const auto& curves = field(value, "curves");
  check_array(curves, project::max_native_synth_curves);
  for (const auto& curve_value : curves) {
    fields(curve_value, {"id", "points"});
    project::NativeCurve curve{string(field(curve_value, "id"), id_limit), {}};
    const auto& points = field(curve_value, "points");
    check_array(points, project::max_native_curve_points);
    for (const auto& point : points) {
      fields(point, {"x", "y", "tension", "segment"});
      const auto segment = string(field(point, "segment"));
      if (segment != "curve" && segment != "step")
        throw ParseError("invalid synth curve segment");
      curve.points.push_back({real(field(point, "x")), real(field(point, "y")),
                              real(field(point, "tension")),
                              segment == "step" ? project::NativeCurveSegment::step
                                                : project::NativeCurveSegment::curve});
    }
    document.curves.push_back(std::move(curve));
  }
  const auto& chains = field(value, "chains");
  check_array(chains, project::max_native_synth_chains);
  for (const auto& chain_value : chains) {
    fields(chain_value, {"id", "modules"});
    project::NativeChain chain{string(field(chain_value, "id"), id_limit), {}};
    const auto& slots = field(chain_value, "modules");
    check_array(slots, project::max_native_chain_slots);
    for (const auto& slot : slots) {
      fields(slot, {"uid", "module"});
      chain.modules.push_back({unsigned_integer<std::uint32_t>(field(slot, "uid")),
                               string(field(slot, "module"), id_limit)});
    }
    document.chains.push_back(std::move(chain));
  }
  const auto& sequences = field(value, "sequences");
  check_array(sequences, project::max_native_synth_sequences);
  for (const auto& sequence_value : sequences) {
    fields(sequence_value, {"id", "length", "notes"});
    project::NativeSequence sequence{string(field(sequence_value, "id"), id_limit),
                                     {signed_integer(field(sequence_value, "length"))},
                                     {}};
    const auto& notes = field(sequence_value, "notes");
    check_array(notes, project::max_native_sequence_notes);
    for (const auto& note : notes) {
      fields(note, {"start", "length", "key", "velocity"});
      sequence.notes.push_back({{signed_integer(field(note, "start"))},
                                {signed_integer(field(note, "length"))},
                                unsigned_integer<std::uint8_t>(field(note, "key")),
                                unsigned_integer<std::uint8_t>(field(note, "velocity"))});
    }
    document.sequences.push_back(std::move(sequence));
  }
  const auto& media = field(value, "media");
  check_array(media, project::max_native_synth_media);
  for (const auto& slot : media) {
    fields(slot, {"id", "frame_length", "builtin", "managed"});
    const auto& builtin = field(slot, "builtin");
    const auto& managed = field(slot, "managed");
    if (builtin.is_null() == managed.is_null())
      throw ParseError("synth media needs exactly one of builtin and managed");
    project::NativeMediaSlot entry{string(field(slot, "id"), id_limit),
                                   project::NativeBuiltinMedia{},
                                   unsigned_integer<std::uint32_t>(field(slot, "frame_length"))};
    if (builtin.is_null())
      entry.media = decode_managed_audio(managed);
    else
      entry.media = project::NativeBuiltinMedia{string(builtin, id_limit)};
    document.media.push_back(std::move(entry));
  }
  const auto& labels = field(value, "labels");
  check_array(labels, project::max_native_synth_labels);
  for (const auto& label : labels) {
    fields(label, {"id", "text"});
    document.labels.push_back({string(field(label, "id"), id_limit),
                               string(field(label, "text"), project::max_native_label_length)});
  }
  if (has_regions) {
    const auto& regions = field(value, "regions");
    // Written only when non-empty: an empty array is not a saved layout.
    if (regions.empty())
      throw ParseError("empty synth regions");
    check_array(regions, project::max_native_regions);
    for (const auto& region : regions) {
      fields(region, {"slot", "media", "start_frame", "tag", "name"});
      document.regions.push_back({unsigned_integer<std::uint32_t>(field(region, "slot")),
                                  string(field(region, "media"), id_limit),
                                  unsigned_integer<std::uint64_t>(field(region, "start_frame")),
                                  string(field(region, "tag"), project::max_native_region_tag),
                                  string(field(region, "name"), project::max_native_region_name)});
    }
  }
  if (has_voice_lanes) {
    const auto& lanes = field(value, "voice_lanes");
    if (lanes.empty())
      throw ParseError("empty synth voice lanes");
    check_array(lanes, project::max_native_voice_lanes);
    for (const auto& lane_value : lanes) {
      fields(lane_value, {"id", "values"});
      project::NativeVoiceLane lane{string(field(lane_value, "id"), id_limit), {}};
      const auto& values = field(lane_value, "values");
      check_array(values, project::max_native_voice_lane_values);
      for (const auto& entry : values) {
        fields(entry, {"voice", "value"});
        lane.values.push_back(
            {unsigned_integer<std::uint32_t>(field(entry, "voice")), real(field(entry, "value"))});
      }
      document.voice_lanes.push_back(std::move(lane));
    }
  }
  return document;
}

[[nodiscard]] Json encode_instrument_source(const project::InstrumentSource& source) {
  return std::visit(
      [](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return {{"kind", "none"}, {"payload", nullptr}};
        } else if constexpr (std::is_same_v<T, project::Vst3PluginInstance>) {
          return {{"kind", "vst3_instrument"}, {"payload", encode_plugin(value)}};
        } else {
          static_assert(std::is_same_v<T, project::NativeInstrumentPatch>);
          Json parameters = Json::array();
          for (const auto& parameter : value.parameters)
            parameters.push_back({{"id", parameter.id}, {"value", parameter.value}});
          return {{"kind", "native_instrument"},
                  {"payload",
                   {{"engine", value.engine},
                    {"engine_version", value.engine_version},
                    {"parameters", std::move(parameters)},
                    {"document", encode_native_document(value.document)}}}};
        }
      },
      source);
}

[[nodiscard]] project::InstrumentSource decode_instrument_source(const Json& value) {
  fields(value, {"kind", "payload"});
  const auto kind = string(field(value, "kind"));
  const auto& payload = field(value, "payload");
  if (kind == "none") {
    if (!payload.is_null())
      throw ParseError("invalid empty instrument source");
    return std::monostate{};
  }
  if (kind == "vst3_instrument")
    return decode_plugin(payload);
  if (kind == "native_instrument") {
    fields(payload, {"engine", "engine_version", "parameters", "document"});
    project::NativeInstrumentPatch patch;
    patch.engine = string(field(payload, "engine"), project::max_native_identifier_length);
    patch.engine_version = unsigned_integer<std::uint32_t>(field(payload, "engine_version"));
    const auto& entries = field(payload, "parameters");
    check_array(entries, project::max_native_instrument_parameters);
    patch.parameters.reserve(entries.size());
    for (const auto& entry : entries) {
      fields(entry, {"id", "value"});
      patch.parameters.push_back({string(field(entry, "id"), project::max_native_identifier_length),
                                  real(field(entry, "value"))});
    }
    patch.document = decode_native_document(field(payload, "document"));
    return patch;
  }
  throw ParseError("unknown instrument source kind");
}

[[nodiscard]] Json encode_effect(const project::EffectInsert& slot) {
  auto [kind, payload] = std::visit(
      [](const auto& value) -> std::pair<std::string_view, Json> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, project::NativeConvolution>) {
          const auto& ir = value.ir;
          return {"native_convolution",
                  {{"generation", value.generation},
                   {"wet", value.wet},
                   {"ir", encode_managed_audio(ir)}}};
        } else if constexpr (std::is_same_v<T, project::NativeEffectPatch>) {
          Json parameters = Json::array();
          for (const auto& parameter : value.parameters)
            parameters.push_back({{"id", parameter.id}, {"value", parameter.value}});
          return {"native_effect",
                  {{"generation", value.generation},
                   {"engine", value.engine},
                   {"engine_version", value.engine_version},
                   {"parameters", std::move(parameters)},
                   {"document", encode_native_document(value.document)},
                   {"link", value.link}}};
        } else {
          return {"vst3_effect", encode_plugin(value)};
        }
      },
      slot.processor);
  return {{"id", slot.id.to_string()},
          {"enabled", slot.enabled},
          {"kind", kind},
          {"payload", std::move(payload)}};
}

[[nodiscard]] project::EffectInsert decode_effect(const Json& value) {
  fields(value, {"id", "enabled", "kind", "payload"});
  const auto& enabled = field(value, "enabled");
  if (!enabled.is_boolean())
    throw ParseError("invalid effect enabled state");
  const auto id = parse_id<project::InsertSlotId>(field(value, "id"));
  const auto kind = string(field(value, "kind"));
  const auto& payload = field(value, "payload");
  if (kind == "vst3_effect")
    return {id, enabled.get<bool>(), decode_plugin(payload)};
  if (kind == "native_convolution") {
    fields(payload, {"generation", "wet", "ir"});
    return {id, enabled.get<bool>(),
            project::NativeConvolution{
                decode_managed_audio(field(payload, "ir")), real(field(payload, "wet")),
                unsigned_integer<std::uint64_t>(field(payload, "generation"))}};
  }
  if (kind == "native_effect") {
    fields(payload, {"generation", "engine", "engine_version", "parameters", "document", "link"});
    project::NativeEffectPatch patch;
    patch.generation = unsigned_integer<std::uint64_t>(field(payload, "generation"));
    patch.engine = string(field(payload, "engine"), project::max_native_identifier_length);
    patch.engine_version = unsigned_integer<std::uint32_t>(field(payload, "engine_version"));
    const auto& entries = field(payload, "parameters");
    check_array(entries, project::max_native_instrument_parameters);
    patch.parameters.reserve(entries.size());
    for (const auto& entry : entries) {
      fields(entry, {"id", "value"});
      patch.parameters.push_back({string(field(entry, "id"), project::max_native_identifier_length),
                                  real(field(entry, "value"))});
    }
    patch.document = decode_native_document(field(payload, "document"));
    patch.link = string(field(payload, "link"), project::max_native_identifier_length);
    return {id, enabled.get<bool>(), std::move(patch)};
  }
  throw ParseError("unknown effect kind");
}

[[nodiscard]] std::optional<project::ClayTake> decode_clay(const Json& value) {
  if (value.is_null())
    return std::nullopt;
  fields(value, {"mode", "analysis_version", "ops", "edits", "key"});
  project::ClayTake take;
  const auto mode = string(field(value, "mode"));
  if (mode == "melodic")
    take.mode = project::ClayMode::melodic;
  else if (mode == "polyphonic")
    take.mode = project::ClayMode::polyphonic;
  else if (mode == "percussive")
    take.mode = project::ClayMode::percussive;
  else
    throw ParseError("invalid Clay mode");
  take.analysis_version = unsigned_integer<std::uint32_t>(field(value, "analysis_version"));
  const auto& ops = field(value, "ops");
  check_array(ops, project::max_clay_ops);
  for (const auto& item : ops) {
    fields(item, {"kind", "note", "other", "frame"});
    const auto kind = string(field(item, "kind"));
    if (kind != "split" && kind != "join")
      throw ParseError("invalid Clay operation");
    take.ops.push_back({kind == "join" ? project::ClayOpKind::join : project::ClayOpKind::split,
                        unsigned_integer<std::uint32_t>(field(item, "note")),
                        unsigned_integer<std::uint32_t>(field(item, "other")),
                        signed_integer(field(item, "frame"))});
  }
  const auto& edits = field(value, "edits");
  check_array(edits, project::max_clay_edits);
  for (const auto& item : edits) {
    fields(item, {"note", "voice", "pitch_cents", "modulation", "drift", "formant_cents", "gain_db",
                  "muted", "placement"});
    project::ClayNoteEdit edit;
    edit.note = unsigned_integer<std::uint32_t>(field(item, "note"));
    edit.voice = unsigned_integer<std::uint32_t>(field(item, "voice"));
    edit.pitch_cents = real(field(item, "pitch_cents"));
    edit.modulation = real(field(item, "modulation"));
    edit.drift = real(field(item, "drift"));
    edit.formant_cents = real(field(item, "formant_cents"));
    edit.gain_db = real(field(item, "gain_db"));
    edit.muted = boolean(field(item, "muted"));
    const auto& placed = field(item, "placement");
    if (!placed.is_null()) {
      fields(placed, {"start", "end"});
      edit.start = signed_integer(field(placed, "start"));
      edit.end = signed_integer(field(placed, "end"));
    }
    take.edits.push_back(edit);
  }
  const auto& key = field(value, "key");
  if (!key.is_null())
    take.key = decode_key(key);
  return take;
}

[[nodiscard]] project::Clip decode_clip(const Json& value) {
  fields(value,
         {"id", "source", "source_start", "source_end", "anchor", "start", "end",
          "settings_generation", "automation", "fades", "processing", "color", "name", "clay"});
  const auto anchor = string(field(value, "anchor"));
  const auto start = signed_integer(field(value, "start"));
  const auto end = signed_integer(field(value, "end"));
  project::Placement placement;
  if (anchor == "musical") {
    placement = project::MusicalInterval{{start}, {end}};
  } else if (anchor == "elapsed") {
    placement = project::ElapsedInterval{{start}, {end}};
  } else {
    throw ParseError("invalid clip anchor");
  }
  project::ClipFades fades;
  {
    const auto& saved = field(value, "fades");
    fields(saved, {"in_fraction", "out_fraction", "in_curve", "out_curve"});
    const auto curve = [](const Json& item) {
      const auto name = string(item);
      if (name == "linear")
        return project::FadeCurve::linear;
      if (name == "smoothstep")
        return project::FadeCurve::smoothstep;
      throw ParseError("invalid clip fade curve");
    };
    fades = {real(field(saved, "in_fraction")), real(field(saved, "out_fraction")),
             curve(field(saved, "in_curve")), curve(field(saved, "out_curve"))};
  }
  project::ClipProcessing processing;
  {
    const auto& saved = field(value, "processing");
    fields(saved, {"pitch_semitones", "preserve_formants", "reviewed_warp_markers", "warp"});
    processing.warp = decode_clip_warp(field(saved, "warp"));
    processing.pitch_semitones = real(field(saved, "pitch_semitones"));
    const auto& preserve = field(saved, "preserve_formants");
    if (!preserve.is_boolean())
      throw ParseError("invalid clip formant setting");
    processing.preserve_formants = preserve.get<bool>();
    const auto& markers = field(saved, "reviewed_warp_markers");
    check_array(markers, project::max_clip_warp_markers);
    for (const auto& marker : markers) {
      fields(marker, {"source_frame", "target_offset"});
      const auto source_frame = signed_integer(field(marker, "source_frame"));
      const auto target_offset = signed_integer(field(marker, "target_offset"));
      processing.reviewed_warp_markers.push_back(
          {{source_frame},
           anchor == "musical"
               ? std::variant<timeline::TickPosition,
                              timeline::SamplePosition>{timeline::TickPosition{target_offset}}
               : std::variant<timeline::TickPosition, timeline::SamplePosition>{
                     timeline::SamplePosition{target_offset}}});
    }
  }
  project::Clip clip{
      parse_id<project::ClipId>(field(value, "id")),
      parse_id<project::SourceId>(field(value, "source")),
      {{signed_integer(field(value, "source_start"))},
       {signed_integer(field(value, "source_end"))}},
      placement,
      unsigned_integer<std::uint64_t>(field(value, "settings_generation")),
      decode_automation(field(value, "automation"), project::max_automation_lanes_per_clip),
      fades,
      std::move(processing)};
  clip.color = decode_color(field(value, "color"));
  clip.name = string(field(value, "name"));
  clip.clay = decode_clay(field(value, "clay"));
  return clip;
}

[[nodiscard]] std::string_view source_encoding_name(project::SourceEncoding encoding) {
  switch (encoding) {
  case project::SourceEncoding::wav:
    return "wav";
  case project::SourceEncoding::aiff:
    return "aiff";
  case project::SourceEncoding::flac:
    return "flac";
  case project::SourceEncoding::mp3:
    return "mp3";
  }
  return {};
}

[[nodiscard]] project::SourceRecord decode_source(const Json& value) {
  fields(value, {"id", "encoding", "sha256", "byte_length", "sample_rate", "channels", "frames",
                 "location"});
  const auto name = string(field(value, "encoding"));
  project::SourceEncoding encoding;
  if (name == "wav")
    encoding = project::SourceEncoding::wav;
  else if (name == "aiff")
    encoding = project::SourceEncoding::aiff;
  else if (name == "flac")
    encoding = project::SourceEncoding::flac;
  else if (name == "mp3")
    encoding = project::SourceEncoding::mp3;
  else
    throw ParseError("unsupported source encoding");
  auto hash = string(field(value, "sha256"));
  if (!hex_name(hash, 64)) {
    throw ParseError("invalid source hash");
  }
  std::optional<project::ExternalSourceLink> external_link;
  {
    const auto& location = field(value, "location");
    if (!location.is_object())
      throw ParseError("invalid source location");
    const auto kind = string(field(location, "kind"));
    if (kind == "managed") {
      fields(location, {"kind"});
    } else if (kind == "external") {
      fields(location, {"kind", "last_known_path"});
      external_link = project::ExternalSourceLink{
          string(field(location, "last_known_path"), project::max_external_source_path_bytes)};
    } else {
      throw ParseError("invalid source location");
    }
  }
  return {parse_id<project::SourceId>(field(value, "id")),
          encoding,
          std::move(hash),
          unsigned_integer<std::uint64_t>(field(value, "byte_length")),
          unsigned_integer<std::uint32_t>(field(value, "sample_rate")),
          unsigned_integer<std::uint32_t>(field(value, "channels")),
          unsigned_integer<std::uint64_t>(field(value, "frames")),
          std::move(external_link)};
}

} // namespace

std::string encode_manifest(const project::Snapshot& snapshot, std::string_view generation,
                            const std::optional<std::string>& parent, SaveKind kind) {
  Json tempo_events = Json::array();
  for (const auto& event : snapshot.tempo.events()) {
    tempo_events.push_back({{"tick", event.tick.value},
                            {"bpm", event.bpm},
                            {"shape", shape_name(event.shape_to_next)}});
  }
  Json meter_events = Json::array();
  for (const auto& event : snapshot.meter.events()) {
    meter_events.push_back({{"tick", event.tick.value},
                            {"numerator", event.numerator},
                            {"denominator", event.denominator}});
  }
  Json tracks = Json::array();
  for (const auto& track : snapshot.tracks) {
    Json clips = Json::array();
    for (const auto& clip : track.clips) {
      clips.push_back(encode_clip(clip));
    }
    Json midi_clips = Json::array();
    for (const auto& clip : track.midi_clips) {
      midi_clips.push_back(encode_midi_clip(clip));
    }
    Json effects = Json::array();
    for (const auto& slot : track.effects) {
      effects.push_back(encode_effect(slot));
    }
    Json sends = Json::array();
    for (const auto& send : track.sends) {
      Json target_insert = nullptr;
      if (send.target_insert)
        target_insert = send.target_insert->to_string();
      sends.push_back(
          {{"id", send.id.to_string()},
           {"destination", send.destination.to_string()},
           {"gain", send.gain},
           {"purpose", send.purpose == project::SendPurpose::sidechain ? "sidechain" : "audible"},
           {"target_insert", std::move(target_insert)}});
    }
    Json main_output = nullptr;
    const auto output = track.main_output;
    if (output.has_value()) {
      main_output = output.value().to_string();
    }
    tracks.push_back({{"id", track.id.to_string()},
                      {"clips", std::move(clips)},
                      {"kind", track_kind_name(track.kind)},
                      {"instrument_source", encode_instrument_source(track.instrument_source)},
                      {"midi_clips", std::move(midi_clips)},
                      {"effects", std::move(effects)},
                      {"main_output", std::move(main_output)},
                      {"sends", std::move(sends)},
                      {"automation", encode_automation(track.automation)},
                      {"muted", track.muted},
                      {"soloed", track.soloed},
                      {"level", track.level},
                      {"pan", track.pan},
                      {"name", track.name},
                      {"color", encode_color(track.color)},
                      {"note_effects", encode_note_effects(track.note_effects)},
                      {"macros", encode_macros(track.macros)},
                      {"frozen", encode_frozen(track.frozen)}});
  }
  Json sources = Json::array();
  for (const auto& source : snapshot.sources) {
    sources.push_back(
        {{"id", source.id.to_string()},
         {"encoding", source_encoding_name(source.encoding)},
         {"sha256", source.sha256},
         {"byte_length", source.byte_length},
         {"sample_rate", source.sample_rate},
         {"channels", source.channels},
         {"frames", source.frames},
         {"location", source.external_link
                          ? Json{{"kind", "external"},
                                 {"last_known_path", source.external_link->last_known_path}}
                          : Json{{"kind", "managed"}}}});
  }
  Json source_tempo_reviews = Json::array();
  for (const auto& review : snapshot.source_tempo_reviews) {
    source_tempo_reviews.push_back(
        {{"source", review.source.to_string()},
         {"detected_bpm", review.detected_bpm},
         {"confidence", review.confidence},
         {"corrected_bpm", review.corrected_bpm ? Json(*review.corrected_bpm) : Json(nullptr)},
         {"generated_beat_frames", review.generated_beat_frames},
         {"user_beat_frames", review.user_beat_frames},
         {"suggestions_truncated", review.suggestions_truncated},
         {"generated_onset_frames", review.generated_onset_frames},
         {"dismissed_onset_frames", review.dismissed_onset_frames},
         {"onsets_truncated", review.onsets_truncated},
         {"downbeat_frame", review.downbeat_frame ? Json(*review.downbeat_frame) : Json(nullptr)}});
  }
  Json source_key_reviews = Json::array();
  for (const auto& review : snapshot.source_key_reviews) {
    Json compatible = Json::array();
    for (const auto& suggestion : review.compatible) {
      compatible.push_back({{"key", encode_key(suggestion.key)},
                            {"relation", key_relation_name(suggestion.relation)}});
    }
    Json detected = nullptr;
    if (review.detected.has_value()) {
      detected = encode_key(review.detected.value_or(project::MusicalKey{}));
    }
    Json correction = nullptr;
    if (review.correction.has_value()) {
      correction = encode_key(review.correction.value_or(project::MusicalKey{}));
    }
    source_key_reviews.push_back({{"source", review.source.to_string()},
                                  {"detected", std::move(detected)},
                                  {"confidence", review.confidence},
                                  {"compatible", std::move(compatible)},
                                  {"correction", std::move(correction)}});
  }
  Json master_effects = Json::array();
  for (const auto& slot : snapshot.master_effects) {
    master_effects.push_back(encode_effect(slot));
  }
  Json markers = Json::array();
  for (const auto& marker : snapshot.markers) {
    markers.push_back({{"id", marker.id.to_string()},
                       {"position", marker.position.value},
                       {"name", marker.name}});
  }
  Json manifest{
      {"schema", {{"major", schema_major}, {"minor", schema_minor}}},
      {"generation", generation},
      {"parent", parent ? Json(*parent) : Json(nullptr)},
      {"kind", kind == SaveKind::explicit_save ? "explicit" : "autosave"},
      {"project",
       {{"id", snapshot.id.to_string()},
        {"revision", snapshot.revision},
        {"sample_rate", snapshot.sample_rate},
        {"timing_version", timeline::timing_map_version},
        {"tempo", {{"revision", snapshot.tempo.revision()}, {"events", std::move(tempo_events)}}},
        {"meter", {{"revision", snapshot.meter.revision()}, {"events", std::move(meter_events)}}},
        {"tracks", std::move(tracks)},
        {"sources", std::move(sources)},
        {"source_tempo_reviews", std::move(source_tempo_reviews)},
        {"source_key_reviews", std::move(source_key_reviews)},
        {"master_effects", std::move(master_effects)},
        {"master_level", snapshot.master_level},
        {"next_insert_slot_serial", snapshot.next_insert_slot_serial},
        {"markers", std::move(markers)},
        {"key", snapshot.key ? encode_key(*snapshot.key) : Json(nullptr)},
        {"lead_bars", snapshot.lead_bars}}}};
  return manifest.dump() + '\n';
}

Result<LoadedGeneration> decode_manifest(std::string_view text) {
  if (text.size() > max_manifest_bytes) {
    return Result<LoadedGeneration>::failure({ErrorCode::resource_exhausted, "manifest too large"});
  }
  return contain_parse_errors<LoadedGeneration>([&]() -> foundation::Result<LoadedGeneration> {
    const auto manifest = parse_strict(text);
    fields(manifest, {"schema", "generation", "parent", "kind", "project"});
    const auto& schema = field(manifest, "schema");
    fields(schema, {"major", "minor"});
    const auto major = unsigned_integer<std::uint32_t>(field(schema, "major"));
    const auto minor = unsigned_integer<std::uint32_t>(field(schema, "minor"));
    // Greenfield: only the current schema exists.
    if (major != schema_major || minor != schema_minor) {
      return Result<LoadedGeneration>::failure(
          {ErrorCode::unsupported, "unsupported manifest schema"});
    }
    auto generation = string(field(manifest, "generation"));
    if (!hex_name(generation, 32)) {
      throw ParseError("invalid generation name");
    }
    std::optional<std::string> parent;
    const auto& parent_value = field(manifest, "parent");
    if (!parent_value.is_null()) {
      parent = string(parent_value);
      if (!hex_name(*parent, 32)) {
        throw ParseError("invalid parent name");
      }
    }
    const auto kind_name = string(field(manifest, "kind"));
    SaveKind kind;
    if (kind_name == "explicit")
      kind = SaveKind::explicit_save;
    else if (kind_name == "autosave")
      kind = SaveKind::autosave;
    else
      throw ParseError("invalid save kind");

    const auto& project_value = field(manifest, "project");
    fields(project_value,
           {"id", "revision", "sample_rate", "timing_version", "tempo", "meter", "tracks",
            "sources", "source_tempo_reviews", "source_key_reviews", "master_effects",
            "master_level", "next_insert_slot_serial", "markers", "key", "lead_bars"});
    const auto timing_version =
        unsigned_integer<std::uint32_t>(field(project_value, "timing_version"));
    if (timing_version != timeline::timing_map_version) {
      return Result<LoadedGeneration>::failure(
          {ErrorCode::unsupported, "unsupported timing format"});
    }

    const auto& tempo_value = field(project_value, "tempo");
    fields(tempo_value, {"revision", "events"});
    std::vector<timeline::TempoEvent> tempo_events;
    const auto& tempo_values = field(tempo_value, "events");
    check_array(tempo_values, max_events);
    for (const auto& event : tempo_values) {
      fields(event, {"tick", "bpm", "shape"});
      tempo_events.push_back({{signed_integer(field(event, "tick"))},
                              real(field(event, "bpm")),
                              parse_shape(string(field(event, "shape")))});
    }
    auto tempo = timeline::TempoMap::create(
        std::move(tempo_events), unsigned_integer<std::uint64_t>(field(tempo_value, "revision")));
    if (!tempo) {
      throw ParseError("invalid tempo map");
    }

    const auto& meter_value = field(project_value, "meter");
    fields(meter_value, {"revision", "events"});
    std::vector<timeline::MeterEvent> meter_events;
    const auto& meter_values = field(meter_value, "events");
    check_array(meter_values, max_events);
    for (const auto& event : meter_values) {
      fields(event, {"tick", "numerator", "denominator"});
      meter_events.push_back({{signed_integer(field(event, "tick"))},
                              unsigned_integer<std::uint8_t>(field(event, "numerator")),
                              unsigned_integer<std::uint8_t>(field(event, "denominator"))});
    }
    auto meter = timeline::MeterMap::create(
        std::move(meter_events), unsigned_integer<std::uint64_t>(field(meter_value, "revision")));
    if (!meter) {
      throw ParseError("invalid meter map");
    }

    std::vector<project::Track> tracks;
    std::size_t clip_count = 0;
    const auto& track_values = field(project_value, "tracks");
    check_array(track_values, max_tracks);
    for (const auto& track : track_values) {
      fields(track, {"id", "clips", "kind", "instrument_source", "midi_clips", "effects",
                     "main_output", "sends", "automation", "muted", "soloed", "level", "pan",
                     "name", "color", "note_effects", "macros", "frozen"});
      std::vector<project::Clip> clips;
      const auto& clip_values = field(track, "clips");
      check_array(clip_values, max_clips);
      if (clip_values.size() > max_clips - clip_count) {
        throw ParseError("too many clips");
      }
      clip_count += clip_values.size();
      for (const auto& clip : clip_values) {
        clips.push_back(decode_clip(clip));
      }
      const auto kind = decode_track_kind(field(track, "kind"));
      auto instrument_source = decode_instrument_source(field(track, "instrument_source"));
      std::vector<project::MidiClip> midi_clips;
      const auto& midi_values = field(track, "midi_clips");
      check_array(midi_values, max_clips);
      if (midi_values.size() > max_clips - clip_count) {
        throw ParseError("too many clips");
      }
      clip_count += midi_values.size();
      for (const auto& midi_clip : midi_values) {
        midi_clips.push_back(decode_midi_clip(midi_clip));
      }
      std::vector<project::EffectInsert> effects;
      const auto& slots = field(track, "effects");
      check_array(slots, project::max_effect_slots_per_path);
      for (const auto& slot : slots)
        effects.push_back(decode_effect(slot));
      std::optional<project::TrackId> main_output;
      if (const auto& output = field(track, "main_output"); !output.is_null()) {
        main_output = parse_id<project::TrackId>(output);
      }
      std::vector<project::SendRoute> sends;
      const auto& routes = field(track, "sends");
      check_array(routes, project::max_sends_per_track);
      for (const auto& route : routes) {
        fields(route, {"id", "destination", "gain", "purpose", "target_insert"});
        auto purpose = project::SendPurpose::audible;
        const auto purpose_name = string(field(route, "purpose"));
        if (purpose_name == "sidechain")
          purpose = project::SendPurpose::sidechain;
        else if (purpose_name != "audible")
          throw ParseError("invalid send purpose");
        std::optional<project::InsertSlotId> target_insert;
        if (const auto& target = field(route, "target_insert"); !target.is_null())
          target_insert = parse_id<project::InsertSlotId>(target);
        sends.push_back({parse_id<project::SendId>(field(route, "id")),
                         parse_id<project::TrackId>(field(route, "destination")),
                         real(field(route, "gain")), purpose, target_insert});
      }
      auto automation =
          decode_automation(field(track, "automation"), project::max_automation_lanes_per_track);
      tracks.emplace_back(parse_id<project::TrackId>(field(track, "id")), std::move(clips), kind,
                          std::move(instrument_source), std::move(midi_clips), std::move(effects),
                          main_output, std::move(sends), std::move(automation));
      auto& decoded = tracks.back();
      decoded.name = string(field(track, "name"), project::max_track_name_bytes);
      decoded.color = decode_color(field(track, "color"));
      decoded.note_effects = decode_note_effects(field(track, "note_effects"));
      decoded.macros = decode_macros(field(track, "macros"));
      decoded.frozen = decode_frozen(field(track, "frozen"));
      const auto& muted = field(track, "muted");
      const auto& soloed = field(track, "soloed");
      if (!muted.is_boolean() || !soloed.is_boolean()) {
        throw ParseError("track mute and solo must be booleans");
      }
      decoded.muted = muted.get<bool>();
      decoded.soloed = soloed.get<bool>();
      decoded.level = real(field(track, "level"));
      decoded.pan = real(field(track, "pan"));
      if (decoded.level < 0.0 || decoded.level > 4.0 || decoded.pan < -1.0 || decoded.pan > 1.0)
        throw ParseError("track level or pan is outside its range");
    }
    std::vector<project::SourceRecord> sources;
    const auto& source_values = field(project_value, "sources");
    check_array(source_values, max_sources);
    for (const auto& source : source_values) {
      sources.push_back(decode_source(source));
    }
    std::vector<project::SourceTempoReview> source_tempo_reviews;
    {
      const auto& reviews = field(project_value, "source_tempo_reviews");
      check_array(reviews, max_sources);
      std::size_t marker_count = 0;
      for (const auto& value : reviews) {
        fields(value,
               {"source", "detected_bpm", "confidence", "corrected_bpm", "generated_beat_frames",
                "user_beat_frames", "suggestions_truncated", "generated_onset_frames",
                "dismissed_onset_frames", "onsets_truncated", "downbeat_frame"});
        auto markers = [&](std::string_view name) {
          const auto& entries = field(value, name);
          check_array(entries, project::max_tempo_markers_per_source);
          if (entries.size() > project::max_tempo_markers_per_project - marker_count) {
            throw ParseError("source tempo marker budget exceeded");
          }
          marker_count += entries.size();
          std::vector<std::uint64_t> frames;
          frames.reserve(entries.size());
          for (const auto& entry : entries) {
            frames.push_back(unsigned_integer<std::uint64_t>(entry));
          }
          return frames;
        };
        std::optional<double> corrected;
        if (!field(value, "corrected_bpm").is_null()) {
          corrected = real(field(value, "corrected_bpm"));
        }
        const auto& truncated = field(value, "suggestions_truncated");
        if (!truncated.is_boolean()) {
          throw ParseError("invalid source tempo truncation flag");
        }
        project::SourceTempoReview review{parse_id<project::SourceId>(field(value, "source")),
                                          real(field(value, "detected_bpm")),
                                          real(field(value, "confidence")),
                                          corrected,
                                          markers("generated_beat_frames"),
                                          markers("user_beat_frames"),
                                          truncated.get<bool>()};
        {
          const auto& onsets_truncated = field(value, "onsets_truncated");
          if (!onsets_truncated.is_boolean())
            throw ParseError("invalid source onset truncation flag");
          review.generated_onset_frames = markers("generated_onset_frames");
          review.dismissed_onset_frames = markers("dismissed_onset_frames");
          review.onsets_truncated = onsets_truncated.get<bool>();
        }
        if (const auto& downbeat = field(value, "downbeat_frame"); !downbeat.is_null())
          review.downbeat_frame = unsigned_integer<std::uint64_t>(downbeat);
        source_tempo_reviews.push_back(std::move(review));
      }
    }
    std::vector<project::SourceKeyReview> source_key_reviews;
    {
      const auto& reviews = field(project_value, "source_key_reviews");
      check_array(reviews, max_sources);
      for (const auto& value : reviews) {
        fields(value, {"source", "detected", "confidence", "compatible", "correction"});
        std::optional<project::MusicalKey> detected;
        if (!field(value, "detected").is_null()) {
          detected = decode_key(field(value, "detected"));
        }
        std::optional<project::MusicalKey> correction;
        if (!field(value, "correction").is_null()) {
          correction = decode_key(field(value, "correction"));
        }
        const auto& suggestions = field(value, "compatible");
        check_array(suggestions, project::max_compatible_keys_per_source);
        std::vector<project::CompatibleKey> compatible;
        for (const auto& suggestion : suggestions) {
          fields(suggestion, {"key", "relation"});
          compatible.push_back({decode_key(field(suggestion, "key")),
                                decode_key_relation(field(suggestion, "relation"))});
        }
        source_key_reviews.push_back({parse_id<project::SourceId>(field(value, "source")), detected,
                                      real(field(value, "confidence")), std::move(compatible),
                                      correction});
      }
    }
    std::vector<project::EffectInsert> master_effects;
    const auto& master_slots = field(project_value, "master_effects");
    check_array(master_slots, project::max_effect_slots_per_path);
    for (const auto& slot : master_slots)
      master_effects.push_back(decode_effect(slot));
    project::Snapshot snapshot{
        parse_id<project::ProjectId>(field(project_value, "id")),
        unsigned_integer<std::uint64_t>(field(project_value, "revision")),
        unsigned_integer<std::uint32_t>(field(project_value, "sample_rate")),
        std::move(tempo).value(),
        std::move(meter).value(),
        std::move(tracks),
        std::move(sources),
        std::move(master_effects),
        std::move(source_tempo_reviews),
        unsigned_integer<std::uint64_t>(field(project_value, "next_insert_slot_serial")),
        std::move(source_key_reviews),
        real(field(project_value, "master_level"))};
    {
      const auto& marker_values = field(project_value, "markers");
      check_array(marker_values, project::max_markers_per_project);
      for (const auto& marker : marker_values) {
        fields(marker, {"id", "position", "name"});
        snapshot.markers.push_back({parse_id<project::MarkerId>(field(marker, "id")),
                                    {signed_integer(field(marker, "position"))},
                                    string(field(marker, "name"), project::max_marker_name_bytes)});
      }
    }
    if (const auto& key = field(project_value, "key"); !key.is_null())
      snapshot.key = decode_key(key);
    snapshot.lead_bars = unsigned_integer<std::uint32_t>(field(project_value, "lead_bars"));
    if (snapshot.lead_bars > project::max_lead_bars)
      throw ParseError("too many bars before bar 1");
    ValidationGate gate;
    auto validated = project::ProjectModel::open(snapshot, gate);
    if (!validated) {
      throw ParseError("invalid project snapshot: " + validated.error().message);
    }
    return Result<LoadedGeneration>::success(
        {std::move(snapshot), std::move(generation), kind, std::move(parent), {}});
  });
}

std::string encode_pointer(std::string_view generation, std::string_view hash) {
  return Json{{"generation", generation}, {"sha256", hash}}.dump() + '\n';
}

Result<std::pair<std::string, std::string>> decode_pointer(std::string_view text) {
  if (text.size() > 256) {
    return Result<std::pair<std::string, std::string>>::failure(
        {ErrorCode::resource_exhausted, "pointer too large"});
  }
  return contain_parse_errors<std::pair<std::string, std::string>>(
      [&]() -> foundation::Result<std::pair<std::string, std::string>> {
        const auto pointer = parse_strict(text);
        fields(pointer, {"generation", "sha256"});
        auto generation = string(field(pointer, "generation"));
        auto hash = string(field(pointer, "sha256"));
        if (!hex_name(generation, 32) || !hex_name(hash, 64)) {
          throw ParseError("invalid pointer");
        }
        return Result<std::pair<std::string, std::string>>::success(
            {std::move(generation), std::move(hash)});
      });
}

bool valid_revision_name(std::string_view name) {
  if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ' ||
      name == "." || name == "..")
    return false;
  return std::ranges::all_of(name, [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == ' ' || ch == '_' || ch == '-' || ch == '.';
  });
}

std::string encode_named_pointer(const NamedPointer& pointer) {
  return Json{{"name", pointer.name}, {"generation", pointer.generation}, {"sha256", pointer.hash}}
             .dump() +
         '\n';
}

Result<NamedPointer> decode_named_pointer(std::string_view text) {
  if (text.size() > 384)
    return Result<NamedPointer>::failure(
        {ErrorCode::resource_exhausted, "revision pointer too large"});
  return contain_parse_errors<NamedPointer>([&]() -> foundation::Result<NamedPointer> {
    const auto pointer = parse_strict(text);
    fields(pointer, {"name", "generation", "sha256"});
    NamedPointer decoded{string(field(pointer, "name"), 64),
                         string(field(pointer, "generation"), 32),
                         string(field(pointer, "sha256"), 64)};
    if (!valid_revision_name(decoded.name) || !hex_name(decoded.generation, 32) ||
        !hex_name(decoded.hash, 64))
      throw ParseError("invalid named revision pointer");
    return Result<NamedPointer>::success(std::move(decoded));
  });
}

} // namespace oscillate::persistence::detail

namespace oscillate::persistence {

namespace {
constexpr std::string_view effect_preset_kind = "oscillate.native-effect-preset";
constexpr std::uint32_t effect_preset_version = 1;
} // namespace

std::string encode_native_effect_preset(const project::NativeEffectPatch& patch) {
  using detail::Json;
  Json parameters = Json::array();
  for (const auto& parameter : patch.parameters)
    parameters.push_back({{"id", parameter.id}, {"value", parameter.value}});
  return Json{{"kind", std::string(effect_preset_kind)},
              {"version", effect_preset_version},
              {"engine", patch.engine},
              {"engine_version", patch.engine_version},
              {"parameters", std::move(parameters)},
              {"document", detail::encode_native_document(patch.document)}}
             .dump(2) +
         '\n';
}

foundation::Result<project::NativeEffectPatch> decode_native_effect_preset(std::string_view text) {
  using R = foundation::Result<project::NativeEffectPatch>;
  using namespace detail;
  if (text.size() > max_effect_preset_bytes)
    return R::failure({ErrorCode::resource_exhausted, "effect preset too large"});
  return contain_parse_errors<project::NativeEffectPatch>(
      [&]() -> foundation::Result<project::NativeEffectPatch> {
        const auto preset = parse_strict(text);
        fields(preset, {"kind", "version", "engine", "engine_version", "parameters", "document"});
        if (string(field(preset, "kind")) != effect_preset_kind ||
            unsigned_integer<std::uint32_t>(field(preset, "version")) != effect_preset_version)
          return R::failure({ErrorCode::unsupported, "not a built-in effect preset"});
        project::NativeEffectPatch patch;
        patch.engine = string(field(preset, "engine"), project::max_native_identifier_length);
        patch.engine_version = unsigned_integer<std::uint32_t>(field(preset, "engine_version"));
        const auto& entries = field(preset, "parameters");
        check_array(entries, project::max_native_instrument_parameters);
        for (const auto& entry : entries) {
          fields(entry, {"id", "value"});
          patch.parameters.push_back(
              {string(field(entry, "id"), project::max_native_identifier_length),
               real(field(entry, "value"))});
        }
        patch.document = decode_native_document(field(preset, "document"));
        return R::success(std::move(patch));
      });
}

namespace {
using foundation::ErrorCode;
constexpr std::string_view rack_kind = "oscillate.rack";
constexpr std::uint32_t rack_version = 1;
// "rack" in ASCII: the high word of the rack's own track and slot ids.
constexpr std::uint64_t rack_id_namespace = 0x7261636bULL;
} // namespace

project::TrackId rack_track_id() {
  return project::TrackId::from_bits({rack_id_namespace, 1}).value();
}

project::InsertSlotId rack_slot_id(std::size_t ordinal) {
  // The high word is never zero, so the id is never the reserved nil one.
  return project::InsertSlotId::from_bits({rack_id_namespace, static_cast<std::uint64_t>(ordinal)})
      .value();
}

foundation::Result<void> validate_rack(const TrackRack& rack) {
  using R = foundation::Result<void>;
  const auto invalid = [](std::string message) {
    return R::failure({ErrorCode::invalid_argument, std::move(message)});
  };
  if (rack.note_effects.size() > project::max_note_effects_per_track)
    return invalid("a rack holds at most eight note effects");
  if (rack.effects.size() > project::max_effect_slots_per_path)
    return invalid("a rack holds too many effects");
  if (rack.macros.size() > project::max_macros_per_track)
    return invalid("a rack holds at most eight macros");
  std::vector<project::InsertSlotId> slots;
  for (const auto& effect : rack.note_effects)
    slots.push_back(effect.id);
  for (const auto& effect : rack.effects) {
    if (!std::holds_alternative<project::NativeEffectPatch>(effect.processor))
      return invalid("a rack holds only built-in effects");
    slots.push_back(effect.id);
  }
  for (std::size_t index = 0; index < slots.size(); ++index) {
    if (slots[index].bits().high != rack_id_namespace || slots[index].bits().low == 0 ||
        std::ranges::count(slots, slots[index]) != 1)
      return invalid("a rack's slots need their own distinct ids");
  }
  for (const auto& macro : rack.macros) {
    if (macro.name.size() > project::max_macro_name_bytes || !std::isfinite(macro.value) ||
        macro.value < 0.0 || macro.value > 1.0 ||
        macro.mappings.size() > project::max_macro_mappings)
      return invalid("a rack macro is out of range");
    for (const auto& mapping : macro.mappings) {
      if (!std::isfinite(mapping.minimum) || !std::isfinite(mapping.maximum))
        return invalid("a rack macro's range is not finite");
      if (const auto* native = std::get_if<project::NativeParameterTarget>(&mapping.destination)) {
        if (native->track != rack_track_id() || !rack.instrument)
          return invalid("a rack macro maps an instrument the rack does not hold");
      } else {
        const auto& target = std::get<project::NativeEffectParameterTarget>(mapping.destination);
        if (std::ranges::find(slots, target.slot) == slots.end())
          return invalid("a rack macro maps a slot the rack does not hold");
      }
    }
  }
  return R::success();
}

std::string encode_rack(const TrackRack& rack) {
  using detail::Json;
  Json effects = Json::array();
  for (const auto& effect : rack.effects)
    effects.push_back(detail::encode_effect(effect));
  return Json{{"kind", std::string(rack_kind)},
              {"version", rack_version},
              {"instrument", rack.instrument ? detail::encode_instrument_source(*rack.instrument)
                                             : Json(nullptr)},
              {"note_effects", detail::encode_note_effects(rack.note_effects)},
              {"effects", std::move(effects)},
              {"macros", detail::encode_macros(rack.macros)}}
             .dump(2) +
         '\n';
}

foundation::Result<TrackRack> decode_rack(std::string_view text) {
  using R = foundation::Result<TrackRack>;
  using namespace detail;
  if (text.size() > max_rack_bytes)
    return R::failure({ErrorCode::resource_exhausted, "rack too large"});
  return contain_parse_errors<TrackRack>([&]() -> foundation::Result<TrackRack> {
    const auto file = parse_strict(text);
    fields(file, {"kind", "version", "instrument", "note_effects", "effects", "macros"});
    if (string(field(file, "kind")) != rack_kind ||
        unsigned_integer<std::uint32_t>(field(file, "version")) != rack_version)
      return R::failure({ErrorCode::unsupported, "not a rack"});
    TrackRack rack;
    if (const auto& instrument = field(file, "instrument"); !instrument.is_null()) {
      auto source = decode_instrument_source(instrument);
      auto* patch = std::get_if<project::NativeInstrumentPatch>(&source);
      if (patch == nullptr)
        throw ParseError("a rack's instrument is a built-in synth");
      rack.instrument = std::move(*patch);
    }
    rack.note_effects = decode_note_effects(field(file, "note_effects"));
    const auto& effects = field(file, "effects");
    check_array(effects, project::max_effect_slots_per_path);
    for (const auto& effect : effects) {
      auto decoded = decode_effect(effect);
      if (auto* patch = std::get_if<project::NativeEffectPatch>(&decoded.processor)) {
        patch->link.clear();
        patch->generation = 1;
      }
      rack.effects.push_back(std::move(decoded));
    }
    rack.macros = decode_macros(field(file, "macros"));
    if (auto valid = validate_rack(rack); !valid)
      return R::failure({ErrorCode::corrupt_data, valid.error().message});
    return R::success(std::move(rack));
  });
}

} // namespace oscillate::persistence
