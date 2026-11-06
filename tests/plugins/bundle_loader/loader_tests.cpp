#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace oscillate::plugins;

namespace {

using oscillate::test::require;

std::vector<std::string> events(const fs::path& path) {
  std::ifstream input(path);
  std::vector<std::string> result;
  for (std::string line; std::getline(input, line);)
    result.push_back(line);
  return result;
}

std::size_t event_index(const std::vector<std::string>& log, const std::string& event) {
  const auto found = std::ranges::find(log, event);
  REQUIRE_NE(found, log.end(), "expected lifecycle event is absent");
  return static_cast<std::size_t>(std::distance(log.begin(), found));
}

VerifiedVst3Effect scan_gain(const fs::path& bundle) {
  Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto result = scanner.scan(bundle);
  REQUIRE_EQ(result.status, ScanStatus::ok, "synthetic gain scan failed");
  require(!result.binary.canonical_binary.empty(), "successful scan has no binary identity");
  REQUIRE_EQ(result.binary.sha256.size(), 64, "invalid SHA-256 receipt");
  REQUIRE_EQ(result.metadata.classes.size(), 1, "expected exactly one gain class");
  return {result.metadata.classes.front(), result.binary};
}

void exercise_host(const VerifiedVst3Effect& verified, const fs::path& log_path) {
  std::string error;
  auto loaded = load_verified_vst3_effect(verified, 48000.0, 128, error);
  REQUIRE_NE(loaded, nullptr, "verified gain plugin failed to load");
  REQUIRE_EQ(loaded->host().latency_samples(), 7, "plugin latency was lost");
  REQUIRE_EQ(loaded->host().identity().id, verified.identity.id, "host identity differs");
  require(loaded->host().activate(error), "gain plugin activation failed");
  std::array<float, 64> in_left{}, in_right{}, out_left{}, out_right{};
  in_left.fill(0.25F);
  in_right.fill(-0.5F);
  const Vst3StereoBlock block{in_left, in_right, out_left, out_right};
  REQUIRE_EQ(loaded->host().process(block), Vst3ProcessStatus::ok, "gain process failed");
  REQUIRE_TRUE(std::ranges::all_of(std::span(out_left).first(7),
                                   [](float sample) { return sample == 0.0F; }),
               "left gain latency or output incorrect");
  REQUIRE_TRUE(std::ranges::all_of(std::span(out_left).subspan(7),
                                   [](float sample) { return std::abs(sample - 0.5F) < 0.00001F; }),
               "left gain latency or output incorrect");
  REQUIRE_TRUE(std::ranges::all_of(std::span(out_right).first(7),
                                   [](float sample) { return sample == 0.0F; }),
               "right gain latency or output incorrect");
  REQUIRE_TRUE(std::ranges::all_of(std::span(out_right).subspan(7),
                                   [](float sample) { return std::abs(sample + 1.0F) < 0.00001F; }),
               "right gain latency or output incorrect");
  loaded->host().suspend();
  const auto saved = loaded->host().save_component_state(error);
  if (!saved)
    throw std::runtime_error("component state did not save");
  REQUIRE_EQ(saved->size(), sizeof(float), "component state size is incorrect");
  float saved_gain = 0.0F;
  std::memcpy(&saved_gain, saved.value().data(), sizeof(saved_gain));
  REQUIRE_EQ(saved_gain, 2.0F, "saved component gain differs");
  auto changed = saved.value();
  const float half = 0.5F;
  std::memcpy(changed.data(), &half, sizeof(half));
  require(loaded->host().restore_component_state(changed, error), "state restore failed");
  require(loaded->host().activate(error), "reactivation after restore failed");
  REQUIRE_EQ(loaded->host().process(block), Vst3ProcessStatus::ok, "restored process failed");
  REQUIRE_LT(std::abs(out_left[7] - 0.125F), 0.00001F, "restored gain not applied");
  loaded.reset();
  const auto log = events(log_path);
  REQUIRE_LT(event_index(log, "component-terminate"), event_index(log, "component-destroy"),
             "component was destroyed before termination");
  REQUIRE_LT(event_index(log, "component-destroy"), event_index(log, "module-exit"),
             "bundle unloaded before component destruction");
  REQUIRE_EQ(std::count(log.begin(), log.end(), "module-exit"), 1,
             "module exit was not called exactly once");
}

void exercise_rejections(const VerifiedVst3Effect& verified, const fs::path& log_path) {
  std::string error;
  auto changed_class = verified;
  changed_class.identity.name = "Different class";
  require(!load_verified_vst3_effect(changed_class, 48000.0, 128, error),
          "changed class metadata accepted");
  REQUIRE_NE(error.find("identity"), std::string::npos, "class error is unclear");
  auto log = events(log_path);
  REQUIRE_EQ(std::count(log.begin(), log.end(), "module-exit"), 2,
             "class rejection did not close module");
  REQUIRE_EQ(std::count(log.begin(), log.end(), "component-create"), 1,
             "rejected class still created processor");

  const auto copied = log_path.parent_path() / "ChangedGain.vst3";
  const auto copied_binary = copied / "Contents/x86_64-linux/ChangedGain.so";
  fs::create_directories(copied_binary.parent_path());
  fs::copy_file(verified.binary.canonical_binary, copied_binary);
  const auto changed_receipt = scan_gain(copied);
  log = events(log_path);
  const auto entries_before_rejection = std::count(log.begin(), log.end(), "module-entry");
  fs::resize_file(copied_binary, fs::file_size(copied_binary) + 1);
  require(!load_verified_vst3_effect(changed_receipt, 48000.0, 128, error),
          "modified binary accepted");
  REQUIRE_NE(error.find("changed"), std::string::npos, "modified binary error is unclear");
  fs::remove(copied_binary);
  require(!load_verified_vst3_effect(changed_receipt, 48000.0, 128, error),
          "missing binary accepted");
  log = events(log_path);
  REQUIRE_EQ(std::count(log.begin(), log.end(), "module-entry"), entries_before_rejection,
             "changed or missing binary reached ModuleEntry");

  for (const auto* mode : {"entry", "factory", "factory-info", "class-count", "class-info",
                           "no-instance", "factory-v1", "factory-vendor-empty", "basic-fail",
                           "class-name-empty", "class-version-control"}) {
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", mode, 1), 0,
               "cannot set fixture failure mode");
    error.clear();
    require(!load_verified_vst3_effect(verified, 48000.0, 128, error),
            "broken plugin was accepted");
    require(!error.empty(), "broken plugin has no diagnostic");
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0, "cannot clear fixture failure mode");
  }

  for (const auto* field : {"id", "category", "vendor", "version", "subcategories"}) {
    auto mismatched = verified;
    if (std::strcmp(field, "id") == 0)
      mismatched.identity.id = "ffffffffffffffffffffffffffffffff";
    else if (std::strcmp(field, "category") == 0)
      mismatched.identity.category = "Wrong category";
    else if (std::strcmp(field, "vendor") == 0)
      mismatched.identity.vendor = "Wrong vendor";
    else if (std::strcmp(field, "version") == 0)
      mismatched.identity.version = "99";
    else
      mismatched.identity.subcategories = "Wrong";
    require(!load_verified_vst3_effect(mismatched, 48000.0, 128, error),
            "mismatched scanned class accepted");
  }
}

} // namespace

int main() {
  try {
    const auto bundle = fs::path(VST3_GAIN_BUNDLE);
    const auto root = bundle.parent_path() / "loader_test_runtime";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto log_path = root / "events.log";
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_EVENTS", log_path.c_str(), 1), 0,
               "cannot set fixture event path");
    const auto verified = scan_gain(bundle);
    fs::remove(log_path); // Scanner executes in a separate process.
    exercise_host(verified, log_path);
    exercise_rejections(verified, log_path);
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "bundle loader test: %s\n", error.what());
    return 1;
  }
}
