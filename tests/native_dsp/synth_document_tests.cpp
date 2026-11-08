#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "synth_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Structured synth state (ADR-0057): a probe engine whose output level is its
// compiled document, so the tests can hear exactly when a document arrives.
namespace {
using namespace synth_test;

constexpr std::array<ParameterDescriptor, 1> probe_parameters{{
    {"probe.gain", "Gain", "Probe", "", ParameterScale::linear, 0.0, 1.0, 1.0},
}};
constexpr EngineDescriptor probe_descriptor{"probe", "Probe", 1, probe_parameters, {}};

// Level = a quarter per route, plus the first frame of a "table" media slot.
struct ProbeState final : DocumentState {
  float level = 0.0F;
};

class ProbeKernel final : public SynthKernel {
public:
  void control(std::span<const double> values, std::uint32_t, const Transport&) noexcept override {
    gain_ = static_cast<float>(values[0]);
  }
  void note_on(const NoteEvent&) noexcept override {}
  void note_off(const NoteEvent&) noexcept override {}
  void render(std::span<engine::StereoFrame> output) noexcept override {
    const float level = state_ ? state_->level * gain_ : 0.0F;
    for (auto& frame : output) {
      frame.left += level;
      frame.right += level;
    }
  }
  void reset() noexcept override {}
  [[nodiscard]] KernelStatus status() const noexcept override { return {}; }
  void adopt_document(const DocumentState& state) noexcept override {
    state_ = static_cast<const ProbeState*>(&state);
  }

private:
  const ProbeState* state_ = nullptr;
  float gain_ = 1.0F;
};

foundation::Result<void> validate_probe(const project::NativeSynthDocument& document) {
  if (document.routes.size() > 4 ||
      !std::ranges::all_of(document.media, [](const auto& slot) { return slot.id == "table"; }))
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "probe rejects this document"});
  return foundation::Result<void>::success();
}

foundation::Result<std::shared_ptr<const DocumentState>>
prepare_probe(const project::NativeSynthDocument& document, const SynthMedia& media, double,
              const DocumentState*) {
  auto state = std::make_shared<ProbeState>();
  state->level = 0.25F * static_cast<float>(document.routes.size());
  for (const auto& entry : media)
    state->level += entry.asset->frames.front().left;
  return foundation::Result<std::shared_ptr<const DocumentState>>::success(std::move(state));
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): a throw fails the test at start-up.
const SynthEngine probe_engine{
    &probe_descriptor,
    [](double) -> std::unique_ptr<SynthKernel> { return std::make_unique<ProbeKernel>(); },
    [](std::span<const double>, double) -> std::uint64_t { return 0; },
    false,
    &validate_probe,
    &prepare_probe,
    nullptr};

project::NativeSynthDocument routes(std::size_t count) {
  project::NativeSynthDocument document;
  for (std::size_t slot = 0; slot < count; ++slot)
    document.routes.push_back({static_cast<std::uint32_t>(slot), "lfo.1", "", "probe.gain"});
  return document;
}

project::ManagedAudioReference reference(std::string sha256) {
  return {project::SourceId::from_bits({0, 1}).value(), std::move(sha256), 1'000, 48'000, 1, 4};
}

SynthMedia table_media(std::string sha256, float first) {
  auto asset = std::make_shared<SynthMediaAsset>();
  asset->source_rate = oscillate::test::sample_rate_48k;
  asset->frames.assign(4, {first, first});
  return {{"table", std::move(sha256), std::move(asset)}};
}

std::unique_ptr<NativeSynthRuntime> probe_runtime(const project::NativeSynthDocument& document,
                                                  SynthMedia media = {}) {
  auto runtime = NativeSynthRuntime::create(probe_engine, std::vector<double>{1.0}, document,
                                            std::move(media), 48'000.0);
  REQUIRE_OK(runtime, "probe runtime is created");
  return std::move(runtime).value();
}

float level(NativeSynthRuntime& runtime, std::uint64_t frame = 0) {
  std::vector<engine::StereoFrame> block(64, {0.0F, 0.0F});
  const auto report = runtime.process(block, {}, frame, 1, {}, {});
  REQUIRE_EQ(report.status, SynthProcessStatus::rendered, "probe block renders");
  return block.back().left;
}

void prepared_document_plays_from_the_first_block() {
  auto runtime = probe_runtime(routes(2));
  REQUIRE_EQ(level(*runtime), 0.5F, "the prepared document is heard at once");
  REQUIRE_EQ(runtime->document(), routes(2), "the runtime reports its document");
}

void document_arrives_with_its_revision() {
  auto runtime = probe_runtime(routes(1));
  (void)level(*runtime);
  auto offered = runtime->offer_document(routes(3), 7);
  REQUIRE_OK(offered, "a document edit is offered live");
  REQUIRE_TRUE(offered.value(), "a document edit is offered live");
  require(runtime->offer_values(std::vector<double>{1.0}, 7), "its values follow");
  REQUIRE_LT(runtime->applied_revision(), 7, "nothing is applied before the next block");
  REQUIRE_EQ(level(*runtime, 64), 0.75F, "the next block hears the new document");
  REQUIRE_EQ(runtime->applied_revision(), 7, "the revision acknowledges document and values");
  // Many edits in a row: each is adopted, and retired ones are released.
  for (std::uint64_t revision = 8; revision < 40; ++revision) {
    const std::size_t count = revision % 5;
    require(runtime->offer_document(routes(count), revision).value(), "each edit is offered");
    require(runtime->offer_values(std::vector<double>{1.0}, revision), "values follow");
    REQUIRE_EQ(level(*runtime, revision * 64), 0.25F * static_cast<float>(count),
               "every edit is heard");
  }
  require(!runtime->offer_document(routes(1), 3).has_value(), "revisions never go backwards");
}

void retry_may_repeat_a_revision() {
  auto runtime = probe_runtime(routes(0));
  require(runtime->offer_document(routes(1), 5).value(), "first offer");
  require(runtime->offer_document(routes(2), 5).value(), "a retry repeats the revision");
  require(runtime->offer_values(std::vector<double>{1.0}, 5), "values follow");
  REQUIRE_EQ(level(*runtime), 0.5F, "the newest offer wins");
}

void new_media_need_a_rebuild() {
  project::NativeSynthDocument document;
  document.media.push_back({"table", reference("aa"), 0});
  auto runtime = probe_runtime(document, table_media("aa", 0.125F));
  REQUIRE_EQ(level(*runtime), 0.125F, "decoded media reach the engine");
  document.routes = routes(1).routes;
  require(runtime->offer_document(document, 2).value(), "same media: live");
  auto replaced = document;
  replaced.media.front().media = reference("bb");
  auto offered = runtime->offer_document(replaced, 3);
  REQUIRE_OK(offered, "different media decline the live path");
  REQUIRE_TRUE(!offered.value(), "different media decline the live path");
  replaced.media.clear();
  require(!runtime->offer_document(replaced, 3).value(), "dropping media declines too");
  REQUIRE_EQ(runtime->document(), document, "a declined document is not adopted");
  require(
      !NativeSynthRuntime::create(probe_engine, std::vector<double>{1.0}, document, {}, 48'000.0)
           .has_value(),
      "a runtime needs every managed slot decoded");
  project::NativeSynthDocument builtin;
  builtin.media.push_back({"table", project::NativeBuiltinMedia{"basic-shapes"}, 2'048});
  REQUIRE_EQ(probe_runtime(builtin)->document(), builtin, "builtin media need no decoding");
}

void engines_without_documents_accept_only_empty_ones() {
  const auto& prism = default_engine();
  REQUIRE_EQ(prism.validate_document, nullptr, "Prism has no structured state");
  REQUIRE_EQ(prism.prepare_document, nullptr, "Prism has no structured state");
  auto patch = native_patch_for_preset(prism, std::nullopt);
  REQUIRE_OK(patch, "its patches have no document");
  REQUIRE_TRUE(patch.value().document.empty(), "its patches have no document");
  auto with_routes = patch.value();
  with_routes.document = routes(1);
  require(!resolve_native_instrument(with_routes).has_value(), "a document is rejected");
  require(
      !NativeSynthRuntime::create(prism, default_values(*prism.descriptor), routes(1), {}, 48'000.0)
           .has_value(),
      "so is a runtime with one");
  auto runtime = make_runtime(prism, default_values(*prism.descriptor));
  require(runtime->offer_document({}, 2).value(), "an empty document is always fine");
  require(!runtime->offer_document(routes(1), 3).has_value(), "a live document is rejected");
}

} // namespace

int main() {
  try {
    prepared_document_plays_from_the_first_block();
    document_arrives_with_its_revision();
    retry_may_repeat_a_revision();
    new_media_need_a_rebuild();
    engines_without_documents_accept_only_empty_ones();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
