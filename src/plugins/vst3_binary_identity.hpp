#pragma once

#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <filesystem>
#include <optional>

namespace oscillate::plugins::detail {

// The bundle's binary as the file system reports it now, without reading it.
struct ResolvedVst3Binary {
  std::filesystem::path canonical_bundle;
  std::filesystem::path canonical_binary;
  Vst3BinaryStamp stamp;
};

[[nodiscard]] std::optional<ResolvedVst3Binary>
resolve_vst3_binary(const std::filesystem::path& bundle);

// Reads and hashes the whole binary; the stamp is the same before and after.
[[nodiscard]] std::optional<Vst3HashedBinary>
hash_resolved_vst3_binary(const std::filesystem::path& bundle);

[[nodiscard]] std::optional<Vst3BinaryIdentity>
fingerprint_vst3_binary(const std::filesystem::path& bundle);

} // namespace oscillate::plugins::detail
