#pragma once

#include "oscillate/plugins/vst3_scan.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace oscillate::plugins {

// Steinberg's Linux search order, excluding the application-local directory.
// Pass that directory as an explicit path when the packaged application has one.
[[nodiscard]] std::vector<std::filesystem::path>
standard_linux_vst3_roots(std::filesystem::path home = {});

struct Vst3CatalogueLimits {
  std::size_t max_bundles = 4096;
  std::size_t max_entries = 100000;
  std::size_t max_search_paths = 128;
};

struct Vst3CatalogueEntry {
  std::filesystem::path canonical_bundle;
  ScanResult scan;
};

struct Vst3CatalogueIssue {
  std::filesystem::path requested_path;
  std::string diagnostic;
};

struct Vst3CatalogueSnapshot {
  std::vector<Vst3CatalogueEntry> entries;
  std::vector<Vst3CatalogueIssue> issues;
  bool truncated = false;
};

// Control/background side only. Explicit paths precede standard roots, and
// bundles within each root are ordered by canonical path. One canonical bundle
// is scanned once per refresh. Scanner receipt metadata is kept verbatim for
// later verified binding. No UI, project state or callback access is involved.
class Vst3Catalogue final {
public:
  using ScanFunction = std::function<ScanResult(const std::filesystem::path&)>;
  using RetryFunction = std::function<void(const std::filesystem::path&)>;

  // Scanner and its helper must outlive this catalogue. Tests may inject
  // deterministic actions through the second constructor.
  explicit Vst3Catalogue(Vst3Scanner& scanner,
                         std::vector<std::filesystem::path> roots = standard_linux_vst3_roots(),
                         Vst3CatalogueLimits limits = {});
  Vst3Catalogue(ScanFunction scan, RetryFunction retry, std::vector<std::filesystem::path> roots,
                Vst3CatalogueLimits limits = {});

  [[nodiscard]] const Vst3CatalogueSnapshot&
  refresh(std::span<const std::filesystem::path> explicit_paths = {});
  // Retry only an already discovered bundle. Clears scanner quarantine, then
  // rescans it once; the returned bool reports whether the bundle was known.
  [[nodiscard]] bool retry(const std::filesystem::path& bundle);
  [[nodiscard]] const Vst3CatalogueSnapshot& snapshot() const noexcept { return snapshot_; }

private:
  ScanFunction scan_;
  RetryFunction retry_;
  std::vector<std::filesystem::path> roots_;
  Vst3CatalogueLimits limits_;
  Vst3CatalogueSnapshot snapshot_;
};

} // namespace oscillate::plugins
