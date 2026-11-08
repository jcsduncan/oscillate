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
inline constexpr SchemaVersion current_schema{1, 47};

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

// The same, with this build's schema, releases and chain.
[[nodiscard]] foundation::Result<nlohmann::json> migrate_manifest(nlohmann::json manifest,
                                                                  SchemaVersion from);

} // namespace oscillate::persistence::detail
