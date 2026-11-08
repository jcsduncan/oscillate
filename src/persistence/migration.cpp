#include "migration.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace oscillate::persistence::detail {
namespace {

using Json = nlohmann::json;
using foundation::ErrorCode;
using Result = foundation::Result<Json>;

// One entry per schema change after the first released schema (ADR-0095).
constexpr std::array<MigrationStep, 0> built_in_steps{};

// The chain a test installed, or null for the built-in one.
std::atomic<const ScopedMigrationChain::Chain*> installed_chain{nullptr};

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
