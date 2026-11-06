#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

namespace oscillate::application {

struct CollectionProgress {
  std::size_t completed_sources;
  std::size_t total_sources;
};

// Worker-only. Copies CURRENT, named revisions and their referenced media into
// a self-contained project. Progress callbacks run on the caller's worker.
// The source project is never modified.
[[nodiscard]] foundation::Result<void> collect_and_save_copy(
    const std::filesystem::path& source_root, const std::filesystem::path& destination_root,
    const std::stop_token& stop = {}, std::optional<std::uint64_t> expected_revision = std::nullopt,
    const std::function<void(CollectionProgress)>& progress = {});

} // namespace oscillate::application
