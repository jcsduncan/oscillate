#include "migration.hpp"

#include "oscillate/project/snapshot.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace oscillate::persistence::detail {
namespace {

using Json = nlohmann::json;
using foundation::ErrorCode;
using Result = foundation::Result<Json>;

constexpr SchemaVersion schema_1_47{1, 47};
constexpr SchemaVersion schema_1_48{1, 48};
constexpr SchemaVersion schema_1_49{1, 49};

// One entry per schema change after the first released schema (ADR-0095).
constexpr std::array<MigrationStep, 2> built_in_steps{
    MigrationStep{schema_1_47, schema_1_48, migrate_1_47_to_1_48},
    MigrationStep{schema_1_48, schema_1_49, migrate_1_48_to_1_49}};

// 1.47's convolution insert and the Cast effect that replaces it in 1.48
// (ADR-0115). The ids are Cast's saved document format, so they are spelled
// here rather than taken from native_dsp: a step is frozen once released.
constexpr std::string_view convolution_kind = "native_convolution";
constexpr std::size_t convolution_fields = 3; // generation, wet, ir
constexpr std::string_view native_effect_kind = "native_effect";
constexpr std::string_view cast_engine = "cast";
constexpr std::uint32_t cast_engine_version = 1;
constexpr std::string_view cast_ir_slot = "ir";
// Unsigned, as the decoder requires: Cast's IR is not cut into frames.
constexpr std::uint32_t cast_ir_frame_length = 0;
constexpr std::string_view cast_mix = "cast.mix";
constexpr std::string_view cast_normalise = "cast.normalise";
constexpr double normalise_off = 0.0;

// A 1.47 `native_convolution` payload {generation, wet, ir} as a Cast
// `native_effect` payload: the IR (a managed mono or stereo file, which Cast
// plays at any rate and up to 10 s, beyond 1.47's 2 s limit) moves to Cast's
// `ir` media slot, wet becomes Cast's mix, and normalisation is off because
// the old core played the IR at its own level. Cast's other parameters keep
// their defaults (no pre-delay, filters, ducking or reshaping, unity wet
// gain). Cast's mix is equal-power where the old one was linear, so a partial
// mix is somewhat louder; 0 and 1 match. Every insert converts: nothing is
// dropped.
Json cast_from_convolution(const Json& payload) {
  Json media = Json::array();
  media.push_back({{"id", cast_ir_slot},
                   {"frame_length", cast_ir_frame_length},
                   {"builtin", nullptr},
                   {"managed", payload.at("ir")}});
  Json parameters = Json::array();
  parameters.push_back({{"id", cast_mix}, {"value", payload.at("wet")}});
  parameters.push_back({{"id", cast_normalise}, {"value", normalise_off}});
  return {{"generation", payload.at("generation")},
          {"engine", cast_engine},
          {"engine_version", cast_engine_version},
          {"parameters", std::move(parameters)},
          {"document",
           {{"routes", Json::array()},
            {"curves", Json::array()},
            {"chains", Json::array()},
            {"sequences", Json::array()},
            {"media", std::move(media)},
            {"labels", Json::array()}}},
          {"link", ""}};
}

void convert_convolutions(Json& inserts) {
  if (!inserts.is_array())
    return;
  for (auto& insert : inserts) {
    // A malformed insert stays as it is, and the decoder rejects it.
    if (!insert.is_object() || !insert.contains("kind") || insert["kind"] != convolution_kind ||
        !insert.contains("payload"))
      continue;
    // 1.47 read exactly these fields; anything else stays unconverted.
    const auto& payload = insert["payload"];
    if (!payload.is_object() || payload.size() != convolution_fields ||
        !payload.contains("generation") || !payload.contains("wet") || !payload.contains("ir"))
      continue;
    insert["payload"] = cast_from_convolution(insert["payload"]);
    insert["kind"] = native_effect_kind;
  }
}

// The chain a test installed, or null for the built-in one.
std::atomic<const ScopedMigrationChain::Chain*> installed_chain{nullptr};

} // namespace

Json migrate_1_48_to_1_49(Json manifest) {
  // The project's settings as a new project starts them, off.
  const project::MasterRebalance off;
  Json roles = Json::array();
  for (const auto& role : off.roles)
    roles.push_back({{"gain_db", role.gain_db},
                     {"low_db", role.low_db},
                     {"low_hz", role.low_hz},
                     {"mid_db", role.mid_db},
                     {"mid_hz", role.mid_hz},
                     {"mid_q", role.mid_q},
                     {"high_db", role.high_db},
                     {"high_hz", role.high_hz}});
  auto& saved = manifest["project"];
  saved["rebalance"] = {{"enabled", off.enabled}, {"roles", std::move(roles)}};
  // No track has a chosen or heard role: each is inferred.
  if (auto tracks = saved.find("tracks"); tracks != saved.end() && tracks->is_array()) {
    for (auto& track : *tracks) {
      if (!track.is_object())
        continue;
      track["mix_role"] = nullptr;
      track["heard_role"] = nullptr;
    }
  }
  return manifest;
}

std::span<const MigrationStep> migration_steps() noexcept { return built_in_steps; }

// 1.48 removed the `native_convolution` insert kind and let a built-in
// effect's document hold managed media; nothing else changed. Inserts live in
// each track's `effects` and in the project's `master_effects`.
Json migrate_1_47_to_1_48(Json manifest) {
  if (!manifest.contains("project") || !manifest["project"].is_object())
    return manifest;
  auto& project = manifest["project"];
  if (project.contains("master_effects"))
    convert_convolutions(project["master_effects"]);
  if (project.contains("tracks") && project["tracks"].is_array()) {
    for (auto& track : project["tracks"]) {
      if (track.is_object() && track.contains("effects"))
        convert_convolutions(track["effects"]);
    }
  }
  return manifest;
}

std::string schema_text(SchemaVersion version) {
  return std::to_string(version.major) + '.' + std::to_string(version.minor);
}

Result migrate_manifest(Json manifest, SchemaVersion from, SchemaVersion current,
                        std::span<const SchemaVersion> released,
                        std::span<const MigrationStep> steps) {
  if (from == current) {
    return Result::success(std::move(manifest));
  }
  if (from > current) {
    return Result::failure(
        {ErrorCode::unsupported, "saved by a newer Oscillate (schema " + schema_text(from) +
                                     "); this version opens up to schema " + schema_text(current)});
  }
  if (released.empty() || from < released.front()) {
    const auto oldest = released.empty() ? current : released.front();
    return Result::failure(
        {ErrorCode::unsupported, "project schema " + schema_text(from) +
                                     " predates the oldest supported release (schema " +
                                     schema_text(oldest) + ") and does not open"});
  }
  auto version = from;
  while (version != current) {
    const auto step = std::ranges::find(steps, version, &MigrationStep::from);
    if (step == steps.end() || step->to <= version || step->to > current) {
      return Result::failure(
          {ErrorCode::internal, "no migration from project schema " + schema_text(version)});
    }
    manifest = step->migrate(std::move(manifest));
    version = step->to;
    manifest["schema"] = {{"major", version.major}, {"minor", version.minor}};
  }
  return Result::success(std::move(manifest));
}

Result migrate_manifest(Json manifest, SchemaVersion from) {
  if (const auto* chain = installed_chain.load(std::memory_order_acquire)) {
    return migrate_manifest(std::move(manifest), from, current_schema, chain->released,
                            chain->steps);
  }
  return migrate_manifest(std::move(manifest), from, current_schema, released_schemas,
                          migration_steps());
}

ScopedMigrationChain::ScopedMigrationChain(std::span<const SchemaVersion> released,
                                           std::span<const MigrationStep> steps) noexcept
    : chain_{released, steps},
      previous_(installed_chain.exchange(&chain_, std::memory_order_acq_rel)) {}

ScopedMigrationChain::~ScopedMigrationChain() {
  installed_chain.store(previous_, std::memory_order_release);
}

} // namespace oscillate::persistence::detail
