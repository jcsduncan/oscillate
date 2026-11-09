#pragma once

#include "oscillate/foundation/error.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string>

// Forward migration of saved manifests (ADR-0095). Loading runs a manifest from
// an older released schema through one pure step per schema change until it is
// in the current schema, then decodes it as usual. Nothing on disk is rewritten.
namespace oscillate::persistence::detail {

struct SchemaVersion {
  std::uint32_t major;
  std::uint32_t minor;
  auto operator<=>(const SchemaVersion&) const = default;
};

// The one schema this build writes.
inline constexpr SchemaVersion current_schema{1, 48};

// Every schema a public release wrote, oldest first. Each has a golden manifest
// in tests/persistence/golden. Empty before the first public beta, so only the
// current schema opens.
inline constexpr std::array<SchemaVersion, 0> released_schemas{};

// One forward step: a pure function from a manifest in `from` to one in `to`.
// The chain rewrites the manifest's own "schema" field after each step.
struct MigrationStep {
  SchemaVersion from;
  SchemaVersion to;
  nlohmann::json (*migrate)(nlohmann::json manifest);
};

// The built-in chain, from the oldest released schema to the current one.
[[nodiscard]] std::span<const MigrationStep> migration_steps() noexcept;

[[nodiscard]] std::string schema_text(SchemaVersion version);

// Brings `manifest`, saved in `from`, to `current` through `steps`. The current
// schema passes through unchanged. A newer schema, or one older than the oldest
// released schema, fails as unsupported with a message saying which.
[[nodiscard]] foundation::Result<nlohmann::json>
migrate_manifest(nlohmann::json manifest, SchemaVersion from, SchemaVersion current,
                 std::span<const SchemaVersion> released, std::span<const MigrationStep> steps);

// The same, with this build's schema, releases and chain, or the chain a
// ScopedMigrationChain installed.
[[nodiscard]] foundation::Result<nlohmann::json> migrate_manifest(nlohmann::json manifest,
                                                                  SchemaVersion from);

// Tests only: loading uses `released` and `steps` instead of the built-in
// lists while this guard lives, so a project saved in a synthetic older schema
// opens through the real store. Guards nest; the spans must outlive the guard.
class ScopedMigrationChain {
public:
  ScopedMigrationChain(std::span<const SchemaVersion> released,
                       std::span<const MigrationStep> steps) noexcept;
  ~ScopedMigrationChain();
  ScopedMigrationChain(const ScopedMigrationChain&) = delete;
  ScopedMigrationChain& operator=(const ScopedMigrationChain&) = delete;
  ScopedMigrationChain(ScopedMigrationChain&&) = delete;
  ScopedMigrationChain& operator=(ScopedMigrationChain&&) = delete;

  struct Chain {
    std::span<const SchemaVersion> released;
    std::span<const MigrationStep> steps;
  };

private:
  Chain chain_;
  const Chain* previous_;
};

} // namespace oscillate::persistence::detail
