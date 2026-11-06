#include "oscillate/persistence/store.hpp"
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/plugins/vst3_scan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
namespace daw = oscillate;
using Json = nlohmann::json;

namespace {

template <typename T> T take(daw::foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

struct Workspace {
  explicit Workspace(const fs::path& parent) {
    fs::create_directories(parent);
    auto pattern = (parent / "vst3-compat-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr)
      throw std::runtime_error("could not create probe project");
    path = std::move(pattern);
  }
  ~Workspace() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
  fs::path path;
};

struct Options {
  fs::path bundle;
  std::string class_id;
  daw::plugins::Vst3HostKind kind{daw::plugins::Vst3HostKind::effect};
  fs::path work_parent;
  std::uint32_t frames{128};
  std::uint32_t stress_blocks{0};
};

[[nodiscard]] std::string_view scan_name(daw::plugins::ScanStatus status) {
  using S = daw::plugins::ScanStatus;
  switch (status) {
  case S::ok:
    return "ok";
  case S::invalid_bundle:
    return "invalid_bundle";
  case S::invalid_metadata:
    return "invalid_metadata";
  case S::scanner_error:
    return "scanner_error";
  case S::crashed:
    return "crashed";
  case S::timed_out:
    return "timed_out";
  case S::oversized_response:
    return "oversized_response";
  case S::quarantined:
    return "quarantined";
  }
  return "unknown";
}

[[nodiscard]] std::string_view editor_name(daw::plugins::Vst3EditorProbe status) {
  using E = daw::plugins::Vst3EditorProbe;
  switch (status) {
  case E::x11_candidate:
    return "x11_candidate_only";
  case E::no_controller:
    return "no_controller";
  case E::no_view:
    return "no_view";
  case E::unsupported_platform:
    return "unsupported_platform";
  case E::wayland_bridge_unavailable:
    return "wayland_bridge_unavailable";
  case E::host_unavailable:
    return "host_unavailable";
  case E::plugin_exception:
    return "plugin_exception";
  }
  return "unknown";
}

[[nodiscard]] std::int64_t thread_cpu_nanos() {
  timespec value{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
    throw std::runtime_error("could not measure probe thread CPU time");
  return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL + value.tv_nsec;
}

[[nodiscard]] bool finite_stereo_block(std::span<const float> left,
                                       std::span<const float> right) noexcept {
  const auto finite = [](float sample) { return std::isfinite(sample); };
  return left.size() == right.size() && std::ranges::all_of(left, finite) &&
         std::ranges::all_of(right, finite);
}

[[nodiscard]] Json base_report(const Options& options) {
  return {{"format", 1},
          {"bundle", options.bundle.string()},
          {"class_id", options.class_id},
          {"kind", options.kind == daw::plugins::Vst3HostKind::effect ? "effect" : "instrument"},
          {"sample_rate", 48000},
          {"block_frames", options.frames},
          {"status", "unverified"},
          {"host_status", "unverified"},
          {"scan", "unverified"},
          {"scan_result", "unverified"},
          {"open", "unverified"},
          {"process", "unverified"},
          {"parameter_event", "unverified"},
          {"state_restore", "unverified"},
          {"controller_state_present", nullptr},
          {"controller_state_sync", "unverified"},
          {"project_reopen", "unverified"},
          {"native_gui", "unverified"},
          {"audible_parameter_response", "unverified"},
          {"diagnostic", ""}};
}

[[nodiscard]] daw::project::Snapshot project_with(const daw::project::Vst3PluginInstance& plugin,
                                                  daw::plugins::Vst3HostKind kind) {
  auto tempo =
      take(daw::timeline::TempoMap::create({{{0}, 120.0, daw::timeline::TempoShape::step}}, 1));
  auto meter = take(daw::timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  daw::project::Track track{id<daw::project::TrackId>(1)};
  if (kind == daw::plugins::Vst3HostKind::instrument) {
    track.kind = daw::project::TrackKind::instrument;
    track.instrument_source = plugin;
  } else {
    track.effects.push_back({id<daw::project::InsertSlotId>(1), true, plugin});
  }
  return {id<daw::project::ProjectId>(1),
          1,
          48000,
          std::move(tempo),
          std::move(meter),
          {std::move(track)}};
}

[[nodiscard]] std::optional<daw::project::Vst3PluginInstance>
reopened_plugin(const daw::project::Snapshot& snapshot, daw::plugins::Vst3HostKind kind) {
  if (snapshot.tracks.size() != 1)
    return std::nullopt;
  const auto& track = snapshot.tracks.front();
  if (kind == daw::plugins::Vst3HostKind::instrument) {
    if (const auto* plugin =
            std::get_if<daw::project::Vst3PluginInstance>(&track.instrument_source))
      return *plugin;
  } else if (track.effects.size() == 1) {
    if (const auto* plugin =
            std::get_if<daw::project::Vst3PluginInstance>(&track.effects.front().processor))
      return *plugin;
  }
  return std::nullopt;
}

[[nodiscard]] Json probe(const Options& options) {
  auto report = base_report(options);
  const auto fail = [&](std::string_view stage, const std::string& reason,
                        bool proven_problem = true) {
    report[std::string(stage)] = "failed";
    report["status"] = proven_problem ? "known-problematic" : "unverified";
    report["host_status"] = report["status"];
    report["diagnostic"] = reason;
    return report;
  };
  daw::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  auto receipt = scanner.scan(options.bundle);
  report["scan_result"] = scan_name(receipt.status);
  if (!receipt.ok())
    return fail("scan", receipt.diagnostic,
                receipt.status != daw::plugins::ScanStatus::invalid_bundle);
  report["scan"] = "passed";
  const auto found =
      std::ranges::find(receipt.metadata.classes, options.class_id, &daw::plugins::Vst3Class::id);
  if (found == receipt.metadata.classes.end())
    return fail("open", "requested class ID was not in the verified scan", false);
  if (found->category != "Audio Module Class" ||
      (options.kind == daw::plugins::Vst3HostKind::effect &&
       found->subcategories.find("Instrument") != std::string::npos) ||
      (options.kind == daw::plugins::Vst3HostKind::instrument &&
       found->subcategories.find("Fx") != std::string::npos &&
       found->subcategories.find("Instrument") == std::string::npos))
    return fail("open", "selected class and requested host kind do not match", false);
  report["plugin_name"] = found->name;
  report["plugin_vendor"] = found->vendor;
  report["plugin_version"] = found->version;
  report["binary_sha256"] = receipt.binary.sha256;
  daw::project::Vst3PluginInstance saved{.id = id<daw::project::PluginInstanceId>(1),
                                         .class_id = found->id,
                                         .vendor = found->vendor,
                                         .name = found->name,
                                         .version = found->version,
                                         .bundle_path = receipt.binary.canonical_bundle.string(),
                                         .binary_sha256 = receipt.binary.sha256,
                                         .component_state = {},
                                         .parameters = {},
                                         .missing = false};
  auto bind = [&](const daw::project::Vst3PluginInstance& instance) {
    return options.kind == daw::plugins::Vst3HostKind::instrument
               ? daw::plugins::bind_vst3_project_instrument(instance, receipt, 48000.0, 128)
               : daw::plugins::bind_vst3_project_effect(instance, receipt, 48000.0, 128);
  };
  auto first = bind(saved);
  if (!first.ready())
    return fail("open", first.diagnostic);
  report["open"] = "passed";
  auto& host = first.effect->host();
  const auto buses = host.audio_bus_limits();
  report["silent_auxiliary_inputs"] = buses.silent_auxiliary_inputs;
  report["discarded_secondary_outputs"] = buses.discarded_secondary_outputs;
  const auto editor = host.probe_editor(daw::plugins::Vst3EditorPlatform::x11_xembed);
  report["x11_editor_capability"] = editor_name(editor);
  std::array<float, 128> left{}, right{}, out_left{}, out_right{};
  left.fill(0.25F);
  right.fill(-0.25F);
  const auto peak = [&] {
    float value = 0.0F;
    for (const auto sample : std::span(out_left).first(options.frames))
      value = std::max(value, std::abs(sample));
    for (const auto sample : std::span(out_right).first(options.frames))
      value = std::max(value, std::abs(sample));
    return value;
  };
  const auto finite_output = [&] {
    return finite_stereo_block(std::span(out_left).first(options.frames),
                               std::span(out_right).first(options.frames));
  };
  int process_calls = 0;
  const auto run = [&](daw::plugins::Vst3EffectHost& target,
                       std::span<const daw::plugins::Vst3ParameterEvent> parameters) {
    if (options.kind == daw::plugins::Vst3HostKind::instrument) {
      const std::array notes{
          daw::plugins::Vst3NoteEvent{.velocity = 0.5F, .note_id = ++process_calls}};
      return target.process_instrument(
          {std::span(out_left).first(options.frames), std::span(out_right).first(options.frames)},
          notes, {.project_sample = 0, .generation = 1}, parameters);
    }
    return target.process(
        {std::span(left).first(options.frames), std::span(right).first(options.frames),
         std::span(out_left).first(options.frames), std::span(out_right).first(options.frames)},
        parameters);
  };
  if (run(host, {}) != daw::plugins::Vst3ProcessStatus::ok || !finite_output())
    return fail("process", "first process call failed or returned nonfinite audio");
  report["process"] = "passed";
  report["first_block_peak"] = peak();

  const auto parameter = std::ranges::find_if(host.parameters(), [](const auto& info) {
    return info.automatable && !info.read_only && info.step_count == 0;
  });
  if (parameter != host.parameters().end()) {
    const std::array event{daw::plugins::Vst3ParameterEvent{
        parameter->id, static_cast<std::uint32_t>(options.frames / 2), 0.375}};
    report["parameter_id"] = parameter->id;
    report["parameter_event_offset"] = options.frames / 2;
    if (run(host, event) != daw::plugins::Vst3ProcessStatus::ok || !finite_output())
      return fail("parameter_event",
                  "continuous event failed or produced nonfinite audio at half block");
    report["parameter_event"] = "forwarded";
    report["parameter_event_output_finite"] = true;
    // The host reports queue delivery. Audible interpolation remains unverified.
  }
  std::string stress_failure;
  const auto stress = [&](daw::plugins::Vst3EffectHost& target, std::uint32_t blocks,
                          std::string_view phase) -> std::optional<Json> {
    using Clock = std::chrono::steady_clock;
    std::vector<std::int64_t> wall_nanos;
    std::vector<std::int64_t> cpu_nanos;
    wall_nanos.reserve(blocks);
    cpu_nanos.reserve(blocks);
    float maximum_peak = 0.0F;
    std::uint32_t deadline_exceeded = 0;
    const auto deadline_ns = static_cast<std::int64_t>(options.frames) * 1'000'000'000LL / 48'000;
    for (std::uint32_t block = 0; block < blocks; ++block) {
      out_left.fill(0.0F);
      out_right.fill(0.0F);
      const auto cpu_start = thread_cpu_nanos();
      const auto wall_start = Clock::now();
      const auto status =
          options.kind == daw::plugins::Vst3HostKind::instrument
              ? target.process_instrument(
                    {std::span(out_left).first(options.frames),
                     std::span(out_right).first(options.frames)},
                    {},
                    {.project_sample = static_cast<std::int64_t>(block + 1) * options.frames,
                     .generation = 1})
              : target.process({std::span(left).first(options.frames),
                                std::span(right).first(options.frames),
                                std::span(out_left).first(options.frames),
                                std::span(out_right).first(options.frames)});
      const auto wall_elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall_start).count();
      const auto cpu_elapsed = thread_cpu_nanos() - cpu_start;
      if (status != daw::plugins::Vst3ProcessStatus::ok) {
        stress_failure = std::string(phase) + " block " + std::to_string(block) +
                         " returned process status " + std::to_string(static_cast<int>(status));
        return std::nullopt;
      }
      if (!finite_output()) {
        stress_failure =
            std::string(phase) + " block " + std::to_string(block) + " returned nonfinite audio";
        return std::nullopt;
      }
      maximum_peak = std::max(maximum_peak, peak());
      wall_nanos.push_back(wall_elapsed);
      cpu_nanos.push_back(cpu_elapsed);
      deadline_exceeded += wall_elapsed > deadline_ns ? 1U : 0U;
    }
    std::ranges::sort(wall_nanos);
    std::ranges::sort(cpu_nanos);
    const auto percentile = [](const std::vector<std::int64_t>& sorted, std::size_t numerator,
                               std::size_t denominator) {
      return static_cast<double>(sorted[(sorted.size() - 1) * numerator / denominator]) / 1'000.0;
    };
    Json metrics{{"blocks", blocks},
                 {"audio_seconds", static_cast<double>(blocks) * options.frames / 48'000.0},
                 {"process_wall_us_p50", percentile(wall_nanos, 1, 2)},
                 {"process_wall_us_p99", percentile(wall_nanos, 99, 100)},
                 {"process_wall_us_max", static_cast<double>(wall_nanos.back()) / 1'000.0},
                 {"process_thread_cpu_us_p50", percentile(cpu_nanos, 1, 2)},
                 {"process_thread_cpu_us_p99", percentile(cpu_nanos, 99, 100)},
                 {"process_thread_cpu_us_max", static_cast<double>(cpu_nanos.back()) / 1'000.0},
                 {"nominal_block_us", static_cast<double>(deadline_ns) / 1'000.0},
                 {"process_calls_over_nominal_period", deadline_exceeded},
                 {"maximum_output_peak", maximum_peak},
                 {"process_status", "all_ok"},
                 {"output_finite", true}};
    return std::optional<Json>(std::move(metrics));
  };
  if (options.stress_blocks != 0) {
    report["stress"] = {
        {"scope", "in-process plugin host only; no device or app graph"},
        {"timing", "steady-clock wall and thread CPU around each host process call"},
        {"allocation_measurement", "not measured"}};
    auto measurement = stress(host, options.stress_blocks, "before_save");
    if (!measurement)
      return fail("process", stress_failure);
    report["stress"]["before_save"] = std::move(*measurement);
  }
  host.suspend();
  std::string error;
  auto state = host.save_state_pair(error);
  if (!state)
    return fail("state_restore", error);
  saved.component_state = std::move(state->component_state);
  saved.controller_state = std::move(state->controller_state);
  report["controller_state_present"] = saved.controller_state.has_value();
  first.effect.reset();

  auto reopened = bind(saved);
  if (!reopened.ready())
    return fail("state_restore", reopened.diagnostic);
  report["controller_state_sync"] = reopened.effect->host().controller_state_sync() ==
                                            daw::plugins::Vst3ControllerStateSync::not_implemented
                                        ? "not_implemented"
                                    : reopened.effect->host().controller_state_sync() ==
                                            daw::plugins::Vst3ControllerStateSync::synchronized
                                        ? "synchronized"
                                        : "unverified";
  out_left.fill(0.0F);
  out_right.fill(0.0F);
  if (run(reopened.effect->host(), {}) != daw::plugins::Vst3ProcessStatus::ok || !finite_output())
    return fail("state_restore", "restored host failed or produced nonfinite audio");
  report["state_restore"] = "passed";
  report["restored_block_peak"] = peak();
  if (options.stress_blocks != 0) {
    auto measurement =
        stress(reopened.effect->host(), std::min(1024U, std::max(128U, options.stress_blocks / 10)),
               "after_restore");
    if (!measurement)
      return fail("state_restore", stress_failure);
    report["stress"]["after_restore"] = std::move(*measurement);
  }
  reopened.effect.reset();

  Workspace workspace(options.work_parent);
  {
    auto writer = take(daw::persistence::ProjectStore::open_writer(workspace.path, true));
    const auto snapshot = project_with(saved, options.kind);
    (void)take(writer.save(snapshot, daw::persistence::SaveKind::explicit_save));
  }
  auto reader = take(daw::persistence::ProjectStore::open_reader(workspace.path));
  const auto durable = reopened_plugin(take(reader.load_current()).snapshot, options.kind);
  if (!durable || *durable != saved)
    return fail("project_reopen", "project manifest changed the plugin identity or state");
  auto final = bind(*durable);
  if (!final.ready())
    return fail("project_reopen", final.diagnostic);
  out_left.fill(0.0F);
  out_right.fill(0.0F);
  if (run(final.effect->host(), {}) != daw::plugins::Vst3ProcessStatus::ok || !finite_output())
    return fail("project_reopen", "reopened plugin failed or produced nonfinite audio");
  report["project_reopen"] = "passed";
  report["reopened_block_peak"] = peak();
  if (options.stress_blocks != 0) {
    auto measurement =
        stress(final.effect->host(), std::min(1024U, std::max(128U, options.stress_blocks / 10)),
               "after_project_reopen");
    if (!measurement)
      return fail("project_reopen", stress_failure);
    report["stress"]["after_project_reopen"] = std::move(*measurement);
  }
  report["host_status"] = "tested";
  report["diagnostic"] =
      "Host and project checks passed; GUI and audible parameter response require manual review";
  return report;
}

[[nodiscard]] bool self_test(const fs::path& work_parent) {
  const std::array<float, 2> finite{0.0F, 0.25F};
  const std::array<float, 2> nan{0.0F, std::numeric_limits<float>::quiet_NaN()};
  const std::array<float, 2> infinity{0.0F, std::numeric_limits<float>::infinity()};
  if (!finite_stereo_block(finite, finite) || finite_stereo_block(finite, nan) ||
      finite_stereo_block(infinity, finite) ||
      finite_stereo_block(std::span(finite).first(1), finite))
    return false;
  daw::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  for (const auto frames : {64U, 128U}) {
    for (const auto& [bundle, kind] :
         {std::pair{fs::path(VST3_CONNECTED_BUNDLE), daw::plugins::Vst3HostKind::effect},
          std::pair{fs::path(VST3_INSTRUMENT_BUNDLE), daw::plugins::Vst3HostKind::instrument}}) {
      const auto receipt = scanner.scan(bundle);
      if (!receipt.ok() || receipt.metadata.classes.empty()) {
        std::cerr << "fixture scan failed: " << bundle << ": " << receipt.diagnostic << '\n';
        return false;
      }
      const Options options{bundle, receipt.metadata.classes.front().id, kind, work_parent, frames};
      const auto result = probe(options);
      if (result["status"] != "unverified" || result["host_status"] != "tested" ||
          result["block_frames"] != frames || result["sample_rate"] != 48000 ||
          result["open"] != "passed" || result["process"] != "passed" ||
          result["state_restore"] != "passed" || result["project_reopen"] != "passed" ||
          result["controller_state_present"] != true ||
          (result["parameter_event"] == "forwarded" &&
           result.value("parameter_event_output_finite", false) != true) ||
          result["native_gui"] != "unverified" || !result.contains("silent_auxiliary_inputs") ||
          !result.contains("discarded_secondary_outputs")) {
        std::cerr << result.dump(2) << '\n';
        return false;
      }
      auto bad_class = options;
      bad_class.class_id = std::string(32, 'f');
      const auto rejected = probe(bad_class);
      if (rejected["status"] != "unverified" || rejected["open"] != "failed" ||
          rejected["project_reopen"] != "unverified") {
        std::cerr << rejected.dump(2) << '\n';
        return false;
      }
      auto wrong_kind = options;
      wrong_kind.kind = kind == daw::plugins::Vst3HostKind::instrument
                            ? daw::plugins::Vst3HostKind::effect
                            : daw::plugins::Vst3HostKind::instrument;
      const auto wrong = probe(wrong_kind);
      if (wrong["status"] != "unverified" || wrong["open"] != "failed") {
        std::cerr << wrong.dump(2) << '\n';
        return false;
      }
    }
  }
  const Options absent{"/nonexistent/NoPlugin.vst3", std::string(32, '0'),
                       daw::plugins::Vst3HostKind::effect, work_parent};
  const auto missing = probe(absent);
  if (missing["status"] != "unverified" || missing["scan"] != "failed" ||
      missing["open"] != "unverified") {
    std::cerr << missing.dump(2) << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] bool stress_self_test(const fs::path& work_parent) {
  daw::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  for (const auto frames : {64U, 128U}) {
    for (const auto& [bundle, kind] :
         {std::pair{fs::path(VST3_CONNECTED_BUNDLE), daw::plugins::Vst3HostKind::effect},
          std::pair{fs::path(VST3_INSTRUMENT_BUNDLE), daw::plugins::Vst3HostKind::instrument}}) {
      const auto receipt = scanner.scan(bundle);
      if (!receipt.ok() || receipt.metadata.classes.empty())
        return false;
      const Options options{bundle, receipt.metadata.classes.front().id, kind, work_parent, frames,
                            256};
      const auto report = probe(options);
      if (report["host_status"] != "tested" || !report.contains("stress") ||
          report["stress"]["before_save"]["blocks"] != 256 ||
          report["stress"]["after_restore"]["blocks"] != 128 ||
          report["stress"]["after_project_reopen"]["blocks"] != 128 ||
          report["stress"]["before_save"]["output_finite"] != true ||
          report["stress"]["after_project_reopen"]["process_status"] != "all_ok") {
        std::cerr << report.dump(2) << '\n';
        return false;
      }
    }
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const fs::path work_parent = fs::temp_directory_path();
    if (argc == 2 && std::string_view(argv[1]) == "--self-test")
      return self_test(work_parent) ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "--self-test-stress")
      return stress_self_test(work_parent) ? 0 : 1;
    if (argc == 3 && std::string_view(argv[1]) == "--list") {
      daw::plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
      const auto receipt = scanner.scan(argv[2]);
      Json listing{{"bundle", argv[2]},
                   {"scan_result", scan_name(receipt.status)},
                   {"diagnostic", receipt.diagnostic},
                   {"classes", Json::array()}};
      if (receipt.ok()) {
        listing["binary_sha256"] = receipt.binary.sha256;
        for (const auto& klass : receipt.metadata.classes)
          listing["classes"].push_back({{"id", klass.id},
                                        {"name", klass.name},
                                        {"category", klass.category},
                                        {"subcategories", klass.subcategories}});
      }
      std::cout << listing.dump(2) << '\n';
      return receipt.ok() ? 0 : 1;
    }
    const bool stress_run = argc == 8 && std::string_view(argv[1]) == "--stress";
    const auto kind_index = stress_run ? 4 : 3;
    if ((!stress_run && argc != 5 && argc != 6) ||
        (std::string_view(argv[kind_index]) != "effect" &&
         std::string_view(argv[kind_index]) != "instrument")) {
      std::cerr << "Usage: oscillate_vst3_compatibility_probe --list BUNDLE\n"
                   "       oscillate_vst3_compatibility_probe BUNDLE CLASS_ID effect|instrument "
                   "WORK_DIR [64|128]\n"
                   "       oscillate_vst3_compatibility_probe --stress BUNDLE CLASS_ID "
                   "effect|instrument WORK_DIR 64|128 BLOCKS(256..60000)\n";
      return 2;
    }
    const auto frames = stress_run  ? std::string_view(argv[6])
                        : argc == 6 ? std::string_view(argv[5])
                                    : std::string_view("128");
    if (frames != "64" && frames != "128") {
      std::cerr << "block size must be 64 or 128 frames\n";
      return 2;
    }
    std::uint32_t stress_blocks = 0;
    if (stress_run) {
      const auto parsed = std::string_view(argv[7]);
      const auto digits = !parsed.empty() && std::ranges::all_of(parsed, [](char ch) {
        return ch >= '0' && ch <= '9';
      });
      if (!digits || parsed.size() > 5) {
        std::cerr << "stress blocks must be 256..60000\n";
        return 2;
      }
      stress_blocks = static_cast<std::uint32_t>(std::stoul(std::string(parsed)));
      if (stress_blocks < 256 || stress_blocks > 60'000) {
        std::cerr << "stress blocks must be 256..60000\n";
        return 2;
      }
    }
    const Options options{argv[stress_run ? 2 : 1],
                          argv[stress_run ? 3 : 2],
                          std::string_view(argv[kind_index]) == "effect"
                              ? daw::plugins::Vst3HostKind::effect
                              : daw::plugins::Vst3HostKind::instrument,
                          argv[stress_run ? 5 : 4],
                          frames == "64" ? 64U : 128U,
                          stress_blocks};
    const auto report = probe(options);
    std::cout << report.dump(2) << '\n';
    return report["status"] == "known-problematic" ? 1 : 0;
  } catch (const std::exception& ex) {
    std::cerr << "probe failed: " << ex.what() << '\n';
    return 2;
  }
}
