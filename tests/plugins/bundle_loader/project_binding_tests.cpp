#include "oscillate/application/vst3/project_binding.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace oscillate;

namespace {

using oscillate::test::require;

plugins::ScanResult scan(const fs::path& bundle) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto result = scanner.scan(bundle);
  require(result.ok(), "gain fixture scan failed");
  REQUIRE_EQ(result.metadata.classes.size(), 1, "unexpected fixture classes");
  return result;
}

plugins::ScanResult scan_connected(const fs::path& bundle) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto result = scanner.scan(bundle);
  REQUIRE_TRUE(result.ok(), "connected split-state fixture scan failed");
  REQUIRE_EQ(result.metadata.classes.size(), 2, "connected split-state fixture scan failed");
  return result;
}

project::Vst3PluginInstance saved(const plugins::ScanResult& receipt) {
  auto id = project::PluginInstanceId::from_bits({0, 1});
  REQUIRE_OK(id, "fixture plugin ID failed");
  const auto& klass = receipt.metadata.classes.front();
  project::Vst3PluginInstance result{.id = id.value(),
                                     .class_id = klass.id,
                                     .vendor = klass.vendor,
                                     .name = klass.name,
                                     .version = klass.version,
                                     .bundle_path = "/does/not/exist/advisory.vst3",
                                     .binary_sha256 = receipt.binary.sha256,
                                     .component_state = {},
                                     .parameters = {{42, 0.5}},
                                     .missing = true};
  const float gain = 0.5F;
  result.component_state.resize(sizeof(gain));
  std::memcpy(result.component_state.data(), &gain, sizeof(gain));
  return result;
}

std::vector<std::string> log_events(const fs::path& path) {
  std::ifstream stream(path);
  std::vector<std::string> events;
  for (std::string line; std::getline(stream, line);)
    events.push_back(line);
  return events;
}

void live_restore(const plugins::ScanResult& receipt, const fs::path& log_path) {
  auto instance = saved(receipt);
  const auto original = instance; // NOLINT(performance-unnecessary-copy-initialization)
  const auto entry_count = std::ranges::count(log_events(log_path), "module-entry");
  auto binding = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  require(binding.ready(), "valid saved plugin failed binding");
  REQUIRE_EQ(binding.status, plugins::Vst3BindingStatus::ready, "wrong ready status");
  REQUIRE_EQ(binding.unresolved_parameter_ids, std::vector<std::uint32_t>{42},
             "unknown parameter was not reported");
  require(!binding.diagnostic.empty(), "unresolved parameter lacks diagnostic");
  REQUIRE_EQ(instance, original, "binding mutated durable placeholder");
  REQUIRE_TRUE(instance.missing, "binding mutated durable placeholder");
  std::array<float, 64> input_left{}, input_right{}, output_left{}, output_right{};
  input_left.fill(0.25F);
  input_right.fill(-0.5F);
  REQUIRE_EQ(binding.effect->host().process({input_left, input_right, output_left, output_right}),
             plugins::Vst3ProcessStatus::ok, "bound plugin did not process");
  REQUIRE_EQ(output_left.front(), 0.0F, "saved state was not restored before activation");
  REQUIRE_LT(std::abs(output_left[7] - 0.125F), 0.00001F,
             "saved state was not restored before activation");
  REQUIRE_EQ(output_right.front(), 0.0F, "restored gain differs between channels");
  REQUIRE_LT(std::abs(output_right[7] + 0.25F), 0.00001F, "restored gain differs between channels");
  binding.effect.reset();
  const auto log = log_events(log_path);
  REQUIRE_EQ(std::ranges::count(log, "module-entry"), entry_count + 1,
             "binding did not load exactly one module");
  REQUIRE_TRUE(!log.empty(), "module did not exit after host teardown");
  REQUIRE_EQ(log.back(), "module-exit", "module did not exit after host teardown");
  const auto destroyed = std::ranges::find(log, "component-destroy");
  const auto exited = std::ranges::find(log, "module-exit");
  REQUIRE_NE(destroyed, log.end(), "component lifetime exceeded module lifetime");
  REQUIRE_NE(exited, log.end(), "component lifetime exceeded module lifetime");
  REQUIRE_LT(destroyed, exited, "component lifetime exceeded module lifetime");
}

void split_controller_restore(const plugins::ScanResult& receipt, const fs::path& log_path) {
  auto instance = saved(receipt);
  instance.parameters = {{101, 0.5}};
  instance.controller_state = std::vector<std::uint8_t>{0x7b};
  REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", "controller-component-not-implemented", 1),
             0, "cannot select optional controller synchronization fixture");
  const auto before = log_events(log_path).size();
  auto binding = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(binding.ready(),
               "independent controller restore must succeed when setComponentState is unsupported");
  REQUIRE_TRUE(binding.diagnostic.empty(),
               "independent controller restore must succeed when setComponentState is unsupported");
  REQUIRE_EQ(binding.effect->host().controller_state_sync(),
             plugins::Vst3ControllerStateSync::not_implemented,
             "independent controller restore must succeed when setComponentState is unsupported");
  const auto events = log_events(log_path);
  const auto component = std::ranges::find(events.begin() + static_cast<std::ptrdiff_t>(before),
                                           events.end(), "component-state");
  const auto skipped = std::ranges::find(events.begin() + static_cast<std::ptrdiff_t>(before),
                                         events.end(), "controller-state-not-implemented");
  const auto controller = std::ranges::find(events.begin() + static_cast<std::ptrdiff_t>(before),
                                            events.end(), "controller-private-restore");
  REQUIRE_NE(component, events.end(),
             "VST3 restore order must be component, component view, private controller state");
  REQUIRE_NE(skipped, events.end(),
             "VST3 restore order must be component, component view, private controller state");
  REQUIRE_NE(controller, events.end(),
             "VST3 restore order must be component, component view, private controller state");
  REQUIRE_LT(component, skipped,
             "VST3 restore order must be component, component view, private controller state");
  REQUIRE_LT(skipped, controller,
             "VST3 restore order must be component, component view, private controller state");
  binding.effect->host().suspend();
  std::string error;
  const auto pair = binding.effect->host().save_state_pair(error);
  REQUIRE_TRUE(pair, "restored independent bytes must save exactly for project reopen");
  REQUIRE_EQ(pair->component_state, instance.component_state,
             "restored independent bytes must save exactly for project reopen");
  REQUIRE_TRUE(pair->controller_state,
               "restored independent bytes must save exactly for project reopen");
  REQUIRE_EQ(*pair->controller_state, *instance.controller_state,
             "restored independent bytes must save exactly for project reopen");
  binding.effect.reset();
  REQUIRE_EQ(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0,
             "cannot clear optional synchronization fixture");

  instance.component_state.clear();
  instance.parameters = {{101, 0.65}};
  auto controller_only = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  require(controller_only.ready(),
          "controller-only chunk must coexist with static parameter-value fallback");
  controller_only.effect->host().suspend();
  const auto controller_only_pair = controller_only.effect->host().save_state_pair(error);
  REQUIRE_TRUE(controller_only_pair,
               "controller-only state was lost while applying saved parameter values");
  REQUIRE_EQ(controller_only_pair->controller_state, instance.controller_state,
             "controller-only state was lost while applying saved parameter values");
  controller_only.effect.reset();

  REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", "controller-private-restore", 1), 0,
             "cannot select controller restore failure fixture");
  auto refused = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!refused.ready(),
               "refused private controller state must never publish a ready plugin");
  REQUIRE_EQ(refused.status, plugins::Vst3BindingStatus::state_rejected,
             "refused private controller state must never publish a ready plugin");
  REQUIRE_TRUE(!refused.diagnostic.empty(),
               "refused private controller state must never publish a ready plugin");
  REQUIRE_EQ(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0,
             "cannot clear controller restore failure fixture");

  instance.controller_state = std::vector<std::uint8_t>{};
  auto empty = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!empty.ready(),
               "present-empty private state must call controller setState and report refusal");
  REQUIRE_EQ(empty.status, plugins::Vst3BindingStatus::state_rejected,
             "present-empty private state must call controller setState and report refusal");
}

void reject_unapplied_values(const plugins::ScanResult& receipt, const fs::path& log_path) {
  auto instance = saved(receipt);
  instance.component_state.clear();
  const auto original = instance; // NOLINT(performance-unnecessary-copy-initialization)
  const auto entries = std::ranges::count(log_events(log_path), "module-entry");
  auto binding = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!binding.ready(),
               "saved normalized values without component state must fail closed");
  REQUIRE_EQ(binding.status, plugins::Vst3BindingStatus::parameter_values_unapplied,
             "saved normalized values without component state must fail closed");
  REQUIRE_EQ(binding.effect, nullptr,
             "saved normalized values without component state must fail closed");
  REQUIRE_TRUE(!binding.diagnostic.empty(),
               "saved normalized values without component state must fail closed");
  REQUIRE_EQ(binding.unresolved_parameter_ids, std::vector<std::uint32_t>{42},
             "unsupported binding must retain unknown parameter diagnostics");
  REQUIRE_EQ(instance, original, "unsupported binding mutated durable parameter intent");
  REQUIRE_EQ(std::ranges::count(log_events(log_path), "module-entry"), entries + 1,
             "unsupported binding did not inspect the verified parameter interface");
  const auto events = log_events(log_path);
  REQUIRE_TRUE(!events.empty(), "unsupported binding leaked its plugin module");
  REQUIRE_EQ(events.back(), "module-exit", "unsupported binding leaked its plugin module");
}

void reject_without_loading(const project::Vst3PluginInstance& instance,
                            const plugins::ScanResult& receipt, plugins::Vst3BindingStatus status,
                            const fs::path& log_path) {
  const auto before = std::ranges::count(log_events(log_path), "module-entry");
  auto binding = plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!binding.ready(), "mismatch returned wrong binding status");
  REQUIRE_EQ(binding.status, status, "mismatch returned wrong binding status");
  REQUIRE_TRUE(!binding.diagnostic.empty(), "mismatch returned wrong binding status");
  REQUIRE_EQ(std::ranges::count(log_events(log_path), "module-entry"), before,
             "rejected identity still loaded plugin");
}

struct RuntimePaths {
  fs::path root;
  fs::path log_path;
};

void failures(const plugins::ScanResult& receipt, const RuntimePaths& paths) {
  const auto& root = paths.root;
  const auto& log_path = paths.log_path;
  auto instance = saved(receipt);
  auto unavailable = receipt;
  unavailable.status = plugins::ScanStatus::quarantined;
  unavailable.diagnostic = "quarantined";
  reject_without_loading(instance, unavailable, plugins::Vst3BindingStatus::scan_unavailable,
                         log_path);

  auto wrong = instance;
  wrong.class_id = std::string(32, 'f');
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.vendor = "other vendor";
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.binary_sha256 = std::string(64, '0');
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.parameters = {{42, 1.5}};
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.parameters = {{42, 0.5}, {42, 0.7}};
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.parameters.assign(project::max_vst3_parameter_references + 1, {42, 0.5});
  reject_without_loading(wrong, receipt, plugins::Vst3BindingStatus::identity_mismatch, log_path);
  wrong = instance;
  wrong.component_state.assign(1, 0);
  auto invalid_state = plugins::bind_vst3_project_effect(wrong, receipt, 48000.0, 128);
  REQUIRE_TRUE(!invalid_state.ready(), "invalid component state became active");
  REQUIRE_EQ(invalid_state.status, plugins::Vst3BindingStatus::state_rejected,
             "invalid component state became active");
  REQUIRE_TRUE(!invalid_state.diagnostic.empty(), "invalid component state became active");

  const auto copied = root / "ChangedGain.vst3";
  const auto binary = copied / "Contents/x86_64-linux/ChangedGain.so";
  fs::create_directories(binary.parent_path());
  fs::copy_file(receipt.binary.canonical_binary, binary);
  const auto copied_receipt = scan(copied);
  auto copied_saved = saved(copied_receipt);
  const auto before = std::ranges::count(log_events(log_path), "module-entry");
  fs::resize_file(binary, fs::file_size(binary) + 1);
  auto changed = plugins::bind_vst3_project_effect(copied_saved, copied_receipt, 48000.0, 128);
  REQUIRE_TRUE(!changed.ready(), "changed bytes accepted after scan");
  REQUIRE_EQ(changed.status, plugins::Vst3BindingStatus::load_failed,
             "changed bytes accepted after scan");
  REQUIRE_TRUE(!changed.diagnostic.empty(), "changed bytes accepted after scan");
  REQUIRE_EQ(std::ranges::count(log_events(log_path), "module-entry"), before,
             "changed binary reached module entry");
}

void relocated(const plugins::ScanResult& receipt, const fs::path& root) {
  const auto bundle = root / "RelocatedGain.vst3";
  const auto binary = bundle / "Contents/x86_64-linux/RelocatedGain.so";
  fs::create_directories(binary.parent_path());
  fs::copy_file(receipt.binary.canonical_binary, binary);
  const auto fresh = scan(bundle);
  auto instance = saved(receipt);
  REQUIRE_NE(fresh.binary.canonical_bundle, receipt.binary.canonical_bundle,
             "relocation fixture did not change path");
  REQUIRE_EQ(fresh.binary.sha256, instance.binary_sha256, "relocated bytes differ");
  auto binding = plugins::bind_vst3_project_effect(instance, fresh, 48000.0, 128);
  require(binding.ready(), "explicitly rescanned relocation failed to bind");
}

// A binary hashed on a worker loads without a second hash: the load compares
// the file's stamp, so a replaced or touched file is refused before it loads.
void hashed_binding(const plugins::ScanResult& receipt, const RuntimePaths& paths) {
  constexpr double hashed_rate = 48000.0;
  constexpr std::uint32_t hashed_block = 128;
  // A modification time the copy cannot already have.
  constexpr auto later = std::chrono::seconds(1);
  const auto bundle = paths.root / "HashedGain.vst3";
  const auto binary = bundle / "Contents/x86_64-linux/HashedGain.so";
  fs::create_directories(binary.parent_path());
  fs::copy_file(receipt.binary.canonical_binary, binary);
  const auto fresh = scan(bundle);
  const auto instance = saved(receipt);
  const auto hashed = plugins::hash_vst3_binary(bundle);
  REQUIRE_TRUE(hashed.has_value(), "copied binary did not hash");
  REQUIRE_EQ(hashed->identity.sha256, fresh.binary.sha256, "worker hash differs from the scan");
  {
    auto binding = plugins::bind_hashed_vst3_project_effect(instance, fresh, *hashed, hashed_rate,
                                                            hashed_block);
    REQUIRE_TRUE(binding.ready(), "hashed binary did not bind: " + binding.diagnostic);
  }

  const auto other = plugins::hash_vst3_binary(receipt.binary.canonical_bundle);
  REQUIRE_TRUE(other.has_value(), "fixture binary did not hash");
  const auto entries = std::ranges::count(log_events(paths.log_path), "module-entry");
  auto foreign =
      plugins::bind_hashed_vst3_project_effect(instance, fresh, *other, hashed_rate, hashed_block);
  REQUIRE_EQ(foreign.status, plugins::Vst3BindingStatus::identity_mismatch,
             "a hash of another bundle was accepted for this scan");

  // Same bytes under a new inode: the stamp differs, so the load is refused.
  const auto replacement = binary.parent_path() / "replacement.so";
  fs::copy_file(binary, replacement);
  fs::rename(replacement, binary);
  auto replaced =
      plugins::bind_hashed_vst3_project_effect(instance, fresh, *hashed, hashed_rate, hashed_block);
  REQUIRE_EQ(replaced.status, plugins::Vst3BindingStatus::load_failed,
             "a binary replaced after hashing was loaded");
  REQUIRE_TRUE(replaced.diagnostic.find("changed") != std::string::npos,
               "refusal does not say the binary changed: " + replaced.diagnostic);

  // Same inode with a later modification time is refused too.
  const auto rehashed = plugins::hash_vst3_binary(bundle);
  REQUIRE_TRUE(rehashed.has_value(), "replaced binary did not hash");
  fs::last_write_time(binary, fs::last_write_time(binary) + later);
  auto touched = plugins::bind_hashed_vst3_project_effect(instance, fresh, *rehashed, hashed_rate,
                                                          hashed_block);
  REQUIRE_EQ(touched.status, plugins::Vst3BindingStatus::load_failed,
             "a binary modified after hashing was loaded");

  // A rewrite that restores the modification time still changes the
  // status change time; a permission write alone stands in for it here.
  const auto rehashed_again = plugins::hash_vst3_binary(bundle);
  REQUIRE_TRUE(rehashed_again.has_value(), "touched binary did not hash");
  fs::permissions(binary, fs::status(binary).permissions());
  auto status_changed = plugins::bind_hashed_vst3_project_effect(instance, fresh, *rehashed_again,
                                                                 hashed_rate, hashed_block);
  REQUIRE_EQ(status_changed.status, plugins::Vst3BindingStatus::load_failed,
             "a binary whose status changed after hashing was loaded");
  REQUIRE_EQ(std::ranges::count(log_events(paths.log_path), "module-entry"), entries,
             "a refused binary reached module entry");
}

void instrument_source_binding(const plugins::ScanResult& receipt) {
  auto instance = saved(receipt);
  instance.component_state.clear();
  instance.parameters = {{1, 0.65}};
  const auto original = instance; // NOLINT(performance-unnecessary-copy-initialization)
  auto binding = plugins::bind_vst3_project_instrument(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(binding.ready(), "saved instrument values did not bind to a verified source");
  REQUIRE_TRUE(binding.unresolved_parameter_ids.empty(),
               "saved instrument values did not bind to a verified source");
  REQUIRE_EQ(instance, original, "instrument binding mutated saved source intent");
  const auto& metadata = binding.effect->host().parameters();
  REQUIRE_EQ(metadata.size(), 1, "instrument parameter metadata was unavailable after binding");
  REQUIRE_EQ(metadata.front().id, 1, "instrument parameter metadata was unavailable after binding");
  REQUIRE_EQ(metadata.front().title, "Level",
             "instrument parameter metadata was unavailable after binding");
  REQUIRE_EQ(binding.effect->host().parameter_precision(1),
             plugins::Vst3ParameterPrecision::sample_offsets_forwarded,
             "instrument parameter metadata was unavailable after binding");
  std::array<float, 64> left{}, right{};
  const std::array note{plugins::Vst3NoteEvent{
      .kind = plugins::Vst3NoteKind::on, .sample_offset = 0, .velocity = 1.0F, .note_id = 1}};
  REQUIRE_EQ(
      binding.effect->host().process_instrument(
          {left, right}, note,
          {.project_sample = 0, .quarter_position = 0.0, .tempo_bpm = 120.0, .generation = 1}),
      plugins::Vst3ProcessStatus::ok, "bound instrument did not apply its saved static parameter");
  REQUIRE_LT(std::fabs(left.front() - 0.65F), 0.00001F,
             "bound instrument did not apply its saved static parameter");
  REQUIRE_LT(std::fabs(right.front() + 0.65F), 0.00001F,
             "bound instrument did not apply its saved static parameter");
  binding.effect.reset();

  instance.parameters = {{42, 0.65}};
  auto unknown = plugins::bind_vst3_project_instrument(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!unknown.ready(), "unavailable instrument parameter was silently applied");
  REQUIRE_EQ(unknown.status, plugins::Vst3BindingStatus::parameter_values_unapplied,
             "unavailable instrument parameter was silently applied");
  REQUIRE_EQ(unknown.unresolved_parameter_ids, std::vector<std::uint32_t>{42},
             "unavailable instrument parameter was silently applied");
  instance.binary_sha256.assign(64, '0');
  auto mismatch = plugins::bind_vst3_project_instrument(instance, receipt, 48000.0, 128);
  REQUIRE_TRUE(!mismatch.ready(), "changed instrument binary passed saved-source binding");
  REQUIRE_EQ(mismatch.status, plugins::Vst3BindingStatus::identity_mismatch,
             "changed instrument binary passed saved-source binding");
}

} // namespace

int main() {
  try {
    const auto bundle = fs::canonical(fs::path(VST3_GAIN_BUNDLE));
    const auto root = bundle.parent_path() / "project_binding_test_runtime";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto log_path = root / "events.log";
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_EVENTS", log_path.c_str(), 1), 0,
               "cannot set fixture event log");
    const auto receipt = scan(bundle);
    fs::remove(log_path); // The scanner ran in a helper process.
    live_restore(receipt, log_path);
    split_controller_restore(scan_connected(fs::canonical(fs::path(VST3_CONNECTED_BUNDLE))),
                             log_path);
    reject_unapplied_values(receipt, log_path);
    failures(receipt, {root, log_path});
    relocated(receipt, root);
    hashed_binding(receipt, {root, log_path});
    instrument_source_binding(scan(fs::canonical(fs::path(VST3_INSTRUMENT_BUNDLE))));
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "project binding test: %s\n", error.what());
    return 1;
  }
}
