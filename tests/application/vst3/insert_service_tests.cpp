#include "oscillate/application/vst3/insert_service.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>

static_assert(!std::is_default_constructible_v<oscillate::plugins::VerifiedVst3EffectSelection>);
static_assert(!std::is_constructible_v<oscillate::plugins::VerifiedVst3EffectSelection,
                                       oscillate::plugins::Vst3Class,
                                       oscillate::plugins::Vst3BinaryIdentity>);
static_assert(
    !std::is_default_constructible_v<oscillate::plugins::VerifiedVst3InstrumentSelection>);
static_assert(!std::is_constructible_v<oscillate::plugins::VerifiedVst3InstrumentSelection,
                                       oscillate::plugins::Vst3Class,
                                       oscillate::plugins::Vst3BinaryIdentity>);

namespace {
namespace fs = std::filesystem;
namespace app = oscillate::application;
namespace plugins = oscillate::plugins;
namespace project = oscillate::project;

using oscillate::test::require;

const project::Vst3PluginInstance& vst3(const project::EffectInsert& insert) {
  return std::get<project::Vst3PluginInstance>(insert.processor);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-insert") {}
};

struct ProjectFixture {
  std::unique_ptr<app::ProjectSession> session;
  project::TrackId track;
};

ProjectFixture create_project(const fs::path& root) {
  auto created = app::ProjectSession::create(root);
  REQUIRE_OK(created, "project creation failed");
  auto session = std::move(created).value();
  REQUIRE_OK(session->tracks().add_instrument_track(), "track creation failed");
  const auto track = session->snapshot().tracks.front().id;
  return {std::move(session), track};
}

ProjectFixture create_audio_project(const fs::path& root) {
  auto created = app::ProjectSession::create(root);
  REQUIRE_OK(created, "audio project creation failed");
  constexpr std::array<std::uint8_t, 46> wav{
      'R', 'I', 'F', 'F', 38,  0,   0,   0,   'W',  'A',  'V', 'E', 'f', 'm',  't', ' ',
      16,  0,   0,   0,   1,   0,   1,   0,   0x80, 0xBB, 0,   0,   0,   0x77, 1,   0,
      2,   0,   16,  0,   'd', 'a', 't', 'a', 2,    0,    0,   0,   0,   0};
  const auto source = root.parent_path() / "effect_source.wav";
  {
    std::ofstream output(source, std::ios::binary);
    output.write(reinterpret_cast<const char*>(wav.data()),
                 static_cast<std::streamsize>(wav.size()));
    require(output.good(), "effect WAV fixture write failed");
  }
  const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "effect WAV fixture open failed");
  auto imported = created.value()->imports().import_wav_audio(fd);
  (void)::close(fd);
  REQUIRE_OK(imported, "effect audio-track import failed");
  return {std::move(created).value(), imported.value()};
}

plugins::Vst3CatalogueEntry scanned_entry(plugins::Vst3Scanner& scanner, const fs::path& bundle) {
  plugins::Vst3Catalogue catalogue(scanner, {});
  const std::array<fs::path, 1> paths{bundle};
  const auto& snapshot = catalogue.refresh(paths);
  REQUIRE_EQ(snapshot.entries.size(), 1, "catalogue did not find selected bundle");
  return snapshot.entries.front();
}

void verified_insert_is_saved_and_undoable(const fs::path& root) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto bundle = fs::path(VST3_FIXTURE_ROOT) / "Valid.vst3";
  const auto entry = scanned_entry(scanner, bundle);
  REQUIRE_TRUE(entry.scan.ok(), "valid fixture was not verified");
  REQUIRE_EQ(entry.scan.metadata.classes.size(), 1, "valid fixture was not verified");
  const auto class_id = entry.scan.metadata.classes.front().id;
  auto fixture = create_audio_project(root / "saved");
  const auto initial_revision = fixture.session->snapshot().revision;

  auto verified = plugins::verify_vst3_effect_selection(entry, class_id, scanner);
  REQUIRE_OK(verified, "worker verification lost exact class or binary identity");
  REQUIRE_EQ(verified.value().binary().sha256, entry.scan.binary.sha256,
             "worker verification lost exact class or binary identity");
  REQUIRE_EQ(verified.value().effect_class().id, class_id,
             "worker verification lost exact class or binary identity");
  auto first =
      plugins::insert_verified_vst3_effect(*fixture.session, verified.value(), fixture.track, 0);
  REQUIRE_OK(first, first ? "" : first.error().message);
  auto second = plugins::insert_verified_vst3_effect(*fixture.session, entry, class_id,
                                                     fixture.track, 0, scanner);
  REQUIRE_OK(second, "verified second effect did not get a stable distinct ID");
  REQUIRE_NE(second.value(), first.value(),
             "verified second effect did not get a stable distinct ID");
  const auto inserted = fixture.session->snapshot().tracks.front().effects;
  REQUIRE_EQ(fixture.session->snapshot().revision, initial_revision + 2,
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(inserted.size(), 2, "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(vst3(inserted[0]).id, second.value(),
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(vst3(inserted[1]).id, first.value(),
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(vst3(inserted[0]).bundle_path, bundle.string(),
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(vst3(inserted[0]).binary_sha256, entry.scan.binary.sha256,
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_EQ(vst3(inserted[0]).class_id, class_id,
             "inserted identity, chain order, or safe initial state changed");
  REQUIRE_TRUE(vst3(inserted[0]).component_state.empty(),
               "inserted identity, chain order, or safe initial state changed");
  REQUIRE_TRUE(vst3(inserted[0]).parameters.empty(),
               "inserted identity, chain order, or safe initial state changed");
  REQUIRE_TRUE(!vst3(inserted[0]).missing,
               "inserted identity, chain order, or safe initial state changed");

  REQUIRE_OK(fixture.session->history().undo(), "insert undo failed");
  REQUIRE_EQ(fixture.session->snapshot().tracks.front().effects.size(), 1,
             "undo did not remove exactly the second insert");
  REQUIRE_EQ(vst3(fixture.session->snapshot().tracks.front().effects.front()).id, first.value(),
             "undo did not remove exactly the second insert");
  REQUIRE_OK(fixture.session->history().redo(), "insert redo failed");
  REQUIRE_EQ(vst3(fixture.session->snapshot().tracks.front().effects.front()).id, second.value(),
             "redo did not restore original insert identity");
  REQUIRE_OK(fixture.session->saving().save(), "redo save failed");
  fixture.session.reset();
  auto reopened = app::ProjectSession::open(root / "saved");
  REQUIRE_OK(reopened, reopened ? "" : reopened.error().message);
  const auto& persisted = reopened.value()->snapshot().tracks.front().effects;
  REQUIRE_EQ(persisted.size(), 2, "save/reopen changed effect identity or chain position");
  REQUIRE_EQ(vst3(persisted[0]).id, second.value(),
             "save/reopen changed effect identity or chain position");
  REQUIRE_EQ(vst3(persisted[1]).id, first.value(),
             "save/reopen changed effect identity or chain position");
  REQUIRE_EQ(persisted[0], inserted[0], "save/reopen changed effect identity or chain position");
}

void refusals_leave_model_unchanged(const fs::path& root) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto original = fs::path(VST3_FIXTURE_ROOT) / "Valid.vst3";
  const auto copy = root / "Valid.vst3";
  fs::copy(original, copy, fs::copy_options::recursive);
  auto entry = scanned_entry(scanner, copy);
  require(entry.scan.ok(), "copied fixture scan failed");
  const auto class_id = entry.scan.metadata.classes.front().id;
  auto fixture = create_audio_project(root / "refusals");
  const auto revision = fixture.session->snapshot().revision;
  const auto refuse = [&](const plugins::Vst3CatalogueEntry& selected, std::string_view chosen,
                          std::size_t index = 0) {
    auto result = plugins::insert_verified_vst3_effect(*fixture.session, selected, chosen,
                                                       fixture.track, index, scanner);
    REQUIRE_TRUE(!result, "refused insert changed project model");
    REQUIRE_EQ(fixture.session->snapshot().revision, revision,
               "refused insert changed project model");
    REQUIRE_TRUE(fixture.session->snapshot().tracks.front().effects.empty(),
                 "refused insert changed project model");
  };

  auto unverified = entry;
  unverified.scan.status = plugins::ScanStatus::invalid_bundle;
  refuse(unverified, class_id);
  refuse(entry, "ffffffffffffffffffffffffffffffff");
  auto wrong_hash = entry;
  wrong_hash.scan.binary.sha256.assign(64, '0');
  refuse(wrong_hash, class_id);
  auto wrong_class = entry;
  wrong_class.scan.metadata.classes.front().vendor = "Changed Vendor";
  refuse(wrong_class, class_id);
  auto instrument_class = entry;
  instrument_class.scan.metadata.classes.front().subcategories = "Instrument";
  refuse(instrument_class, class_id);
  refuse(entry, class_id, 1);

  const auto binary = copy / "Contents/x86_64-linux/Valid.so";
  {
    std::ofstream output(binary, std::ios::app | std::ios::binary);
    output.put('x');
    require(output.good(), "binary-change fixture write failed");
  }
  refuse(entry, class_id);
  fs::remove(binary);
  refuse(entry, class_id);
  auto missing = scanned_entry(scanner, copy);
  require(!missing.scan.ok(), "missing binary was accepted by scanner");
  refuse(missing, class_id);
}

void failed_save_retains_undoable_insert(const fs::path& root) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto entry = scanned_entry(scanner, fs::path(VST3_FIXTURE_ROOT) / "Valid.vst3");
  const auto class_id = entry.scan.metadata.classes.front().id;
  auto fixture = create_audio_project(root / "save-failure");
  REQUIRE_OK(fixture.session->saving().save(), "save-failure fixture setup failed");
  fixture.session.reset();

  bool fail_save = true;
  auto opened = app::ProjectSession::open(
      root / "save-failure", [&fail_save](oscillate::persistence::SaveStage stage) {
        return fail_save && stage == oscillate::persistence::SaveStage::manifest_written;
      });
  REQUIRE_OK(opened, "save-failure fixture reopen failed");
  auto session = std::move(opened).value();
  const auto revision = session->snapshot().revision;
  const auto result =
      plugins::insert_verified_vst3_effect(*session, entry, class_id, fixture.track, 0, scanner);
  REQUIRE_TRUE(!result, "failed save hid or rolled back accepted insert");
  REQUIRE_NE(result.error().message.find("save failed"), std::string::npos,
             "failed save hid or rolled back accepted insert");
  REQUIRE_EQ(session->snapshot().revision, revision + 1,
             "failed save hid or rolled back accepted insert");
  REQUIRE_EQ(session->snapshot().tracks.front().effects.size(), 1,
             "failed save hid or rolled back accepted insert");
  const auto id = vst3(session->snapshot().tracks.front().effects.front()).id;
  REQUIRE_OK(session->history().undo(), "accepted insert lost undo/redo after save failure");
  REQUIRE_TRUE(session->snapshot().tracks.front().effects.empty(),
               "accepted insert lost undo/redo after save failure");
  REQUIRE_OK(session->history().redo(), "accepted insert lost undo/redo after save failure");
  REQUIRE_EQ(vst3(session->snapshot().tracks.front().effects.front()).id, id,
             "accepted insert lost undo/redo after save failure");
  fail_save = false;
  REQUIRE_OK(session->saving().save(), "retry save failed");
  session.reset();
  auto persisted = app::ProjectSession::open(root / "save-failure");
  REQUIRE_OK(persisted, "retry save did not preserve inserted identity");
  REQUIRE_EQ(vst3(persisted.value()->snapshot().tracks.front().effects.front()).id, id,
             "retry save did not preserve inserted identity");
}

void instrument_selection_and_track_boundaries(const fs::path& root) {
  plugins::Vst3Scanner scanner(VST3_HELPER_PATH);
  const auto entry = scanned_entry(scanner, VST3_INSTRUMENT_BUNDLE);
  REQUIRE_TRUE(entry.scan.ok(), "instrument fixture was not scanned");
  REQUIRE_EQ(entry.scan.metadata.classes.size(), 1, "instrument fixture was not scanned");
  const auto class_id = entry.scan.metadata.classes.front().id;
  auto verified = plugins::verify_vst3_instrument_selection(entry, class_id, scanner);
  REQUIRE_OK(verified,
             "instrument selection accepted the wrong class kind or lost binary identity");
  REQUIRE_EQ(verified.value().instrument_class().id, class_id,
             "instrument selection accepted the wrong class kind or lost binary identity");
  REQUIRE_EQ(verified.value().binary().sha256, entry.scan.binary.sha256,
             "instrument selection accepted the wrong class kind or lost binary identity");
  REQUIRE_TRUE(!plugins::verify_vst3_effect_selection(entry, class_id, scanner),
               "instrument selection accepted the wrong class kind or lost binary identity");
  auto forged = entry;
  forged.scan.binary.sha256.assign(64, '0');
  REQUIRE_TRUE(!plugins::verify_vst3_instrument_selection(forged, class_id, scanner),
               "instrument selection accepted a changed binary or class");
  REQUIRE_TRUE(!plugins::verify_vst3_instrument_selection(entry, "wrong class", scanner),
               "instrument selection accepted a changed binary or class");

  const auto effect_entry = scanned_entry(scanner, fs::path(VST3_FIXTURE_ROOT) / "Valid.vst3");
  auto effect = plugins::verify_vst3_effect_selection(
      effect_entry, effect_entry.scan.metadata.classes.front().id, scanner);
  REQUIRE_OK(effect, "effect selection fixture failed");

  auto bus_fixture = create_project(root / "bus-effect");
  const auto bus_target =
      bus_fixture.session->tracks().add_bus_track(project::TrackKind::return_bus);
  REQUIRE_OK(bus_target, "bus-effect fixture creation failed");
  const auto bus_effect = plugins::insert_verified_vst3_effect(*bus_fixture.session, effect.value(),
                                                               bus_target.value(), 0);
  REQUIRE_OK(bus_effect, "verified effect could not enter a return bus");
  bus_fixture.session.reset();
  auto reopened_bus = app::ProjectSession::open(root / "bus-effect");
  REQUIRE_OK(reopened_bus, "verified bus effect did not survive save and reopen");
  REQUIRE_EQ(reopened_bus.value()->snapshot().tracks[1].effects.size(), 1,
             "verified bus effect did not survive save and reopen");
  REQUIRE_EQ(vst3(reopened_bus.value()->snapshot().tracks[1].effects.front()).id,
             bus_effect.value(), "verified bus effect did not survive save and reopen");
  const auto source_after_bus =
      plugins::create_verified_vst3_instrument_track(*reopened_bus.value(), verified.value());
  // The new instrument goes in above the return bus, which keeps its effect.
  const auto& bus_tracks = reopened_bus.value()->snapshot().tracks;
  REQUIRE_OK(source_after_bus,
             "VST3 instrument could not be created after one verified bus effect");
  REQUIRE_EQ(bus_tracks.size(), 3,
             "VST3 instrument could not be created after one verified bus effect");
  REQUIRE_EQ(bus_tracks[1].id, source_after_bus.value(),
             "VST3 instrument could not be created after one verified bus effect");
  REQUIRE_EQ(bus_tracks[2].id, bus_target.value(),
             "VST3 instrument could not be created after one verified bus effect");
  REQUIRE_EQ(bus_tracks[2].effects.size(), 1,
             "VST3 instrument could not be created after one verified bus effect");

  auto effect_first = create_audio_project(root / "effect-before-instrument");
  require(plugins::insert_verified_vst3_effect(*effect_first.session, effect.value(),
                                               effect_first.track, 0)
              .has_value(),
          "effect-first setup failed");
  const auto effect_first_revision = effect_first.session->snapshot().revision;
  auto instrument_after_effect =
      plugins::create_verified_vst3_instrument_track(*effect_first.session, verified.value());
  REQUIRE_OK(instrument_after_effect,
             "verified instrument could not be created after a separate VST3 audio effect");
  REQUIRE_EQ(effect_first.session->snapshot().revision, effect_first_revision + 1,
             "verified instrument could not be created after a separate VST3 audio effect");
  REQUIRE_EQ(effect_first.session->snapshot().tracks.size(), 2,
             "verified instrument could not be created after a separate VST3 audio effect");
  REQUIRE_EQ(effect_first.session->snapshot().tracks.front().effects.size(), 1,
             "verified instrument could not be created after a separate VST3 audio effect");
  REQUIRE_EQ(effect_first.session->snapshot().tracks.back().id, instrument_after_effect.value(),
             "verified instrument could not be created after a separate VST3 audio effect");
  effect_first.session.reset();
  auto effect_first_reopened = app::ProjectSession::open(root / "effect-before-instrument");
  REQUIRE_OK(effect_first_reopened,
             "effect-first instrument creation did not survive save and reopen");
  REQUIRE_EQ(effect_first_reopened.value()->snapshot().tracks.size(), 2,
             "effect-first instrument creation did not survive save and reopen");
  REQUIRE_EQ(effect_first_reopened.value()->snapshot().tracks.front().effects.size(), 1,
             "effect-first instrument creation did not survive save and reopen");
  REQUIRE_TRUE(std::holds_alternative<project::Vst3PluginInstance>(
                   effect_first_reopened.value()->snapshot().tracks.back().instrument_source),
               "effect-first instrument creation did not survive save and reopen");
  auto two_effects = create_audio_project(root / "two-effects-before-instrument");
  REQUIRE_TRUE(plugins::insert_verified_vst3_effect(*two_effects.session, effect.value(),
                                                    two_effects.track, 0)
                   .has_value(),
               "two-effect setup failed");
  REQUIRE_TRUE(plugins::insert_verified_vst3_effect(*two_effects.session, effect.value(),
                                                    two_effects.track, 1)
                   .has_value(),
               "two-effect setup failed");
  // An instrument plays beside any chains (ADR-0050).
  require(plugins::create_verified_vst3_instrument_track(*two_effects.session, verified.value()) &&
              two_effects.session->snapshot().tracks.size() == 2,
          "instrument creation was refused beside a two-effect chain");

  auto fixture = create_project(root / "instrument-track");
  auto bus = fixture.session->tracks().add_bus_track(project::TrackKind::group_bus);
  REQUIRE_OK(bus, "bus target fixture failed");
  const auto before = fixture.session->snapshot().revision;
  // Any instrument track's chain takes an effect (ADR-0050).
  auto effect_on_instrument = create_project(root / "instrument-track-effect");
  require(plugins::insert_verified_vst3_effect(*effect_on_instrument.session, effect.value(),
                                               effect_on_instrument.track, 0)
              .has_value(),
          "a verified effect was refused on an instrument track");
  auto wrong_track =
      plugins::insert_verified_vst3_instrument(*fixture.session, verified.value(), bus.value());
  REQUIRE_TRUE(!wrong_track, "verified instrument entered a bus track");
  REQUIRE_EQ(fixture.session->snapshot().revision, before,
             "verified instrument entered a bus track");
  auto inserted =
      plugins::insert_verified_vst3_instrument(*fixture.session, verified.value(), fixture.track);
  REQUIRE_OK(inserted, "verified instrument did not enter its empty instrument track");
  REQUIRE_EQ(fixture.session->snapshot().revision, before + 1,
             "verified instrument did not enter its empty instrument track");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 fixture.session->snapshot().tracks.front().instrument_source)
                 .id,
             inserted.value(), "verified instrument did not enter its empty instrument track");
  auto duplicate =
      plugins::insert_verified_vst3_instrument(*fixture.session, verified.value(), fixture.track);
  REQUIRE_TRUE(!duplicate, "second instrument insert changed the model");
  REQUIRE_EQ(fixture.session->snapshot().revision, before + 1,
             "second instrument insert changed the model");
  auto chain_effect =
      plugins::insert_verified_vst3_effect(*fixture.session, effect.value(), fixture.track, 0);
  REQUIRE_OK(chain_effect, "verified effect did not enter the instrument processing chain");
  REQUIRE_EQ(fixture.session->snapshot().tracks.front().effects.size(), 1,
             "verified effect did not enter the instrument processing chain");
  REQUIRE_EQ(vst3(fixture.session->snapshot().tracks.front().effects.front()).id,
             chain_effect.value(), "verified effect did not enter the instrument processing chain");
  const auto chained_revision = fixture.session->snapshot().revision;
  auto second_chain_effect =
      plugins::insert_verified_vst3_effect(*fixture.session, effect.value(), fixture.track, 1);
  REQUIRE_OK(second_chain_effect,
             "instrument chain did not accept a second serial verified effect");
  REQUIRE_EQ(fixture.session->snapshot().revision, chained_revision + 1,
             "instrument chain did not accept a second serial verified effect");
  REQUIRE_EQ(fixture.session->snapshot().tracks.front().effects.size(), 2,
             "instrument chain did not accept a second serial verified effect");
  // A chain takes any number of effects (ADR-0050).
  const auto third_chain_effect =
      plugins::insert_verified_vst3_effect(*fixture.session, effect.value(), fixture.track, 2);
  REQUIRE_OK(third_chain_effect, "instrument chain did not accept a third serial effect");
  REQUIRE_EQ(fixture.session->snapshot().tracks.front().effects.size(), 3,
             "instrument chain did not accept a third serial effect");
  fixture.session.reset();
  auto chained = app::ProjectSession::open(root / "instrument-track");
  REQUIRE_OK(chained, "instrument processing chain did not survive reopen");
  REQUIRE_EQ(chained.value()->snapshot().tracks.front().effects.size(), 3,
             "instrument processing chain did not survive reopen");
  REQUIRE_EQ(vst3(chained.value()->snapshot().tracks.front().effects[0]).id, chain_effect.value(),
             "instrument processing chain did not survive reopen");
  REQUIRE_EQ(vst3(chained.value()->snapshot().tracks.front().effects[1]).id,
             second_chain_effect.value(), "instrument processing chain did not survive reopen");
  REQUIRE_EQ(vst3(chained.value()->snapshot().tracks.front().effects[2]).id,
             third_chain_effect.value(), "instrument processing chain did not survive reopen");

  auto paired = create_project(root / "paired-existing-track");
  REQUIRE_TRUE(
      plugins::insert_verified_vst3_instrument(*paired.session, verified.value(), paired.track)
          .has_value(),
      "first paired source/effect path setup failed");
  REQUIRE_TRUE(
      plugins::insert_verified_vst3_effect(*paired.session, effect.value(), paired.track, 0)
          .has_value(),
      "first paired source/effect path setup failed");
  const auto paired_second = paired.session->tracks().add_instrument_track();
  REQUIRE_OK(paired_second, "second empty paired instrument track was not created");
  const auto paired_second_id = paired.session->snapshot().tracks.back().id;
  REQUIRE_TRUE(
      plugins::insert_verified_vst3_instrument(*paired.session, verified.value(), paired_second_id)
          .has_value(),
      "verified second source and effect did not enter the existing instrument track");
  REQUIRE_TRUE(
      plugins::insert_verified_vst3_effect(*paired.session, effect.value(), paired_second_id, 0)
          .has_value(),
      "verified second source and effect did not enter the existing instrument track");
  paired.session.reset();
  auto paired_reopened = app::ProjectSession::open(root / "paired-existing-track");
  REQUIRE_TRUE(paired_reopened, "paired verified source/effect paths did not reopen");
  REQUIRE_EQ(paired_reopened.value()->snapshot().tracks.size(), 2,
             "paired verified source/effect paths did not reopen");
  REQUIRE_EQ(paired_reopened.value()->snapshot().tracks.front().effects.size(), 1,
             "paired verified source/effect paths did not reopen");
  REQUIRE_EQ(paired_reopened.value()->snapshot().tracks.back().effects.size(), 1,
             "paired verified source/effect paths did not reopen");
  REQUIRE_TRUE(std::holds_alternative<project::Vst3PluginInstance>(
                   paired_reopened.value()->snapshot().tracks.back().instrument_source),
               "paired verified source/effect paths did not reopen");

  auto conflict = create_project(root / "instrument-conflict");
  REQUIRE_OK(conflict.session->tracks().add_instrument_track(), "second instrument fixture failed");
  const auto second = conflict.session->snapshot().tracks.back().id;
  REQUIRE_OK(conflict.session->midi_clips().add_midi_clip(second, {0}),
             "second MIDI source fixture failed");
  // Instrument sources of any kinds play together (ADR-0050).
  require(
      plugins::insert_verified_vst3_instrument(*conflict.session, verified.value(), conflict.track)
          .has_value(),
      "a verified instrument was refused beside a second MIDI source");

  auto created = app::ProjectSession::create(root / "drag-created-instrument");
  REQUIRE_OK(created, "drag-created project fixture failed");
  const auto empty_revision = created.value()->snapshot().revision;
  REQUIRE_TRUE(!created.value()->instruments().create_vst3_instrument_track({}),
               "invalid instrument identity left an empty track");
  REQUIRE_EQ(created.value()->snapshot().revision, empty_revision,
             "invalid instrument identity left an empty track");
  REQUIRE_TRUE(created.value()->snapshot().tracks.empty(),
               "invalid instrument identity left an empty track");
  auto new_track =
      plugins::create_verified_vst3_instrument_track(*created.value(), verified.value());
  REQUIRE_OK(new_track, "drag-created track and instrument were not one command");
  REQUIRE_EQ(created.value()->snapshot().revision, empty_revision + 1,
             "drag-created track and instrument were not one command");
  REQUIRE_EQ(created.value()->snapshot().tracks.size(), 1,
             "drag-created track and instrument were not one command");
  REQUIRE_EQ(created.value()->snapshot().tracks.front().id, new_track.value(),
             "drag-created track and instrument were not one command");
  REQUIRE_EQ(std::get<project::Vst3PluginInstance>(
                 created.value()->snapshot().tracks.front().instrument_source)
                 .class_id,
             class_id, "drag-created track and instrument were not one command");
  REQUIRE_OK(created.value()->history().undo(),
             "drag-created track and instrument were not one undo step");
  REQUIRE_TRUE(created.value()->snapshot().tracks.empty(),
               "drag-created track and instrument were not one undo step");
  REQUIRE_OK(created.value()->history().redo(),
             "drag-created track and instrument were not one undo step");
  REQUIRE_EQ(created.value()->snapshot().tracks.front().id, new_track.value(),
             "drag-created track and instrument were not one undo step");
  REQUIRE_OK(created.value()->saving().save(), "drag-created project save failed");
  created.value().reset();
  auto reopened = app::ProjectSession::open(root / "drag-created-instrument");
  REQUIRE_OK(reopened, "drag-created track did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 1, "drag-created track did not reopen");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.front().id, new_track.value(),
             "drag-created track did not reopen");
  const auto occupied_revision = reopened.value()->snapshot().revision;
  const auto second_new_track =
      plugins::create_verified_vst3_instrument_track(*reopened.value(), verified.value());
  REQUIRE_TRUE(second_new_track, "second VST3 drag did not create an independent source track");
  REQUIRE_NE(second_new_track.value(), new_track.value(),
             "second VST3 drag did not create an independent source track");
  REQUIRE_EQ(reopened.value()->snapshot().revision, occupied_revision + 1,
             "second VST3 drag did not create an independent source track");
  REQUIRE_EQ(reopened.value()->snapshot().tracks.size(), 2,
             "second VST3 drag did not create an independent source track");
  // Any number of instrument tracks (ADR-0050).
  require(plugins::create_verified_vst3_instrument_track(*reopened.value(), verified.value()) &&
              reopened.value()->snapshot().tracks.size() == 3,
          "a third VST3 drag did not create a source track");
}

} // namespace

int main() {
  try {
    Directory directory;
    verified_insert_is_saved_and_undoable(directory.path());
    refusals_leave_model_unchanged(directory.path());
    failed_save_retains_undoable_insert(directory.path());
    instrument_selection_and_track_boundaries(directory.path());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "VST3 insert service: %s\n", error.what());
    return 1;
  }
}
