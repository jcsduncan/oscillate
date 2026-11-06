#include "effect_test_support.hpp"

#include "oscillate/engine/graph.hpp"
#include "oscillate/native_inserts/effect_stage.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native effect stage passed",
      {{"press_listens_to_its_key", [] { press_listens_to_its_key(); }}});
}
