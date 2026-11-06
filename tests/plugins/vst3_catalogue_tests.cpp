#include "oscillate/plugins/vst3_catalogue.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace plugins = oscillate::plugins;

using oscillate::test::require;

class FixtureTree final {
public:
  FixtureTree() : path_(CATALOGUE_FIXTURE_DIR) {
    std::error_code error;
    fs::remove_all(path_, error);
    require(!error, "could not clear catalogue fixture directory");
    fs::create_directories(path_);
  }
  ~FixtureTree() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
  fs::path path_;
};

class FakeScanner final {
public:
  plugins::ScanResult scan(const fs::path& bundle) {
    ++calls_[bundle];
    if (!fs::is_directory(bundle))
      return {.status = plugins::ScanStatus::invalid_bundle,
              .metadata = {},
              .diagnostic = "missing fixture",
              .binary = {}};
    if (bundle.filename() == "Quarantine.vst3") {
      auto& failures = failures_[bundle];
      if (failures >= 2)
        return {.status = plugins::ScanStatus::quarantined,
                .metadata = {},
                .diagnostic = "quarantined fixture",
                .binary = {}};
      ++failures;
      return {.status = plugins::ScanStatus::invalid_metadata,
              .metadata = {},
              .diagnostic = "bad fixture",
              .binary = {}};
    }
    return {.status = plugins::ScanStatus::ok,
            .metadata = {.vendor = "Fixture Vendor",
                         .classes = {{"0123456789abcdef0123456789abcdef", "Fx", "Gain",
                                      "Fixture Vendor", "1.0", ""}}},
            .diagnostic = {},
            .binary = {.canonical_bundle = bundle,
                       .canonical_binary = bundle / "Contents/x86_64-linux/Gain.so",
                       .byte_size = 123,
                       .sha256 = std::string(64, 'a')}};
  }
  void retry(const fs::path& bundle) { failures_.erase(bundle); }
  [[nodiscard]] unsigned calls(const fs::path& bundle) const {
    const auto found = calls_.find(bundle);
    return found == calls_.end() ? 0 : found->second;
  }

private:
  std::map<fs::path, unsigned> calls_;
  std::map<fs::path, unsigned> failures_;
};

plugins::Vst3Catalogue catalogue(FakeScanner& scanner, std::vector<fs::path> roots,
                                 plugins::Vst3CatalogueLimits limits = {}) {
  return {[&scanner](const auto& path) { return scanner.scan(path); },
          [&scanner](const auto& path) { scanner.retry(path); }, std::move(roots), limits};
}

void standard_roots_follow_documented_priority() {
  const auto home = fs::path("/tmp/oscillate-catalogue-home");
  const auto roots = plugins::standard_linux_vst3_roots(home);
  REQUIRE_EQ(roots,
             std::vector<fs::path>({home / ".vst3", "/usr/lib64/vst3", "/usr/lib/vst3",
                                    "/usr/local/lib64/vst3", "/usr/local/lib/vst3"}),
             "standard Linux VST3 search roots or priority changed");
  if (const auto* environment_home = std::getenv("HOME");
      environment_home != nullptr && environment_home[0] != '\0') {
    REQUIRE_EQ(plugins::standard_linux_vst3_roots().front(), fs::path(environment_home) / ".vst3",
               "default user plugin root did not use HOME");
  }
}

void canonical_dedup_and_stable_order(const fs::path& fixture) {
  const auto root = fixture / "root";
  fs::create_directories(root / "B.vst3");
  fs::create_directories(root / "A.vst3");
  fs::create_directory_symlink(root / "A.vst3", root / "Alias.vst3");
  FakeScanner fake;
  auto service = catalogue(fake, {root, root});
  const std::array<fs::path, 2> explicit_paths{root / "B.vst3", root / "Alias.vst3"};
  const auto& first = service.refresh(explicit_paths);
  REQUIRE_TRUE(!first.truncated, "explicit priority or canonical bundle deduplication failed");
  REQUIRE_EQ(first.entries.size(), 2, "explicit priority or canonical bundle deduplication failed");
  REQUIRE_EQ(first.entries[0].canonical_bundle, fs::canonical(root / "B.vst3"),
             "explicit priority or canonical bundle deduplication failed");
  REQUIRE_EQ(first.entries[1].canonical_bundle, fs::canonical(root / "A.vst3"),
             "explicit priority or canonical bundle deduplication failed");
  REQUIRE_TRUE(first.entries[0].scan.ok(),
               "verified binary receipt was lost or duplicate alias scanned twice");
  REQUIRE_EQ(first.entries[0].scan.binary.canonical_bundle, first.entries[0].canonical_bundle,
             "verified binary receipt was lost or duplicate alias scanned twice");
  REQUIRE_EQ(first.entries[0].scan.binary.byte_size, 123,
             "verified binary receipt was lost or duplicate alias scanned twice");
  REQUIRE_EQ(first.entries[0].scan.binary.sha256, std::string(64, 'a'),
             "verified binary receipt was lost or duplicate alias scanned twice");
  REQUIRE_EQ(fake.calls(first.entries[0].canonical_bundle), 1,
             "verified binary receipt was lost or duplicate alias scanned twice");
  REQUIRE_EQ(fake.calls(first.entries[1].canonical_bundle), 1,
             "verified binary receipt was lost or duplicate alias scanned twice");
  const auto first_path = first.entries[0].canonical_bundle;
  const auto second_path = first.entries[1].canonical_bundle;
  const auto& second = service.refresh(explicit_paths);
  REQUIRE_EQ(second.entries.size(), 2, "catalogue refresh order changed or skipped a fresh scan");
  REQUIRE_EQ(second.entries[0].canonical_bundle, first_path,
             "catalogue refresh order changed or skipped a fresh scan");
  REQUIRE_EQ(second.entries[1].canonical_bundle, second_path,
             "catalogue refresh order changed or skipped a fresh scan");
  REQUIRE_EQ(fake.calls(second.entries[0].canonical_bundle), 2,
             "catalogue refresh order changed or skipped a fresh scan");
}

void invalid_bundles_and_limits(const fs::path& fixture) {
  const auto root = fixture / "limited";
  fs::create_directories(root / "C.vst3");
  fs::create_directories(root / "A.vst3");
  fs::create_directories(root / "B.vst3");
  const auto absent = fixture / "Absent.vst3";
  FakeScanner fake;
  auto service = catalogue(fake, {root}, {.max_bundles = 2});
  const std::array<fs::path, 1> explicit_paths{absent};
  const auto& bounded = service.refresh(explicit_paths);
  REQUIRE_TRUE(bounded.truncated,
               "invalid explicit bundle status or bounded deterministic selection failed");
  REQUIRE_EQ(bounded.entries.size(), 2,
             "invalid explicit bundle status or bounded deterministic selection failed");
  REQUIRE_EQ(bounded.entries[0].scan.status, plugins::ScanStatus::invalid_bundle,
             "invalid explicit bundle status or bounded deterministic selection failed");
  REQUIRE_EQ(bounded.entries[1].canonical_bundle, fs::canonical(root / "A.vst3"),
             "invalid explicit bundle status or bounded deterministic selection failed");
  REQUIRE_EQ(fake.calls(fs::canonical(root / "B.vst3")), 0,
             "invalid explicit bundle status or bounded deterministic selection failed");

  auto entry_limit = catalogue(fake, {root}, {.max_entries = 0});
  const auto& only_explicit = entry_limit.refresh(explicit_paths);
  REQUIRE_EQ(only_explicit.entries.size(), 1, "directory-entry bound was not reported");
  REQUIRE_TRUE(only_explicit.truncated, "directory-entry bound was not reported");
  auto search_limit = catalogue(fake, {root}, {.max_search_paths = 1});
  const auto& searched = search_limit.refresh(explicit_paths);
  REQUIRE_EQ(searched.entries.size(), 1, "search-path bound was not reported");
  REQUIRE_TRUE(searched.truncated, "search-path bound was not reported");

  auto result_limit = catalogue(fake, {root}, {.max_bundles = 1});
  const auto& one_result = result_limit.refresh(explicit_paths);
  REQUIRE_EQ(one_result.entries.size(), 1,
             "result bound did not stop discovery after explicit bundle");
  REQUIRE_TRUE(one_result.truncated, "result bound did not stop discovery after explicit bundle");
  REQUIRE_EQ(one_result.entries.front().scan.status, plugins::ScanStatus::invalid_bundle,
             "result bound did not stop discovery after explicit bundle");
}

void malformed_paths_and_missing_root_are_bounded(const fs::path& fixture) {
  const auto missing_root = fixture / "missing_root";
  const auto symlink_loop = fixture / "Loop.vst3";
  fs::create_symlink(symlink_loop, symlink_loop);
  FakeScanner fake;
  auto service = catalogue(fake, {missing_root});
  const std::array<fs::path, 2> explicit_paths{fs::path{}, symlink_loop};
  const auto& snapshot = service.refresh(explicit_paths);
  REQUIRE_TRUE(snapshot.entries.empty(),
               "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_TRUE(snapshot.truncated,
               "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_EQ(snapshot.issues.size(), 2,
             "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_TRUE(snapshot.issues[0].requested_path.empty(),
               "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_EQ(snapshot.issues[0].diagnostic, "Plugin search path is empty",
             "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_EQ(snapshot.issues[1].requested_path, symlink_loop,
             "malformed explicit paths did not preserve actionable discovery errors");
  REQUIRE_EQ(snapshot.issues[1].diagnostic, "Cannot resolve plugin search path",
             "malformed explicit paths did not preserve actionable discovery errors");
  require(!service.retry({}), "empty retry path was accepted");
}

void injected_actions_must_be_valid() {
  bool rejected = false;
  try {
    plugins::Vst3Catalogue service({}, {}, {});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "empty injected scanner actions were accepted");
}

void quarantine_and_explicit_retry(const fs::path& fixture) {
  const auto bundle = fixture / "Quarantine.vst3";
  fs::create_directories(bundle);
  FakeScanner fake;
  auto service = catalogue(fake, {});
  const std::array<fs::path, 1> explicit_paths{bundle};
  REQUIRE_EQ(service.refresh(explicit_paths).entries[0].scan.status,
             plugins::ScanStatus::invalid_metadata,
             "repeated scanner failures did not expose quarantine");
  REQUIRE_EQ(service.refresh(explicit_paths).entries[0].scan.status,
             plugins::ScanStatus::invalid_metadata,
             "repeated scanner failures did not expose quarantine");
  REQUIRE_EQ(service.refresh(explicit_paths).entries[0].scan.status,
             plugins::ScanStatus::quarantined,
             "repeated scanner failures did not expose quarantine");
  REQUIRE_TRUE(!service.retry(fixture / "Unknown.vst3"),
               "retry of unknown bundle changed scanner state");
  REQUIRE_EQ(fake.calls(bundle), 3, "retry of unknown bundle changed scanner state");
  REQUIRE_TRUE(service.retry(bundle), "explicit retry did not clear quarantine and rescan once");
  REQUIRE_EQ(service.snapshot().entries[0].scan.status, plugins::ScanStatus::invalid_metadata,
             "explicit retry did not clear quarantine and rescan once");
  REQUIRE_EQ(fake.calls(bundle), 4, "explicit retry did not clear quarantine and rescan once");
}

void real_scanner_receipt_is_preserved() {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  plugins::Vst3Catalogue service(scanner, {});
  const auto bundle = fs::path(VST3_FIXTURE_ROOT) / "Valid.vst3";
  const std::array<fs::path, 1> explicit_paths{bundle};
  const auto& found = service.refresh(explicit_paths);
  REQUIRE_EQ(found.entries.size(), 1, "production scanner receipt lost verified binary identity");
  REQUIRE_TRUE(found.entries[0].scan.ok(),
               "production scanner receipt lost verified binary identity");
  REQUIRE_EQ(found.entries[0].scan.binary.canonical_bundle, fs::canonical(bundle),
             "production scanner receipt lost verified binary identity");
  REQUIRE_EQ(found.entries[0].scan.binary.sha256.size(), 64,
             "production scanner receipt lost verified binary identity");
  REQUIRE_TRUE(service.retry(bundle), "production scanner retry did not rescan the known fixture");
  REQUIRE_TRUE(service.snapshot().entries[0].scan.ok(),
               "production scanner retry did not rescan the known fixture");
}

} // namespace

int main() {
  try {
    FixtureTree fixture;
    standard_roots_follow_documented_priority();
    canonical_dedup_and_stable_order(fixture.path());
    invalid_bundles_and_limits(fixture.path());
    malformed_paths_and_missing_root_are_bounded(fixture.path());
    injected_actions_must_be_valid();
    quarantine_and_explicit_retry(fixture.path());
    real_scanner_receipt_is_preserved();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 catalogue: %s\n", error.what());
    return 1;
  }
}
