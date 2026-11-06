#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "oscillate/test/wav.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;
using namespace oscillate::persistence;
namespace fs = std::filesystem;
using Json = nlohmann::json;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t low) { return take(T::from_bits({0, low})); }

std::string sha256(std::string_view bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  REQUIRE_EQ(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr),
             1, "test manifest digest failed");
  REQUIRE_EQ(length, 32, "test manifest digest failed");
  constexpr std::string_view digits = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(64);
  for (unsigned index = 0; index < length; ++index) {
    encoded.push_back(digits[digest[index] >> 4U]);
    encoded.push_back(digits[digest[index] & 0xfU]);
  }
  return encoded;
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-store") {}
};

project::Snapshot snapshot(std::uint64_t revision = 1) {
  auto tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter * 4}, 90.0, timeline::TempoShape::step}},
      2));
  auto meter = take(timeline::MeterMap::create({{{0}, 4, 4}}, 3));
  project::Clip first{id<project::ClipId>(3),
                      id<project::SourceId>(4),
                      {{0}, {48000}},
                      project::MusicalInterval{{0}, {timeline::ticks_per_quarter}},
                      7};
  project::Clip second{id<project::ClipId>(5),
                       id<project::SourceId>(4),
                       {{48000}, {96000}},
                       project::ElapsedInterval{{48000}, {72000}},
                       8};
  return {
      id<project::ProjectId>(1), revision,         48000,
      std::move(tempo),          std::move(meter), {{id<project::TrackId>(2), {first, second}}}};
}

template <typename Native> Native* native_effect(project::Snapshot& state) {
  for (auto& slot : state.master_effects) {
    if (auto* value = std::get_if<Native>(&slot.processor))
      return value;
  }
  return nullptr;
}
template <typename Native> const Native* native_effect(const project::Snapshot& state) {
  for (const auto& slot : state.master_effects) {
    if (const auto* value = std::get_if<Native>(&slot.processor))
      return value;
  }
  return nullptr;
}
project::EffectInsert effect_slot(std::uint64_t number, project::Vst3PluginInstance plugin) {
  return {id<project::InsertSlotId>(number), true, std::move(plugin)};
}

void round_trip_and_autosave() {
  Directory temp;
  auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  REQUIRE_TRUE(!store.load_current(), "missing CURRENT must be distinguishable");
  REQUIRE_EQ(store.load_current().error().code, foundation::ErrorCode::not_found,
             "missing CURRENT must be distinguishable");
  auto saved = take(store.save(snapshot(), SaveKind::explicit_save));
  REQUIRE_EQ(saved.status, SaveStatus::committed, "explicit save not committed");
  auto loaded = take(store.load_current());
  REQUIRE_EQ(loaded.generation, saved.generation, "current generation mismatch");
  REQUIRE_EQ(loaded.snapshot.revision, 1, "current generation mismatch");
  REQUIRE_EQ(loaded.snapshot.tempo.events().size(), 2, "snapshot fields did not round trip");
  REQUIRE_EQ(loaded.snapshot.tracks[0].clips[1].settings_generation, 8,
             "snapshot fields did not round trip");
  REQUIRE_EQ(loaded.source_diagnostics.size(), 1,
             "unregistered shared clip source was not diagnosed once");
  REQUIRE_EQ(loaded.source_diagnostics[0].id, id<project::SourceId>(4),
             "unregistered shared clip source was not diagnosed once");
  REQUIRE_EQ(loaded.source_diagnostics[0].status,
             LoadedGeneration::SourceDiagnostic::Status::unresolved,
             "unregistered shared clip source was not diagnosed once");
  auto autosaved = take(store.save(snapshot(2), SaveKind::autosave));
  REQUIRE_EQ(autosaved.status, SaveStatus::committed, "autosave not committed");
  REQUIRE_EQ(take(store.load_current()).snapshot.revision, 1, "autosave advanced CURRENT");
  REQUIRE_EQ(take(store.load_autosave()).snapshot.revision, 2, "autosave pointer wrong");
  const auto saved_pointer = root / "state" / "CURRENT.saved-test";
  fs::copy_file(root / "state" / "CURRENT", saved_pointer);
  fs::copy_file(root / "state" / "AUTOSAVE", root / "state" / "CURRENT",
                fs::copy_options::overwrite_existing);
  auto wrong_kind = store.load_current();
  REQUIRE_TRUE(!wrong_kind, "CURRENT accepted an autosave generation");
  REQUIRE_EQ(wrong_kind.error().code, foundation::ErrorCode::corrupt_data,
             "CURRENT accepted an autosave generation");
  fs::copy_file(saved_pointer, root / "state" / "CURRENT", fs::copy_options::overwrite_existing);
  fs::remove(saved_pointer);
  REQUIRE_TRUE(fs::is_directory(root / "assets"),
               "durable assets and disposable cache directories must be distinct");
  REQUIRE_TRUE(fs::is_directory(root / "cache"),
               "durable assets and disposable cache directories must be distinct");
  auto reader = take(ProjectStore::open_reader(root));
  REQUIRE_TRUE(!reader.save(snapshot(), SaveKind::explicit_save), "reader unexpectedly wrote");
  REQUIRE_EQ(reader.save(snapshot(), SaveKind::explicit_save).error().code,
             foundation::ErrorCode::conflict, "reader unexpectedly wrote");
  REQUIRE_EQ(take(reader.recovery_candidates()).size(), 2, "recovery list lost generations");
}

void track_channel_controls_round_trip() {
  auto state = snapshot();
  state.tracks[0].muted = true;
  state.tracks[0].soloed = true;
  state.tracks[0].level = 0.5;
  state.tracks[0].pan = -0.25;
  state.master_level = 0.625;
  const auto encoded =
      detail::encode_manifest(state, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  require(take(detail::decode_manifest(encoded)).snapshot.tracks[0].muted,
          "saved track mute did not round-trip");
  require(take(detail::decode_manifest(encoded)).snapshot.tracks[0].soloed,
          "saved track solo did not round-trip");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks[0].level, 0.5,
             "saved track level/pan did not round-trip");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks[0].pan, -0.25,
             "saved track level/pan did not round-trip");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.master_level, 0.625,
             "saved master output level did not round-trip");
  auto malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0].erase("muted");
  require(!detail::decode_manifest(malformed.dump()), "new schema accepted missing mute");
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["muted"] = "true";
  require(!detail::decode_manifest(malformed.dump()), "new schema accepted non-boolean mute");
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0].erase("soloed");
  require(!detail::decode_manifest(malformed.dump()), "new schema accepted missing solo");
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["soloed"] = "true";
  require(!detail::decode_manifest(malformed.dump()), "new schema accepted non-boolean solo");
  for (const auto* field : {"level", "pan"}) {
    malformed = Json::parse(encoded);
    malformed["project"]["tracks"][0].erase(field);
    require(!detail::decode_manifest(malformed.dump()),
            "new schema accepted missing channel field");
  }
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["level"] = 5.0;
  require(!detail::decode_manifest(malformed.dump()), "out-of-range level was accepted");
  malformed = Json::parse(encoded);
  malformed["project"]["tracks"][0]["pan"] = -1.5;
  require(!detail::decode_manifest(malformed.dump()), "out-of-range pan was accepted");
  malformed = Json::parse(encoded);
  malformed["project"].erase("master_level");
  require(!detail::decode_manifest(malformed.dump()), "new schema accepted missing master level");
  malformed = Json::parse(encoded);
  malformed["project"]["master_level"] = 4.1;
  require(!detail::decode_manifest(malformed.dump()), "out-of-range master level was accepted");
}

void clip_processing_round_trip() {
  auto state = snapshot();
  state.tracks[0].clips[0].processing = {
      7.0, false, {{{12'000}, timeline::TickPosition{timeline::ticks_per_quarter / 2}}}, {}};
  state.tracks[0].clips[1].processing = {
      -5.25, true, {{{60'000}, timeline::SamplePosition{12'000}}}, {}};
  const auto generation = std::string(32, 'a');
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "clip processing intent did not round-trip");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "clip processing intent did not round-trip");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "clip-processing", true));
  (void)take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "saved clip processing did not reopen");
  auto bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["pitch_semitones"] = 48.5;
  require(!detail::decode_manifest(bad.dump()), "out-of-range saved pitch accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["preserve_formants"] = 1;
  require(!detail::decode_manifest(bad.dump()), "nonboolean formant setting accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["reviewed_warp_markers"][0]
     ["target_offset"] = timeline::ticks_per_quarter;
  require(!detail::decode_manifest(bad.dump()), "out-of-bounds saved warp marker accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"].erase("pitch_semitones");
  require(!detail::decode_manifest(bad.dump()), "missing clip processing field accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["reviewed_warp_markers"][0]
     ["source_frame"] = 48'000;
  require(!detail::decode_manifest(bad.dump()), "source-end warp marker accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["reviewed_warp_markers"][0]["unexpected"] =
      0;
  require(!detail::decode_manifest(bad.dump()), "unknown warp marker field accepted");
}

void clip_warp_round_trip() {
  auto state = snapshot();
  state.tracks[0].clips[0].processing = {
      -36.5, false, {{{12'000}, timeline::TickPosition{timeline::ticks_per_quarter / 2}}}, {}};
  auto& warp = state.tracks[0].clips[0].processing.warp;
  warp.mode = project::WarpMode::beats;
  warp.mode_overridden = true;
  warp.beats = {project::WarpBeatsLoop::back_and_forth, 35.5};
  warp.tones = {12.0};
  warp.texture = {480.0, 77.0};
  warp.complex_pro = {20.0, 200.0};
  state.tracks[0].clips[1].processing.pitch_semitones = 7.25;
  const auto generation = std::string(32, 'b');
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "clip warp settings did not round-trip");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "clip warp settings did not round-trip");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "clip-warp", true));
  (void)take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "saved clip warp settings did not reopen");

  const auto rejects = [&encoded](const auto& corrupt, const char* message) {
    auto bad = Json::parse(encoded);
    corrupt(bad["project"]["tracks"][0]["clips"][0]["processing"]["warp"]);
    require(!detail::decode_manifest(bad.dump()), message);
  };
  rejects([](Json& value) { value["mode"] = "stretch"; }, "unknown warp mode accepted");
  rejects([](Json& value) { value["mode"] = 4; }, "numeric warp mode accepted");
  rejects([](Json& value) { value["mode_overridden"] = "yes"; }, "nonboolean override accepted");
  rejects([](Json& value) { value.erase("texture"); }, "missing warp parameters accepted");
  rejects([](Json& value) { value["unexpected"] = 0; }, "unknown warp field accepted");
  rejects([](Json& value) { value["beats"]["loop"] = "reverse"; }, "unknown Beats loop accepted");
  rejects([](Json& value) { value["beats"]["envelope"] = 101.0; },
          "out-of-range Beats envelope accepted");
  rejects([](Json& value) { value["tones"]["grain_ms"] = 201.0; },
          "out-of-range Tones grain accepted");
  rejects([](Json& value) { value["texture"]["flux"] = -1.0; }, "out-of-range flux accepted");
  rejects([](Json& value) { value["complex_pro"]["envelope"] = 300.0; },
          "out-of-range Complex Pro envelope accepted");
  rejects([](Json& value) { value["complex_pro"]["extra"] = 1.0; },
          "unknown Complex Pro field accepted");
  auto bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"].erase("warp");
  require(!detail::decode_manifest(bad.dump()), "missing warp record accepted");
  bad = Json::parse(encoded);
  bad["project"]["tracks"][0]["clips"][0]["processing"]["pitch_semitones"] = -48.25;
  require(!detail::decode_manifest(bad.dump()), "out-of-range pitch accepted");
}

void faults_preserve_previous_save() {
  const std::vector<SaveStage> stages{
      SaveStage::manifest_written,     SaveStage::generation_flushed,
      SaveStage::generation_published, SaveStage::generations_flushed,
      SaveStage::pointer_written,      SaveStage::pointer_published,
      SaveStage::state_flush_failed};
  for (const auto stage : stages) {
    Directory temp;
    auto root = temp.path() / "project";
    auto initial = take(ProjectStore::open_writer(root, true));
    auto baseline = take(initial.save(snapshot(), SaveKind::explicit_save));
    initial = take(ProjectStore::open_reader(root)); // release writer lock
    auto faulted = take(ProjectStore::open_writer(
        root, false, [stage](SaveStage reached) { return stage == reached; }));
    auto result = faulted.save(snapshot(2), SaveKind::explicit_save);
    const bool past_pointer =
        stage == SaveStage::pointer_published || stage == SaveStage::state_flush_failed;
    if (past_pointer) {
      REQUIRE_TRUE(result, "post-pointer fault must be uncertain");
      REQUIRE_EQ(result.value().status, SaveStatus::uncertain,
                 "post-pointer fault must be uncertain");
      REQUIRE_EQ(take(faulted.load_current()).snapshot.revision, 2,
                 "complete generation absent after pointer publication");
    } else {
      require(!result, "pre-pointer fault must fail");
      REQUIRE_EQ(take(faulted.load_current()).generation, baseline.generation,
                 "failed save replaced CURRENT");
    }
    require(!take(faulted.recovery_candidates()).empty(), "old generation lost");
  }
}

void writer_reopen_cleans_only_owned_save_stages() {
  const std::array scenarios{std::pair{SaveStage::manifest_written, SaveKind::explicit_save},
                             std::pair{SaveStage::pointer_written, SaveKind::explicit_save},
                             std::pair{SaveStage::pointer_written, SaveKind::autosave}};
  for (const auto [stage, kind] : scenarios) {
    Directory temp;
    const auto root = temp.path() / "project";
    auto writer = take(ProjectStore::open_writer(root, true));
    const auto baseline = take(writer.save(snapshot(), SaveKind::explicit_save));
    writer = take(ProjectStore::open_reader(root));
    auto faulted = take(ProjectStore::open_writer(
        root, false, [stage](SaveStage reached) { return reached == stage; }));
    require(!faulted.save(snapshot(2), kind), "interrupted save unexpectedly committed");
    const auto state = root / "state";
    const auto generations = state / "generations";
    const auto stage_directory = stage == SaveStage::manifest_written ? generations : state;
    const std::string_view stage_prefix = stage == SaveStage::manifest_written ? ".tmp-"
                                          : kind == SaveKind::autosave         ? ".AUTOSAVE-"
                                                                               : ".CURRENT-";
    fs::path abandoned;
    for (const auto& entry : fs::directory_iterator(stage_directory))
      if (entry.path().filename().string().starts_with(stage_prefix))
        abandoned = entry.path();
    require(!abandoned.empty(), "fault fixture did not leave the expected temporary stage");

    const auto unknown = generations / (".tmp-" + std::string(32, 'b'));
    fs::create_directory(unknown);
    std::ofstream(unknown / "keep.txt") << "not a project stage";
    const auto empty_stage = generations / (".tmp-" + std::string(32, 'e'));
    fs::create_directory(empty_stage);
    const auto linked_stage = generations / (".tmp-" + std::string(32, 'd'));
    fs::create_directory_symlink(unknown, linked_stage);
    const auto sentinel = state / "sentinel";
    std::ofstream(sentinel) << "leave alone";
    const auto symlink = state / (".AUTOSAVE-" + std::string(32, 'c'));
    fs::create_symlink(sentinel, symlink);
    faulted = take(ProjectStore::open_reader(root));
    writer = take(ProjectStore::open_writer(root, false));
    REQUIRE_TRUE(!fs::exists(abandoned), "writer reopen did not clean abandoned save stages");
    REQUIRE_TRUE(!fs::exists(empty_stage), "writer reopen did not clean abandoned save stages");
    REQUIRE_TRUE(fs::exists(unknown / "keep.txt"),
                 "writer cleanup damaged unrelated entries or the committed project");
    REQUIRE_TRUE(fs::is_symlink(linked_stage),
                 "writer cleanup damaged unrelated entries or the committed project");
    REQUIRE_TRUE(fs::is_symlink(symlink),
                 "writer cleanup damaged unrelated entries or the committed project");
    REQUIRE_TRUE(fs::exists(sentinel),
                 "writer cleanup damaged unrelated entries or the committed project");
    REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
               "writer cleanup damaged unrelated entries or the committed project");
  }
}

void orphaned_generation_remains_recoverable_without_changing_save_choices() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto explicit_save = take(writer.save(snapshot(1), SaveKind::explicit_save));
  const auto autosave = take(writer.save(snapshot(2), SaveKind::autosave));
  writer = take(ProjectStore::open_reader(root));

  auto interrupted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::generation_published; }));
  auto failed = interrupted.save(snapshot(3), SaveKind::explicit_save);
  REQUIRE_TRUE(!failed, "interruption before pointer publication was reported as a save");
  REQUIRE_EQ(failed.error().code, foundation::ErrorCode::io_failure,
             "interruption before pointer publication was reported as a save");
  REQUIRE_EQ(take(interrupted.load_current()).generation, explicit_save.generation,
             "complete orphan changed the saved or autosaved choice");
  REQUIRE_EQ(take(interrupted.load_autosave()).generation, autosave.generation,
             "complete orphan changed the saved or autosaved choice");
  auto candidates = take(interrupted.recovery_candidates());
  REQUIRE_EQ(candidates.size(), 3, "complete orphan was not offered for recovery");
  std::ranges::sort(candidates, {},
                    [](const LoadedGeneration& item) { return item.snapshot.revision; });
  REQUIRE_EQ(candidates[0].snapshot.revision, 1,
             "recovery choices lost their revisions, kind or ancestry");
  REQUIRE_EQ(candidates[1].snapshot.revision, 2,
             "recovery choices lost their revisions, kind or ancestry");
  REQUIRE_EQ(candidates[2].snapshot.revision, 3,
             "recovery choices lost their revisions, kind or ancestry");
  REQUIRE_EQ(candidates[2].kind, SaveKind::explicit_save,
             "recovery choices lost their revisions, kind or ancestry");
  REQUIRE_EQ(candidates[2].parent, explicit_save.generation,
             "recovery choices lost their revisions, kind or ancestry");

  interrupted = take(ProjectStore::open_reader(root));
  auto reopened = take(ProjectStore::open_writer(root, false));
  const auto committed = take(reopened.save(snapshot(3), SaveKind::explicit_save));
  REQUIRE_EQ(committed.status, SaveStatus::committed,
             "later save lost the earlier recovery choices or advanced AUTOSAVE");
  REQUIRE_EQ(take(reopened.load_current()).generation, committed.generation,
             "later save lost the earlier recovery choices or advanced AUTOSAVE");
  REQUIRE_EQ(take(reopened.load_autosave()).generation, autosave.generation,
             "later save lost the earlier recovery choices or advanced AUTOSAVE");
  REQUIRE_EQ(take(reopened.recovery_candidates()).size(), 4,
             "later save lost the earlier recovery choices or advanced AUTOSAVE");
}

void corrupt_autosave_blocks_new_save_but_preserves_recovery() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto saved = take(writer.save(snapshot(1), SaveKind::explicit_save));
  const auto autosaved = take(writer.save(snapshot(2), SaveKind::autosave));
  const auto pointer = root / "state" / "AUTOSAVE";
  {
    std::ofstream out(pointer, std::ios::trunc);
    out << "damaged autosave pointer";
  }
  const auto rejected = writer.save(snapshot(3), SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "save hid a damaged autosave pointer or replaced the last explicit save");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::corrupt_data,
             "save hid a damaged autosave pointer or replaced the last explicit save");
  REQUIRE_EQ(take(writer.load_current()).generation, saved.generation,
             "save hid a damaged autosave pointer or replaced the last explicit save");
  const auto choices = take(writer.recovery_candidates());
  REQUIRE_EQ(choices.size(), 2,
             "damaged autosave pointer hid its complete generation from recovery");
  REQUIRE_TRUE(std::ranges::any_of(choices,
                                   [&](const auto& item) {
                                     return item.generation == autosaved.generation &&
                                            item.snapshot.revision == 2;
                                   }),
               "damaged autosave pointer hid its complete generation from recovery");
  REQUIRE_EQ(take(writer.adopt_generation(autosaved.generation, SaveKind::autosave)).status,
             SaveStatus::committed, "validated autosave could not repair its damaged pointer");
  REQUIRE_EQ(take(writer.load_autosave()).generation, autosaved.generation,
             "validated autosave could not repair its damaged pointer");
  REQUIRE_EQ(take(writer.save(snapshot(3), SaveKind::explicit_save)).status, SaveStatus::committed,
             "repaired autosave pointer did not permit a later save");
  REQUIRE_EQ(take(writer.load_current()).snapshot.revision, 3,
             "repaired autosave pointer did not permit a later save");
}

void recovery_adoption_validates_choice_and_durability() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto first = take(writer.save(snapshot(), SaveKind::explicit_save));
  const auto autosave = take(writer.save(snapshot(2), SaveKind::autosave));
  const auto later = take(writer.save(snapshot(3), SaveKind::explicit_save));
  std::ofstream(root / "state" / "CURRENT", std::ios::trunc) << "damaged pointer";
  REQUIRE_TRUE(!writer.load_current(), "damaged explicit pointer hid recovery choices");
  REQUIRE_EQ(take(writer.recovery_candidates()).size(), 3,
             "damaged explicit pointer hid recovery choices");
  auto wrong_kind = writer.adopt_generation(autosave.generation, SaveKind::explicit_save);
  REQUIRE_TRUE(!wrong_kind, "autosave was adopted as an explicit save");
  REQUIRE_EQ(wrong_kind.error().code, foundation::ErrorCode::conflict,
             "autosave was adopted as an explicit save");
  auto invalid = writer.adopt_generation("../CURRENT", SaveKind::explicit_save);
  REQUIRE_TRUE(!invalid, "invalid recovery generation path accepted");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "invalid recovery generation path accepted");
  auto reader = take(ProjectStore::open_reader(root));
  auto read_only = reader.adopt_generation(first.generation, SaveKind::explicit_save);
  REQUIRE_TRUE(!read_only, "read-only recovery rewrote CURRENT");
  REQUIRE_EQ(read_only.error().code, foundation::ErrorCode::conflict,
             "read-only recovery rewrote CURRENT");
  const auto adopted = take(writer.adopt_generation(first.generation, SaveKind::explicit_save));
  REQUIRE_EQ(adopted.status, SaveStatus::committed, "recovery choice did not restore normal saves");
  REQUIRE_EQ(take(writer.load_current()).generation, first.generation,
             "recovery choice did not restore normal saves");
  REQUIRE_EQ(take(writer.save(snapshot(4), SaveKind::explicit_save)).status, SaveStatus::committed,
             "recovery choice did not restore normal saves");
  require(fs::exists(root / "state" / "generations" / later.generation),
          "adoption discarded the formerly current generation");

  Directory foreign_temp;
  const auto foreign_root = foreign_temp.path() / "foreign";
  auto foreign_writer = take(ProjectStore::open_writer(foreign_root, true));
  auto foreign_state = snapshot(5);
  foreign_state.id = id<project::ProjectId>(99);
  const auto foreign = take(foreign_writer.save(foreign_state, SaveKind::explicit_save));
  fs::copy(foreign_root / "state" / "generations" / foreign.generation,
           root / "state" / "generations" / foreign.generation, fs::copy_options::recursive);
  auto rejected = writer.adopt_generation(foreign.generation, SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "recovery selected a different project's generation");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "recovery selected a different project's generation");
  std::ofstream(root / "state" / "generations" / later.generation / "manifest.sha256",
                std::ios::trunc)
      << std::string(64, '0') << '\n';
  auto tampered = writer.adopt_generation(later.generation, SaveKind::explicit_save);
  REQUIRE_TRUE(!tampered, "recovery adopted a generation with a damaged checksum");
  REQUIRE_EQ(tampered.error().code, foundation::ErrorCode::corrupt_data,
             "recovery adopted a generation with a damaged checksum");
}

void adoption_interruption_does_not_hide_pointer_state() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto first = take(writer.save(snapshot(), SaveKind::explicit_save));
  const auto later = take(writer.save(snapshot(2), SaveKind::explicit_save));
  writer = take(ProjectStore::open_reader(root));

  auto before = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::pointer_written; }));
  auto interrupted = before.adopt_generation(first.generation, SaveKind::explicit_save);
  REQUIRE_TRUE(!interrupted, "pre-publication recovery fault changed CURRENT");
  REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::io_failure,
             "pre-publication recovery fault changed CURRENT");
  REQUIRE_EQ(take(before.load_current()).generation, later.generation,
             "pre-publication recovery fault changed CURRENT");
  before = take(ProjectStore::open_reader(root));

  auto after = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::pointer_published; }));
  const auto uncertain = take(after.adopt_generation(first.generation, SaveKind::explicit_save));
  REQUIRE_EQ(uncertain.status, SaveStatus::uncertain,
             "post-publication recovery fault claimed durable success or hid the new pointer");
  REQUIRE_EQ(take(after.load_current()).generation, first.generation,
             "post-publication recovery fault claimed durable success or hid the new pointer");
}

void corrupt_pointer_and_manifest() {
  Directory temp;
  auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  auto saved = take(store.save(snapshot(), SaveKind::explicit_save));
  {
    std::ofstream out(root / "state" / "CURRENT", std::ios::trunc);
    out << "not json";
  }
  auto missing = store.load_current();
  REQUIRE_TRUE(!missing, "corrupt CURRENT incorrectly treated as missing");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::corrupt_data,
             "corrupt CURRENT incorrectly treated as missing");
  REQUIRE_EQ(take(store.recovery_candidates()).size(), 1,
             "complete generation not offered after pointer corruption");
  {
    std::ofstream out(root / "state" / "generations" / saved.generation / "manifest.json",
                      std::ios::app);
    out << "tampered";
  }
  require(take(store.recovery_candidates()).empty(), "tampered generation offered for recovery");
}

void state_directory_replacement_cannot_commit_a_detached_save() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(snapshot(), SaveKind::explicit_save));
  writer = take(ProjectStore::open_reader(root)); // release writer lock

  bool swapped = false;
  writer = take(ProjectStore::open_writer(root, false, [&](SaveStage stage) {
    if (stage != SaveStage::pointer_written)
      return false;
    const auto displaced = root / "state-displaced";
    fs::rename(root / "state", displaced);
    fs::create_directories(root / "state" / "generations");
    fs::copy(displaced / "generations" / baseline.generation,
             root / "state" / "generations" / baseline.generation, fs::copy_options::recursive);
    fs::copy_file(displaced / "CURRENT", root / "state" / "CURRENT");
    swapped = true;
    return false;
  }));
  const auto result = writer.save(snapshot(2), SaveKind::explicit_save);
  REQUIRE_TRUE(swapped, "detached state directory was reported as a committed save");
  REQUIRE_TRUE(!result, "detached state directory was reported as a committed save");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::conflict,
             "detached state directory was reported as a committed save");
  const auto reopened = take(ProjectStore::open_reader(root));
  REQUIRE_EQ(take(reopened.load_current()).generation, baseline.generation,
             "failed detached save replaced the reopened project");
}

void replaced_writer_lock_cannot_publish_a_generation() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(snapshot(), SaveKind::explicit_save));
  writer = take(ProjectStore::open_reader(root)); // release initial lock

  bool replaced = false;
  writer = take(ProjectStore::open_writer(root, false, [&](SaveStage stage) {
    if (stage != SaveStage::pointer_written)
      return false;
    fs::remove(root / "state" / "WRITER.lock");
    std::ofstream(root / "state" / "WRITER.lock") << "replacement";
    replaced = true;
    return false;
  }));
  const auto interrupted = writer.save(snapshot(2), SaveKind::explicit_save);
  REQUIRE_TRUE(replaced, "writer with a displaced lock published a generation");
  REQUIRE_TRUE(!interrupted, "writer with a displaced lock published a generation");
  REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::conflict,
             "writer with a displaced lock published a generation");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "failed save advanced CURRENT after lock replacement");

  // The old fd is still locked, but a new writer can lock the replacement inode.
  auto successor = take(ProjectStore::open_writer(root, false));
  REQUIRE_EQ(take(successor.save(snapshot(2), SaveKind::explicit_save)).status,
             SaveStatus::committed,
             "replacement lock prevented the successor from saving the visible project");
  auto reopened = take(ProjectStore::open_reader(root));
  REQUIRE_EQ(take(reopened.load_current()).snapshot.revision, 2,
             "successor save did not reopen after lock replacement");
}

void two_writers_and_symlinks() {
  Directory temp;
  auto root = temp.path() / "project";
  auto first = take(ProjectStore::open_writer(root, true));
  auto second = ProjectStore::open_writer(root, false);
  REQUIRE_TRUE(!second, "second writer not rejected");
  REQUIRE_EQ(second.error().code, foundation::ErrorCode::conflict, "second writer not rejected");
  auto alias = temp.path() / "alias";
  fs::create_directory_symlink(root, alias);
  require(!ProjectStore::open_reader(alias), "symlink project root accepted");
  first = take(ProjectStore::open_reader(root));
  fs::remove(root / "state" / "generations");
  fs::create_directory_symlink(temp.path(), root / "state" / "generations");
  require(!ProjectStore::open_reader(root), "symlink generations directory accepted");
}

void foreign_project_and_generation_collision() {
  Directory temp;
  auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto saved = take(store.save(snapshot(), SaveKind::explicit_save));
  auto foreign = snapshot(2);
  foreign.id = id<project::ProjectId>(99);
  auto rejected = store.save(foreign, SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "foreign project ID accepted");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict, "foreign project ID accepted");
  REQUIRE_EQ(take(store.load_current()).generation, saved.generation,
             "foreign project replaced CURRENT");
  store = take(ProjectStore::open_reader(root));
  auto collision = take(ProjectStore::open_writer(root, false, [root](SaveStage stage) {
    if (stage != SaveStage::generation_flushed)
      return false;
    const auto generations = root / "state" / "generations";
    for (const auto& entry : fs::directory_iterator(generations)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with(".tmp-")) {
        const auto target = generations / name.substr(5);
        fs::create_directory(target);
        std::ofstream(target / "sentinel") << "untouched";
        return false;
      }
    }
    throw std::runtime_error("temporary generation missing");
  }));
  auto failed = collision.save(snapshot(2), SaveKind::explicit_save);
  require(!failed, "generation collision accepted");
  REQUIRE_EQ(take(collision.load_current()).generation, saved.generation,
             "generation collision advanced CURRENT");
}

void malformed_writer_lock_is_bounded() {
  Directory temp;
  auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  store = take(ProjectStore::open_reader(root));
  fs::remove(root / "state" / "WRITER.lock");
  REQUIRE_EQ(::mkfifo((root / "state" / "WRITER.lock").c_str(), 0600), 0,
             "could not create FIFO fixture");
  auto malformed = ProjectStore::open_writer(root, false);
  REQUIRE_TRUE(!malformed, "FIFO writer lock accepted");
  REQUIRE_EQ(malformed.error().code, foundation::ErrorCode::corrupt_data,
             "FIFO writer lock accepted");
}

void pointer_target_and_path_validation() {
  Directory temp;
  auto root = temp.path() / "project";
  require(!ProjectStore::open_reader(root), "missing project root accepted");
  auto store = take(ProjectStore::open_writer(root, true));
  auto initial = take(store.save(snapshot(), SaveKind::explicit_save));
  auto invalid = snapshot(2);
  invalid.tracks[0].clips[1].placement = project::ElapsedInterval{{0}, {100}};
  require(!store.save(invalid, SaveKind::explicit_save), "overlapping project saved");
  REQUIRE_EQ(take(store.load_current()).generation, initial.generation,
             "invalid snapshot changed CURRENT");
  auto excessive = snapshot(2);
  excessive.tracks.clear();
  for (std::size_t i = 0; i <= detail::max_tracks; ++i) {
    excessive.tracks.emplace_back(id<project::TrackId>(100 + i));
  }
  auto limited = store.save(excessive, SaveKind::explicit_save);
  REQUIRE_TRUE(!limited, "over-limit snapshot encoded before rejection");
  REQUIRE_EQ(limited.error().code, foundation::ErrorCode::resource_exhausted,
             "over-limit snapshot encoded before rejection");
  REQUIRE_EQ(take(store.load_current()).generation, initial.generation,
             "over-limit snapshot changed CURRENT");
  auto excessive_clips = snapshot(2);
  excessive_clips.tracks[0].clips.assign(detail::max_clips + 1,
                                         excessive_clips.tracks[0].clips.front());
  auto clip_limit = store.save(excessive_clips, SaveKind::explicit_save);
  REQUIRE_TRUE(!clip_limit, "over-limit clip count accepted");
  REQUIRE_EQ(clip_limit.error().code, foundation::ErrorCode::resource_exhausted,
             "over-limit clip count accepted");
  const auto current = root / "state" / "CURRENT";
  {
    std::ofstream out(current, std::ios::trunc);
    out << detail::encode_pointer(initial.generation, std::string(64, '0'));
  }
  auto wrong_hash = store.load_current();
  REQUIRE_TRUE(!wrong_hash, "pointer checksum mismatch accepted");
  REQUIRE_EQ(wrong_hash.error().code, foundation::ErrorCode::corrupt_data,
             "pointer checksum mismatch accepted");
  REQUIRE_EQ(take(store.recovery_candidates()).size(), 1,
             "valid generation hidden after pointer checksum mismatch");
  store = take(ProjectStore::open_reader(root));
  fs::remove(current);
  fs::create_symlink(temp.path() / "outside", current);
  require(!store.load_current(), "symlink CURRENT accepted");
  fs::remove(current);
  const auto generation = root / "state" / "generations" / initial.generation;
  fs::remove(generation / "manifest.json");
  {
    std::ofstream out(current);
    out << detail::encode_pointer(initial.generation, std::string(64, '0'));
  }
  auto incomplete = store.load_current();
  REQUIRE_TRUE(!incomplete, "missing manifest under CURRENT classified as missing pointer");
  REQUIRE_EQ(incomplete.error().code, foundation::ErrorCode::corrupt_data,
             "missing manifest under CURRENT classified as missing pointer");
  require(take(store.recovery_candidates()).empty(), "incomplete generation offered for recovery");
}

void no_follow_project_layout() {
  Directory temp;
  auto root = temp.path() / "project";
  fs::create_directory(root);
  fs::create_directory_symlink(temp.path(), root / "assets");
  require(!ProjectStore::open_writer(root, true), "symlink assets directory accepted");
  fs::remove(root / "assets");
  auto store = take(ProjectStore::open_writer(root, true));
  store = take(ProjectStore::open_reader(root));
  fs::remove(root / "state" / "WRITER.lock");
  fs::create_symlink(temp.path() / "outside", root / "state" / "WRITER.lock");
  require(!ProjectStore::open_writer(root, false), "symlink writer lock accepted");
}

void orphan_autosave_keeps_project_identity() {
  Directory temp;
  auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  auto autosaved = take(store.save(snapshot(), SaveKind::autosave));
  REQUIRE_EQ(autosaved.status, SaveStatus::committed, "initial autosave failed");
  REQUIRE_TRUE(!store.load_current(), "autosave unexpectedly created CURRENT");
  REQUIRE_EQ(store.load_current().error().code, foundation::ErrorCode::not_found,
             "autosave unexpectedly created CURRENT");
  auto foreign = snapshot(2);
  foreign.id = id<project::ProjectId>(99);
  auto rejected = store.save(foreign, SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "foreign project replaced orphan autosave identity");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "foreign project replaced orphan autosave identity");
  REQUIRE_EQ(take(store.load_autosave()).generation, autosaved.generation,
             "foreign project changed AUTOSAVE");
}

void held_parent_survives_ancestor_swap() {
  Directory temp;
  const auto parent = temp.path() / "parent";
  const auto relocated = temp.path() / "relocated";
  const auto root = parent / "project";
  fs::create_directory(parent);
  auto store = take(ProjectStore::open_writer(root, true, [parent, relocated](SaveStage stage) {
    if (stage != SaveStage::root_parent_flushed)
      return false;
    fs::rename(parent, relocated);
    fs::create_directory(parent);
    fs::create_directory(parent / "project");
    return false;
  }));
  REQUIRE_EQ(take(store.save(snapshot(), SaveKind::explicit_save)).status, SaveStatus::committed,
             "save through held root failed");
  require(fs::exists(relocated / "project" / "state" / "CURRENT"),
          "held parent lost original project");
  require(!fs::exists(root / "state"), "path-swapped project received storage writes");
}

void swapped_root_symlink_is_rejected() {
  Directory temp;
  const auto parent = temp.path() / "parent";
  const auto root = parent / "project";
  const auto moved = parent / "moved";
  const auto outside = temp.path() / "outside";
  fs::create_directory(parent);
  fs::create_directory(outside);
  auto opened = ProjectStore::open_writer(root, true, [root, moved, outside](SaveStage stage) {
    if (stage != SaveStage::root_parent_flushed)
      return false;
    fs::rename(root, moved);
    fs::create_directory_symlink(outside, root);
    return false;
  });
  require(!opened, "swapped symlink project root accepted");
  require(!fs::exists(outside / "state"), "storage escaped to swapped symlink target");
}

void recovery_scan_is_bounded() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto generations = root / "state" / "generations";
  const auto valid = take(store.save(snapshot(), SaveKind::explicit_save));
  for (int i = 0; i < 1100; ++i) {
    char name[33]{};
    std::snprintf(name, sizeof(name), "%032x", i);
    const auto directory = generations / name;
    fs::create_directory(directory);
    const auto manifest = detail::encode_manifest(snapshot(static_cast<std::uint64_t>(i) + 2), name,
                                                  std::nullopt, SaveKind::explicit_save);
    std::ofstream(directory / "manifest.json") << manifest;
    std::ofstream(directory / "manifest.sha256") << sha256(manifest) << '\n';
  }
  const auto recovered = take(store.recovery_candidates());
  REQUIRE_EQ(recovered.size(), 256,
             "more than 1024 complete generations hid bounded recovery choices");
  REQUIRE_TRUE(
      std::ranges::all_of(recovered, [](const auto& item) { return item.snapshot.revision >= 2; }),
      "more than 1024 complete generations hid bounded recovery choices");
  REQUIRE_EQ(take(store.load_current()).generation, valid.generation,
             "more than 1024 complete generations hid bounded recovery choices");
  for (int i = 0; i < 7100; ++i) {
    std::ofstream(generations / (".junk-" + std::to_string(i))) << 'x';
  }
  auto candidates = store.recovery_candidates();
  REQUIRE_TRUE(!candidates, "invalid entries bypassed recovery scan limit");
  REQUIRE_EQ(candidates.error().code, foundation::ErrorCode::resource_exhausted,
             "invalid entries bypassed recovery scan limit");
  auto pruning = store.prune_generations();
  REQUIRE_TRUE(!pruning, "oversized generation namespace bypassed retention scan limit");
  REQUIRE_EQ(pruning.error().code, foundation::ErrorCode::resource_exhausted,
             "oversized generation namespace bypassed retention scan limit");
}

void durable_save_prunes_only_unreferenced_generations() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  REQUIRE_EQ(take(writer.prune_generations()), 0, "empty project had generations to prune");
  const auto incomplete = root / "state" / "generations" / std::string(32, 'e');
  fs::create_directory(incomplete);
  const auto first = take(writer.save(snapshot(), SaveKind::explicit_save));
  REQUIRE_OK(writer.name_current_revision("First"));
  const auto autosave = take(writer.save(snapshot(2), SaveKind::autosave));
  std::vector<std::string> later;
  for (std::uint64_t revision = 3; revision <= 14; ++revision) {
    const auto saved = take(writer.save(snapshot(revision), SaveKind::explicit_save));
    REQUIRE_EQ(saved.status, SaveStatus::committed, "durable save reported retention failure");
    REQUIRE_TRUE(!saved.maintenance_warning, "durable save reported retention failure");
    later.push_back(saved.generation);
    if (revision == 3)
      std::ofstream(root / "state" / "generations" / saved.generation / "user-note")
          << "preserve me";
  }
  const auto generations = root / "state" / "generations";
  std::size_t count = 0;
  for (const auto& item : fs::directory_iterator(generations))
    if (item.is_directory())
      ++count;
  REQUIRE_EQ(count, 14, "retention removed a protected generation or kept unbounded history");
  REQUIRE_TRUE(fs::exists(incomplete),
               "retention removed a protected generation or kept unbounded history");
  REQUIRE_TRUE(fs::exists(generations / first.generation),
               "retention removed a protected generation or kept unbounded history");
  REQUIRE_TRUE(fs::exists(generations / autosave.generation),
               "retention removed a protected generation or kept unbounded history");
  REQUIRE_TRUE(fs::exists(generations / later.front() / "user-note"),
               "retention removed a protected generation or kept unbounded history");
  REQUIRE_TRUE(fs::exists(generations / later.back()),
               "retention removed a protected generation or kept unbounded history");
  REQUIRE_EQ(take(writer.load_current()).generation, later.back(),
             "retention removed a protected generation or kept unbounded history");
  REQUIRE_EQ(take(writer.load_autosave()).generation, autosave.generation,
             "retention removed a protected generation or kept unbounded history");
  REQUIRE_EQ(take(writer.load_named_revision("First")).generation, first.generation,
             "retention removed a protected generation or kept unbounded history");
  REQUIRE_EQ(take(writer.recovery_candidates()).size(), 13,
             "retained generations were not available for recovery");
  REQUIRE_EQ(take(writer.prune_generations()), 0,
             "retention removed a generation on an unchanged second pass");
  auto reader = take(ProjectStore::open_reader(root));
  auto read_only_prune = reader.prune_generations();
  REQUIRE_TRUE(!read_only_prune, "read-only store pruned project history");
  REQUIRE_EQ(read_only_prune.error().code, foundation::ErrorCode::conflict,
             "read-only store pruned project history");

  Directory foreign_temp;
  const auto foreign_root = foreign_temp.path() / "foreign";
  auto foreign_writer = take(ProjectStore::open_writer(foreign_root, true));
  auto foreign_state = snapshot();
  foreign_state.id = id<project::ProjectId>(99);
  const auto foreign = take(foreign_writer.save(foreign_state, SaveKind::explicit_save));
  fs::copy(foreign_root / "state" / "generations" / foreign.generation,
           generations / foreign.generation, fs::copy_options::recursive);
  REQUIRE_EQ(take(writer.prune_generations()), 0,
             "retention deleted an unrelated project's generation");
  REQUIRE_TRUE(fs::exists(generations / foreign.generation),
               "retention deleted an unrelated project's generation");

  std::ofstream(root / "state" / "revisions" / "README") << "unknown entry";
  const auto cautious = take(writer.save(snapshot(15), SaveKind::explicit_save));
  REQUIRE_EQ(cautious.status, SaveStatus::committed,
             "retention warning hid or invalidated a durable save");
  REQUIRE_TRUE(cautious.maintenance_warning, "retention warning hid or invalidated a durable save");
  REQUIRE_EQ(take(writer.load_current()).generation, cautious.generation,
             "retention warning hid or invalidated a durable save");
  std::ofstream(root / "state" / "CURRENT", std::ios::trunc) << "damaged pointer";
  const auto refused = writer.prune_generations();
  REQUIRE_TRUE(!refused, "corrupt CURRENT allowed retention to delete recovery history");
  REQUIRE_EQ(refused.error().code, foundation::ErrorCode::corrupt_data,
             "corrupt CURRENT allowed retention to delete recovery history");
  REQUIRE_TRUE(fs::exists(generations / cautious.generation),
               "corrupt CURRENT allowed retention to delete recovery history");
  REQUIRE_TRUE(fs::exists(generations / first.generation),
               "corrupt CURRENT allowed retention to delete recovery history");
}

void autosave_only_generation_is_retained() {
  Directory temp;
  auto writer = take(ProjectStore::open_writer(temp.path() / "project", true));
  const auto saved = take(writer.save(snapshot(), SaveKind::autosave));
  REQUIRE_EQ(saved.status, SaveStatus::committed,
             "retention lost an autosave before the first explicit save");
  REQUIRE_TRUE(!saved.maintenance_warning,
               "retention lost an autosave before the first explicit save");
  REQUIRE_EQ(take(writer.prune_generations()), 0,
             "retention lost an autosave before the first explicit save");
  REQUIRE_EQ(take(writer.load_autosave()).generation, saved.generation,
             "retention lost an autosave before the first explicit save");
}

void per_pointer_revisions_never_regress() {
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "project", true));
  REQUIRE_OK(store.save(snapshot(3), SaveKind::explicit_save));
  auto old_current = store.save(snapshot(2), SaveKind::explicit_save);
  REQUIRE_TRUE(!old_current, "CURRENT revision regressed");
  REQUIRE_EQ(old_current.error().code, foundation::ErrorCode::conflict,
             "CURRENT revision regressed");
  REQUIRE_OK(store.save(snapshot(5), SaveKind::autosave));
  auto old_autosave = store.save(snapshot(4), SaveKind::autosave);
  REQUIRE_TRUE(!old_autosave, "AUTOSAVE revision regressed");
  REQUIRE_EQ(old_autosave.error().code, foundation::ErrorCode::conflict,
             "AUTOSAVE revision regressed");
  REQUIRE_EQ(take(store.load_current()).snapshot.revision, 3,
             "rejected revision changed a pointer");
  REQUIRE_EQ(take(store.load_autosave()).snapshot.revision, 5,
             "rejected revision changed a pointer");
  REQUIRE_OK(store.save(snapshot(4), SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.revision, 4,
             "explicit recovery choice was blocked by newer ignored autosave");
}

void corrupt_staged_checksum_never_publishes() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(store.save(snapshot(), SaveKind::explicit_save));
  store = take(ProjectStore::open_reader(root));
  auto faulted = take(ProjectStore::open_writer(root, false, [root](SaveStage stage) {
    if (stage != SaveStage::generation_flushed)
      return false;
    for (const auto& entry : fs::directory_iterator(root / "state" / "generations")) {
      if (entry.path().filename().string().starts_with(".tmp-")) {
        std::ofstream(entry.path() / "manifest.sha256", std::ios::trunc)
            << std::string(64, '0') << '\n';
        return false;
      }
    }
    throw std::runtime_error("temporary generation missing");
  }));
  auto rejected = faulted.save(snapshot(2), SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "tampered staged checksum published");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::corrupt_data,
             "tampered staged checksum published");
  REQUIRE_EQ(take(faulted.load_current()).generation, baseline.generation,
             "tampered staged checksum advanced CURRENT");
}

void foreign_autosave_blocks_explicit_save() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto original = take(store.save(snapshot(), SaveKind::explicit_save));
  Directory other;
  const auto other_root = other.path() / "foreign";
  auto foreign_store = take(ProjectStore::open_writer(other_root, true));
  auto foreign = snapshot(2);
  foreign.id = id<project::ProjectId>(99);
  const auto foreign_generation = take(foreign_store.save(foreign, SaveKind::autosave));
  fs::copy(other_root / "state" / "generations" / foreign_generation.generation,
           root / "state" / "generations" / foreign_generation.generation,
           fs::copy_options::recursive);
  fs::copy_file(other_root / "state" / "AUTOSAVE", root / "state" / "AUTOSAVE");
  auto rejected = store.save(snapshot(3), SaveKind::explicit_save);
  REQUIRE_TRUE(!rejected, "foreign AUTOSAVE identity ignored during explicit save");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "foreign AUTOSAVE identity ignored during explicit save");
  auto pruning = store.prune_generations();
  REQUIRE_TRUE(!pruning, "conflicting project pointers allowed history pruning");
  REQUIRE_EQ(pruning.error().code, foundation::ErrorCode::conflict,
             "conflicting project pointers allowed history pruning");
  REQUIRE_TRUE(fs::exists(root / "state" / "generations" / original.generation),
               "conflicting project pointers allowed history pruning");
  REQUIRE_TRUE(fs::exists(root / "state" / "generations" / foreign_generation.generation),
               "conflicting project pointers allowed history pruning");
  REQUIRE_EQ(take(store.load_current()).generation, original.generation,
             "foreign AUTOSAVE changed CURRENT");
}

void recovery_manifest_budget_is_bounded() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto generations = root / "state" / "generations";
  for (int i = 0; i < 5; ++i) {
    const auto name = std::string(31, 'a') + static_cast<char>('0' + i);
    const auto directory = generations / name;
    fs::create_directory(directory);
    std::ofstream(directory / "manifest.json") << 'x';
    fs::resize_file(directory / "manifest.json", std::uintmax_t{15} * 1024U * 1024U);
  }
  auto candidates = store.recovery_candidates();
  REQUIRE_TRUE(!candidates, "invalid candidate manifests bypassed total read budget");
  REQUIRE_EQ(candidates.error().code, foundation::ErrorCode::resource_exhausted,
             "invalid candidate manifests bypassed total read budget");
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
  const auto at = text.find(from);
  REQUIRE_NE(at, std::string::npos, "fixture token missing: " + std::string(from));
  text.replace(at, from.size(), to);
  return text;
}

void bounded_manifest_codec() {
  const std::string generation(32, 'a');
  const auto valid =
      detail::encode_manifest(snapshot(), generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_EQ(take(detail::decode_manifest(valid)).snapshot.tracks.size(), 1,
             "canonical manifest rejected");
  REQUIRE_EQ(valid,
             detail::encode_manifest(snapshot(), generation, std::nullopt, SaveKind::explicit_save),
             "writer output is not deterministic");
  struct Case {
    std::string_view from;
    std::string_view to;
    foundation::ErrorCode code;
  };
  const std::vector<Case> cases{
      {"\"schema\"", "\"wrong\"", foundation::ErrorCode::corrupt_data},
      {"\"major\":1", "\"major\":2", foundation::ErrorCode::unsupported},
      {"\"timing_version\":1", "\"timing_version\":2", foundation::ErrorCode::unsupported},
      {"\"generation\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"", "\"generation\":\"../bad\"",
       foundation::ErrorCode::corrupt_data},
      {"\"parent\":null", "\"parent\":\"../bad\"", foundation::ErrorCode::corrupt_data},
      {"\"id\":\"00000000-0000-0000-0000-000000000001\"", "\"id\":\"bad\"",
       foundation::ErrorCode::corrupt_data},
      {"\"kind\":\"explicit\"", "\"kind\":\"bad\"", foundation::ErrorCode::corrupt_data},
      {"\"shape\":\"linear_bpm\"", "\"shape\":\"bad\"", foundation::ErrorCode::corrupt_data},
      {"\"bpm\":120", "\"bpm\":oops", foundation::ErrorCode::corrupt_data},
      {"\"events\":[{\"bpm\":120.0,\"shape\":\"linear_bpm\",\"tick\":0}",
       "\"events\":[{\"bpm\":120.0,\"shape\":\"linear_bpm\",\"tick\":oops}",
       foundation::ErrorCode::corrupt_data},
      {"\"numerator\":4", "\"numerator\":256", foundation::ErrorCode::corrupt_data},
      {"\"anchor\":\"musical\"", "\"anchor\":\"bad\"", foundation::ErrorCode::corrupt_data},
      {"\"settings_generation\":7", "\"settings_generation\":0",
       foundation::ErrorCode::corrupt_data},
      {"\"settings_generation\":7", "\"settings_generation\":7.0",
       foundation::ErrorCode::corrupt_data},
      {"\"sample_rate\":48000", "\"sample_rate\":123", foundation::ErrorCode::corrupt_data},
      {"\"sample_rate\":48000", "\"sample_rate\":48000.0", foundation::ErrorCode::corrupt_data},
      {"\"source_end\":48000", "\"source_end\":0", foundation::ErrorCode::corrupt_data},
      {"\"source_start\":0", "\"source_start\":18446744073709551616",
       foundation::ErrorCode::corrupt_data},
      {"\"parent\":null", "\"parent\":[]", foundation::ErrorCode::corrupt_data},
      {"\"bpm\":120.0", "\"bpm\":1e999", foundation::ErrorCode::corrupt_data},
  };
  for (const auto& item : cases) {
    const auto parsed = detail::decode_manifest(replaced(valid, item.from, item.to));
    REQUIRE_TRUE(!parsed, "malformed manifest accepted");
    REQUIRE_EQ(parsed.error().code, item.code, "malformed manifest accepted");
  }
  require(!detail::decode_manifest(valid + "garbage"), "trailing bytes accepted");
  const auto reordered =
      replaced(valid, "\"generation\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"kind\":\"explicit\"",
               "\"kind\":\"explicit\",\"generation\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"");
  REQUIRE_EQ(take(detail::decode_manifest(reordered)).snapshot.revision, 1,
             "reordered JSON fields rejected");
  const auto escaped =
      replaced(valid, "\"generation\":\"aaaaaaaa", "\"generation\":\"aaa\\u0061aaaa");
  REQUIRE_EQ(take(detail::decode_manifest(escaped)).generation, generation,
             "valid escaped JSON string rejected");
  require(!detail::decode_manifest(replaced(valid, "\"kind\":\"explicit\"",
                                            "\"kind\":\"explicit\",\"kind\":\"autosave\"")),
          "duplicate top-level JSON key accepted");
  require(!detail::decode_manifest(replaced(valid, "\"kind\":\"explicit\"",
                                            "\"kind\":\"explicit\",\"k\\u0069nd\":\"autosave\"")),
          "escaped duplicate JSON key accepted");
  require(!detail::decode_manifest(replaced(valid, "\"schema\":{\"major\":1,\"minor\":47}",
                                            "\"schema\":{\"major\":1,\"major\":1,\"minor\":47}")),
          "duplicate nested JSON key accepted");
  require(!detail::decode_manifest(
              replaced(valid, "\"kind\":\"explicit\"", "\"kind\":\"explicit\",\"unknown\":0")),
          "unknown JSON field accepted");
  require(!detail::decode_manifest(
              replaced(valid, "\"kind\":\"explicit\"", "\"kind\":\"exp\\qlicit\"")),
          "malformed JSON escape accepted");
  require(!detail::decode_manifest(
              replaced(valid, "\"kind\":\"explicit\"", "\"kind\":\"bad\\nvalue\"")),
          "escaped string accepted by canonical parser");
  require(
      !detail::decode_manifest(replaced(valid, "\"kind\":\"explicit\"", "\"kind\":\"unfinished")),
      "unterminated string accepted");
  for (const auto field : {"\"tempo\"", "\"meter\""}) {
    auto empty_events = valid;
    const auto section = empty_events.find(field);
    const auto array = empty_events.find('[', section);
    const auto close = empty_events.find(']', array);
    REQUIRE_NE(section, std::string::npos, "event fixture malformed");
    REQUIRE_NE(array, std::string::npos, "event fixture malformed");
    REQUIRE_NE(close, std::string::npos, "event fixture malformed");
    empty_events.replace(array, close - array + 1, "[]");
    require(!detail::decode_manifest(empty_events), "empty timing map accepted");
  }
  require(!detail::decode_manifest(std::string(16U * 1024U * 1024U + 1U, 'x')),
          "oversize manifest accepted");
  require(!detail::decode_pointer("{\"generation\":\"../bad\",\"sha256\":\"abc\"}"),
          "unsafe pointer accepted");
  const auto hash = std::string(64, 'b');
  REQUIRE_EQ(take(detail::decode_pointer("{\"sha256\":\"" + hash + "\",\"generation\":\"" +
                                         generation + "\"}")),
             std::pair(generation, hash), "reordered pointer rejected");
  require(!detail::decode_pointer("{\"generation\":\"" + generation + "\",\"generation\":\"" +
                                  generation + "\",\"sha256\":\"" + hash + "\"}"),
          "duplicate pointer key accepted");
  require(!detail::decode_pointer(std::string(257, ' ')), "oversize pointer accepted");
  auto all_shapes = snapshot();
  all_shapes.tempo = take(timeline::TempoMap::create(
      {{{0}, 120.0, timeline::TempoShape::step},
       {{timeline::ticks_per_quarter}, 121.0, timeline::TempoShape::linear_bpm},
       {{timeline::ticks_per_quarter * 2}, 122.0, timeline::TempoShape::ease_in},
       {{timeline::ticks_per_quarter * 3}, 123.0, timeline::TempoShape::ease_out},
       {{timeline::ticks_per_quarter * 4}, 124.0, timeline::TempoShape::ease_in_out},
       {{timeline::ticks_per_quarter * 5}, 125.0, timeline::TempoShape::step}},
      4));
  auto decoded = take(detail::decode_manifest(
      detail::encode_manifest(all_shapes, generation, std::nullopt, SaveKind::autosave)));
  REQUIRE_EQ(decoded.kind, SaveKind::autosave, "tempo shapes did not round trip");
  REQUIRE_EQ(decoded.snapshot.tempo.events().size(), 6, "tempo shapes did not round trip");
  auto multiple_tracks = snapshot();
  multiple_tracks.tracks.push_back({id<project::TrackId>(6), {}});
  auto multi = take(detail::decode_manifest(
      detail::encode_manifest(multiple_tracks, generation, std::nullopt, SaveKind::explicit_save)));
  REQUIRE_EQ(multi.snapshot.tracks.size(), 2, "multiple and empty tracks did not round trip");
  REQUIRE_TRUE(multi.snapshot.tracks[1].clips.empty(),
               "multiple and empty tracks did not round trip");
}

void write_wav(const fs::path& path) {
  // Two seconds of a ramp that wraps every 65,536 samples.
  constexpr std::uint32_t frames = 2 * oscillate::test::sample_rate_48k;
  std::vector<std::int16_t> ramp(frames);
  for (std::uint32_t i = 0; i < frames; ++i)
    ramp[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(i));
  oscillate::test::write_wav_pcm16(path, oscillate::test::sample_rate_48k,
                                   oscillate::test::mono_channels, ramp);
}

void midi_round_trip_and_validation() {
  constexpr auto quarter = timeline::ticks_per_quarter;
  auto state = snapshot();
  project::MidiClip musical{id<project::ClipId>(7),
                            project::MusicalInterval{{0}, {2 * quarter}},
                            {quarter},
                            true,
                            {{id<project::NoteId>(9), {quarter / 4}, {quarter / 2}, 60, 100}},
                            11};
  project::MidiClip empty_elapsed{id<project::ClipId>(8),
                                  project::ElapsedInterval{{72'000}, {96'000}},
                                  {quarter},
                                  false,
                                  {},
                                  12};
  state.tracks.emplace_back(
      id<project::TrackId>(6), std::vector<project::Clip>{}, project::TrackKind::instrument,
      project::NativeInstrumentPatch{"prism", 1, {{"prism.filter.cutoff", 900.0}}},
      std::vector<project::MidiClip>{musical, empty_elapsed});
  state.tracks.emplace_back(id<project::TrackId>(10), std::vector<project::Clip>{},
                            project::TrackKind::instrument);
  const auto generation = std::string(32, 'a');
  const auto manifest =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(manifest.find("\"minor\":47"), std::string::npos,
             "MIDI writer did not use the current schema");
  auto decoded = take(detail::decode_manifest(manifest));
  REQUIRE_EQ(decoded.snapshot.tracks, state.tracks,
             "MIDI, built-in synth or empty instrument track lost on decode");
  REQUIRE_TRUE(std::holds_alternative<std::monostate>(decoded.snapshot.tracks[2].instrument_source),
               "MIDI, built-in synth or empty instrument track lost on decode");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "project", true));
  REQUIRE_OK(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "MIDI/instrument state did not survive durable save and reopen");
  const auto recovery = take(store.recovery_candidates());
  REQUIRE_EQ(recovery.size(), 1, "recovery scan lost MIDI/instrument state");
  REQUIRE_EQ(recovery[0].snapshot.tracks, state.tracks, "recovery scan lost MIDI/instrument state");

  struct Bad {
    std::string_view from;
    std::string_view to;
  };
  const std::array<Bad, 10> malformed{{
      {"\"kind\":\"instrument\"", "\"kind\":\"unknown\""},
      {"\"engine\":\"prism\"", "\"engine\":7"},
      {"\"engine_version\":1", "\"engine_version\":-1"},
      {"\"value\":900.0", "\"value\":null"},
      {"\"pattern_length\":960000000", "\"pattern_length\":0"},
      {"\"loop\":true", "\"loop\":1"},
      {"\"velocity\":100", "\"velocity\":0"},
      {"\"key\":60", "\"key\":128"},
      {"\"velocity\":100", "\"velocity\":100,\"extra\":0"},
      {"\"settings_generation\":11", "\"settings_generation\":0"},
  }};
  for (const auto& bad : malformed) {
    auto result = detail::decode_manifest(replaced(manifest, bad.from, bad.to));
    REQUIRE_TRUE(!result, "malformed MIDI state accepted");
    REQUIRE_EQ(result.error().code, foundation::ErrorCode::corrupt_data,
               "malformed MIDI state accepted");
  }
  state.tracks[1].midi_clips[0].notes.push_back(state.tracks[1].midi_clips[0].notes[0]);
  require(!detail::decode_manifest(
              detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save)),
          "duplicate persistent NoteId accepted");
}

// Schema 1.44 (ADR-0063): a built-in synth document's regions, omitted
// when the document has none.
void native_regions_round_trip() {
  auto state = snapshot();
  project::NativeInstrumentPatch patch{"crate", 1, {}};
  patch.document.media = {{"sample", project::NativeBuiltinMedia{"break"}, 0}};
  patch.document.regions = {{0, "sample", 0, "kick", "Kick"}, {5, "sample", 4'800, "snare", ""}};
  state.tracks.emplace_back(id<project::TrackId>(998), std::vector<project::Clip>{},
                            project::TrackKind::instrument, patch);
  auto bare = patch;
  bare.document.regions.clear();
  state.tracks.emplace_back(id<project::TrackId>(999), std::vector<project::Clip>{},
                            project::TrackKind::instrument, bare);
  const auto encoded =
      detail::encode_manifest(state, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  auto current = Json::parse(encoded);
  auto& tracks = current["project"]["tracks"];
  const auto document_of = [&](std::size_t index) -> Json& {
    return tracks[index]["instrument_source"]["payload"]["document"];
  };
  const auto with_regions = tracks.size() - 2;
  REQUIRE_EQ(current["schema"]["minor"], 47,
             "regions are written as specified and omitted when empty");
  REQUIRE_EQ(document_of(with_regions)["regions"].size(), 2,
             "regions are written as specified and omitted when empty");
  REQUIRE_EQ(document_of(with_regions)["regions"][1],
             (Json{{"slot", 5},
                   {"media", "sample"},
                   {"start_frame", 4'800},
                   {"tag", "snare"},
                   {"name", ""}}),
             "regions are written as specified and omitted when empty");
  REQUIRE_TRUE(!document_of(with_regions + 1).contains("regions"),
               "regions are written as specified and omitted when empty");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "regions did not round-trip");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "regions", true));
  REQUIRE_OK(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "durable regions did not reopen");

  const auto rejects = [&](auto&& edit, std::string_view what) {
    auto changed = current;
    edit(changed["project"]["tracks"]);
    require(!detail::decode_manifest(changed.dump()), std::string(what) + " was accepted");
  };
  rejects(
      [&](Json& t) {
        t[with_regions + 1]["instrument_source"]["payload"]["document"]["regions"] = Json::array();
      },
      "an empty regions array (written only when non-empty)");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][0]["extra"] = 1;
      },
      "an unknown region field");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][0].erase("tag");
      },
      "a region without its tag");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][0]["start_frame"] =
            -1;
      },
      "a negative region start");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][1]["slot"] = 0;
      },
      "a duplicate region slot");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][1]["media"] =
            "missing";
      },
      "a region in an unknown media slot");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"]["payload"]["document"]["regions"][1]["start_frame"] =
            0;
      },
      "regions out of order in one media");
  rejects(
      [&](Json& t) {
        t[with_regions]["instrument_source"] = Json{{"kind", "sampler"}, {"payload", nullptr}};
      },
      "the removed sampler instrument source");
}

// ADR-0073: a synth document's painted voice lanes (written only when it
// has some), with voices strictly increasing and values in -1..1.
void native_voice_lanes_round_trip() {
  auto state = snapshot();
  project::NativeInstrumentPatch patch{"nova", 1, {}};
  patch.document.voice_lanes = {{"notes.pitch", {{0, 0.25}, {3, -1.0}}},
                                {"unison.a.pan", {{6, 0.5}}}};
  state.tracks.emplace_back(id<project::TrackId>(997), std::vector<project::Clip>{},
                            project::TrackKind::instrument, patch);
  const auto encoded =
      detail::encode_manifest(state, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  auto current = Json::parse(encoded);
  auto& tracks = current["project"]["tracks"];
  const auto last = tracks.size() - 1;
  const auto& lanes = tracks[last]["instrument_source"]["payload"]["document"]["voice_lanes"];
  REQUIRE_EQ(lanes.size(), 2, "voice lanes are written as specified");
  REQUIRE_EQ(lanes[0],
             (Json{{"id", "notes.pitch"},
                   {"values", Json::array({Json{{"voice", 0}, {"value", 0.25}},
                                           Json{{"voice", 3}, {"value", -1.0}}})}}),
             "voice lanes are written as specified");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "voice lanes did not round-trip");
  const auto rejects = [&](auto&& edit, std::string_view what) {
    auto changed = current;
    edit(changed["project"]["tracks"][last]["instrument_source"]["payload"]["document"]);
    require(!detail::decode_manifest(changed.dump()), std::string(what) + " was accepted");
  };
  rejects([](Json& d) { d["voice_lanes"] = Json::array(); },
          "an empty voice lanes array (written only when non-empty)");
  rejects([](Json& d) { d["voice_lanes"][0]["values"][1]["voice"] = 0; }, "voices out of order");
  rejects([](Json& d) { d["voice_lanes"][0]["values"][0]["value"] = 1.5; },
          "a value outside -1..1");
  rejects([](Json& d) { d["voice_lanes"][1]["id"] = "notes.pitch"; }, "a duplicate lane id");
  rejects([](Json& d) { d["voice_lanes"][0]["values"][0]["extra"] = 1; },
          "an unknown voice value field");
}

void managed_wav_receipts_and_missing_diagnostics() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "input.wav";
  write_wav(input);
  int input_fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(input_fd, 0, "WAV fixture could not be opened");
  auto first = take(ProjectStore::open_writer(root, true));
  auto receipt = take(first.stage_wav(id<project::SourceId>(4), input_fd));
  REQUIRE_EQ(receipt.record().frames, 96'000, "staged metadata wrong");
  REQUIRE_EQ(receipt.record().sample_rate, 48'000, "staged metadata wrong");
  REQUIRE_EQ(receipt.record().channels, 1, "staged metadata wrong");
  REQUIRE_EQ(receipt.record().byte_length, 192'044, "staged metadata wrong");
  auto project = snapshot();
  project.sources.push_back(receipt.record());
  require(!first.save(project, SaveKind::explicit_save), "save accepted no receipt");
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  REQUIRE_OK(first.save(project, SaveKind::explicit_save, receipts));
  fs::remove(input);
  auto reader = take(ProjectStore::open_reader(root));
  auto loaded = take(reader.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, project.sources,
             "managed source did not survive original removal");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "managed source did not survive original removal");
  const int managed_fd = take(reader.open_managed_source(project.sources[0]));
  REQUIRE_GE(managed_fd, 0, "managed source was not opened by stable identity");
  ::close(managed_fd);
  auto unsafe = project.sources[0];
  unsafe.sha256 = "../outside";
  require(!reader.open_managed_source(unsafe), "untrusted source path escaped assets");
  auto second = take(first.stage_wav(id<project::SourceId>(9), input_fd));
  REQUIRE_EQ(second.record().sha256, receipts[0].record().sha256,
             "identical input did not deduplicate");
  const auto asset = root / "assets" / (second.record().sha256 + ".wav");
  require(fs::exists(asset), "managed asset absent");
  first = take(ProjectStore::open_reader(root));
  auto reopened = take(ProjectStore::open_writer(root, false));
  project.revision = 2;
  REQUIRE_OK(reopened.save(project, SaveKind::explicit_save));
  {
    std::fstream tamper(asset, std::ios::in | std::ios::out | std::ios::binary);
    tamper.seekp(100);
    tamper.put('\x7f');
  }
  require(!reopened.open_managed_source(project.sources[0]),
          "same-length source tampering passed full read verification");
  fs::remove(asset);
  auto missing = take(reopened.load_current());
  REQUIRE_EQ(missing.snapshot.sources, project.sources,
             "missing source discarded edits or diagnostics");
  REQUIRE_EQ(missing.source_diagnostics.size(), 1, "missing source discarded edits or diagnostics");
  REQUIRE_EQ(missing.source_diagnostics[0].id, id<project::SourceId>(4),
             "missing source discarded edits or diagnostics");
  REQUIRE_EQ(missing.source_diagnostics[0].status,
             LoadedGeneration::SourceDiagnostic::Status::missing,
             "missing source discarded edits or diagnostics");
  require(!reopened.save(project, SaveKind::explicit_save), "save accepted missing asset");
  require(!reopened.open_managed_source(project.sources[0]), "missing managed source opened");
  ::close(input_fd);
}

void external_link_schema_reopen_and_missing_diagnostics() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "linked.wav";
  write_wav(input);
  std::ifstream stream(input, std::ios::binary);
  const std::string original((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
  REQUIRE_EQ(original.size(), 192'044, "linked fixture size changed");
  project::SourceRecord source{id<project::SourceId>(4),
                               project::SourceEncoding::wav,
                               sha256(original),
                               original.size(),
                               48'000,
                               1,
                               96'000,
                               project::ExternalSourceLink{input.string()}};
  auto state = snapshot();
  state.sources.push_back(source);
  auto writer = take(ProjectStore::open_writer(root, true));
  REQUIRE_OK(writer.save(state, SaveKind::explicit_save));
  std::ifstream unchanged_stream(input, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_stream)),
                              std::istreambuf_iterator<char>());
  REQUIRE_TRUE(fs::is_empty(root / "assets"), "external-link save copied or modified source bytes");
  REQUIRE_EQ(unchanged, original, "external-link save copied or modified source bytes");
  auto loaded = take(writer.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, state.sources,
             "external source was lost or treated as a managed asset");
  REQUIRE_EQ(loaded.snapshot.tracks, state.tracks,
             "external source was lost or treated as a managed asset");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "external source was lost or treated as a managed asset");
  REQUIRE_TRUE(!writer.open_managed_source(source),
               "external source was lost or treated as a managed asset");
  const int verified_fd = take(writer.open_verified_source(source));
  std::array<char, 4> header{};
  REQUIRE_EQ(::pread(verified_fd, header.data(), header.size(), 0), 4,
             "verified external descriptor did not retain source bytes");
  REQUIRE_EQ(std::string_view(header.data(), header.size()), "RIFF",
             "verified external descriptor did not retain source bytes");
  ::close(verified_fd);
  auto stale = source;
  stale.external_link = project::ExternalSourceLink{(temp.path() / "other.wav").string()};
  require(!writer.open_verified_source(stale),
          "unsaved external path was accepted for playback consumption");
  std::stop_source cancelled;
  cancelled.request_stop();
  require(!writer.open_verified_source(source, cancelled.get_token()),
          "cancelled external verification opened media");
  {
    std::fstream damaged(input, std::ios::in | std::ios::out | std::ios::binary);
    damaged.seekp(100);
    damaged.put('\x7f');
  }
  require(!writer.open_verified_source(source),
          "same-size damaged external source reached playback reader");
  {
    std::ofstream restored(input, std::ios::binary | std::ios::trunc);
    restored.write(original.data(), static_cast<std::streamsize>(original.size()));
  }

  const auto encoded =
      detail::encode_manifest(state, std::string(32, 'a'), std::nullopt, SaveKind::explicit_save);
  auto json = Json::parse(encoded);
  REQUIRE_EQ(json["schema"]["minor"], 47, "typed external link was not serialized");
  REQUIRE_EQ(json["project"]["sources"][0]["location"]["kind"], "external",
             "typed external link was not serialized");
  for (const auto& bad_path :
       {"relative.wav", "/tmp/../outside.wav", "/tmp//outside.wav", "/tmp/./outside.wav"}) {
    auto bad = json;
    bad["project"]["sources"][0]["location"]["last_known_path"] = bad_path;
    require(!detail::decode_manifest(bad.dump()), "malformed external path decoded");
  }
  for (const auto& bad_kind : {"unknown", "cache"}) {
    auto bad = json;
    bad["project"]["sources"][0]["location"]["kind"] = bad_kind;
    require(!detail::decode_manifest(bad.dump()), "foreign source kind decoded");
  }
  auto malformed = json;
  malformed["project"]["sources"][0]["location"]["untrusted"] = "value";
  require(!detail::decode_manifest(malformed.dump()), "unknown location field decoded");
  malformed = json;
  malformed["project"]["sources"][0].erase("location");
  require(!detail::decode_manifest(malformed.dump()), "source without location decoded");

  auto foreign = state;
  foreign.sources.front().sha256 = std::string(64, 'a');
  require(!writer.save(foreign, SaveKind::explicit_save),
          "foreign source bytes were accepted as a new external identity");
  foreign = state;
  auto& link = foreign.sources.front().external_link;
  if (!link)
    throw std::runtime_error("external-link test fixture lost its link");
  link->last_known_path.push_back(static_cast<char>(0xff));
  require(!writer.save(foreign, SaveKind::explicit_save),
          "invalid UTF-8 source path reached manifest serialization");
  const std::vector<std::string> unsafe_paths{
      "/tmp/\x01.wav",
      "/tmp/\xc0\xaf.wav",
      "/tmp/\xe0\x80\xaf.wav",
      "/tmp/\xed\xa0\x80.wav",
      "/tmp/\xf4\x90\x80\x80.wav",
      "/tmp/\xe2\x28\xa1.wav",
      "/tmp/\xf0\x9f\x92",
      std::string(project::max_external_source_path_bytes + 1, 'a')};
  for (const auto& path : unsafe_paths) {
    link->last_known_path = path;
    require(!project::validate_source_record(foreign.sources.front()),
            "unsafe external source path passed model validation");
  }
  link->last_known_path = "/tmp/\xc3\xa9.wav";
  REQUIRE_OK(project::validate_source_record(foreign.sources.front()),
             "valid UTF-8 external source path was rejected");
  fs::remove(input);
  auto reopened = take(ProjectStore::open_reader(root));
  loaded = take(reopened.load_current());
  REQUIRE_EQ(loaded.snapshot.sources, state.sources,
             "missing external source did not preserve identity and clip references");
  REQUIRE_EQ(loaded.snapshot.tracks, state.tracks,
             "missing external source did not preserve identity and clip references");
  REQUIRE_EQ(loaded.source_diagnostics.size(), 1,
             "missing external source did not preserve identity and clip references");
  REQUIRE_EQ(loaded.source_diagnostics[0].id, source.id,
             "missing external source did not preserve identity and clip references");
  REQUIRE_EQ(loaded.source_diagnostics[0].status,
             LoadedGeneration::SourceDiagnostic::Status::missing,
             "missing external source did not preserve identity and clip references");
  require(!reopened.open_verified_source(source), "missing external source opened for playback");
  fs::create_symlink(root / "assets", input);
  auto symlinked = take(reopened.load_current());
  REQUIRE_EQ(symlinked.source_diagnostics.size(), 1,
             "external symlink was followed or reported as a valid source");
  REQUIRE_EQ(symlinked.source_diagnostics[0].status,
             LoadedGeneration::SourceDiagnostic::Status::damaged,
             "external symlink was followed or reported as a valid source");
  fs::remove(input);
  state.revision = 2;
  REQUIRE_OK(writer.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(reopened.load_current()).snapshot.revision, 2,
             "missing external link prevented retention of later project edits");
  require(fs::is_empty(root / "assets"), "external source became a managed asset");
}

void external_relink_verifies_before_pointer_publication() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto original_path = temp.path() / "original.wav";
  const auto candidate_path = temp.path() / "candidate.wav";
  const auto later_path = temp.path() / "later.wav";
  write_wav(original_path);
  std::ifstream source_stream(original_path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(source_stream)),
                          std::istreambuf_iterator<char>());
  auto state = snapshot();
  state.sources.push_back({id<project::SourceId>(4), project::SourceEncoding::wav, sha256(bytes),
                           bytes.size(), 48'000, 1, 96'000,
                           project::ExternalSourceLink{original_path.string()}});
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(state, SaveKind::explicit_save));
  REQUIRE_OK(writer.name_current_revision("Before relink"));
  fs::remove(original_path);
  state.revision = 2;
  state.sources.front().external_link = project::ExternalSourceLink{candidate_path.string()};
  fs::copy_file(root / "state" / "generations" / baseline.generation / "manifest.json",
                candidate_path);
  REQUIRE_NE(fs::file_size(candidate_path), state.sources.front().byte_length,
             "wrong-file fixture unexpectedly matched source length");
  REQUIRE_TRUE(!writer.save(state, SaveKind::explicit_save),
               "wrong candidate advanced saved project");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "wrong candidate advanced saved project");
  fs::remove(candidate_path);
  REQUIRE_TRUE(!writer.save(state, SaveKind::explicit_save),
               "missing candidate advanced saved project");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "missing candidate advanced saved project");
  {
    std::ofstream output(candidate_path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  {
    std::fstream altered(candidate_path, std::ios::in | std::ios::out | std::ios::binary);
    altered.seekp(100);
    altered.put('\x7f');
  }
  REQUIRE_EQ(fs::file_size(candidate_path), state.sources.front().byte_length,
             "same-size changed source bytes advanced saved project");
  REQUIRE_TRUE(!writer.save(state, SaveKind::explicit_save),
               "same-size changed source bytes advanced saved project");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "same-size changed source bytes advanced saved project");
  {
    std::ofstream output(candidate_path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const auto relinked = take(writer.save(state, SaveKind::explicit_save));
  auto loaded = take(writer.load_current());
  REQUIRE_EQ(loaded.generation, relinked.generation,
             "verified relink changed identity, clips or managed assets");
  REQUIRE_EQ(loaded.snapshot.sources, state.sources,
             "verified relink changed identity, clips or managed assets");
  REQUIRE_EQ(loaded.snapshot.tracks, state.tracks,
             "verified relink changed identity, clips or managed assets");
  REQUIRE_TRUE(loaded.source_diagnostics.empty(),
               "verified relink changed identity, clips or managed assets");
  REQUIRE_TRUE(fs::is_empty(root / "assets"),
               "verified relink changed identity, clips or managed assets");

  auto historical = state;
  historical.revision = 3;
  historical.sources.front().external_link = project::ExternalSourceLink{original_path.string()};
  REQUIRE_TRUE(!writer.save(historical, SaveKind::explicit_save),
               "named revision authorized a missing historical link without re-verification");
  REQUIRE_EQ(take(writer.load_current()).generation, relinked.generation,
             "named revision authorized a missing historical link without re-verification");

  fs::copy_file(candidate_path, later_path);
  state.revision = 3;
  state.sources.front().external_link = project::ExternalSourceLink{later_path.string()};
  writer = take(ProjectStore::open_reader(root));
  bool changed_at_pointer = false;
  auto changing_writer = take(ProjectStore::open_writer(root, false, [&](SaveStage stage) {
    if (stage == SaveStage::pointer_written) {
      std::fstream changed(later_path, std::ios::in | std::ios::out | std::ios::binary);
      changed.seekp(100);
      changed.put('\x7f');
      changed_at_pointer = true;
    }
    return false;
  }));
  REQUIRE_TRUE(!changing_writer.save(state, SaveKind::explicit_save),
               "candidate changed after verification advanced CURRENT");
  REQUIRE_TRUE(changed_at_pointer, "candidate changed after verification advanced CURRENT");
  REQUIRE_EQ(take(changing_writer.load_current()).generation, relinked.generation,
             "candidate changed after verification advanced CURRENT");
  changing_writer = take(ProjectStore::open_reader(root));
  fs::copy_file(candidate_path, later_path, fs::copy_options::overwrite_existing);
  auto interrupted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::pointer_written; }));
  REQUIRE_TRUE(!interrupted.save(state, SaveKind::explicit_save),
               "interrupted relink advanced CURRENT");
  REQUIRE_EQ(take(interrupted.load_current()).generation, relinked.generation,
             "interrupted relink advanced CURRENT");
  interrupted = take(ProjectStore::open_reader(root));
  writer = take(ProjectStore::open_writer(root, false));
  const auto retried = take(writer.save(state, SaveKind::explicit_save));
  loaded = take(writer.load_current());
  REQUIRE_EQ(loaded.generation, retried.generation,
             "relink retry lost encoded identity or referenced clips");
  REQUIRE_EQ(loaded.snapshot.sources, state.sources,
             "relink retry lost encoded identity or referenced clips");
  REQUIRE_EQ(loaded.snapshot.tracks, state.tracks,
             "relink retry lost encoded identity or referenced clips");
  REQUIRE_TRUE(fs::is_empty(root / "assets"),
               "relink retry lost encoded identity or referenced clips");
}

void external_worker_proof_is_exact_and_writer_bound() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto original = temp.path() / "original.wav";
  const auto replacement = temp.path() / "replacement.wav";
  write_wav(original);
  fs::copy_file(original, replacement);
  std::ifstream input(original, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  auto state = snapshot();
  state.sources.push_back({id<project::SourceId>(4), project::SourceEncoding::wav, sha256(bytes),
                           bytes.size(), 48'000, 1, 96'000,
                           project::ExternalSourceLink{original.string()}});
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(state, SaveKind::explicit_save));
  state.revision++;
  state.sources.front().external_link = project::ExternalSourceLink{replacement.string()};
  auto task =
      take(writer.begin_external_source_verification(state.sources.front(), state.revision));
  auto proof = take(std::move(task).run());
  auto wrong_revision = state;
  wrong_revision.revision++;
  REQUIRE_TRUE(!writer.save(wrong_revision, SaveKind::explicit_save, {}, &proof),
               "worker proof authorized a different project revision");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "worker proof authorized a different project revision");
  auto wrong_path = state;
  wrong_path.sources.front().external_link = project::ExternalSourceLink{original.string()};
  REQUIRE_TRUE(!writer.save(wrong_path, SaveKind::explicit_save, {}, &proof),
               "worker proof authorized another source path");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "worker proof authorized another source path");
  writer = take(ProjectStore::open_reader(root));
  auto other_writer = take(ProjectStore::open_writer(root, false));
  REQUIRE_TRUE(!other_writer.save(state, SaveKind::explicit_save, {}, &proof),
               "worker proof authorized another writer instance");
  REQUIRE_EQ(take(other_writer.load_current()).generation, baseline.generation,
             "worker proof authorized another writer instance");
  task =
      take(other_writer.begin_external_source_verification(state.sources.front(), state.revision));
  proof = take(std::move(task).run());
  const auto saved = take(other_writer.save(state, SaveKind::explicit_save, {}, &proof));
  REQUIRE_EQ(take(other_writer.load_current()).generation, saved.generation,
             "matching worker proof failed to publish the external relink");
}

void damaged_managed_source_keeps_saved_edits_and_recovery_choice() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "input.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "damaged source fixture did not open");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
  const auto source = receipt.record();
  auto state = snapshot();
  state.sources.push_back(source);
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto first = take(writer.save(state, SaveKind::explicit_save, receipts));
  state.revision = 2;
  const auto second = take(writer.save(state, SaveKind::explicit_save));
  const auto asset = root / "assets" / (source.sha256 + ".wav");
  fs::resize_file(asset, source.byte_length - 1);
  writer = take(ProjectStore::open_reader(root));
  const auto reopened = take(writer.load_current());
  REQUIRE_EQ(reopened.generation, second.generation,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.snapshot.revision, state.revision,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.snapshot.sources, state.sources,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.snapshot.tracks, state.tracks,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.source_diagnostics.size(), 1,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.source_diagnostics[0].id, source.id,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  REQUIRE_EQ(reopened.source_diagnostics[0].status,
             LoadedGeneration::SourceDiagnostic::Status::damaged,
             "damaged managed media discarded saved edits or lacked a precise diagnostic");
  require(!writer.open_managed_source(source), "truncated managed source opened");
  const auto choices = take(writer.recovery_candidates());
  REQUIRE_EQ(choices.size(), 2, "asset damage hid complete project generations from recovery");
  REQUIRE_TRUE(std::ranges::all_of(choices,
                                   [](const LoadedGeneration& candidate) {
                                     return candidate.source_diagnostics.size() == 1 &&
                                            candidate.source_diagnostics[0].status ==
                                                LoadedGeneration::SourceDiagnostic::Status::damaged;
                                   }),
               "asset damage hid complete project generations from recovery");
  require(std::ranges::any_of(choices,
                              [&](const LoadedGeneration& candidate) {
                                return candidate.generation == first.generation;
                              }),
          "last complete project generation was lost after media damage");
  ::close(fd);
}

void missing_managed_wav_requires_exact_recovery_candidate() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "input.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "restore source fixture did not open");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
  const auto expected = receipt.record();
  auto state = snapshot();
  state.sources.push_back(expected);
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto saved = take(writer.save(state, SaveKind::explicit_save, receipts));
  const auto manifest = root / "state" / "generations" / saved.generation / "manifest.json";
  std::ifstream before_file(manifest);
  const std::string before(std::istreambuf_iterator<char>{before_file}, {});
  const auto asset = root / "assets" / (expected.sha256 + ".wav");
  fs::remove(asset);
  REQUIRE_EQ(take(writer.load_current()).source_diagnostics.size(), 1,
             "missing managed source was not diagnosed before recovery");

  const auto wrong_path = temp.path() / "wrong.wav";
  fs::copy_file(input, wrong_path);
  {
    std::fstream wrong(wrong_path, std::ios::binary | std::ios::in | std::ios::out);
    wrong.seekp(100);
    wrong.put('\x7f');
  }
  const int wrong_fd = ::open(wrong_path.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(wrong_fd, 0, "wrong candidate fixture did not open");
  auto rejected = writer.restore_managed_source(saved.generation, expected, wrong_fd);
  REQUIRE_TRUE(!rejected, "different same-length bytes replaced a saved source");
  REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
             "different same-length bytes replaced a saved source");
  REQUIRE_TRUE(!fs::exists(asset), "different same-length bytes replaced a saved source");
  ::close(wrong_fd);

  auto unregistered = expected;
  unregistered.id = id<project::SourceId>(99);
  REQUIRE_TRUE(!writer.restore_managed_source(saved.generation, unregistered, fd),
               "unregistered source or traversal generation was accepted");
  REQUIRE_TRUE(!writer.restore_managed_source("../outside", expected, fd),
               "unregistered source or traversal generation was accepted");
  unregistered = expected;
  unregistered.sha256 = "../outside";
  REQUIRE_TRUE(!writer.restore_managed_source(saved.generation, unregistered, fd),
               "malicious source hash escaped managed assets");
  REQUIRE_TRUE(!fs::exists(temp.path() / "outside"),
               "malicious source hash escaped managed assets");
  const auto fifo = temp.path() / "candidate.fifo";
  REQUIRE_EQ(::mkfifo(fifo.c_str(), 0600), 0, "restore FIFO fixture failed");
  const int fifo_fd = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  REQUIRE_GE(fifo_fd, 0, "non-regular candidate was accepted");
  REQUIRE_TRUE(!writer.restore_managed_source(saved.generation, expected, fifo_fd),
               "non-regular candidate was accepted");
  ::close(fifo_fd);
  fs::create_symlink(wrong_path, asset);
  require(!writer.restore_managed_source(saved.generation, expected, fd),
          "symlink at managed source path was followed or replaced");
  fs::remove(asset);

  REQUIRE_EQ(take(writer.restore_managed_source(saved.generation, expected, fd)),
             RestoreStatus::committed, "exact candidate did not restore missing managed source");
  REQUIRE_EQ(take(writer.load_current()).snapshot.sources, state.sources,
             "source identity or diagnostics changed after recovery");
  REQUIRE_TRUE(take(writer.load_current()).source_diagnostics.empty(),
               "source identity or diagnostics changed after recovery");
  std::ifstream after_file(manifest);
  const std::string after(std::istreambuf_iterator<char>{after_file}, {});
  REQUIRE_EQ(before, after, "managed repair rewrote project manifest");
  writer = take(ProjectStore::open_reader(root));
  const int restored = take(writer.open_managed_source(expected));
  ::close(restored);
  ::close(fd);
}

void interrupted_managed_restore_reopens_without_substituting_media() {
  for (const auto stage : {SaveStage::asset_file_flushed, SaveStage::asset_published}) {
    Directory temp;
    const auto root = temp.path() / "project";
    const auto input = temp.path() / "input.wav";
    write_wav(input);
    const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "restore fault input did not open");
    auto writer = take(ProjectStore::open_writer(root, true));
    auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
    const auto expected = receipt.record();
    auto state = snapshot();
    state.sources.push_back(expected);
    std::array<SourceReceipt, 1> receipts{std::move(receipt)};
    const auto saved = take(writer.save(state, SaveKind::explicit_save, receipts));
    const auto asset = root / "assets" / (expected.sha256 + ".wav");
    fs::remove(asset);
    writer = take(ProjectStore::open_reader(root));
    auto faulted = take(ProjectStore::open_writer(
        root, false, [stage](SaveStage reached) { return reached == stage; }));
    auto result = faulted.restore_managed_source(saved.generation, expected, fd);
    const bool linked = stage == SaveStage::asset_published;
    if (linked)
      require(result && result.value() == RestoreStatus::uncertain && fs::exists(asset),
              "after-link fault did not report uncertain visible asset");
    else
      require(!result && !fs::exists(asset), "pre-link fault installed source bytes");
    faulted = take(ProjectStore::open_reader(root));
    const auto reopened = take(faulted.load_current());
    REQUIRE_EQ(reopened.snapshot.sources, state.sources,
               "interrupted restore changed saved identity or reopen diagnostics");
    REQUIRE_EQ(reopened.source_diagnostics.empty(), linked,
               "interrupted restore changed saved identity or reopen diagnostics");
    if (linked) {
      const int verified = take(faulted.open_managed_source(expected));
      ::close(verified);
    }
    ::close(fd);
  }
}

void generation_namespace_swap_blocks_managed_restore_publication() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "input.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "restore namespace fixture did not open");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
  const auto expected = receipt.record();
  auto state = snapshot();
  state.sources.push_back(expected);
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto saved = take(writer.save(state, SaveKind::explicit_save, receipts));
  const auto asset = root / "assets" / (expected.sha256 + ".wav");
  fs::remove(asset);
  writer = take(ProjectStore::open_reader(root));

  bool swapped = false;
  auto faulted = take(ProjectStore::open_writer(root, false, [&](SaveStage reached) {
    if (reached == SaveStage::asset_file_flushed) {
      fs::rename(root / "state" / "generations", root / "state" / "old-generations");
      fs::create_directory(root / "state" / "generations");
      swapped = true;
    }
    return false;
  }));
  auto result = faulted.restore_managed_source(saved.generation, expected, fd);
  REQUIRE_TRUE(swapped, "replaced generation namespace allowed source publication");
  REQUIRE_TRUE(!result, "replaced generation namespace allowed source publication");
  REQUIRE_EQ(result.error().code, foundation::ErrorCode::conflict,
             "replaced generation namespace allowed source publication");
  REQUIRE_TRUE(!fs::exists(asset), "replaced generation namespace allowed source publication");
  fs::remove(root / "state" / "generations");
  fs::rename(root / "state" / "old-generations", root / "state" / "generations");
  faulted = take(ProjectStore::open_reader(root));
  REQUIRE_EQ(take(faulted.load_current()).source_diagnostics.size(), 1,
             "namespace swap fixture changed recovery state");
  ::close(fd);
}

void staged_asset_failures_and_foreign_receipts() {
  Directory temp;
  const auto input = temp.path() / "source.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture open failed");
  auto first = take(ProjectStore::open_writer(temp.path() / "first", true));
  auto foreign = take(first.stage_wav(id<project::SourceId>(4), fd));
  auto second = take(ProjectStore::open_writer(temp.path() / "second", true));
  auto project = snapshot();
  project.sources.push_back(foreign.record());
  std::array<SourceReceipt, 1> foreign_receipts{std::move(foreign)};
  require(!second.save(project, SaveKind::explicit_save, foreign_receipts),
          "foreign receipt authorized source");
  first = take(ProjectStore::open_reader(temp.path() / "first"));
  for (const auto stage :
       {SaveStage::asset_file_flushed, SaveStage::asset_published, SaveStage::assets_flushed}) {
    auto interrupted = take(ProjectStore::open_writer(
        temp.path() / "first", false, [stage](SaveStage reached) { return reached == stage; }));
    require(!interrupted.stage_wav(id<project::SourceId>(9), fd),
            "interrupted stage issued receipt");
  }
  require(!fs::exists(temp.path() / "first" / "state" / "CURRENT"),
          "asset interruption published project state");
  ::close(fd);
}

void asset_directory_swap_cannot_redirect_staging() {
  Directory temp;
  const auto input = temp.path() / "source.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture open failed");
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  fs::rename(root / "assets", root / "old-assets");
  fs::create_directory(temp.path() / "outside");
  fs::create_directory_symlink(temp.path() / "outside", root / "assets");
  require(!store.stage_wav(id<project::SourceId>(4), fd),
          "asset directory swap did not stop staging");
  require(fs::is_empty(temp.path() / "outside"), "asset staging escaped project root");
  ::close(fd);
}

void unpublished_wav_stage_is_disposable_and_reopen_cleans_only_stages() {
  Directory temp;
  const auto input = temp.path() / "source.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture open failed");
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  {
    auto task = take(writer.begin_wav_stage(id<project::SourceId>(4), fd));
    auto prepared = take(std::move(task).run());
    REQUIRE_EQ(prepared.record().sample_rate, 48'000,
               "worker did not produce private WAV staging file");
    REQUIRE_TRUE(!fs::is_empty(root / "assets"), "worker did not produce private WAV staging file");
  }
  require(fs::is_empty(root / "assets"), "dropping a prepared WAV left a stage or asset");
  auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
  const auto published = root / "assets" / (receipt.record().sha256 + ".wav");
  auto project = snapshot();
  project.sources.push_back(receipt.record());
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  REQUIRE_OK(writer.save(project, SaveKind::explicit_save, receipts), "fixture source save failed");
  const std::string abandoned = ".stage-00000000000000000000000000000001";
  const std::string foreign = ".stage-00000000000000000000000000000002";
  {
    std::ofstream output(root / "assets" / abandoned, std::ios::binary);
    output << "partial worker bytes";
  }
  fs::create_symlink(input, root / "assets" / foreign);
  {
    std::ofstream output(root / "assets" / "keep.txt", std::ios::binary);
    output << "foreign";
  }
  writer = take(ProjectStore::open_reader(root)); // release sole-writer lock
  auto reopened = take(ProjectStore::open_writer(root, false));
  REQUIRE_TRUE(!fs::exists(root / "assets" / abandoned),
               "reopen cleanup removed a published/foreign asset or retained abandoned stage");
  REQUIRE_TRUE(fs::is_symlink(root / "assets" / foreign),
               "reopen cleanup removed a published/foreign asset or retained abandoned stage");
  REQUIRE_TRUE(fs::exists(root / "assets" / "keep.txt"),
               "reopen cleanup removed a published/foreign asset or retained abandoned stage");
  REQUIRE_TRUE(fs::exists(published),
               "reopen cleanup removed a published/foreign asset or retained abandoned stage");
  REQUIRE_EQ(take(reopened.load_current()).snapshot.sources, project.sources,
             "stage cleanup damaged saved source reference");
  ::close(fd);
}

void reopen_cleans_abandoned_sources_beyond_old_scan_limit() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto retained = root / "assets" / "keep.txt";
  std::ofstream(retained) << "user file";
  for (unsigned index = 0; index < 4'100; ++index) {
    char suffix[33]{};
    std::snprintf(suffix, sizeof(suffix), "%032x", index);
    fs::create_hard_link(retained, root / "assets" / (std::string(".stage-") + suffix));
  }
  writer = take(ProjectStore::open_reader(root));
  writer = take(ProjectStore::open_writer(root, false));
  REQUIRE_TRUE(fs::exists(retained),
               "writer reopen silently left abandoned asset stages past the scan limit");
  REQUIRE_TRUE(std::ranges::none_of(fs::directory_iterator(root / "assets"),
                                    [](const fs::directory_entry& entry) {
                                      return entry.path().filename().string().starts_with(
                                          ".stage-");
                                    }),
               "writer reopen silently left abandoned asset stages past the scan limit");
}

void changed_staged_bytes_never_receive_a_source_receipt() {
  Directory temp;
  const auto input = temp.path() / "source.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "fixture open failed");
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  {
    auto staged = take(std::move(take(store.begin_wav_stage(id<project::SourceId>(4), fd))).run());
    for (const auto& entry : fs::directory_iterator(root / "assets")) {
      if (entry.path().filename().string().starts_with(".stage-")) {
        std::ofstream append(entry.path(), std::ios::binary | std::ios::app);
        append.put('\0');
      }
    }
    auto rejected = store.publish_wav_stage(std::move(staged));
    REQUIRE_TRUE(!rejected, "modified staged WAV received a source receipt");
    REQUIRE_EQ(rejected.error().code, foundation::ErrorCode::conflict,
               "modified staged WAV received a source receipt");
  }
  REQUIRE_TRUE(fs::is_empty(root / "assets"),
               "modified stage created a managed asset or generation");
  REQUIRE_TRUE(!store.load_current(), "modified stage created a managed asset or generation");
  ::close(fd);
}

void master_effect_round_trip_and_validation() {
  const auto generation = std::string(32, 'a');
  auto state = snapshot();
  state.master_effects.push_back(
      {id<project::InsertSlotId>(100), true,
       project::NativeEffectPatch{"ceiling", 1, {{"ceiling.ceiling", -2.0}}, 7}});
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "current schema lost built-in master effect state");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.master_effects, state.master_effects,
             "current schema lost built-in master effect state");
  require(!detail::decode_manifest(replaced(encoded, "\"generation\":7", "\"generation\":0")),
          "zero effect generation accepted");
  Directory temp;
  const auto root = temp.path() / "master";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.master_effects, state.master_effects,
             "saved built-in master effect did not reopen");
  store = take(ProjectStore::open_reader(root));
  auto faulted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::manifest_written; }));
  state.revision = 2;
  native_effect<project::NativeEffectPatch>(state)->parameters.front().value = -3.0;
  REQUIRE_TRUE(!faulted.save(state, SaveKind::explicit_save),
               "interrupted master effect save replaced previous generation");
  REQUIRE_EQ(take(faulted.load_current()).generation, baseline.generation,
             "interrupted master effect save replaced previous generation");
  REQUIRE_EQ(native_effect<project::NativeEffectPatch>(take(faulted.load_current()).snapshot)
                 ->parameters.front()
                 .value,
             -2.0, "interrupted master effect save replaced previous generation");
}

void source_encodings_round_trip() {
  const auto generation = std::string(32, 'a');
  for (const auto [encoding, name] : {std::pair{project::SourceEncoding::wav, "wav"},
                                      std::pair{project::SourceEncoding::aiff, "aiff"},
                                      std::pair{project::SourceEncoding::flac, "flac"},
                                      std::pair{project::SourceEncoding::mp3, "mp3"}}) {
    auto state = snapshot();
    state.sources.push_back({id<project::SourceId>(4), encoding, std::string(64, 'a'),
                             encoding == project::SourceEncoding::wav ? 44U : 1U, 48'000, 2,
                             96'000});
    const auto encoded =
        detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
    REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
               "source writer did not use the current schema");
    REQUIRE_NE(encoded.find(std::string("\"encoding\":\"") + name + "\""), std::string::npos,
               "source encoding tag changed");
    const auto decoded = take(detail::decode_manifest(encoded));
    REQUIRE_EQ(decoded.snapshot.sources, state.sources,
               "encoded source identity did not round trip");
    if (encoding != project::SourceEncoding::wav) {
#if !OSCILLATE_HAVE_CODEC
      Directory temp;
      auto store = take(ProjectStore::open_writer(temp.path() / "project", true));
      const auto open = store.open_managed_source(state.sources[0]);
      REQUIRE_TRUE(!open, "non-WAV source was passed to the WAV verifier");
      REQUIRE_EQ(open.error().code, foundation::ErrorCode::unsupported,
                 "non-WAV source was passed to the WAV verifier");
      const auto save = store.save(state, SaveKind::explicit_save);
      REQUIRE_TRUE(!save, "non-WAV source was silently saved without a decoder-backed receipt");
      REQUIRE_EQ(save.error().code, foundation::ErrorCode::unsupported,
                 "non-WAV source was silently saved without a decoder-backed receipt");
#endif
    }
    require(!detail::decode_manifest(replaced(encoded, std::string("\"encoding\":\"") + name + "\"",
                                              "\"encoding\":\"unknown\"")),
            "unknown source encoding accepted");
    require(!detail::decode_manifest(
                replaced(encoded, std::string("\"encoding\":\"") + name + "\"", "\"encoding\":1")),
            "non-string source encoding accepted");
    const auto length_field =
        std::string("\"byte_length\":") + (encoding == project::SourceEncoding::wav ? "44" : "1");
    require(!detail::decode_manifest(replaced(encoded, length_field, "\"byte_length\":0")),
            "zero-length encoded source accepted");
  }
  auto invalid = snapshot();
  invalid.sources.push_back({id<project::SourceId>(4),
                             std::bit_cast<project::SourceEncoding>(std::uint8_t{99}),
                             std::string(64, 'a'), 100, 48'000, 2, 96'000});
  require(!detail::decode_manifest(
              detail::encode_manifest(invalid, generation, std::nullopt, SaveKind::explicit_save)),
          "unknown model encoding was serialized as a valid source");
}

void vst3_round_trip() {
  auto state = snapshot();
  const project::Vst3PluginInstance plugin{id<project::PluginInstanceId>(20),
                                           std::string(32, 'a'),
                                           "Fixture Vendor",
                                           "Gain",
                                           "1.0",
                                           "/missing/Gain.vst3",
                                           std::string(64, 'b'),
                                           {0, 1, 254, 255},
                                           {{7, 0.5}},
                                           true};
  state.tracks[0].effects.push_back(effect_slot(1000, plugin));
  const auto generation = std::string(32, 'a');
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "current VST3 schema or canonical state missing");
  REQUIRE_NE(encoded.find("\"component_state_base64\":\"AAH+/w==\""), std::string::npos,
             "current VST3 schema or canonical state missing");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "VST3 placeholder roundtrip lost identity/state/chain");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "project", true));
  REQUIRE_OK(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "durable VST3 placeholder lost on reopen");
  for (const auto& bad : {
           replaced(encoded, "AAH+/w==", "AAH+/w=A"),
           replaced(encoded, "AAH+/w==", "!!!!!!!!"),
           replaced(encoded, "AAH+/w==", "AAH+/w"),
           replaced(encoded, "\"normalized\":0.5", "\"normalized\":2.0"),
           replaced(encoded, "\"class_id\":\"" + std::string(32, 'a') + "\"",
                    "\"class_id\":\"bad\""),
           replaced(encoded, "\"missing\":true", "\"missing\":1"),
       }) {
    require(!detail::decode_manifest(bad), "malformed VST3 manifest accepted");
  }
}

void routing_round_trip_and_failed_save() {
  auto state = snapshot();
  const auto group = id<project::TrackId>(30);
  const auto returns = id<project::TrackId>(31);
  state.tracks.emplace_back(group, std::vector<project::Clip>{}, project::TrackKind::group_bus);
  state.tracks.emplace_back(returns, std::vector<project::Clip>{}, project::TrackKind::return_bus);
  state.tracks[0].main_output = group;
  state.tracks[0].sends.push_back({id<project::SendId>(40), returns, 0.25});
  state.tracks[1].sends.push_back({id<project::SendId>(41), returns, 0.5});
  const auto generation = std::string(32, 'a');
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "schema 1.8 routing roundtrip lost destinations or send gains");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "schema 1.8 routing roundtrip lost destinations or send gains");
  for (const auto& bad : {
           replaced(encoded, "\"gain\":0.25", "\"gain\":5.0"),
           replaced(encoded, "\"destination\":\"" + returns.to_string() + "\"",
                    "\"destination\":\"" + id<project::TrackId>(99).to_string() + "\""),
           replaced(encoded, "\"main_output\":\"" + group.to_string() + "\"",
                    "\"main_output\":\"" + returns.to_string() + "\""),
       }) {
    require(!detail::decode_manifest(bad), "malformed routing manifest accepted");
  }

  Directory temp;
  const auto root = temp.path() / "project";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "durable routing state lost on reopen");
  store = take(ProjectStore::open_reader(root));
  auto faulted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::manifest_written; }));
  state.revision = 2;
  state.tracks[0].sends[0].gain = 0.75;
  REQUIRE_TRUE(!faulted.save(state, SaveKind::explicit_save),
               "interrupted routing save replaced last valid revision");
  REQUIRE_EQ(take(faulted.load_current()).generation, baseline.generation,
             "interrupted routing save replaced last valid revision");
  REQUIRE_EQ(take(faulted.load_current()).snapshot.tracks[0].sends[0].gain, 0.25,
             "interrupted routing save replaced last valid revision");
}

void automation_round_trip_and_failures() {
  const auto generation = std::string(32, 'a');
  auto state = snapshot();
  const project::Vst3PluginInstance missing{id<project::PluginInstanceId>(200),
                                            std::string(32, 'a'),
                                            "Fixture Vendor",
                                            "Gain",
                                            "1.0",
                                            "/missing/Gain.vst3",
                                            std::string(64, 'b'),
                                            {1, 2, 3},
                                            {{7, 0.5}},
                                            true};
  state.tracks[0].effects.push_back(effect_slot(1001, missing));
  const project::AutomationDomain domain{project::AutomationRepresentation::vst3_normalized,
                                         "vst3:param:7",
                                         "",
                                         0.0,
                                         1.0,
                                         0.5,
                                         false,
                                         0};
  project::AutomationLane track_lane{
      id<project::AutomationLaneId>(201),
      project::Vst3ParameterTarget{missing.id, 7},
      domain,
      project::AutomationTimebase::musical,
      {{id<project::AutomationPointId>(202), 0, 0.2, project::AutomationShape::linear},
       {id<project::AutomationPointId>(203), timeline::ticks_per_quarter, 0.8}},
      1};
  track_lane.points[1].to_next = project::AutomationShape::quadratic;
  track_lane.points.push_back({id<project::AutomationPointId>(211), timeline::ticks_per_quarter * 2,
                               0.6, project::AutomationShape::exponential});
  track_lane.points.push_back({id<project::AutomationPointId>(212), timeline::ticks_per_quarter * 3,
                               0.4, project::AutomationShape::ease_in_out});
  track_lane.points.push_back(
      {id<project::AutomationPointId>(213), timeline::ticks_per_quarter * 4, 0.5});
  const project::AutomationLane clip_lane{id<project::AutomationLaneId>(204),
                                          project::Vst3ParameterTarget{missing.id, 7},
                                          domain,
                                          project::AutomationTimebase::elapsed,
                                          {{id<project::AutomationPointId>(205),
                                            0,
                                            0.7,
                                            project::AutomationShape::bezier,
                                            {0.2, 0.1, 0.8, 0.9}}},
                                          1};
  state.tracks[0].automation.push_back(track_lane);
  state.tracks[0].clips[0].automation.push_back(clip_lane);
  const auto instrument_track = id<project::TrackId>(206);
  state.tracks.emplace_back(instrument_track, std::vector<project::Clip>{},
                            project::TrackKind::instrument);
  state.tracks.back().instrument_source = project::NativeInstrumentPatch{"prism", 1, {}};
  state.tracks.back().midi_clips.emplace_back(
      id<project::ClipId>(208), project::MusicalInterval{{0}, {timeline::ticks_per_quarter * 2}},
      timeline::TickPosition{timeline::ticks_per_quarter}, false, std::vector<project::MidiNote>{});
  state.tracks.back().midi_clips.back().automation.push_back(
      {id<project::AutomationLaneId>(209),
       project::TrackControlTarget{instrument_track,
                                   project::TrackControlTarget::Key::channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        1.0, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::musical,
       {{id<project::AutomationPointId>(210), 0, 0.5}},
       1});
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "automation 1.9 codec lost track/clip lane or missing plugin identity");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "automation 1.9 codec lost track/clip lane or missing plugin identity");
  Directory temp;
  const auto root = temp.path() / "automation";
  auto store = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "saved automation lanes did not reopen exactly");
  store = take(ProjectStore::open_reader(root));
  auto faulted = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::manifest_written; }));
  state.revision = 2;
  state.tracks[0].automation[0].points[0].value = 0.4;
  REQUIRE_TRUE(!faulted.save(state, SaveKind::explicit_save),
               "interrupted automation save replaced last good generation");
  REQUIRE_EQ(take(faulted.load_current()).generation, baseline.generation,
             "interrupted automation save replaced last good generation");
  REQUIRE_EQ(take(faulted.load_current()).snapshot.tracks[0].automation, std::vector{track_lane},
             "interrupted automation save replaced last good generation");

  auto orphan = state;
  orphan.tracks[0].automation[0].target =
      project::Vst3ParameterTarget{id<project::PluginInstanceId>(999), 7};
  auto duplicate = state;
  duplicate.tracks[0].clips[0].automation[0].id = track_lane.id;
  auto repeated_point = state;
  repeated_point.tracks[0].clips[0].automation[0].points[0].id = track_lane.points[0].id;
  auto nonfinite = state;
  nonfinite.tracks[0].automation[0].points[0].value = std::numeric_limits<double>::quiet_NaN();
  auto bad_handles = state;
  bad_handles.tracks[0].clips[0].automation[0].points[0].handles.x1 = 1.1;
  auto oversized = state;
  oversized.tracks[0].automation[0].points.resize(project::max_automation_points_per_lane + 1,
                                                  oversized.tracks[0].automation[0].points[0]);
  for (const auto& invalid :
       {orphan, duplicate, repeated_point, nonfinite, bad_handles, oversized}) {
    require(!detail::decode_manifest(detail::encode_manifest(invalid, generation, std::nullopt,
                                                             SaveKind::explicit_save)),
            "invalid automation project passed codec validation");
  }
  REQUIRE_TRUE(!detail::decode_manifest(replaced(encoded, "\"to_next\":\"linear\"",
                                                 "\"to_next\":\"linear\",\"extra\":1")),
               "unknown automation field or timebase accepted");
  REQUIRE_TRUE(!detail::decode_manifest(
                   replaced(encoded, "\"to_next\":\"linear\"", "\"to_next\":\"unknown\"")),
               "unknown automation field or timebase accepted");
  REQUIRE_TRUE(!detail::decode_manifest(
                   replaced(encoded, "\"timebase\":\"musical\"", "\"timebase\":\"invalid\"")),
               "unknown automation field or timebase accepted");
}

void send_gain_round_trip() {
  const auto generation = std::string(32, 'a');
  auto state = snapshot();
  const auto returned = id<project::TrackId>(330);
  const auto send = id<project::SendId>(331);
  state.tracks.emplace_back(returned, std::vector<project::Clip>{}, project::TrackKind::return_bus);
  state.tracks[0].sends.push_back({send, returned, 0.5});
  const project::AutomationDomain domain{project::AutomationRepresentation::physical,
                                         "mixer.send_gain",
                                         "linear",
                                         0.0,
                                         4.0,
                                         1.0,
                                         false,
                                         0,
                                         project::AutomationScale::decibel};
  const project::AutomationLane track_lane{id<project::AutomationLaneId>(332),
                                           project::SendGainTarget{send},
                                           domain,
                                           project::AutomationTimebase::elapsed,
                                           {{id<project::AutomationPointId>(333), 0, 0.25}},
                                           1};
  const project::AutomationLane clip_lane{id<project::AutomationLaneId>(334),
                                          project::SendGainTarget{send},
                                          domain,
                                          project::AutomationTimebase::elapsed,
                                          {{id<project::AutomationPointId>(335), 0, 0.75}},
                                          1};
  state.tracks[0].automation.push_back(track_lane);
  state.tracks[0].clips[0].automation.push_back(clip_lane);
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "schema 1.11 lost send gain target or descriptor metadata");
  REQUIRE_NE(encoded.find("\"kind\":\"send_gain\""), std::string::npos,
             "schema 1.11 lost send gain target or descriptor metadata");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "schema 1.11 lost send gain target or descriptor metadata");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "send-gain", true));
  REQUIRE_EQ(take(store.save(state, SaveKind::explicit_save)).status, SaveStatus::committed,
             "send gain automation did not survive durable save/reopen");
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "send gain automation did not survive durable save/reopen");
  REQUIRE_TRUE(!detail::decode_manifest(replaced(encoded, "\"send_id\"", "\"unknown\"")),
               "malformed send gain target was accepted");
  REQUIRE_TRUE(!detail::decode_manifest(
                   replaced(encoded, send.to_string(), id<project::SendId>(999).to_string())),
               "malformed send gain target was accepted");
}

void named_revisions_survive_reopen_and_preserve_current() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto first = take(writer.save(snapshot(), SaveKind::explicit_save));
  REQUIRE_EQ(take(writer.name_current_revision("First mix")).status, SaveStatus::committed,
             "first named revision was not committed");
  const auto second = take(writer.save(snapshot(2), SaveKind::explicit_save));
  REQUIRE_OK(writer.save(snapshot(3), SaveKind::autosave));
  REQUIRE_EQ(take(writer.name_current_revision("Mix v2")).generation, second.generation,
             "named revision did not follow CURRENT after autosave");
  const auto entries = take(writer.list_named_revisions());
  REQUIRE_TRUE(entries.issues.empty(), "named revision listing lost or mislabeled generations");
  REQUIRE_EQ(entries.revisions.size(), 2, "named revision listing lost or mislabeled generations");
  REQUIRE_EQ(entries.revisions[0].name, "First mix",
             "named revision listing lost or mislabeled generations");
  REQUIRE_EQ(entries.revisions[0].generation, first.generation,
             "named revision listing lost or mislabeled generations");
  REQUIRE_EQ(entries.revisions[1].name, "Mix v2",
             "named revision listing lost or mislabeled generations");
  REQUIRE_EQ(entries.revisions[1].generation, second.generation,
             "named revision listing lost or mislabeled generations");
  for (const auto& entry : entries.revisions) {
    REQUIRE_EQ(entry.entry.size(), 32,
               "display name became a path segment or pointer was not a regular file");
    REQUIRE_TRUE(fs::is_regular_file(root / "state" / "revisions" / entry.entry),
                 "display name became a path segment or pointer was not a regular file");
  }
  REQUIRE_TRUE(!fs::exists(root / "state" / "revisions" / "First mix"),
               "display name was used as a filesystem path");
  REQUIRE_TRUE(!fs::exists(root / "state" / "revisions" / "Mix v2"),
               "display name was used as a filesystem path");
  REQUIRE_TRUE(!writer.name_current_revision("First mix"), "duplicate revision name was accepted");
  REQUIRE_EQ(writer.name_current_revision("First mix").error().code,
             foundation::ErrorCode::conflict, "duplicate revision name was accepted");
  writer = take(ProjectStore::open_reader(root));
  REQUIRE_EQ(take(writer.load_named_revision("First mix")).snapshot.revision, 1,
             "reopen lost named, current, or autosaved generations");
  REQUIRE_EQ(take(writer.load_named_revision("Mix v2")).snapshot.revision, 2,
             "reopen lost named, current, or autosaved generations");
  REQUIRE_EQ(take(writer.load_current()).snapshot.revision, 2,
             "reopen lost named, current, or autosaved generations");
  REQUIRE_EQ(take(writer.load_autosave()).snapshot.revision, 3,
             "reopen lost named, current, or autosaved generations");
  require(!writer.name_current_revision("Read only"), "reader created a named revision");
}

void named_revision_sources_remain_authorized_after_current_drops_them() {
  Directory temp;
  const auto root = temp.path() / "project";
  const auto input = temp.path() / "input.wav";
  write_wav(input);
  const int fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "named revision source fixture did not open");
  auto writer = take(ProjectStore::open_writer(root, true));
  auto receipt = take(writer.stage_wav(id<project::SourceId>(4), fd));
  REQUIRE_EQ(::close(fd), 0, "named revision source fixture did not close");
  auto with_source = snapshot();
  with_source.sources.push_back(receipt.record());
  std::array<SourceReceipt, 1> receipts{std::move(receipt)};
  const auto named_save = take(writer.save(with_source, SaveKind::explicit_save, receipts));
  REQUIRE_EQ(take(writer.name_current_revision("Source mix")).generation, named_save.generation,
             "source generation was not named");
  REQUIRE_OK(writer.save(snapshot(2), SaveKind::explicit_save));
  auto restored = take(writer.load_named_revision("Source mix")).snapshot;
  restored.revision = 3;
  const auto resaved = take(writer.save(restored, SaveKind::explicit_save));
  REQUIRE_EQ(resaved.status, SaveStatus::committed,
             "named-revision-only managed source could not be saved after restore");
  REQUIRE_EQ(take(writer.load_current()).snapshot.sources, with_source.sources,
             "named-revision-only managed source could not be saved after restore");
}

void named_revision_requires_valid_current_and_ignores_foreign_entries() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  auto missing = writer.name_current_revision("Before first save");
  REQUIRE_TRUE(!missing, "named revision was created without a saved generation");
  REQUIRE_EQ(missing.error().code, foundation::ErrorCode::not_found,
             "named revision was created without a saved generation");
  REQUIRE_TRUE(!fs::exists(root / "state" / "revisions"),
               "named revision was created without a saved generation");
  const auto saved = take(writer.save(snapshot(), SaveKind::explicit_save));
  const auto pointer = root / "state" / "CURRENT";
  std::ifstream original(pointer, std::ios::binary);
  const std::string valid_pointer(std::istreambuf_iterator<char>{original}, {});
  {
    std::ofstream out(pointer, std::ios::trunc);
    out << "damaged current pointer";
  }
  auto damaged = writer.name_current_revision("Broken");
  REQUIRE_TRUE(!damaged, "named revision was created from a damaged CURRENT pointer");
  REQUIRE_EQ(damaged.error().code, foundation::ErrorCode::corrupt_data,
             "named revision was created from a damaged CURRENT pointer");
  REQUIRE_TRUE(!fs::exists(root / "state" / "revisions"),
               "named revision was created from a damaged CURRENT pointer");
  {
    std::ofstream out(pointer, std::ios::trunc);
    out << valid_pointer;
  }
  REQUIRE_OK(writer.name_current_revision("Restored"));
  const auto revision_dir = root / "state" / "revisions";
  std::ofstream(revision_dir / "README") << "foreign file";
  const auto listing = take(writer.list_named_revisions());
  REQUIRE_EQ(listing.revisions.size(), 1,
             "foreign named-revision entry hid a valid saved revision");
  REQUIRE_EQ(listing.revisions[0].generation, saved.generation,
             "foreign named-revision entry hid a valid saved revision");
  REQUIRE_EQ(listing.issues.size(), 1, "foreign named-revision entry hid a valid saved revision");
  REQUIRE_EQ(listing.issues[0].entry, "README",
             "foreign named-revision entry hid a valid saved revision");
  REQUIRE_EQ(listing.issues[0].kind, NamedRevisionIssue::Kind::malformed,
             "foreign named-revision entry hid a valid saved revision");
  REQUIRE_EQ(take(writer.load_named_revision("Restored")).generation, saved.generation,
             "foreign named-revision entry hid a valid saved revision");
  const auto invalid = writer.load_named_revision("../outside");
  REQUIRE_TRUE(!invalid, "named revision lookup accepted a traversal name");
  REQUIRE_EQ(invalid.error().code, foundation::ErrorCode::invalid_argument,
             "named revision lookup accepted a traversal name");
}

void named_revision_interruption_and_hostile_entries() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(snapshot(), SaveKind::explicit_save));
  const std::vector<std::string> invalid_names{
      "", "..", "../escape", "a/b", "a\\b", std::string(65, 'x'), std::string("a\0b", 3)};
  for (const auto& name : invalid_names)
    require(!writer.name_current_revision(name), "malicious or oversized name was accepted");
  REQUIRE_TRUE(!fs::exists(root / "state" / "revisions"), "invalid name changed the filesystem");
  REQUIRE_TRUE(!fs::exists(temp.path() / "escape"), "invalid name changed the filesystem");
  writer = take(ProjectStore::open_reader(root));
  writer = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::revision_pointer_written; }));
  auto interrupted = writer.name_current_revision("Interrupted");
  REQUIRE_TRUE(!interrupted, "pre-publication interruption did not fail");
  REQUIRE_EQ(interrupted.error().code, foundation::ErrorCode::io_failure,
             "pre-publication interruption did not fail");
  auto incomplete = take(writer.list_named_revisions());
  REQUIRE_TRUE(incomplete.revisions.empty(),
               "unpublished pointer was falsely listed as a named revision");
  REQUIRE_EQ(incomplete.issues.size(), 1,
             "unpublished pointer was falsely listed as a named revision");
  REQUIRE_EQ(incomplete.issues[0].kind, NamedRevisionIssue::Kind::incomplete,
             "unpublished pointer was falsely listed as a named revision");
  REQUIRE_EQ(take(writer.load_current()).generation, baseline.generation,
             "interrupted named revision advanced CURRENT");
  writer = take(ProjectStore::open_reader(root));
  writer = take(ProjectStore::open_writer(
      root, false, [](SaveStage stage) { return stage == SaveStage::revision_pointer_published; }));
  auto uncertain = take(writer.name_current_revision("Published"));
  REQUIRE_EQ(uncertain.status, SaveStatus::uncertain,
             "post-publication interruption did not expose a valid uncertain pointer");
  REQUIRE_EQ(take(writer.load_named_revision("Published")).generation, baseline.generation,
             "post-publication interruption did not expose a valid uncertain pointer");
  writer = take(ProjectStore::open_reader(root));
  auto entries = take(writer.list_named_revisions());
  REQUIRE_EQ(entries.revisions.size(), 1, "reopen lost publication or incomplete-entry diagnostic");
  REQUIRE_EQ(entries.issues.size(), 1, "reopen lost publication or incomplete-entry diagnostic");
  const auto revision_dir = root / "state" / "revisions";
  const auto original = revision_dir / entries.revisions.front().entry;
  const auto duplicate = revision_dir / std::string(32, 'a');
  fs::copy_file(original, duplicate);
  auto duplicated = take(writer.list_named_revisions());
  REQUIRE_TRUE(duplicated.revisions.empty(),
               "duplicate display names selected an arbitrary generation");
  REQUIRE_EQ(duplicated.issues.size(), 3,
             "duplicate display names selected an arbitrary generation");
  REQUIRE_TRUE(!writer.load_named_revision("Published"),
               "duplicate display names selected an arbitrary generation");
  REQUIRE_EQ(writer.load_named_revision("Published").error().code,
             foundation::ErrorCode::corrupt_data,
             "duplicate display names selected an arbitrary generation");
  fs::remove(duplicate);
  const auto stale = revision_dir / std::string(32, 'b');
  std::ofstream(stale) << "{\"generation\":\"" << std::string(32, 'c')
                       << "\",\"name\":\"Stale\",\"sha256\":\"" << std::string(64, '0') << "\"}\n";
  std::ofstream(revision_dir / std::string(32, 'e')) << "not json";
  const auto outside = temp.path() / "outside";
  std::ofstream(outside) << "untouched";
  fs::create_symlink(outside, revision_dir / std::string(32, 'd'));
  auto malformed = take(writer.list_named_revisions());
  REQUIRE_EQ(malformed.revisions.size(), 1,
             "stale, symlink, or absent revision was not reported safely");
  REQUIRE_EQ(malformed.issues.size(), 4,
             "stale, symlink, or absent revision was not reported safely");
  REQUIRE_TRUE(!writer.load_named_revision("Stale"),
               "stale, symlink, or absent revision was not reported safely");
  REQUIRE_EQ(writer.load_named_revision("Stale").error().code, foundation::ErrorCode::corrupt_data,
             "stale, symlink, or absent revision was not reported safely");
  REQUIRE_TRUE(!writer.load_named_revision("Unknown"),
               "stale, symlink, or absent revision was not reported safely");
  std::ifstream original_outside(outside);
  std::string bytes;
  original_outside >> bytes;
  REQUIRE_EQ(bytes, "untouched", "named revision scan followed an external symlink");

  writer = take(ProjectStore::open_writer(root, false));
  REQUIRE_EQ(take(writer.name_current_revision("Interrupted")).status, SaveStatus::committed,
             "incomplete pointer reserved its display name forever");
  const auto retried = take(writer.list_named_revisions());
  REQUIRE_EQ(retried.revisions.size(), 2,
             "retry failed to publish a valid named revision beside an incomplete pointer");
  REQUIRE_EQ(retried.issues.size(), 4,
             "retry failed to publish a valid named revision beside an incomplete pointer");
  REQUIRE_EQ(take(writer.load_named_revision("Interrupted")).generation, baseline.generation,
             "retry failed to publish a valid named revision beside an incomplete pointer");
}

void replaced_revision_directory_cannot_publish_a_name() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  const auto baseline = take(writer.save(snapshot(), SaveKind::explicit_save));
  writer = take(ProjectStore::open_reader(root));
  bool replaced = false;
  writer = take(ProjectStore::open_writer(root, false, [&](SaveStage stage) {
    if (stage != SaveStage::revision_pointer_written)
      return false;
    fs::rename(root / "state" / "revisions", root / "state" / "revisions-displaced");
    fs::create_directory(root / "state" / "revisions");
    replaced = true;
    return false;
  }));
  const auto named = writer.name_current_revision("Displaced");
  REQUIRE_TRUE(replaced, "named revision was published through a detached directory");
  REQUIRE_TRUE(!named, "named revision was published through a detached directory");
  REQUIRE_EQ(named.error().code, foundation::ErrorCode::conflict,
             "named revision was published through a detached directory");
  const auto reader = take(ProjectStore::open_reader(root));
  REQUIRE_TRUE(take(reader.list_named_revisions()).revisions.empty(),
               "directory replacement changed the visible project or revision list");
  REQUIRE_EQ(take(reader.load_current()).generation, baseline.generation,
             "directory replacement changed the visible project or revision list");
}

void named_revision_validation_is_bounded() {
  Directory temp;
  const auto root = temp.path() / "project";
  auto writer = take(ProjectStore::open_writer(root, true));
  REQUIRE_OK(writer.save(snapshot(), SaveKind::explicit_save));
  REQUIRE_OK(writer.name_current_revision("Original"));
  std::ifstream current_file(root / "state" / "CURRENT");
  const std::string current_bytes(std::istreambuf_iterator<char>{current_file}, {});
  const auto pointer = take(detail::decode_pointer(current_bytes));
  for (unsigned index = 0; index < 256; ++index) {
    char entry[33]{};
    std::snprintf(entry, sizeof(entry), "%032x", index);
    std::ofstream(root / "state" / "revisions" / entry)
        << "{\"generation\":\"" << pointer.first << "\",\"name\":\"R" << index << "\",\"sha256\":\""
        << pointer.second << "\"}\n";
  }
  const auto bounded = writer.list_named_revisions();
  REQUIRE_TRUE(!bounded, "oversized named revision set was decoded without a bound");
  REQUIRE_EQ(bounded.error().code, foundation::ErrorCode::resource_exhausted,
             "oversized named revision set was decoded without a bound");
}

void channel_level_automation_schema21_round_trip() {
  auto state = snapshot();
  const auto track = state.tracks.front().id;
  state.tracks.front().level = 0.7;
  state.tracks.front().pan = -0.25;
  state.tracks.front().automation.push_back(
      {id<project::AutomationLaneId>(8'001),
       project::TrackControlTarget{track, project::TrackControlTarget::Key::channel_level},
       {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0, 4.0,
        0.7, false, 0, project::AutomationScale::decibel},
       project::AutomationTimebase::musical,
       {{id<project::AutomationPointId>(8'002), 0, 0.5, project::AutomationShape::linear},
        {id<project::AutomationPointId>(8'003), timeline::ticks_per_quarter, 1.0}},
       1});
  const auto generation = std::string(32, 'a');
  const auto encoded =
      detail::encode_manifest(state, generation, std::nullopt, SaveKind::explicit_save);
  REQUIRE_NE(encoded.find("\"minor\":47"), std::string::npos,
             "channel automation schema 1.21 did not preserve level lane and balance");
  REQUIRE_EQ(take(detail::decode_manifest(encoded)).snapshot.tracks, state.tracks,
             "channel automation schema 1.21 did not preserve level lane and balance");
  require(!detail::decode_manifest(replaced(encoded, "\"minor\":47", "\"minor\":20")),
          "schema 1.20 silently accepted a 1.21 channel automation target");
  Directory temp;
  auto store = take(ProjectStore::open_writer(temp.path() / "channel-automation", true));
  (void)take(store.save(state, SaveKind::explicit_save));
  REQUIRE_EQ(take(store.load_current()).snapshot.tracks, state.tracks,
             "saved channel automation did not reopen exactly");
}

} // namespace

int main() {
  try {
    round_trip_and_autosave();
    track_channel_controls_round_trip();
    clip_processing_round_trip();
    clip_warp_round_trip();
    faults_preserve_previous_save();
    writer_reopen_cleans_only_owned_save_stages();
    orphaned_generation_remains_recoverable_without_changing_save_choices();
    corrupt_autosave_blocks_new_save_but_preserves_recovery();
    recovery_adoption_validates_choice_and_durability();
    adoption_interruption_does_not_hide_pointer_state();
    corrupt_pointer_and_manifest();
    state_directory_replacement_cannot_commit_a_detached_save();
    replaced_writer_lock_cannot_publish_a_generation();
    two_writers_and_symlinks();
    foreign_project_and_generation_collision();
    malformed_writer_lock_is_bounded();
    bounded_manifest_codec();
    pointer_target_and_path_validation();
    no_follow_project_layout();
    orphan_autosave_keeps_project_identity();
    held_parent_survives_ancestor_swap();
    swapped_root_symlink_is_rejected();
    recovery_scan_is_bounded();
    durable_save_prunes_only_unreferenced_generations();
    autosave_only_generation_is_retained();
    per_pointer_revisions_never_regress();
    corrupt_staged_checksum_never_publishes();
    foreign_autosave_blocks_explicit_save();
    recovery_manifest_budget_is_bounded();
    midi_round_trip_and_validation();
    native_regions_round_trip();
    native_voice_lanes_round_trip();
    managed_wav_receipts_and_missing_diagnostics();
    external_link_schema_reopen_and_missing_diagnostics();
    external_relink_verifies_before_pointer_publication();
    external_worker_proof_is_exact_and_writer_bound();
    damaged_managed_source_keeps_saved_edits_and_recovery_choice();
    missing_managed_wav_requires_exact_recovery_candidate();
    interrupted_managed_restore_reopens_without_substituting_media();
    generation_namespace_swap_blocks_managed_restore_publication();
    staged_asset_failures_and_foreign_receipts();
    asset_directory_swap_cannot_redirect_staging();
    unpublished_wav_stage_is_disposable_and_reopen_cleans_only_stages();
    reopen_cleans_abandoned_sources_beyond_old_scan_limit();
    changed_staged_bytes_never_receive_a_source_receipt();
    master_effect_round_trip_and_validation();
    source_encodings_round_trip();
    vst3_round_trip();
    routing_round_trip_and_failed_save();
    automation_round_trip_and_failures();
    channel_level_automation_schema21_round_trip();
    send_gain_round_trip();
    named_revisions_survive_reopen_and_preserve_current();
    named_revision_sources_remain_authorized_after_current_drops_them();
    named_revision_requires_valid_current_and_ignores_foreign_entries();
    named_revision_interruption_and_hostile_entries();
    replaced_revision_directory_cannot_publish_a_name();
    named_revision_validation_is_bounded();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
