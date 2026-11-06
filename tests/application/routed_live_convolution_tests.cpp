#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
namespace fs = std::filesystem;
using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }
struct Directory : public oscillate::test::TempDir {
  Directory() : TempDir("oscillate-live-convolution") {}
  const std::filesystem::path root{path()};
};
struct Gate final : project::TransportGate {
  struct Lease final : project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};
void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  u16(bytes, static_cast<std::uint16_t>(value));
  u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}
void write_ir(const fs::path& path, std::uint32_t frames = 2) {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  u32(bytes, 36 + frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(bytes, 16);
  u16(bytes, 1);
  u16(bytes, 2);
  u32(bytes, 48'000);
  u32(bytes, 48'000 * 4);
  u16(bytes, 4);
  u16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  u32(bytes, frames * 4);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto tap =
        frames == 2
            ? (frame == 0 ? 16'384 : 8'192)
            : static_cast<int>(200.0 * std::exp(-5.0 * frame / frames) * std::sin(0.139 * frame));
    u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(tap)));
    u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(tap)));
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  require(out.good(), "IR fixture write failed");
}
project::Snapshot saved_snapshot(const project::SourceRecord& ir) {
  auto tempo = take(timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 1));
  project::Snapshot saved{
      id<project::ProjectId>(1),   1,   48'000, std::move(tempo), std::move(meter),
      {{id<project::TrackId>(2)}}, {ir}};
  project::ManagedAudioReference reference{ir.id,          ir.sha256,   ir.byte_length,
                                           ir.sample_rate, ir.channels, ir.frames};
  saved.master_effects.push_back(
      {id<project::InsertSlotId>(3), true, project::NativeConvolution{reference, 1.0, 1}});
  return saved;
}
engine::RoutedMixerLimits limits(std::uint32_t block_frames) {
  return {{1, block_frames},
          block_frames,
          6ULL * block_frames * sizeof(engine::StereoFrame),
          5ULL * (block_frames + 1) * sizeof(engine::StereoFrame)};
}
engine::RoutedMixerLimits probe_limits(std::uint32_t block_frames) {
  return {{32, block_frames},
          block_frames,
          64ULL * 32 * block_frames * sizeof(engine::StereoFrame),
          64ULL * 32 * (block_frames + 1) * sizeof(engine::StereoFrame)};
}
application::LiveConvolutionAdmission admission(const persistence::ProjectStore& store,
                                                const project::SourceRecord& ir,
                                                std::uint32_t block_frames) {
  return {&store,
          {12ULL * 1024 * 1024, 300, 100, 0, ir.sha256, ir.frames, ir.sample_rate, block_frames}};
}
void near(float actual, float expected, std::string_view message) {
  REQUIRE_LT(std::abs(actual - expected), 0.0001F, message);
}
void audible_admission_and_fail_closed(std::uint32_t block_frames) {
  Directory dir;
  auto store = take(persistence::ProjectStore::open_writer(dir.root / "project", true));
  const auto file = dir.root / "impulse.wav";
  write_ir(file);
  const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "IR fixture open failed");
  auto receipt = take(store.stage_wav(id<project::SourceId>(4), fd));
  REQUIRE_EQ(::close(fd), 0, "IR fixture close failed");
  auto saved = saved_snapshot(receipt.record());
  REQUIRE_OK(store.save(saved, persistence::SaveKind::explicit_save, std::span(&receipt, 1)));
  Gate gate;
  auto state = take(project::ProjectModel::open(saved, gate));
  auto low = admission(store, receipt.record(), block_frames);
  low.budget.available_callback_us = 199;
  application::RoutedPlaybackControl rejected(state, limits(block_frames), nullptr, low);
  REQUIRE_TRUE(!rejected.submit_current(),
               "CPU admission failure did not retain the saved inactive insert");
  REQUIRE_EQ(rejected.status().application, application::PlaybackApplication::failed,
             "CPU admission failure did not retain the saved inactive insert");
  REQUIRE_EQ(rejected.status().active_graph_revision, 0,
             "CPU admission failure did not retain the saved inactive insert");
  REQUIRE_EQ(state.snapshot()->master_effects.size(), 1,
             "CPU admission failure did not retain the saved inactive insert");
  auto low_memory = admission(store, receipt.record(), block_frames);
  low_memory.budget.available_preparation_bytes = 1024;
  application::RoutedPlaybackControl out_of_memory(state, limits(block_frames), nullptr,
                                                   low_memory);
  REQUIRE_TRUE(!out_of_memory.submit_current(),
               "memory admission failure activated an unprepared insert");
  REQUIRE_EQ(out_of_memory.status().application, application::PlaybackApplication::failed,
             "memory admission failure activated an unprepared insert");
  auto wrong_block = admission(store, receipt.record(), block_frames);
  wrong_block.budget.measured_block_frames = block_frames == 64 ? 128 : 64;
  application::RoutedPlaybackControl unmeasured(state, limits(block_frames), nullptr, wrong_block);
  require(!unmeasured.submit_current(), "different block-size measurement was admitted");
  auto wrong_ir = admission(store, receipt.record(), block_frames);
  wrong_ir.budget.measured_ir_frames += 1;
  application::RoutedPlaybackControl unqualified_ir(state, limits(block_frames), nullptr, wrong_ir);
  require(!unqualified_ir.submit_current(), "different IR measurement was admitted");
  application::RoutedPlaybackControl control(state, limits(block_frames), nullptr,
                                             admission(store, receipt.record(), block_frames));
  auto started = control.submit_current();
  REQUIRE_OK(started, started ? "" : started.error().message);
  std::array<engine::StereoFrame, 128> input{};
  std::array<engine::StereoFrame, 128> output{};
  input.front() = {1.0F, 1.0F};
  std::array<std::span<const engine::StereoFrame>, 2> sources{};
  REQUIRE_EQ(control.source_layout().size(), 1, "fixture source layout missing");
  sources[control.source_layout().front().topological_index] = std::span(input).first(block_frames);
  counted_allocations = 0;
  counting_allocations = true;
  const auto initial_result =
      control.render_block(sources, std::span(output).first(block_frames), {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(initial_result, engine::RoutedRenderStatus::rendered,
             "convolution graph did not render without callback allocation");
  REQUIRE_EQ(counted_allocations, 0,
             "convolution graph did not render without callback allocation");
  near(output[0].left, 0.5F, "IR first tap was not audible");
  near(output[1].left, 0.25F, "IR second tap was not audible");
  control.observe();
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::active,
             "admitted convolution revision did not activate");
  require(!control.reset_convolution_after_quiesce(0),
          "zero transport generation was accepted for convolution reset");
  // A planned loop wrap changes the transport generation used by the source
  // bank, while retaining the processor generation and its pending IR tail.
  input.fill({});
  input[block_frames - 1] = {1.0F, 1.0F};
  REQUIRE_EQ(
      control.render_block(sources, std::span(output).first(block_frames), {block_frames, 1, 1}),
      engine::RoutedRenderStatus::rendered, "pre-wrap convolution block failed");
  near(output[block_frames - 1].left, 0.5F, "pre-wrap IR head disappeared");
  input.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  const auto wrapped =
      control.render_block(sources, std::span(output).first(block_frames), {0, 2, 1, true});
  counting_allocations = false;
  REQUIRE_EQ(wrapped, engine::RoutedRenderStatus::rendered,
             "planned wrap did not preserve a callback-safe convolution runtime");
  REQUIRE_EQ(counted_allocations, 0,
             "planned wrap did not preserve a callback-safe convolution runtime");
  near(output.front().left, 0.25F, "IR tail was lost across a planned loop wrap");
  near(output[1].left, 0.0F, "planned loop wrap duplicated the IR tail");
  input.front() = {1.0F, 1.0F};
  REQUIRE_EQ(
      control.render_block(sources, std::span(output).first(block_frames), {block_frames, 3}),
      engine::RoutedRenderStatus::rendered,
      "new transport generation rendered stale convolution history before reset");
  REQUIRE_EQ(output.front().left, 0.0F,
             "new transport generation rendered stale convolution history before reset");
  // This call stands at a joined callback boundary. The same operation used
  // by the Quick device owner advances the transport and discards IR history.
  audio_io::TransportCursor cursor;
  REQUIRE_OK(cursor.seek_after_quiesce(0),
             "seek fixture could not match the active transport generation");
  REQUIRE_OK(cursor.seek_after_quiesce(0),
             "seek fixture could not match the active transport generation");
  REQUIRE_TRUE(
      !control.seek_convolution_after_quiesce(cursor, std::numeric_limits<std::uint64_t>::max()),
      "invalid joined seek changed the cursor");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 0, "invalid joined seek changed the cursor");
  REQUIRE_EQ(cursor.completed_snapshot().generation, 3, "invalid joined seek changed the cursor");
  REQUIRE_OK(control.seek_convolution_after_quiesce(cursor, 2ULL * block_frames),
             "joined seek did not advance cursor and reset convolution");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 2ULL * block_frames,
             "joined seek did not advance cursor and reset convolution");
  REQUIRE_EQ(cursor.completed_snapshot().generation, 4,
             "joined seek did not advance cursor and reset convolution");
  input.fill({});
  REQUIRE_EQ(control.render_block(sources, std::span(output).first(block_frames),
                                  {2ULL * block_frames, 4}),
             engine::RoutedRenderStatus::rendered, "post-seek graph did not render");
  near(output[0].left, 0.0F, "old IR history leaked across seek");
  // Feed one sample and change master gain at the next frame. The pending
  // second tap must survive the callback-safe gain overlay on the same runtime.
  input[0] = {1.0F, 1.0F};
  sources[control.source_layout().front().topological_index] = std::span(input).first(1);
  REQUIRE_EQ(control.render_block(sources, std::span(output).first(1), {3ULL * block_frames, 4}),
             engine::RoutedRenderStatus::rendered, "pre-edit convolution impulse did not render");
  near(output[0].left, 0.5F, "pre-edit IR head disappeared");
  REQUIRE_TRUE(!control.reset_convolution_after_quiesce(4),
               "stale application reset erased a live convolution tail");
  REQUIRE_TRUE(!control.reset_convolution_after_quiesce(3),
               "stale application reset erased a live convolution tail");
  input[0] = {0.0F, 0.0F};
  auto changed = take(state.apply({project::SetMasterLevel{0.5}}));
  REQUIRE_OK(control.begin_edit(changed, "master level"), "master level edit rejected");
  REQUIRE_TRUE(!control.seek_convolution_after_quiesce(cursor, 9ULL * block_frames),
               "in-flight graph edit allowed a seek or changed the cursor");
  REQUIRE_EQ(cursor.completed_snapshot().next_sample, 2ULL * block_frames,
             "in-flight graph edit allowed a seek or changed the cursor");
  REQUIRE_EQ(cursor.completed_snapshot().generation, 4,
             "in-flight graph edit allowed a seek or changed the cursor");
  counted_allocations = 0;
  counting_allocations = true;
  const auto resumed =
      control.render_block(sources, std::span(output).first(1), {3ULL * block_frames + 1, 4});
  counting_allocations = false;
  REQUIRE_EQ(resumed, engine::RoutedRenderStatus::rendered,
             "same IR did not survive allocation-free gain overlay");
  REQUIRE_EQ(counted_allocations, 0, "same IR did not survive allocation-free gain overlay");
  REQUIRE_GE(output[0].left, 0.1249F, "pending IR tap was lost during master gain ramp");
  REQUIRE_LE(output[0].left, 0.2501F, "pending IR tap was lost during master gain ramp");
  control.observe();
  input.fill({});
  sources[control.source_layout().front().topological_index] = std::span(input).first(block_frames);
  REQUIRE_EQ(control.render_block(sources, std::span(output).first(block_frames),
                                  {3ULL * block_frames + 2, 4}),
             engine::RoutedRenderStatus::rendered,
             "master gain ramp did not complete on the active IR graph");
  sources[control.source_layout().front().topological_index] = std::span(input).first(1);
  const auto settled_position = 4ULL * block_frames + 2;
  const auto active_revision = control.status().active_graph_revision;
  auto changed_convolution =
      std::get<project::NativeConvolution>(state.snapshot()->master_effects.front().processor);
  changed_convolution.wet = 0.5;
  const auto changed_ir = take(state.apply({project::SetNativeEffectSettings{
      project::MasterSignalPath{}, state.snapshot()->master_effects.front().id,
      changed_convolution}}));
  // A convolution change is refused at once, with no hold.
  require(!control.begin_edit(changed_ir, "convolution wet") &&
              control.status().application == application::PlaybackApplication::failed &&
              control.status().active_graph_revision == active_revision &&
              state.snapshot()->master_effects.front().processor ==
                  changed_ir.snapshot->master_effects.front().processor,
          "live IR replacement bypassed admission or discarded the saved edit");
  input[0] = {1.0F, 1.0F};
  REQUIRE_EQ(control.render_block(sources, std::span(output).first(1), {settled_position, 4}),
             engine::RoutedRenderStatus::rendered,
             "previous convolution graph did not survive rejected edit");
  near(output[0].left, 0.25F, "rejected convolution edit changed active sound");
  const auto removed = take(state.apply({project::RemoveEffectInsert{
      project::MasterSignalPath{}, state.snapshot()->master_effects.front().id}}));
  REQUIRE_TRUE(!control.begin_edit(removed, "remove convolution"),
               "live convolution removal bypassed admission or discarded the saved edit");
  REQUIRE_EQ(control.status().application, application::PlaybackApplication::failed,
             "live convolution removal bypassed admission or discarded the saved edit");
  REQUIRE_EQ(control.status().active_graph_revision, active_revision,
             "live convolution removal bypassed admission or discarded the saved edit");
  REQUIRE_TRUE(state.snapshot()->master_effects.empty(),
               "live convolution removal bypassed admission or discarded the saved edit");
  input[0] = {1.0F, 1.0F};
  REQUIRE_EQ(control.render_block(sources, std::span(output).first(1), {settled_position + 1, 4}),
             engine::RoutedRenderStatus::rendered,
             "previous convolution graph did not survive rejected removal");
  near(output[0].left, 0.375F, "rejected convolution removal lost the active IR tail");
}

void routed_callback_probe(std::uint32_t block_frames, bool with_convolution) {
  Directory dir;
  auto store = take(persistence::ProjectStore::open_writer(dir.root / "project", true));
  const auto file = dir.root / "dense-ir.wav";
  write_ir(file, 96'000);
  const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "probe IR open failed");
  auto receipt = take(store.stage_wav(id<project::SourceId>(4), fd));
  REQUIRE_EQ(::close(fd), 0, "probe IR close failed");
  auto saved = saved_snapshot(receipt.record());
  if (!with_convolution)
    saved.master_effects.clear();
  for (std::uint64_t track = 1; track < 32; ++track)
    saved.tracks.emplace_back(id<project::TrackId>(track + 4));
  REQUIRE_OK(store.save(saved, persistence::SaveKind::explicit_save, std::span(&receipt, 1)));
  Gate gate;
  auto state = take(project::ProjectModel::open(saved, gate));
  application::RoutedPlaybackControl control(state, probe_limits(block_frames), nullptr,
                                             with_convolution
                                                 ? admission(store, receipt.record(), block_frames)
                                                 : application::LiveConvolutionAdmission{});
  auto submitted = control.submit_current();
  REQUIRE_OK(submitted, submitted ? "" : submitted.error().message);
  REQUIRE_EQ(control.source_layout().size(), 32, "probe did not prepare 32 source tracks");
  std::array<engine::StereoFrame, 128> input{};
  input.fill({0.00002F, -0.00002F});
  std::array<engine::StereoFrame, 128> output{};
  std::vector<std::span<const engine::StereoFrame>> sources(control.topological_order().size());
  for (const auto& source : control.source_layout())
    sources[source.topological_index] = std::span(input).first(block_frames);
  std::vector<double> measured;
  std::vector<double> long_tier;
  std::vector<double> ordinary;
  const auto warm_blocks = 2ULL * 48'000 / block_frames;
  const auto measured_blocks = 3ULL * 48'000 / block_frames;
  measured.reserve(measured_blocks);
  long_tier.reserve(measured_blocks / (1024 / block_frames) + 1);
  ordinary.reserve(measured_blocks);
  std::size_t callback_allocations = 0;
  for (std::uint64_t block = 0; block < warm_blocks + measured_blocks; ++block) {
    const auto cursor = block * block_frames;
    counted_allocations = 0;
    const auto started = std::chrono::steady_clock::now();
    counting_allocations = true;
    const auto rendered =
        control.render_block(sources, std::span(output).first(block_frames), {cursor, 1});
    counting_allocations = false;
    const auto finished = std::chrono::steady_clock::now();
    REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
               "probe routed graph failed to render");
    callback_allocations += counted_allocations;
    if (block < warm_blocks)
      continue;
    const auto us = std::chrono::duration<double, std::micro>(finished - started).count();
    measured.push_back(us);
    if (cursor % 1024 == 0)
      long_tier.push_back(us);
    else
      ordinary.push_back(us);
  }
  std::ranges::sort(measured);
  std::ranges::sort(long_tier);
  std::ranges::sort(ordinary);
  const auto period_us = block_frames * 1'000'000.0 / 48'000.0;
  const auto over_period =
      std::ranges::count_if(measured, [period_us](double us) { return us > period_us; });
  REQUIRE_EQ(callback_allocations, 0, "probe allocated from a routed render callback");
  std::cout << (with_convolution ? "convolution" : "baseline") << ',' << block_frames << ','
            << measured.size() << ',' << measured[measured.size() / 2] << ','
            << measured[measured.size() * 99 / 100] << ',' << measured.back() << ','
            << long_tier[long_tier.size() / 2] << ',' << long_tier.back() << ','
            << ordinary[ordinary.size() / 2] << ',' << ordinary.back() << ',' << over_period << ','
            << callback_allocations << '\n';
}

// Screen a graph prepared for 128 frames when the device supplies alternating
// 64- and 128-frame callbacks. The synthetic IR budget admits preparation only;
// this probe does not qualify either format for physical-device admission.
void routed_variable_callback_probe(bool with_convolution) {
  Directory dir;
  auto store = take(persistence::ProjectStore::open_writer(dir.root / "project", true));
  const auto file = dir.root / "dense-ir.wav";
  write_ir(file, 96'000);
  const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "variable probe IR open failed");
  auto receipt = take(store.stage_wav(id<project::SourceId>(4), fd));
  REQUIRE_EQ(::close(fd), 0, "variable probe IR close failed");
  auto saved = saved_snapshot(receipt.record());
  if (!with_convolution)
    saved.master_effects.clear();
  for (std::uint64_t track = 1; track < 32; ++track)
    saved.tracks.emplace_back(id<project::TrackId>(track + 4));
  REQUIRE_OK(store.save(saved, persistence::SaveKind::explicit_save, std::span(&receipt, 1)));
  Gate gate;
  auto state = take(project::ProjectModel::open(saved, gate));
  application::RoutedPlaybackControl control(state, probe_limits(128), nullptr,
                                             with_convolution
                                                 ? admission(store, receipt.record(), 128)
                                                 : application::LiveConvolutionAdmission{});
  auto submitted = control.submit_current();
  REQUIRE_OK(submitted, submitted ? "" : submitted.error().message);
  REQUIRE_EQ(control.source_layout().size(), 32, "variable probe needs 32 source tracks");
  std::array<engine::StereoFrame, 128> input{};
  input.fill({0.00002F, -0.00002F});
  std::array<engine::StereoFrame, 128> output{};
  std::vector<std::span<const engine::StereoFrame>> sources(control.topological_order().size());
  std::array<std::vector<double>, 2> measured;
  std::array<std::vector<double>, 2> tier;
  std::size_t callback_allocations = 0;
  std::uint64_t cursor = 0;
  std::uint64_t block_index = 0;
  while (cursor < 5ULL * 48'000) {
    const auto frames = block_index % 2 == 0 ? 64U : 128U;
    for (const auto& source : control.source_layout())
      sources[source.topological_index] = std::span(input).first(frames);
    counted_allocations = 0;
    const auto started = std::chrono::steady_clock::now();
    counting_allocations = true;
    const auto rendered =
        control.render_block(sources, std::span(output).first(frames), {cursor, 1});
    counting_allocations = false;
    const auto finished = std::chrono::steady_clock::now();
    REQUIRE_EQ(rendered, engine::RoutedRenderStatus::rendered,
               "variable probe graph failed to render");
    callback_allocations += counted_allocations;
    if (cursor >= 2ULL * 48'000) {
      const auto us = std::chrono::duration<double, std::micro>(finished - started).count();
      const auto index = frames == 64 ? 0U : 1U;
      measured[index].push_back(us);
      if (cursor % 1024 == 0 || cursor / 1024 != (cursor + frames - 1) / 1024)
        tier[index].push_back(us);
    }
    cursor += frames;
    ++block_index;
  }
  REQUIRE_EQ(cursor, 5ULL * 48'000, "variable callback schedule changed probe duration");
  REQUIRE_EQ(callback_allocations, 0, "variable probe allocated in callback");
  for (std::size_t index = 0; index < measured.size(); ++index) {
    auto& times = measured[index];
    auto& long_tier = tier[index];
    REQUIRE_TRUE(!times.empty(), "variable probe missed a callback phase");
    REQUIRE_TRUE(!long_tier.empty(), "variable probe missed a callback phase");
    std::ranges::sort(times);
    std::ranges::sort(long_tier);
    const auto frames = index == 0 ? 64U : 128U;
    const auto period_us = frames * 1'000'000.0 / 48'000.0;
    const auto over_period =
        std::ranges::count_if(times, [period_us](double us) { return us > period_us; });
    std::cout << (with_convolution ? "convolution-variable" : "baseline-variable") << ',' << frames
              << ',' << times.size() << ',' << times[times.size() / 2] << ','
              << times[times.size() * 99 / 100] << ',' << times.back() << ','
              << long_tier[long_tier.size() / 2] << ',' << long_tier.back() << ',' << over_period
              << ',' << callback_allocations << '\n';
  }
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--probe") {
      std::cout << "path,frames,callbacks,median_us,p99_us,worst_us,"
                   "long_tier_median_us,long_tier_worst_us,ordinary_median_us,"
                   "ordinary_worst_us,over_period,callback_allocations\n";
      for (const auto frames : {64U, 128U}) {
        routed_callback_probe(frames, false);
        routed_callback_probe(frames, true);
      }
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--variable-probe") {
      std::cout << "path,frames,callbacks,median_us,p99_us,worst_us,"
                   "long_tier_median_us,long_tier_worst_us,over_period,callback_allocations\n";
      routed_variable_callback_probe(false);
      routed_variable_callback_probe(true);
      return 0;
    }
    REQUIRE_EQ(argc, 1, "usage: routed_live_convolution_tests [--probe|--variable-probe]");
    audible_admission_and_fail_closed(64);
    audible_admission_and_fail_closed(128);
    std::cout << "Live convolution admission passed at 64 and 128 frames\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
