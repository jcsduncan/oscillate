#include "oscillate/application/vst3/project_binding.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace oscillate;

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {

using oscillate::test::require;

std::vector<std::string> events(const fs::path& path) {
  std::ifstream input(path);
  std::vector<std::string> result;
  for (std::string line; std::getline(input, line);)
    result.push_back(line);
  return result;
}

std::size_t position(const std::vector<std::string>& log, const char* event) {
  const auto found = std::ranges::find(log, event);
  REQUIRE_NE(found, log.end(), "expected VST3 lifecycle event is absent");
  return static_cast<std::size_t>(std::distance(log.begin(), found));
}

plugins::ScanResult scanned(const fs::path& bundle) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto result = scanner.scan(bundle);
  REQUIRE_TRUE(result.ok(), "connected bundle scan did not return two verified classes");
  REQUIRE_EQ(result.metadata.classes.size(), 2,
             "connected bundle scan did not return two verified classes");
  REQUIRE_EQ(result.binary.sha256.size(), 64,
             "connected bundle scan did not return two verified classes");
  return result;
}

plugins::VerifiedVst3Effect processor(const plugins::ScanResult& receipt) {
  const auto found = std::ranges::find_if(receipt.metadata.classes, [](const auto& klass) {
    return klass.category == "Audio Module Class" && klass.name == "Connected Gain";
  });
  REQUIRE_NE(found, receipt.metadata.classes.end(), "connected processor class is missing");
  return {*found, receipt.binary};
}

void assert_static_project_binding(const plugins::ScanResult& receipt) {
  const auto id = project::PluginInstanceId::from_bits({0, 1});
  REQUIRE_OK(id, "fixture plugin ID failed");
  const auto klass = processor(receipt).identity;
  const project::Vst3PluginInstance saved{.id = id.value(),
                                          .class_id = klass.id,
                                          .vendor = klass.vendor,
                                          .name = klass.name,
                                          .version = klass.version,
                                          .bundle_path = "/advisory/ConnectedGain.vst3",
                                          .binary_sha256 = receipt.binary.sha256,
                                          .component_state = {},
                                          .parameters = {{101, 0.25}},
                                          .missing = true};
  const auto before = saved; // NOLINT(performance-unnecessary-copy-initialization)
  auto binding = plugins::bind_vst3_project_effect(saved, receipt, 48000.0, 128);
  REQUIRE_TRUE(binding.ready(), "synchronous project binding did not restore a known static value");
  REQUIRE_EQ(saved, before, "synchronous project binding did not restore a known static value");
  std::array<float, 64> left{}, right{}, output_left{}, output_right{};
  left.fill(0.25F);
  right.fill(-0.5F);
  REQUIRE_EQ(binding.effect->host().process({left, right, output_left, output_right}),
             plugins::Vst3ProcessStatus::ok,
             "bound static VST3 value was absent from first audible block");
  REQUIRE_LT(std::abs(output_left.front() - 0.25F), 0.00001F,
             "bound static VST3 value was absent from first audible block");
  REQUIRE_LT(std::abs(output_right.back() + 0.5F), 0.00001F,
             "bound static VST3 value was absent from first audible block");
  auto unknown = saved;
  unknown.parameters = {{999, 0.5}};
  auto rejected = plugins::bind_vst3_project_effect(unknown, receipt, 48000.0, 128);
  REQUIRE_TRUE(!rejected.ready(), "unknown static VST3 parameter did not fail closed");
  REQUIRE_EQ(rejected.status, plugins::Vst3BindingStatus::parameter_values_unapplied,
             "unknown static VST3 parameter did not fail closed");
  REQUIRE_EQ(rejected.unresolved_parameter_ids, std::vector<std::uint32_t>{999},
             "unknown static VST3 parameter did not fail closed");
}

void saved_editor_state_reopens(const plugins::ScanResult& receipt) {
  const auto id = project::PluginInstanceId::from_bits({0, 2});
  REQUIRE_OK(id, "fixture plugin ID failed");
  const auto verified = processor(receipt);
  std::string error;
  auto editor = plugins::load_verified_vst3_effect(verified, 48000.0, 128, error);
  REQUIRE_NE(editor, nullptr, "editor fixture did not load");
  const std::array<plugins::Vst3ParameterEvent, 1> edit{{{101, 0, 0.25}}};
  REQUIRE_TRUE(editor->host().stage_static_parameter_values(edit, error),
               "editor fixture did not apply the parameter to its processor");
  REQUIRE_TRUE(editor->host().activate(error),
               "editor fixture did not apply the parameter to its processor");
  REQUIRE_TRUE(editor->host().flush_static_parameter_values(error),
               "editor fixture did not apply the parameter to its processor");
  editor->host().suspend();
  auto state = editor->host().save_component_state(error);
  if (!state || state->size() != sizeof(float))
    throw std::runtime_error("editor fixture did not capture post-edit component state");
  editor.reset();

  const project::Vst3PluginInstance saved{.id = id.value(),
                                          .class_id = verified.identity.id,
                                          .vendor = verified.identity.vendor,
                                          .name = verified.identity.name,
                                          .version = verified.identity.version,
                                          .bundle_path = "/advisory/ConnectedGain.vst3",
                                          .binary_sha256 = receipt.binary.sha256,
                                          .component_state = std::move(state.value()),
                                          .parameters = {{101, 0.25}},
                                          .missing = true};
  const auto original = saved; // NOLINT(performance-unnecessary-copy-initialization)
  auto binding = plugins::bind_vst3_project_effect(saved, receipt, 48000.0, 128);
  REQUIRE_TRUE(binding.ready(), "post-edit state did not bind unchanged with restored latency");
  REQUIRE_EQ(binding.effect->host().latency_samples(), 7,
             "post-edit state did not bind unchanged with restored latency");
  REQUIRE_EQ(saved, original, "post-edit state did not bind unchanged with restored latency");
  std::array<float, 64> input_left{}, input_right{}, output_left{}, output_right{};
  input_left.fill(0.25F);
  input_right.fill(-0.5F);
  REQUIRE_EQ(binding.effect->host().process({input_left, input_right, output_left, output_right}),
             plugins::Vst3ProcessStatus::ok,
             "post-edit component state did not restore the first audible block");
  REQUIRE_LT(std::abs(output_left.front() - 0.25F), 0.00001F,
             "post-edit component state did not restore the first audible block");
  REQUIRE_LT(std::abs(output_right.front() + 0.5F), 0.00001F,
             "post-edit component state did not restore the first audible block");

  // References are recovery metadata when component state exists. A mismatched
  // reference must not silently replace valid opaque plugin state.
  auto conflicting = saved;
  conflicting.parameters = {{101, 0.5}, {999, 0.75}};
  auto restored = plugins::bind_vst3_project_effect(conflicting, receipt, 48000.0, 128);
  REQUIRE_TRUE(restored.ready(), "opaque-state binding lost unresolved reference diagnostics");
  REQUIRE_EQ(restored.unresolved_parameter_ids, std::vector<std::uint32_t>{999},
             "opaque-state binding lost unresolved reference diagnostics");
  REQUIRE_TRUE(!restored.diagnostic.empty(),
               "opaque-state binding lost unresolved reference diagnostics");
  output_left.fill(0.0F);
  output_right.fill(0.0F);
  REQUIRE_EQ(restored.effect->host().process({input_left, input_right, output_left, output_right}),
             plugins::Vst3ProcessStatus::ok,
             "conflicting reference overrode valid state or changed durable intent");
  REQUIRE_LT(std::abs(output_left.front() - 0.25F), 0.00001F,
             "conflicting reference overrode valid state or changed durable intent");
  REQUIRE_EQ(conflicting.parameters,
             (std::vector<project::Vst3ParameterReference>{{101, 0.5}, {999, 0.75}}),
             "conflicting reference overrode valid state or changed durable intent");
}

void first_audible(const plugins::VerifiedVst3Effect& verified, const fs::path& log_path,
                   std::size_t frames) {
  std::string error;
  auto loaded = plugins::load_verified_vst3_effect(verified, 48000.0, 128, error);
  REQUIRE_TRUE(loaded, "verified dynamic bundle did not load its connected parameter");
  REQUIRE_EQ(loaded->host().parameters().size(), 1,
             "verified dynamic bundle did not load its connected parameter");
  REQUIRE_EQ(loaded->host().parameters()[0].id, 101,
             "verified dynamic bundle did not load its connected parameter");
  const std::array<plugins::Vst3ParameterEvent, 1> static_gain{{{101, 0, 0.25}}};
  require(loaded->host().stage_static_parameter_values(static_gain, error),
          "dynamic bundle controller rejected a static value");
  std::array<float, 128> input_left{}, input_right{}, output_left{}, output_right{};
  input_left.fill(0.25F);
  input_right.fill(-0.5F);
  plugins::Vst3StereoBlock block{
      std::span(input_left).first(frames), std::span(input_right).first(frames),
      std::span(output_left).first(frames), std::span(output_right).first(frames)};
  bool active = false;
  bool flushed = false;
  plugins::Vst3ProcessStatus before = plugins::Vst3ProcessStatus::ok;
  plugins::Vst3ProcessStatus after = plugins::Vst3ProcessStatus::inactive;
  std::thread processing_thread([&] {
    active = loaded->host().activate(error);
    if (active) {
      before = loaded->host().process(block);
      flushed = loaded->host().flush_static_parameter_values(error);
    }
    process_allocations = 0;
    counting_process_allocations = true;
    if (flushed)
      after = loaded->host().process(block);
    counting_process_allocations = false;
  });
  processing_thread.join();
  REQUIRE_TRUE(active,
               "first audible block did not receive static gain without callback allocation");
  REQUIRE_EQ(before, plugins::Vst3ProcessStatus::inactive,
             "first audible block did not receive static gain without callback allocation");
  REQUIRE_TRUE(flushed,
               "first audible block did not receive static gain without callback allocation");
  REQUIRE_EQ(after, plugins::Vst3ProcessStatus::ok,
             "first audible block did not receive static gain without callback allocation");
  REQUIRE_EQ(process_allocations, 0,
             "first audible block did not receive static gain without callback allocation");
  REQUIRE_LT(std::abs(output_left[0] - 0.25F), 0.00001F,
             "first audible block did not receive static gain without callback allocation");
  REQUIRE_LT(std::abs(output_right[frames - 1] + 0.5F), 0.00001F,
             "first audible block did not receive static gain without callback allocation");

  loaded->host().suspend();
  auto state = loaded->host().save_component_state(error);
  if (!state || state->size() != sizeof(float))
    throw std::runtime_error("dynamic component state save failed");
  const float half_gain = 0.5F;
  std::memcpy(state->data(), &half_gain, sizeof(half_gain));
  require(loaded->host().restore_component_state(*state, error),
          "dynamic component/controller state restore failed");
  REQUIRE_TRUE(loaded->host().activate(error),
               "restored component state did not determine the next audio block");
  REQUIRE_EQ(loaded->host().process(block), plugins::Vst3ProcessStatus::ok,
             "restored component state did not determine the next audio block");
  REQUIRE_LT(std::abs(output_left[0] - 0.125F), 0.00001F,
             "restored component state did not determine the next audio block");
  REQUIRE_LT(std::abs(output_right[0] + 0.25F), 0.00001F,
             "restored component state did not determine the next audio block");
  loaded.reset();
  const auto log = events(log_path);
  REQUIRE_LT(position(log, "component-initialize"), position(log, "controller-initialize"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "controller-initialize"), position(log, "component-connect"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "component-connect"), position(log, "controller-connect"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "controller-disconnect"), position(log, "component-disconnect"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "component-disconnect"), position(log, "controller-terminate"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "controller-terminate"), position(log, "controller-destroy"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "component-destroy"), position(log, "module-exit"),
             "dynamic VST3 connection, message, or teardown order is wrong");
  REQUIRE_LT(position(log, "controller-notify"), position(log, "component-notify"),
             "dynamic VST3 connection, message, or teardown order is wrong");
}

void rejected_connections(const plugins::VerifiedVst3Effect& verified, const fs::path& log_path) {
  for (const auto* mode : {"component-connect", "controller-connect"}) {
    fs::remove(log_path);
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", mode, 1), 0,
               "cannot set connected-bundle failure mode");
    std::string error;
    auto loaded = plugins::load_verified_vst3_effect(verified, 48000.0, 128, error);
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0,
               "cannot clear connected-bundle failure mode");
    REQUIRE_TRUE(!loaded, "rejected connection returned a live bundle");
    REQUIRE_TRUE(!error.empty(), "rejected connection returned a live bundle");
    const auto log = events(log_path);
    REQUIRE_LT(position(log, "component-disconnect"), position(log, "controller-destroy"),
               "connection rejection leaked a peer or module");
    REQUIRE_LT(position(log, "controller-destroy"), position(log, "module-exit"),
               "connection rejection leaked a peer or module");
    REQUIRE_LT(position(log, "component-destroy"), position(log, "module-exit"),
               "connection rejection leaked a peer or module");
    if (std::strcmp(mode, "controller-connect") == 0)
      REQUIRE_LT(position(log, "controller-disconnect"), position(log, "component-disconnect"),
                 "controller failure cleanup order is wrong");
  }
}

void rejected_static_values(const plugins::VerifiedVst3Effect& verified, const fs::path& log_path) {
  const std::array<plugins::Vst3ParameterEvent, 1> gain{{{101, 0, 0.25}}};
  for (const auto* mode : {"controller-value", "flush"}) {
    fs::remove(log_path);
    std::string error;
    auto loaded = plugins::load_verified_vst3_effect(verified, 48000.0, 128, error);
    REQUIRE_NE(loaded, nullptr, "static failure fixture did not load");
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_FAIL", mode, 1), 0,
               "cannot set static-value failure mode");
    const bool staged = loaded->host().stage_static_parameter_values(gain, error);
    if (std::strcmp(mode, "controller-value") == 0) {
      REQUIRE_TRUE(!staged, "controller refusal left dynamic effect ready");
      REQUIRE_TRUE(!loaded->host().static_parameter_values_ready(),
                   "controller refusal left dynamic effect ready");
    } else {
      bool active = false;
      bool flushed = true;
      std::thread processing_thread([&] {
        active = loaded->host().activate(error);
        if (active)
          flushed = loaded->host().flush_static_parameter_values(error);
      });
      processing_thread.join();
      REQUIRE_TRUE(staged, "processor flush refusal left dynamic effect ready");
      REQUIRE_TRUE(active, "processor flush refusal left dynamic effect ready");
      REQUIRE_TRUE(!flushed, "processor flush refusal left dynamic effect ready");
      REQUIRE_TRUE(!loaded->host().static_parameter_values_ready(),
                   "processor flush refusal left dynamic effect ready");
    }
    REQUIRE_EQ(unsetenv("OSCILLATE_VST3_LOADER_TEST_FAIL"), 0,
               "cannot clear static-value failure mode");
    loaded.reset();
    const auto log = events(log_path);
    REQUIRE_LT(position(log, "controller-disconnect"), position(log, "component-disconnect"),
               "failed static stage leaked the dynamic bundle");
    REQUIRE_LT(position(log, "component-disconnect"), position(log, "module-exit"),
               "failed static stage leaked the dynamic bundle");
  }
}

} // namespace

int main() {
  try {
    const auto bundle = fs::canonical(fs::path(VST3_CONNECTED_BUNDLE));
    const auto root = bundle.parent_path() / "connected_test_runtime";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto log_path = root / "events.log";
    REQUIRE_EQ(setenv("OSCILLATE_VST3_LOADER_TEST_EVENTS", log_path.c_str(), 1), 0,
               "cannot set connected-bundle log");
    const auto receipt = scanned(bundle);
    fs::remove(log_path); // Scanner runs out of process.
    assert_static_project_binding(receipt);
    saved_editor_state_reopens(receipt);
    fs::remove(log_path);
    for (const auto frames : {64UL, 128UL}) {
      fs::remove(log_path);
      first_audible(processor(receipt), log_path, frames);
    }
    rejected_connections(processor(receipt), log_path);
    rejected_static_values(processor(receipt), log_path);
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "connected bundle test: %s\n", error.what());
    return 1;
  }
}
