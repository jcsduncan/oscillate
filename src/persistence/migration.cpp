#include "migration.hpp"

#include <algorithm>
#include <utility>

namespace oscillate::persistence::detail {
namespace {

using Json = nlohmann::json;
using foundation::ErrorCode;
using Result = foundation::Result<Json>;

// One entry per schema change after the first released schema (ADR-0095).
constexpr std::array<MigrationStep, 0> built_in_steps{};

} // namespace

std::span<const MigrationStep> migration_steps() noexcept { return built_in_steps; }

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
        {ErrorCode::unsupported, "project saved by a newer Oscillate (schema " + schema_text(from) +
                                     "); this version opens schema " + schema_text(current) +
                                     " and older released schemas"});
  }
  if (released.empty() || from < released.front()) {
    return Result::failure({ErrorCode::unsupported, "project schema " + schema_text(from) +
                                                        " predates the first public release "
                                                        "and does not open"});
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
  return migrate_manifest(std::move(manifest), from, current_schema, released_schemas,
                          migration_steps());
}

} // namespace oscillate::persistence::detail
