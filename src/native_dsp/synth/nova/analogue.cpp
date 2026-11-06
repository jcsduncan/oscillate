#include "analogue_state.hpp"

#include "modulation.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::native_dsp::synth::nova {
namespace {

using foundation::ErrorCode;
using foundation::Result;

// Mixed into every seeded draw, so Analogue's values share nothing with the
// other seeded parts of a note (random phase, the Random source).
constexpr std::uint64_t analogue_salt = 0xA7A1'06E5'0B5E'ED00ULL;
// Re-roll and flutter draw from their own streams, sharing nothing with the
// fixed offsets.
constexpr std::uint64_t reroll_salt = 0x2E20'11ED'C0DE'5EEDULL;
constexpr std::uint64_t flutter_salt = 0xF1A7'7E20'0BAD'CAFEULL;
// Spreads a note number or project frame over all 64 bits before mixing.
constexpr std::uint64_t golden_ratio_64 = 0x9E37'79B9'7F4A'7C15ULL;
constexpr std::array<std::string_view, analogue_oscillators> osc_ids{"a", "b", "c"};

Result<void> invalid(std::string message) {
  return Result<void>::failure({ErrorCode::invalid_argument, "Nova: " + std::move(message)});
}

struct LaneId {
  AnalogueKind kind = AnalogueKind::notes;
  std::size_t osc = 0;
  AnalogueLane lane = AnalogueLane::pitch;
};

// "notes.<lane>" or "unison.<o>.<lane>", for a lane that reaches its kind.
[[nodiscard]] std::optional<LaneId> parse_lane_id(std::string_view id) noexcept {
  const auto dot = id.find('.');
  if (dot == std::string_view::npos)
    return std::nullopt;
  const auto kind = analogue_kind_from_id(id.substr(0, dot));
  if (!kind)
    return std::nullopt;
  auto rest = id.substr(dot + 1);
  LaneId parsed{*kind, 0, AnalogueLane::pitch};
  if (*kind == AnalogueKind::unison) {
    const auto osc_dot = rest.find('.');
    if (osc_dot == std::string_view::npos)
      return std::nullopt;
    const auto found = std::ranges::find(osc_ids, rest.substr(0, osc_dot));
    if (found == osc_ids.end())
      return std::nullopt;
    parsed.osc = static_cast<std::size_t>(found - osc_ids.begin());
    rest = rest.substr(osc_dot + 1);
  }
  const auto lane = analogue_lane_from_id(rest);
  if (!lane || !analogue_lane_applies(*kind, *lane))
    return std::nullopt;
  parsed.lane = *lane;
  return parsed;
}

[[nodiscard]] double& edit_at(AnalogueEdits& edits, const LaneId& id, std::size_t voice) noexcept {
  const auto lane = static_cast<std::size_t>(id.lane);
  return id.kind == AnalogueKind::notes ? edits.notes[lane][voice]
                                        : edits.unison[id.osc][lane][voice];
}

} // namespace

std::optional<AnalogueKind> analogue_kind_from_id(std::string_view id) noexcept {
  for (std::size_t k = 0; k < analogue_kind_ids.size(); ++k)
    if (analogue_kind_ids[k] == id)
      return static_cast<AnalogueKind>(k);
  return std::nullopt;
}

std::optional<AnalogueLane> analogue_lane_from_id(std::string_view id) noexcept {
  for (std::size_t l = 0; l < analogue_lane_ids.size(); ++l)
    if (analogue_lane_ids[l] == id)
      return static_cast<AnalogueLane>(l);
  return std::nullopt;
}

bool analogue_lane_applies(AnalogueKind kind, AnalogueLane lane) noexcept {
  if (kind == AnalogueKind::notes)
    return true;
  return lane == AnalogueLane::pitch || lane == AnalogueLane::level || lane == AnalogueLane::pan ||
         lane == AnalogueLane::phase;
}

std::size_t analogue_voice_limit(AnalogueKind kind) noexcept {
  return kind == AnalogueKind::notes ? analogue_note_voices : analogue_unison_voices;
}

std::string analogue_lane_document_id(AnalogueKind kind, std::size_t osc, AnalogueLane lane) {
  std::string id(analogue_kind_ids[static_cast<std::size_t>(kind)]);
  if (kind == AnalogueKind::unison)
    id += "." + std::string(osc_ids[std::min(osc, osc_ids.size() - 1)]);
  return id + "." + std::string(analogue_lane_ids[static_cast<std::size_t>(lane)]);
}

double analogue_seeded(std::uint32_t seed, AnalogueKind kind, std::size_t osc, AnaloguePart part,
                       std::size_t voice) noexcept {
  const std::uint64_t key =
      (static_cast<std::uint64_t>(seed) << 40U) ^ (static_cast<std::uint64_t>(kind) << 36U) ^
      (static_cast<std::uint64_t>(osc) << 32U) ^ (static_cast<std::uint64_t>(part) << 24U) ^
      static_cast<std::uint64_t>(voice);
  return 2.0 * hash_unit(key ^ analogue_salt) - 1.0;
}

double analogue_rerolled(std::uint32_t seed, AnalogueKind kind, std::size_t osc, AnaloguePart part,
                         std::size_t voice, std::uint64_t note) noexcept {
  const std::uint64_t key =
      (static_cast<std::uint64_t>(seed) << 40U) ^ (static_cast<std::uint64_t>(kind) << 36U) ^
      (static_cast<std::uint64_t>(osc) << 32U) ^ (static_cast<std::uint64_t>(part) << 24U) ^
      static_cast<std::uint64_t>(voice);
  return 2.0 * hash_unit(key ^ reroll_salt ^ (note * golden_ratio_64)) - 1.0;
}

std::uint64_t analogue_flutter_seed(std::uint32_t seed, AnalogueKind kind, std::size_t osc,
                                    std::size_t voice, std::uint64_t project_frame) noexcept {
  const std::uint64_t key =
      (static_cast<std::uint64_t>(seed) << 40U) ^ (static_cast<std::uint64_t>(kind) << 36U) ^
      (static_cast<std::uint64_t>(osc) << 32U) ^ static_cast<std::uint64_t>(voice);
  // The LCG's draws pass through hash_unit, which mixes every bit.
  return key ^ flutter_salt ^ (project_frame * golden_ratio_64);
}

double analogue_with_reroll(const AnalogueSettings& settings, AnaloguePart part, double value,
                            double unit) noexcept {
  // Mod A and Mod B are sources with Analogue off too; re-roll is Analogue's
  // and stays out of them then.
  if (!settings.on || settings.reroll <= 0.0)
    return value;
  if (part == AnaloguePart::mod_a || part == AnaloguePart::mod_b)
    return std::clamp(value + settings.reroll * unit, -1.0, 1.0);
  const double tolerance = settings.tolerance[static_cast<std::size_t>(part)];
  return std::clamp(
      value + settings.reroll * unit * tolerance * settings.age * analogue_age_headroom, -1.0, 1.0);
}

double analogue_scaled(const AnalogueSettings& settings, AnaloguePart part, double unit) noexcept {
  if (part == AnaloguePart::mod_a || part == AnaloguePart::mod_b)
    return unit;
  const double tolerance = settings.tolerance[static_cast<std::size_t>(part)];
  return std::clamp(unit * tolerance * settings.age * analogue_age_headroom, -1.0, 1.0);
}

AnalogueEdits::AnalogueEdits() noexcept {
  constexpr double seeded = std::numeric_limits<double>::quiet_NaN();
  for (auto& lane : notes)
    lane.fill(seeded);
  for (auto& osc : unison)
    for (auto& lane : osc)
      lane.fill(seeded);
}

Result<void> validate_analogue_lanes(std::span<const project::NativeVoiceLane> lanes) {
  for (const auto& lane : lanes) {
    const auto id = parse_lane_id(lane.id);
    if (!id)
      return invalid("unknown voice lane '" + lane.id + "'");
    for (const auto& value : lane.values) {
      if (value.voice >= analogue_voice_limit(id->kind))
        return invalid("voice " + std::to_string(value.voice) + " is out of range in '" + lane.id +
                       "'");
      if (!std::isfinite(value.value) || value.value < -1.0 || value.value > 1.0)
        return invalid("a value in '" + lane.id + "' is outside -1..1");
    }
  }
  return Result<void>::success();
}

AnalogueEdits compile_analogue_edits(std::span<const project::NativeVoiceLane> lanes) {
  AnalogueEdits edits;
  for (const auto& lane : lanes) {
    const auto id = parse_lane_id(lane.id);
    if (!id)
      continue;
    for (const auto& value : lane.values) {
      if (value.voice < analogue_voice_limit(id->kind))
        edit_at(edits, *id, value.voice) = value.value;
    }
  }
  return edits;
}

std::vector<AnalogueLaneValue> analogue_lane_values(std::span<const double> values,
                                                    const project::NativeSynthDocument& document,
                                                    AnalogueKind kind, std::size_t osc,
                                                    AnalogueLane lane, std::size_t voice_count) {
  std::vector<AnalogueLaneValue> result;
  if (values.size() != descriptor().parameters.size() || osc >= analogue_oscillators ||
      !analogue_lane_applies(kind, lane))
    return result;
  const auto settings = analogue_settings([&](std::size_t i) { return values[i]; });
  const LaneId id{kind, kind == AnalogueKind::unison ? osc : 0, lane};
  const auto document_id = analogue_lane_document_id(kind, osc, lane);
  const auto found =
      std::ranges::find(document.voice_lanes, document_id, &project::NativeVoiceLane::id);
  const auto count = std::min(voice_count, analogue_voice_limit(kind));
  const auto part = static_cast<AnaloguePart>(lane);
  result.reserve(count);
  for (std::size_t v = 0; v < count; ++v) {
    AnalogueLaneValue entry;
    if (found != document.voice_lanes.end()) {
      const auto painted = std::ranges::find(found->values, static_cast<std::uint32_t>(v),
                                             &project::NativeVoiceValue::voice);
      if (painted != found->values.end()) {
        entry = {std::clamp(painted->value, -1.0, 1.0), true};
        result.push_back(entry);
        continue;
      }
    }
    entry.value =
        analogue_scaled(settings, part, analogue_seeded(settings.seed, kind, id.osc, part, v));
    result.push_back(entry);
  }
  return result;
}

bool set_analogue_lane_value(project::NativeSynthDocument& document, AnalogueKind kind,
                             std::size_t osc, AnalogueLane lane, std::size_t voice,
                             std::optional<double> value) {
  if (osc >= analogue_oscillators || !analogue_lane_applies(kind, lane) ||
      voice >= analogue_voice_limit(kind) || (value && !std::isfinite(*value)))
    return false;
  const auto id = analogue_lane_document_id(kind, osc, lane);
  auto found = std::ranges::find(document.voice_lanes, id, &project::NativeVoiceLane::id);
  if (!value) {
    if (found == document.voice_lanes.end())
      return true;
    std::erase_if(found->values,
                  [&](const project::NativeVoiceValue& entry) { return entry.voice == voice; });
    if (found->values.empty())
      document.voice_lanes.erase(found);
    return true;
  }
  if (found == document.voice_lanes.end()) {
    if (document.voice_lanes.size() >= project::max_native_voice_lanes)
      return false;
    document.voice_lanes.push_back({id, {}});
    found = document.voice_lanes.end() - 1;
  }
  const auto voice_id = static_cast<std::uint32_t>(voice);
  const project::NativeVoiceValue entry{voice_id, std::clamp(*value, -1.0, 1.0)};
  // Voices stay in increasing order (the model's rule).
  const auto at =
      std::ranges::lower_bound(found->values, voice_id, {}, &project::NativeVoiceValue::voice);
  if (at != found->values.end() && at->voice == voice_id) {
    *at = entry;
  } else {
    found->values.insert(at, entry);
  }
  return true;
}

bool clear_analogue_lane(project::NativeSynthDocument& document, AnalogueKind kind, std::size_t osc,
                         AnalogueLane lane) {
  const auto id = analogue_lane_document_id(kind, osc, lane);
  return std::erase_if(document.voice_lanes,
                       [&](const project::NativeVoiceLane& entry) { return entry.id == id; }) > 0;
}

} // namespace oscillate::native_dsp::synth::nova
