#include "oscillate/application/project_session.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/manual_effect_edit.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace engine = oscillate::engine;
namespace foundation = oscillate::foundation;
namespace plugins = oscillate::plugins;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::string_view class_id = "abcdef019182faeb566d624153675854";
constexpr std::string_view required_hash =
    "b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec";
constexpr std::uint32_t cutoff_id = 1945359057;
constexpr std::uint32_t resonance_id = 8095466;
} // namespace

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {

using oscillate::test::require;

using oscillate::test::take;

template <typename Id> Id id(std::uint64_t number) { return take(Id::from_bits({0, number})); }

struct Options {
  fs::path bundle;
  fs::path scratch;
  std::uint32_t frames = 64;
  std::uint32_t blocks = 512;
  std::uint32_t parameter_id = cutoff_id;
  double value = 0.05;
  bool perform_edit = true;
};

std::uint32_t positive(std::string_view text) {
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    throw std::runtime_error("expected a positive integer");
  return value;
}

Options parse(int argc, char** argv) {
  if (argc != 5 && argc != 6)
    throw std::runtime_error(
        "usage: oscillate_surge_live_graph BUNDLE SCRATCH 64|128 BLOCKS [cutoff|resonance|steady]");
  Options options{argv[1], argv[2], positive(argv[3]), positive(argv[4])};
  if (argc == 6) {
    if (std::string_view(argv[5]) == "resonance") {
      options.parameter_id = resonance_id;
      options.value = 0.95;
    } else if (std::string_view(argv[5]) == "steady") {
      options.perform_edit = false;
    } else {
      throw std::runtime_error("only the scoped resonance or steady surveys are available");
    }
  }
  if ((options.frames != 64 && options.frames != 128) || options.blocks < 32 ||
      options.blocks > 4096)
    throw std::runtime_error("frames must be 64 or 128 and blocks must be 32..4096");
  if (!fs::is_directory(options.scratch))
    throw std::runtime_error("scratch directory must exist inside the selected build tree");
  return options;
}

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

class Adoption final : public app::Vst3ManualLiveAdoption {
public:
  Adoption(project::ProjectModel& mirror, app::RoutedPlaybackControl& playback)
      : mirror_(mirror), playback_(playback) {}
  foundation::Result<void> preflight(project::PluginInstanceId plugin,
                                     plugins::Vst3ActiveGraphIdentity identity) override {
    playback_.observe();
    return playback_.preflight_vst3_manual_edit(plugin, identity);
  }
  foundation::Result<void> begin(const project::Snapshot& saved, project::PluginInstanceId plugin,
                                 plugins::Vst3ActiveGraphIdentity identity) override {
    auto adopted = mirror_.adopt_authoritative_snapshot(saved);
    if (!adopted)
      return foundation::Result<void>::failure(adopted.error());
    return playback_.begin_vst3_manual_edit(adopted.value(), "Surge cutoff", plugin, identity);
  }
  foundation::Result<void> settle(plugins::Vst3ManualEventReceipt receipt) override {
    return playback_.settle_vst3_manual_edit(receipt);
  }

private:
  project::ProjectModel& mirror_;
  app::RoutedPlaybackControl& playback_;
};

plugins::ScanResult scan_surge(const fs::path& bundle) {
  plugins::Vst3Scanner scanner(VST3_QUALIFICATION_HELPER);
  auto scan = scanner.scan(bundle);
  require(scan.ok(), "Surge XT out-of-process scan failed");
  REQUIRE_EQ(scan.binary.sha256, required_hash, "installed Surge XT binary hash changed");
  const auto klass = std::ranges::find(scan.metadata.classes, class_id, &plugins::Vst3Class::id);
  REQUIRE_NE(klass, scan.metadata.classes.end(), "Surge XT instrument class is absent");
  REQUIRE_EQ(klass->category, "Audio Module Class", "Surge XT instrument class is absent");
  return scan;
}

// One fresh host outside the project graph distinguishes a VST3 processing
// allocation from an allocation introduced by the routed mixer or publication.
std::size_t measure_host_allocations(const plugins::ScanResult& scanned,
                                     const plugins::Vst3Class& klass, const Options& options,
                                     bool send_parameter) {
  std::string error;
  auto loaded =
      plugins::load_verified_vst3_instrument({klass, scanned.binary}, 48'000.0, 128, error);
  REQUIRE_NE(loaded, nullptr, "isolated Surge host load failed");
  require(loaded->host().activate(error), "isolated Surge host activation failed");
  const auto initial_latency = loaded->host().latency_samples();
  std::uint32_t restart_flags = loaded->host().take_restart_requests();
  std::array<float, 128> left{}, right{};
  std::array<plugins::Vst3NoteEvent, 2> notes{};
  const std::array parameter{plugins::Vst3ParameterEvent{options.parameter_id, 0, options.value}};
  counted_allocations = 0;
  std::vector<std::pair<std::uint32_t, std::size_t>> events;
  for (std::uint32_t block = 0; block < options.blocks; ++block) {
    const auto first_sample = static_cast<std::uint64_t>(block) * options.frames;
    const auto onset = ((first_sample + 5'999) / 6'000) * 6'000;
    std::size_t note_count = 0;
    if (onset < first_sample + options.frames && onset < 96'000) {
      const auto ordinal = static_cast<std::int32_t>(onset / 6'000);
      const auto offset = static_cast<std::uint32_t>(onset - first_sample);
      if (ordinal != 0)
        notes[note_count++] = {.kind = plugins::Vst3NoteKind::off,
                               .sample_offset = offset,
                               .pitch = 60,
                               .note_id = ordinal};
      notes[note_count++] = {.kind = plugins::Vst3NoteKind::on,
                             .sample_offset = offset,
                             .pitch = 60,
                             .velocity = 0.8F,
                             .note_id = ordinal + 1};
    }
    const auto before = counted_allocations;
    counting_allocations = true;
    const auto status = loaded->host().process_instrument(
        {std::span(left).first(options.frames), std::span(right).first(options.frames)},
        std::span<const plugins::Vst3NoteEvent>(notes.data(), note_count),
        {.project_sample = static_cast<std::int64_t>(block) * options.frames, .generation = 1},
        send_parameter && block == options.blocks / 4
            ? std::span<const plugins::Vst3ParameterEvent>(parameter)
            : std::span<const plugins::Vst3ParameterEvent>{});
    counting_allocations = false;
    REQUIRE_EQ(status, plugins::Vst3ProcessStatus::ok, "isolated Surge host process failed");
    restart_flags |= loaded->host().take_restart_requests();
    if (counted_allocations != before)
      events.emplace_back(block, counted_allocations - before);
  }
  std::cout << "isolated_host callback_allocations=" << counted_allocations
            << " class_id=" << klass.id << " manual_event=" << (send_parameter ? "on" : "off")
            << " allocating_blocks=" << events.size() << " initial_latency=" << initial_latency
            << " cached_latency_after_events=" << loaded->host().latency_samples()
            << " active_restart_flags=" << restart_flags << '\n';
  for (const auto [block, count] : events)
    std::cout << "isolated_allocation_event block=" << block << " count=" << count << '\n';
  loaded->host().suspend();
  auto state = loaded->host().save_component_state(error);
  REQUIRE_OK(state, "isolated Surge state capture failed");
  loaded.reset();
  auto restored =
      plugins::load_verified_vst3_instrument({klass, scanned.binary}, 48'000.0, 128, error);
  REQUIRE_NE(restored, nullptr, "isolated Surge fresh-state restore failed");
  REQUIRE_TRUE(restored->host().restore_component_state(*state, error),
               "isolated Surge fresh-state restore failed");
  std::cout << "isolated_reopened_latency=" << restored->host().latency_samples() << '\n';
  const auto total_allocations = counted_allocations;
  counted_allocations = 0;
  return total_allocations;
}

app::Vst3InsertIdentity identity_from(const plugins::ScanResult& scan) {
  const auto klass = std::ranges::find(scan.metadata.classes, class_id, &plugins::Vst3Class::id);
  return {.class_id = klass->id,
          .vendor = klass->vendor,
          .name = klass->name,
          .version = klass->version,
          .canonical_bundle_path = scan.binary.canonical_bundle.string(),
          .binary_sha256 = scan.binary.sha256};
}

const project::Vst3PluginInstance& saved_instrument(const project::Snapshot& snapshot,
                                                    project::TrackId track) {
  const auto found = std::ranges::find(snapshot.tracks, track, &project::Track::id);
  REQUIRE_NE(found, snapshot.tracks.end(), "saved instrument source is absent");
  REQUIRE_TRUE(std::holds_alternative<project::Vst3PluginInstance>(found->instrument_source),
               "saved instrument source is absent");
  return std::get<project::Vst3PluginInstance>(found->instrument_source);
}

struct RunEvidence {
  std::size_t nonzero_blocks = 0;
  std::size_t nonfinite = 0;
  std::size_t faults = 0;
  std::size_t callback_over_period = 0;
  std::size_t allocating_blocks = 0;
  std::size_t max_allocations_in_block = 0;
  std::vector<std::pair<std::uint32_t, std::size_t>> allocation_events;
  double max_callback_us = 0.0;
  std::uint64_t final_revision = 0;
};

RunEvidence run(const Options& options) {
  const auto scanned = scan_surge(options.bundle);
  const auto klass = std::ranges::find(scanned.metadata.classes, class_id, &plugins::Vst3Class::id);
  std::string parameter_error;
  auto metadata_host = plugins::load_verified_vst3_instrument({*klass, scanned.binary}, 48'000.0,
                                                              128, parameter_error);
  REQUIRE_NE(metadata_host, nullptr, "Surge parameter metadata host failed");
  const auto parameter = std::ranges::find(metadata_host->host().parameters(), options.parameter_id,
                                           &plugins::Vst3ParameterInfo::id);
  REQUIRE_NE(parameter, metadata_host->host().parameters().end(),
             "selected Surge parameter is not continuous and writable");
  REQUIRE_TRUE(parameter->automatable, "selected Surge parameter is not continuous and writable");
  REQUIRE_TRUE(!parameter->read_only, "selected Surge parameter is not continuous and writable");
  REQUIRE_EQ(parameter->step_count, 0, "selected Surge parameter is not continuous and writable");
  metadata_host.reset();
  (void)measure_host_allocations(scanned, *klass, options, options.perform_edit);
  const auto root = options.scratch / ("surge-live-graph-" + std::to_string(::getpid()) + "-" +
                                       std::to_string(options.frames));
  fs::remove_all(root);
  auto session = take(app::ProjectSession::create(root));
  const auto track =
      take(session->instruments().create_vst3_instrument_track(identity_from(scanned)));
  const auto group = take(session->tracks().add_bus_track(project::TrackKind::group_bus));
  const auto returns = take(session->tracks().add_bus_track(project::TrackKind::return_bus));
  take(session->routing().set_main_output(track, group));
  (void)take(session->routing().add_send(track, returns, 0.25));
  (void)take(session->effects().add_effect_engine(project::MasterSignalPath{}, 0, "shape"));
  take(session->midi_clips().add_midi_clip(track, {0}));
  const auto clip_id = std::ranges::find(session->snapshot().tracks, track, &project::Track::id)
                           ->midi_clips.front()
                           .id;
  for (std::int64_t index = 0; index < 16; ++index)
    take(session->notes().add_note(clip_id, {index * (timeline::ticks_per_quarter / 4)}, 60));
  take(session->saving().save());
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto plugin = saved_instrument(session->snapshot(), track).id;
  REQUIRE_EQ(saved_instrument(session->snapshot(), track).binary_sha256, required_hash,
             "reopened instrument identity changed");

  Gate gate;
  auto mirror = take(project::ProjectModel::open(session->snapshot(), gate));
  plugins::Vst3InstrumentTrackProvider provider(plugins::Vst3ProcessingMode::realtime, true,
                                                {options.parameter_id});
  provider.set_scan_receipt(plugin, scanned);
  app::RoutedPlaybackControl playback(mirror, {{4'096, 4'096}, 128, 1ULL << 20, 16ULL << 20},
                                      &provider);
  take(playback.submit_current());
  Adoption adoption(mirror, playback);
  std::vector<std::span<const engine::StereoFrame>> sources(playback.topological_order().size());
  std::array<engine::StereoFrame, 128> output{};
  const auto period_us = static_cast<double>(options.frames) * 1'000'000.0 / 48'000.0;
  RunEvidence evidence;
  const auto edit_block = std::max<std::uint32_t>(16, options.blocks / 4);
  const auto initial_revision = session->snapshot().revision;
  std::unique_ptr<app::Vst3ManualInstrumentEdit> edit;
  for (std::uint32_t block = 0; block < options.blocks; ++block) {
    if (options.perform_edit && block == edit_block) {
      playback.observe();
      const auto binding = playback.active_vst3_instrument_manual_events(
          track, plugin, playback.active_topology_revision());
      REQUIRE_OK(binding, "qualified active Surge slot is unavailable");
      edit = std::make_unique<app::Vst3ManualInstrumentEdit>(*session, binding->slot, &adoption);
      const auto original = saved_instrument(session->snapshot(), track);
      auto references = original.parameters;
      std::erase_if(references,
                    [&](const auto& reference) { return reference.id == options.parameter_id; });
      references.push_back({options.parameter_id, options.value});
      std::ranges::sort(references, {}, &project::Vst3ParameterReference::id);
      const auto committed = take(edit->commit(
          session->snapshot().revision, track, original, original.component_state,
          std::move(references), {options.parameter_id, 0, options.value}, binding->identity));
      REQUIRE_EQ(committed.phase, app::Vst3ManualEditPhase::pending,
                 "Surge cutoff edit did not save as pending");
      REQUIRE_EQ(playback.status().active_graph_revision, initial_revision,
                 "saved edit claimed to be audible before the callback receipt");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto allocations_before = counted_allocations;
    counting_allocations = true;
    const auto status =
        playback.render_block(sources, std::span(output).first(options.frames),
                              {static_cast<std::uint64_t>(block) * options.frames, 1});
    counting_allocations = false;
    const auto block_allocations = counted_allocations - allocations_before;
    evidence.allocating_blocks += block_allocations != 0 ? 1U : 0U;
    evidence.max_allocations_in_block =
        std::max(evidence.max_allocations_in_block, block_allocations);
    if (block_allocations != 0)
      evidence.allocation_events.emplace_back(block, block_allocations);
    const auto duration =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    evidence.max_callback_us = std::max(evidence.max_callback_us, duration);
    evidence.callback_over_period += duration > period_us ? 1U : 0U;
    REQUIRE_EQ(status, engine::RoutedRenderStatus::rendered, "Surge routed graph did not render");
    bool nonzero = false;
    for (const auto frame : std::span(output).first(options.frames)) {
      evidence.nonfinite += !std::isfinite(frame.left) || !std::isfinite(frame.right);
      nonzero |= std::abs(frame.left) > 0.00001F || std::abs(frame.right) > 0.00001F;
    }
    evidence.nonzero_blocks += nonzero ? 1U : 0U;
    if (playback.processor_fault())
      ++evidence.faults;
    if (edit && block == edit_block) {
      REQUIRE_EQ(edit->observe().phase, app::Vst3ManualEditPhase::applied,
                 "Surge cutoff edit received no applied callback receipt");
      playback.observe();
      REQUIRE_EQ(playback.status().active_graph_revision, session->snapshot().revision,
                 "applied Surge edit did not advance audible revision");
    }
  }
  playback.observe();
  evidence.final_revision = playback.status().active_graph_revision;
  require(provider.stop_after_join(), "Surge instrument did not stop cleanly");
  take(session->saving().save());
  session.reset();
  session = take(app::ProjectSession::open(root));
  const auto& reopened = saved_instrument(session->snapshot(), track);
  const auto cutoff = std::ranges::find(reopened.parameters, options.parameter_id,
                                        &project::Vst3ParameterReference::id);
  REQUIRE_TRUE((!options.perform_edit ||
                (cutoff != reopened.parameters.end() && cutoff->normalized_value == options.value)),
               "live Surge project did not survive save/reopen");
  REQUIRE_EQ(reopened.binary_sha256, required_hash,
             "live Surge project did not survive save/reopen");
  REQUIRE_EQ(session->snapshot().revision, evidence.final_revision,
             "live Surge project did not survive save/reopen");
  std::cout << "observed mode=" << (options.perform_edit ? "edit" : "steady")
            << " nonzero_blocks=" << evidence.nonzero_blocks << " nonfinite=" << evidence.nonfinite
            << " faults=" << evidence.faults << " callback_allocations=" << counted_allocations
            << " allocating_blocks=" << evidence.allocating_blocks
            << " max_allocations_in_block=" << evidence.max_allocations_in_block
            << " callback_over_period=" << evidence.callback_over_period
            << " max_callback_us=" << evidence.max_callback_us << '\n';
  for (const auto [block, count] : evidence.allocation_events)
    std::cout << "allocation_event block=" << block << " count=" << count << '\n';
  std::cout << std::flush;
  REQUIRE_GT(evidence.nonzero_blocks, options.blocks / 4,
             "whole-graph audio or processor-fault invariant failed");
  REQUIRE_EQ(evidence.nonfinite, 0, "whole-graph audio or processor-fault invariant failed");
  REQUIRE_EQ(evidence.faults, 0, "whole-graph audio or processor-fault invariant failed");
  REQUIRE_EQ(counted_allocations, 0, "Surge callback allocation gate failed");
  return evidence;
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 6 && std::string_view(argv[1]) == "--host-only") {
      Options options;
      options.frames = positive(argv[4]);
      options.blocks = positive(argv[5]);
      REQUIRE_TRUE((options.frames == 64 || options.frames == 128),
                   "host-only probe needs 64/128 frames and at most 4096 blocks");
      REQUIRE_LE(options.blocks, 4096,
                 "host-only probe needs 64/128 frames and at most 4096 blocks");
      plugins::Vst3Scanner scanner(VST3_QUALIFICATION_HELPER);
      const auto scanned = scanner.scan(argv[2]);
      require(scanned.ok(), "synthetic instrument scan failed");
      const auto klass = std::ranges::find(scanned.metadata.classes, std::string_view(argv[3]),
                                           &plugins::Vst3Class::id);
      REQUIRE_NE(klass, scanned.metadata.classes.end(), "synthetic instrument class is absent");
      REQUIRE_EQ(klass->category, "Audio Module Class", "synthetic instrument class is absent");
      const auto allocations = measure_host_allocations(scanned, *klass, options, false);
      REQUIRE_EQ(allocations, 0, "synthetic host callback allocated");
      return 0;
    }
    const auto options = parse(argc, argv);
    const auto evidence = run(options);
    std::cout << "surge_live_graph frames=" << options.frames << " blocks=" << options.blocks
              << " mode=" << (options.perform_edit ? "edit" : "steady")
              << " parameter_id=" << options.parameter_id << " plugin_sha256=" << required_hash
              << " nonzero_blocks=" << evidence.nonzero_blocks
              << " nonfinite=" << evidence.nonfinite << " faults=" << evidence.faults
              << " callback_allocations=" << counted_allocations
              << " callback_over_period=" << evidence.callback_over_period
              << " max_callback_us=" << evidence.max_callback_us
              << " reopened_revision=" << evidence.final_revision << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Surge live graph: " << error.what() << '\n';
    return 1;
  }
}
