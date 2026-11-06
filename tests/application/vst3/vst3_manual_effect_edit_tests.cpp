#include "oscillate/application/vst3/manual_effect_edit.hpp"
#include "oscillate/test/check.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using oscillate::test::require;

struct Fixture {
  std::filesystem::path root =
      std::filesystem::current_path() / ("vst3-manual-edit-" + std::to_string(::getpid()));
  std::unique_ptr<application::ProjectSession> session;
  project::TrackId track = project::TrackId::from_bits({0, 1}).value();
  project::InsertSlotId insert = project::InsertSlotId::from_bits({0, 1}).value();
  project::Vst3PluginInstance original{.id = project::PluginInstanceId::from_bits({0, 1}).value(),
                                       .class_id = {},
                                       .vendor = {},
                                       .name = {},
                                       .version = {},
                                       .bundle_path = {},
                                       .binary_sha256 = {},
                                       .component_state = {},
                                       .parameters = {},
                                       .missing = false};

  Fixture() {
    std::filesystem::remove_all(root);
    auto created = application::ProjectSession::create(root);
    REQUIRE_OK(created, "project create failed");
    session = std::move(created.value());
    auto added = session->tracks().add_bus_track(project::TrackKind::group_bus);
    REQUIRE_OK(added, "bus create failed");
    track = added.value();
    const application::Vst3InsertIdentity identity{std::string(32, 'a'), "Fixture Vendor",
                                                   "Fixture Effect",     "1.0",
                                                   "/opt/fixture.vst3",  std::string(64, 'b')};
    auto plugin = session->effects().insert_vst3_plugin(track, 0, identity);
    REQUIRE_OK(plugin, "effect insertion failed");
    insert = session->snapshot().tracks.front().effects.front().id;
    original = std::get<project::Vst3PluginInstance>(
        session->snapshot().tracks.front().effects.front().processor);
  }
  ~Fixture() { std::filesystem::remove_all(root); }
  auto saved_plugin() const {
    return std::get<project::Vst3PluginInstance>(
        session->snapshot().tracks.front().effects.front().processor);
  }
};

void receipt_and_reopen() {
  Fixture fixture;
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, true);
  slot->activate(303);
  application::Vst3ManualEffectEdit edit(*fixture.session, slot);
  const auto prior = fixture.session->snapshot().revision;
  const auto committed = edit.commit(prior, fixture.track, fixture.insert, fixture.original,
                                     {1, 2, 3}, {{7, 0.65}}, {7, 0, 0.65}, {303, 101, 202});
  REQUIRE_TRUE(committed, "saved revision was implied live before receipt");
  REQUIRE_EQ(committed.value().phase, application::Vst3ManualEffectPhase::pending,
             "saved revision was implied live before receipt");
  REQUIRE_EQ(committed.value().model_revision, prior + 1,
             "saved revision was implied live before receipt");
  REQUIRE_EQ(committed.value().saved_revision, prior + 1,
             "saved revision was implied live before receipt");
  REQUIRE_EQ(committed.value().live_revision, 0, "saved revision was implied live before receipt");
  REQUIRE_OK(slot->claim(), "callback did not claim admitted event");
  slot->finish(true, 512);
  const auto applied = edit.observe();
  REQUIRE_EQ(applied.phase, application::Vst3ManualEffectPhase::applied,
             "receipt did not identify audible revision and sample");
  REQUIRE_EQ(applied.live_revision, prior + 1,
             "receipt did not identify audible revision and sample");
  REQUIRE_EQ(applied.applied_sample, 512, "receipt did not identify audible revision and sample");
  REQUIRE_EQ(fixture.saved_plugin().component_state, std::vector<std::uint8_t>({1, 2, 3}),
             "captured state was not saved as one edit");
  REQUIRE_EQ(fixture.saved_plugin().parameters,
             std::vector<project::Vst3ParameterReference>({{7, 0.65}}),
             "captured state was not saved as one edit");
  REQUIRE_TRUE(fixture.session->history().undo(), "undo did not restore complete plugin state");
  REQUIRE_EQ(fixture.saved_plugin(), fixture.original,
             "undo did not restore complete plugin state");
  REQUIRE_TRUE(fixture.session->history().redo(), "undo did not restore complete plugin state");
  fixture.session.reset();
  auto reopened = application::ProjectSession::open(fixture.root);
  REQUIRE_TRUE(reopened, "opaque state did not reopen");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks.front().effects.front().processor)
                 .component_state,
             std::vector<std::uint8_t>({1, 2, 3}), "opaque state did not reopen");
}

class RefusingAdoption final : public application::Vst3ManualLiveAdoption {
public:
  foundation::Result<void> preflight(project::PluginInstanceId,
                                     plugins::Vst3ActiveGraphIdentity) override {
    return foundation::Result<void>::success();
  }
  foundation::Result<void> begin(const project::Snapshot&, project::PluginInstanceId,
                                 plugins::Vst3ActiveGraphIdentity) override {
    return foundation::Result<void>::success();
  }
  foundation::Result<void> settle(plugins::Vst3ManualEventReceipt) override {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "active graph changed before receipt"});
  }
};

void applied_receipt_requires_playback_adoption() {
  Fixture fixture;
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, true);
  slot->activate(303);
  RefusingAdoption adoption;
  application::Vst3ManualEffectEdit edit(*fixture.session, slot, &adoption);
  const auto prior = fixture.session->snapshot().revision;
  const auto saved = edit.commit(prior, fixture.track, fixture.insert, fixture.original, {1, 2},
                                 {{7, 0.65}}, {7, 0, 0.65}, {303, 101, 202});
  REQUIRE_TRUE(saved, "adoption fixture did not save a pending edit");
  REQUIRE_EQ(saved.value().phase, application::Vst3ManualEffectPhase::pending,
             "adoption fixture did not save a pending edit");
  REQUIRE_OK(slot->claim(), "adoption fixture event was not claimed");
  slot->finish(true, 512);
  const auto observed = edit.observe();
  REQUIRE_EQ(observed.phase, application::Vst3ManualEffectPhase::failed,
             "callback receipt was presented as live after playback adoption failed");
  REQUIRE_EQ(observed.live_revision, 0,
             "callback receipt was presented as live after playback adoption failed");
  REQUIRE_EQ(observed.applied_sample, -1,
             "callback receipt was presented as live after playback adoption failed");
  REQUIRE_TRUE(observed.live_refusal,
               "callback receipt was presented as live after playback adoption failed");
  REQUIRE_EQ(observed.live_refusal->message, "active graph changed before receipt",
             "callback receipt was presented as live after playback adoption failed");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior + 1,
             "callback receipt was presented as live after playback adoption failed");
}

void rejection_and_failed_receipt() {
  Fixture fixture;
  auto unverified = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, false);
  unverified->activate(303);
  application::Vst3ManualEffectEdit closed(*fixture.session, unverified);
  const auto prior = fixture.session->snapshot().revision;
  const auto refused = closed.commit(prior, fixture.track, fixture.insert, fixture.original, {1},
                                     {{7, 0.5}}, {7, 0, 0.5}, {303, 101, 202});
  REQUIRE_TRUE(!refused, "unverified latency committed a live edit");
  REQUIRE_NE(refused.error().message.find("latency"), std::string::npos,
             "unverified latency committed a live edit");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior,
             "unverified latency committed a live edit");
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, true);
  slot->activate(303);
  application::Vst3ManualEffectEdit edit(*fixture.session, slot);
  REQUIRE_TRUE(!edit.commit(prior, fixture.track, fixture.insert, fixture.original, {1}, {{7, 0.6}},
                            {7, 0, 0.5}, {303, 101, 202}),
               "mismatched clone capture committed");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior, "mismatched clone capture committed");
  REQUIRE_TRUE(!edit.commit(prior, fixture.track, fixture.insert, fixture.original, {1}, {{7, 0.5}},
                            {7, 0, 0.5}, {304, 101, 202}),
               "wrong graph accepted edit");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior, "wrong graph accepted edit");
  const auto accepted = edit.commit(prior, fixture.track, fixture.insert, fixture.original, {1},
                                    {{7, 0.5}}, {7, 0, 0.5}, {303, 101, 202});
  REQUIRE_TRUE(accepted, "verified edit rejected");
  REQUIRE_EQ(accepted.value().phase, application::Vst3ManualEffectPhase::pending,
             "verified edit rejected");
  slot->deactivate();
  const auto failed = edit.observe();
  REQUIRE_EQ(failed.phase, application::Vst3ManualEffectPhase::failed,
             "retired graph did not expose saved but failed live edit");
  REQUIRE_EQ(failed.saved_revision, prior + 1,
             "retired graph did not expose saved but failed live edit");
  REQUIRE_EQ(failed.live_revision, 0, "retired graph did not expose saved but failed live edit");
  REQUIRE_EQ(fixture.saved_plugin().component_state, std::vector<std::uint8_t>({1}),
             "retired graph did not expose saved but failed live edit");
}

void automation_takes_precedence() {
  Fixture fixture;
  require(fixture.session->effects()
              .set_vst3_editor_state(fixture.session->snapshot().revision, fixture.track,
                                     fixture.insert, fixture.original, {}, {{7, 0.4}})
              .has_value(),
          "baseline parameter reference failed");
  fixture.original = fixture.saved_plugin();
  require(fixture.session->parameter_automation()
              .set_vst3_parameter_automation_point(fixture.track, fixture.original.id, 7, 0, 0.4)
              .has_value(),
          "automation lane create failed");
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, true);
  slot->activate(303);
  application::Vst3ManualEffectEdit edit(*fixture.session, slot);
  const auto prior = fixture.session->snapshot().revision;
  REQUIRE_TRUE(!edit.commit(prior, fixture.track, fixture.insert, fixture.original, {1}, {{7, 0.5}},
                            {7, 0, 0.5}, {303, 101, 202}),
               "manual edit bypassed saved automation precedence");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior,
             "manual edit bypassed saved automation precedence");
}

struct InstrumentFixture {
  std::filesystem::path root =
      std::filesystem::current_path() / ("vst3-manual-instrument-" + std::to_string(::getpid()));
  std::unique_ptr<application::ProjectSession> session;
  project::TrackId track = project::TrackId::from_bits({0, 1}).value();
  project::Vst3PluginInstance original{.id = project::PluginInstanceId::from_bits({0, 1}).value(),
                                       .class_id = {},
                                       .vendor = {},
                                       .name = {},
                                       .version = {},
                                       .bundle_path = {},
                                       .binary_sha256 = {},
                                       .component_state = {},
                                       .parameters = {},
                                       .missing = false};

  InstrumentFixture() {
    std::filesystem::remove_all(root);
    auto created = application::ProjectSession::create(root);
    REQUIRE_OK(created, "instrument project create failed");
    session = std::move(created.value());
    const application::Vst3InsertIdentity identity{
        std::string(32, 'c'),           "Fixture Vendor",    "Fixture Instrument", "1.0",
        "/opt/fixture-instrument.vst3", std::string(64, 'd')};
    auto added = session->instruments().create_vst3_instrument_track(identity);
    REQUIRE_OK(added, "instrument source creation failed");
    track = added.value();
    original = saved_plugin();
  }
  ~InstrumentFixture() { std::filesystem::remove_all(root); }
  project::Vst3PluginInstance saved_plugin() const {
    return std::get<project::Vst3PluginInstance>(
        session->snapshot().tracks.front().instrument_source);
  }
};

void instrument_receipt_and_reopen() {
  InstrumentFixture fixture;
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      401, 402, std::vector<std::uint32_t>{9}, std::vector<std::uint32_t>{}, true);
  slot->activate(403);
  application::Vst3ManualInstrumentEdit edit(*fixture.session, slot);
  const auto prior = fixture.session->snapshot().revision;
  const auto saved = edit.commit(prior, fixture.track, fixture.original, {4, 5}, {{9, 0.7}},
                                 {9, 0, 0.7}, {403, 401, 402});
  REQUIRE_TRUE(saved, "instrument edit did not save before live submission");
  REQUIRE_EQ(saved.value().phase, application::Vst3ManualEditPhase::pending,
             "instrument edit did not save before live submission");
  REQUIRE_EQ(saved.value().saved_revision, prior + 1,
             "instrument edit did not save before live submission");
  REQUIRE_EQ(saved.value().live_revision, 0, "instrument edit did not save before live submission");
  REQUIRE_EQ(fixture.saved_plugin().component_state, std::vector<std::uint8_t>({4, 5}),
             "instrument edit did not save before live submission");
  require(!edit.commit(prior + 1, fixture.track, fixture.saved_plugin(), {4, 6}, {{9, 0.8}},
                       {9, 0, 0.8}, {403, 401, 402}),
          "instrument edit accepted a second event with receipt pending");
  REQUIRE_OK(slot->claim(), "instrument event was not claimed");
  slot->finish(true, 1024);
  const auto applied = edit.observe();
  REQUIRE_EQ(applied.phase, application::Vst3ManualEditPhase::applied,
             "instrument receipt did not identify applied project sample");
  REQUIRE_EQ(applied.live_revision, prior + 1,
             "instrument receipt did not identify applied project sample");
  REQUIRE_EQ(applied.applied_sample, 1024,
             "instrument receipt did not identify applied project sample");
  REQUIRE_TRUE(fixture.session->history().undo(), "instrument edit was not one undo step");
  REQUIRE_EQ(fixture.saved_plugin(), fixture.original, "instrument edit was not one undo step");
  REQUIRE_TRUE(fixture.session->history().redo(), "instrument edit was not one undo step");
  fixture.session.reset();
  auto reopened = application::ProjectSession::open(fixture.root);
  REQUIRE_TRUE(reopened, "instrument parameter edit did not reopen");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks.front().instrument_source)
                 .parameters,
             std::vector<project::Vst3ParameterReference>({{9, 0.7}}),
             "instrument parameter edit did not reopen");
}

void instrument_rejections_and_failed_receipt() {
  InstrumentFixture fixture;
  const auto prior = fixture.session->snapshot().revision;
  auto unverified = std::make_shared<plugins::Vst3ManualEventSlot>(
      401, 402, std::vector<std::uint32_t>{9}, std::vector<std::uint32_t>{}, false);
  unverified->activate(403);
  application::Vst3ManualInstrumentEdit closed(*fixture.session, unverified);
  REQUIRE_TRUE(!closed.commit(prior, fixture.track, fixture.original, {4}, {{9, 0.7}}, {9, 0, 0.7},
                              {403, 401, 402}),
               "unverified instrument latency committed a change");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior,
             "unverified instrument latency committed a change");
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      401, 402, std::vector<std::uint32_t>{9}, std::vector<std::uint32_t>{}, true);
  slot->activate(403);
  application::Vst3ManualInstrumentEdit edit(*fixture.session, slot);
  REQUIRE_TRUE(!edit.commit(prior, fixture.track, fixture.original, {4}, {{9, 0.8}}, {9, 0, 0.7},
                            {403, 401, 402}),
               "invalid instrument capture or graph committed a change");
  REQUIRE_TRUE(!edit.commit(prior, fixture.track, fixture.original, {4}, {{9, 0.7}}, {9, 0, 0.7},
                            {404, 401, 402}),
               "invalid instrument capture or graph committed a change");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior,
             "invalid instrument capture or graph committed a change");
  const auto accepted = edit.commit(prior, fixture.track, fixture.original, {4}, {{9, 0.7}},
                                    {9, 0, 0.7}, {403, 401, 402});
  REQUIRE_TRUE(accepted, "verified instrument edit rejected");
  REQUIRE_EQ(accepted.value().phase, application::Vst3ManualEditPhase::pending,
             "verified instrument edit rejected");
  slot->deactivate();
  const auto failed = edit.observe();
  REQUIRE_EQ(failed.phase, application::Vst3ManualEditPhase::failed,
             "instrument retirement did not expose failed live delivery");
  REQUIRE_EQ(failed.saved_revision, prior + 1,
             "instrument retirement did not expose failed live delivery");
  REQUIRE_EQ(failed.live_revision, 0, "instrument retirement did not expose failed live delivery");

  fixture.original = fixture.saved_plugin();
  const auto current = fixture.session->snapshot().revision;
  require(fixture.session->parameter_automation()
              .set_vst3_parameter_automation_point(fixture.track, fixture.original.id, 9, 0, 0.7)
              .has_value(),
          "instrument automation create failed");
  REQUIRE_TRUE(!edit.commit(fixture.session->snapshot().revision, fixture.track, fixture.original,
                            {5}, {{9, 0.8}}, {9, 0, 0.8}, {403, 401, 402}),
               "instrument manual edit bypassed automation");
  REQUIRE_EQ(fixture.session->snapshot().revision, current + 1,
             "instrument manual edit bypassed automation");
}

void instrument_source_identity_is_rechecked() {
  InstrumentFixture fixture;
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      401, 402, std::vector<std::uint32_t>{9}, std::vector<std::uint32_t>{}, true);
  slot->activate(403);
  application::Vst3ManualInstrumentEdit edit(*fixture.session, slot);
  require(fixture.session->instruments()
              .set_vst3_instrument_state(fixture.session->snapshot().revision, fixture.track,
                                         fixture.original, {8}, {{9, 0.6}})
              .has_value(),
          "instrument baseline state save failed");
  const auto current = fixture.session->snapshot().revision;
  REQUIRE_TRUE(!edit.commit(current, fixture.track, fixture.original, {9}, {{9, 0.7}}, {9, 0, 0.7},
                            {403, 401, 402}),
               "stale instrument source identity committed a change");
  REQUIRE_EQ(fixture.session->snapshot().revision, current,
             "stale instrument source identity committed a change");
}

void controller_chunks_commit_atomically_and_reopen() {
  Fixture fixture;
  const auto prior = fixture.session->snapshot().revision;
  const auto captured = fixture.original;
  const std::optional<std::vector<std::uint8_t>> controller{{7, 8}};
  REQUIRE_TRUE(fixture.session->effects()
                   .set_vst3_editor_state(prior, fixture.track, fixture.insert, captured, {1, 2},
                                          controller, {{7, 0.65}})
                   .has_value(),
               "effect state pair was not one committed revision");
  REQUIRE_EQ(fixture.session->snapshot().revision, prior + 1,
             "effect state pair was not one committed revision");
  REQUIRE_EQ(fixture.saved_plugin().component_state, std::vector<std::uint8_t>({1, 2}),
             "effect state pair was not one committed revision");
  REQUIRE_EQ(fixture.saved_plugin().controller_state, controller,
             "effect state pair was not one committed revision");
  REQUIRE_OK(fixture.session->history().undo(),
             "one undo/redo did not restore the whole effect state pair");
  REQUIRE_EQ(fixture.saved_plugin().controller_state, captured.controller_state,
             "one undo/redo did not restore the whole effect state pair");
  REQUIRE_EQ(fixture.saved_plugin().component_state, captured.component_state,
             "one undo/redo did not restore the whole effect state pair");
  REQUIRE_OK(fixture.session->history().redo(),
             "one undo/redo did not restore the whole effect state pair");
  REQUIRE_EQ(fixture.saved_plugin().controller_state, controller,
             "one undo/redo did not restore the whole effect state pair");
  REQUIRE_OK(fixture.session->saving().save(), "effect state pair re-save failed");
  fixture.session.reset();
  auto reopened = application::ProjectSession::open(fixture.root);
  REQUIRE_OK(reopened, "effect state pair reopen failed");
  fixture.session = std::move(reopened.value());
  REQUIRE_EQ(fixture.saved_plugin().controller_state, controller,
             "effect controller chunk or references were lost on reopen");
  REQUIRE_EQ(fixture.saved_plugin().parameters,
             (std::vector<project::Vst3ParameterReference>{{7, 0.65}}),
             "effect controller chunk or references were lost on reopen");

  const auto reopened_original = fixture.saved_plugin();
  const auto revision = fixture.session->snapshot().revision;
  REQUIRE_TRUE(!fixture.session->effects().set_vst3_editor_state(
                   revision, fixture.track, fixture.insert, reopened_original, {3},
                   std::optional<std::vector<std::uint8_t>>{
                       std::vector<std::uint8_t>(project::max_vst3_state_bytes + 1)},
                   {{7, 0.75}}),
               "oversized controller chunk mutated the effect state pair");
  REQUIRE_EQ(fixture.session->snapshot().revision, revision,
             "oversized controller chunk mutated the effect state pair");
  REQUIRE_EQ(fixture.saved_plugin(), reopened_original,
             "oversized controller chunk mutated the effect state pair");
  REQUIRE_TRUE(fixture.session->effects().set_vst3_editor_state(
                   revision, fixture.track, fixture.insert, reopened_original, {3},
                   std::optional<std::vector<std::uint8_t>>{std::vector<std::uint8_t>{}},
                   {{7, 0.75}}),
               "present-empty controller state collapsed into absent");
  REQUIRE_TRUE(fixture.saved_plugin().controller_state,
               "present-empty controller state collapsed into absent");
  REQUIRE_TRUE(fixture.saved_plugin().controller_state->empty(),
               "present-empty controller state collapsed into absent");

  InstrumentFixture instrument;
  const auto instrument_prior = instrument.session->snapshot().revision;
  const auto source = instrument.original;
  REQUIRE_TRUE(instrument.session->instruments().set_vst3_instrument_state(
                   instrument_prior, instrument.track, source, {4, 5}, controller, {{9, 0.7}}),
               "instrument source state pair was not one undoable revision");
  REQUIRE_EQ(instrument.session->snapshot().revision, instrument_prior + 1,
             "instrument source state pair was not one undoable revision");
  REQUIRE_EQ(instrument.saved_plugin().controller_state, controller,
             "instrument source state pair was not one undoable revision");
  REQUIRE_TRUE(instrument.session->history().undo(),
               "instrument source state pair was not one undoable revision");
  REQUIRE_TRUE(!instrument.saved_plugin().controller_state,
               "instrument source state pair was not one undoable revision");
  REQUIRE_TRUE(instrument.session->history().redo(),
               "instrument source state pair was not one undoable revision");
  REQUIRE_EQ(instrument.saved_plugin().controller_state, controller,
             "instrument source state pair was not one undoable revision");
  REQUIRE_OK(instrument.session->saving().save(), "instrument state pair save failed");
  instrument.session.reset();
  auto instrument_reopened = application::ProjectSession::open(instrument.root);
  REQUIRE_OK(instrument_reopened, "instrument state pair reopen failed");
  instrument.session = std::move(instrument_reopened.value());
  REQUIRE_EQ(instrument.saved_plugin().controller_state, controller,
             "instrument controller and component chunks did not reopen together");
  REQUIRE_EQ(instrument.saved_plugin().component_state, std::vector<std::uint8_t>({4, 5}),
             "instrument controller and component chunks did not reopen together");
}

void qualified_edit_can_publish_current_on_a_worker() {
  Fixture fixture;
  auto slot = std::make_shared<plugins::Vst3ManualEventSlot>(
      101, 202, std::vector<std::uint32_t>{7}, std::vector<std::uint32_t>{}, true);
  slot->activate(303);
  application::Vst3ManualEffectEdit edit(*fixture.session, slot);
  const auto accepted = edit.commit(fixture.session->snapshot().revision, fixture.track,
                                    fixture.insert, fixture.original, {1}, {{7, 0.5}}, {7, 0, 0.5},
                                    {303, 101, 202}, 1, application::EditorSavePolicy::background);
  REQUIRE_TRUE(accepted, "qualified live edit should reach the model before disk publication");
  REQUIRE_TRUE(accepted.value().persistence_pending,
               "qualified live edit should reach the model before disk publication");
  REQUIRE_TRUE(fixture.session->saving().has_unsaved_changes(),
               "qualified live edit should reach the model before disk publication");
  {
    auto task = fixture.session->saving().begin_background_save();
    REQUIRE_OK(task, "qualified VST3 state could not capture a save task");
    const auto revision = task.value().revision();
    auto outcome = std::move(task.value()).run();
    REQUIRE_TRUE(outcome, "qualified VST3 worker save did not commit");
    REQUIRE_EQ(outcome.value().status, persistence::SaveStatus::committed,
               "qualified VST3 worker save did not commit");
    fixture.session->saving().finish_background_save(revision, outcome.value());
  }
  require(!fixture.session->saving().has_unsaved_changes(),
          "qualified VST3 completion did not mark CURRENT durable");
  fixture.session.reset();
  auto reopened = application::ProjectSession::open(fixture.root);
  REQUIRE_TRUE(reopened, "qualified VST3 background save did not reopen");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 reopened.value()->snapshot().tracks.front().effects.front().processor)
                 .component_state,
             std::vector<std::uint8_t>{1}, "qualified VST3 background save did not reopen");
}
} // namespace

int main() {
  receipt_and_reopen();
  applied_receipt_requires_playback_adoption();
  rejection_and_failed_receipt();
  automation_takes_precedence();
  instrument_receipt_and_reopen();
  instrument_rejections_and_failed_receipt();
  instrument_source_identity_is_rechecked();
  controller_chunks_commit_atomically_and_reopen();
  qualified_edit_can_publish_current_on_a_worker();
}
