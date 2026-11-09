#include "effect_test_support.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/synth_media.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_inserts/effect_stage.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A built-in effect as an insert stage (ADR-0043): its live monitoring.
namespace {
using namespace effect_test;
namespace inserts = oscillate::native_inserts;

using Frames = std::vector<engine::StereoFrame>;

// Live monitoring (never saved): Listen replaces the stage's output with its
// sidechain key, and the key meter holds the key's peak.
void press_listens_to_its_key() {
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  auto project_id = project::ProjectId::from_bits({0, 1});
  REQUIRE_TRUE(tempo, "a test project");
  REQUIRE_TRUE(meter, "a test project");
  REQUIRE_TRUE(project_id, "a test project");
  const project::Snapshot snapshot{project_id.value(),       1, 48'000, std::move(tempo).value(),
                                   std::move(meter).value(), {}};
  inserts::StageContext context;
  context.sample_rate = oscillate::test::sample_rate_48k;
  context.snapshot = &snapshot;
  auto made = inserts::prepare_native_stage(project::NativeEffectPatch{"press"}, context);
  REQUIRE_OK(made, "Press prepares as a stage");
  auto prepared = std::move(made).value();
  auto* stage = dynamic_cast<inserts::NativeEffectStage*>(prepared.runtime.get());
  REQUIRE_NE(stage, nullptr, "Press prepares as a built-in effect stage");
  const auto same = [](const Frames& a, const Frames& b) {
    return std::ranges::equal(a, b, [](engine::StereoFrame x, engine::StereoFrame y) {
      return x.left == y.left && x.right == y.right;
    });
  };
  Frames audio(128, engine::StereoFrame{0.25F, -0.25F});
  Frames key(128);
  for (std::size_t i = 0; i < key.size(); ++i)
    key[i] = {0.5F * std::sin(0.05F * static_cast<float>(i)), 0.4F};
  stage->process_with_sidechain(audio, key, {0, 1});
  REQUIRE_TRUE(!same(audio, key), "the key is metered but not heard");
  REQUIRE_GE(stage->key_peak(), 0.4F, "the key is metered but not heard");
  stage->set_key_listen(true);
  std::ranges::fill(audio, engine::StereoFrame{0.25F, -0.25F});
  stage->process_with_sidechain(audio, key, {128, 1});
  require(same(audio, key), "Listen outputs the key in place of the processed audio");
  stage->set_key_listen(false);
  std::ranges::fill(audio, engine::StereoFrame{0.25F, -0.25F});
  stage->process_with_sidechain(audio, key, {256, 1});
  require(!same(audio, key), "Listen off restores the processed audio");
}

// A structural switch stages a running stage's lanes and tempo for its
// revision (ADR-0098). A switch withdrawn before it took over is published
// again for the same revision, so staging it again must succeed; an older
// revision is still refused.
void a_withdrawn_switch_stages_its_lanes_again() {
  constexpr std::uint64_t first_revision = 1;
  constexpr std::uint64_t switch_revision = 2;
  constexpr std::size_t block_frames = 64;
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  auto project_id = project::ProjectId::from_bits({0, 1});
  REQUIRE_TRUE(tempo && meter && project_id, "a test project");
  const project::Snapshot snapshot{
      project_id.value(), first_revision, oscillate::test::sample_rate_48k,
      tempo.value(),      meter.value(),  {}};
  auto edited = snapshot;
  edited.revision = switch_revision;
  inserts::StageContext context;
  context.sample_rate = oscillate::test::sample_rate_48k;
  context.snapshot = &snapshot;
  auto made = inserts::prepare_native_stage(project::NativeEffectPatch{"press"}, context);
  REQUIRE_OK(made, "Press prepares as a stage");
  auto prepared = std::move(made).value();
  auto* stage = dynamic_cast<inserts::NativeEffectStage*>(prepared.runtime.get());
  REQUIRE_NE(stage, nullptr, "Press prepares as a built-in effect stage");
  // More withdrawals than the mailbox holds plans: each re-stage frees the
  // plan it supersedes.
  constexpr std::size_t withdrawals = 12;
  for (std::size_t attempt = 0; attempt < withdrawals; ++attempt)
    REQUIRE_OK(stage->stage_automation(edited, {}, switch_revision),
               "the switch published again could not stage its lanes again");
  REQUIRE_TRUE(!stage->stage_automation(snapshot, {}, switch_revision),
               "an older revision was staged");
  Frames audio(block_frames);
  stage->graph_activated(switch_revision);
  stage->process(audio, {0, 1});
  REQUIRE_EQ(stage->applied_automation_revision(), switch_revision,
             "the staged lanes did not play from the switching block");
}

// An engine that plays media (Cast, ADR-0115) has its document's managed
// media decoded through the context's loader as its stage is prepared: a
// path without a loader refuses it, a loader's failure fails the stage, and
// decoded media reach the engine.
void a_media_engine_decodes_through_the_loader() {
  constexpr std::string_view media_slot = "ir";
  constexpr std::uint64_t ir_frames = 64;
  constexpr std::uint64_t encoded_bytes = 300;
  constexpr std::size_t sha256_hex_length = 64;
  auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  auto project_id = project::ProjectId::from_bits({0, 1});
  auto source_id = project::SourceId::from_bits({0, 1});
  REQUIRE_TRUE(tempo && meter && project_id && source_id, "a test project");
  const project::Snapshot snapshot{
      project_id.value(), 1, oscillate::test::sample_rate_48k, tempo.value(), meter.value(), {}};
  const auto* engine = oscillate::native_dsp::effects::find_engine("cast");
  REQUIRE_NE(engine, nullptr, "Cast is registered");
  REQUIRE_TRUE(engine->media, "Cast plays media");
  const project::ManagedAudioReference reference{source_id.value(),
                                                 std::string(sha256_hex_length, 'a'),
                                                 encoded_bytes,
                                                 oscillate::test::sample_rate_48k,
                                                 oscillate::test::stereo_channels,
                                                 ir_frames};
  project::NativeEffectPatch patch{"cast"};
  patch.document.media.push_back({std::string(media_slot), reference, 0});

  inserts::StageContext context;
  context.sample_rate = oscillate::test::sample_rate_48k;
  context.snapshot = &snapshot;
  const auto unloaded = inserts::prepare_native_stage(patch, context);
  REQUIRE_TRUE(!unloaded, "a path without a loader refuses managed media");
  REQUIRE_EQ(unloaded.error().code, foundation::ErrorCode::unsupported,
             "a path without a loader refuses managed media");

  std::size_t calls = 0;
  context.media = [&](const project::NativeSynthDocument& document) {
    ++calls;
    REQUIRE_EQ(document, patch.document, "the loader decodes the stage's own document");
    return foundation::Result<engine::SynthMedia>::failure(
        {foundation::ErrorCode::unavailable, "no store"});
  };
  const auto failed = inserts::prepare_native_stage(patch, context);
  REQUIRE_TRUE(!failed, "a loader's failure fails the stage");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::unavailable,
             "a loader's failure fails the stage");
  REQUIRE_EQ(calls, 1U, "the loader runs once per stage");

  context.media = [&](const project::NativeSynthDocument&) {
    auto asset = std::make_shared<engine::SynthMediaAsset>();
    asset->source_rate = oscillate::test::sample_rate_48k;
    asset->frames.assign(ir_frames, engine::StereoFrame{});
    asset->frames.front() = {1.0F, 1.0F};
    return foundation::Result<engine::SynthMedia>::success(
        engine::SynthMedia{{std::string(media_slot), reference.sha256, std::move(asset)}});
  };
  REQUIRE_OK(inserts::prepare_native_stage(patch, context), "decoded media reach the engine");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native effect stage passed",
      {{"press_listens_to_its_key", [] { press_listens_to_its_key(); }},
       {"a_withdrawn_switch_stages_its_lanes_again",
        [] { a_withdrawn_switch_stages_its_lanes_again(); }},
       {"a_media_engine_decodes_through_the_loader",
        [] { a_media_engine_decodes_through_the_loader(); }}});
}
