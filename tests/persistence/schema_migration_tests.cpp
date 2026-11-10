#include "migration.hpp"
#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// ADR-0095: every released schema has a golden manifest that opens through the
// migration chain and round-trips; newer and pre-release schemas are refused.
namespace {
using namespace oscillate;
using namespace oscillate::persistence;
using detail::current_schema;
using detail::MigrationStep;
using detail::SchemaVersion;
using foundation::ErrorCode;
using Json = nlohmann::json;
using oscillate::test::require;
using oscillate::test::take;

constexpr std::string_view golden_dir_text = OSCILLATE_SCHEMA_GOLDEN_DIR;
constexpr std::string_view golden_prefix = "schema-";
constexpr std::string_view golden_extension = ".json";
constexpr std::string_view expected_suffix = ".expected";
constexpr std::uint32_t project_sample_rate = 48'000;
constexpr std::uint64_t project_revision = 2;
constexpr double project_bpm = 120.0;
constexpr std::uint32_t beats_per_bar = 4;
constexpr std::uint32_t beat_note = 4;
constexpr char generation_digit = 'a';
constexpr std::size_t generation_length = 32;

std::string golden_name(SchemaVersion version) {
  return std::string(golden_prefix) + detail::schema_text(version) + std::string(golden_extension);
}

// The golden's essential content, written down when the golden is saved, so a
// migration that drops tracks, clips, notes, effects or lanes fails.
std::string expected_name(SchemaVersion version) {
  return std::string(golden_prefix) + detail::schema_text(version) + std::string(expected_suffix) +
         std::string(golden_extension);
}

Json content(const project::Snapshot& state) {
  auto tracks = Json::array();
  for (const auto& track : state.tracks) {
    std::size_t notes = 0;
    for (const auto& clip : track.midi_clips) {
      notes += clip.notes.size();
    }
    tracks.push_back({{"name", track.name},
                      {"audio_clips", track.clips.size()},
                      {"midi_clips", track.midi_clips.size()},
                      {"notes", notes},
                      {"effects", track.effects.size()},
                      {"automation_lanes", track.automation.size()}});
  }
  return {{"tracks", std::move(tracks)}, {"markers", state.markers.size()}};
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "golden manifest is readable: " + path.string());
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string encode(const LoadedGeneration& loaded) {
  return detail::encode_manifest(loaded.snapshot, loaded.generation, loaded.parent, loaded.kind);
}

project::Snapshot project_state(std::uint64_t revision) {
  auto tempo =
      take(timeline::TempoMap::create({{{0}, project_bpm, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, beats_per_bar, beat_note}}, 1));
  return {take(project::ProjectId::from_bits({0, 1})),
          revision,
          project_sample_rate,
          std::move(tempo),
          std::move(meter),
          {},
          {}};
}

std::string current_manifest() {
  return detail::encode_manifest(project_state(project_revision),
                                 std::string(generation_length, generation_digit), std::nullopt,
                                 SaveKind::explicit_save);
}

std::string with_schema(SchemaVersion version) {
  auto manifest = Json::parse(current_manifest());
  manifest["schema"] = {{"major", version.major}, {"minor", version.minor}};
  return manifest.dump();
}

// The golden set is exactly the released schemas, and each one opens in the
// current schema and saves bytes that reopen to the same bytes.
void golden_manifests_open_and_round_trip() {
  const std::filesystem::path golden_dir{golden_dir_text};
  std::set<std::string> expected;
  for (const auto version : detail::released_schemas) {
    expected.insert(golden_name(version));
    expected.insert(expected_name(version));
  }
  std::set<std::string> present;
  for (const auto& entry : std::filesystem::directory_iterator(golden_dir)) {
    if (entry.path().extension() == golden_extension) {
      present.insert(entry.path().filename().string());
    }
  }
  require(present == expected,
          "one golden manifest and its expected content per released schema, and no others");

  for (const auto version : detail::released_schemas) {
    const auto name = golden_name(version);
    const auto opened = take(detail::decode_manifest(read_file(golden_dir / name)));
    REQUIRE_EQ(content(opened.snapshot),
               Json::parse(read_file(golden_dir / expected_name(version))),
               name + " keeps its tracks, clips, notes, effects and lanes");
    const auto saved = encode(opened);
    const auto saved_json = Json::parse(saved);
    REQUIRE_EQ(saved_json["schema"]["minor"].get<std::uint32_t>(), current_schema.minor,
               name + " saves in the current schema");
    const auto reopened = take(detail::decode_manifest(saved));
    REQUIRE_EQ(encode(reopened), saved, name + " round-trips");
  }
}

// The built-in chain runs from the oldest released schema to the current one,
// one step after another.
void chain_covers_every_released_schema() {
  const auto steps = detail::migration_steps();
  const auto& released = detail::released_schemas;
  for (std::size_t index = 1; index < released.size(); ++index) {
    require(released[index - 1] < released[index], "released schemas are listed oldest first");
  }
  if (released.empty()) {
    require(steps.empty(), "no migrations before the first released schema");
    return;
  }
  require(released.back() <= current_schema, "no released schema is newer than this build");
  if (released.front() == current_schema) {
    require(steps.empty(), "no migrations while the current schema is the only release");
    return;
  }
  require(!steps.empty() && steps.front().from == released.front(),
          "the chain starts at the oldest released schema");
  for (std::size_t index = 1; index < steps.size(); ++index) {
    require(steps[index - 1].to == steps[index].from, "each step starts where the last ended");
  }
  require(steps.back().to == current_schema, "the chain ends at the current schema");
}

void current_schema_opens() {
  REQUIRE_OK(detail::decode_manifest(with_schema(current_schema)),
             "the current schema opens unchanged");
}

void newer_schema_is_refused() {
  for (const auto newer : {SchemaVersion{current_schema.major, current_schema.minor + 1},
                           SchemaVersion{current_schema.major + 1, 0}}) {
    const auto result = detail::decode_manifest(with_schema(newer));
    require(!result, "a newer schema does not open");
    REQUIRE_EQ(result.error().code, ErrorCode::unsupported, "a newer schema is unsupported");
    require(result.error().message.find("newer Oscillate") != std::string::npos,
            "the refusal says the project comes from a newer Oscillate: " + result.error().message);
  }
}

void pre_release_schema_is_refused() {
  constexpr auto oldest = detail::released_schemas.front();
  static_assert(oldest.minor > 0, "a development schema precedes the oldest release");
  constexpr SchemaVersion development{oldest.major, oldest.minor - 1};
  const auto result = detail::decode_manifest(with_schema(development));
  require(!result, "a development schema does not open");
  REQUIRE_EQ(result.error().code, ErrorCode::unsupported, "a development schema is unsupported");
  require(result.error().message.find("predates the oldest supported release (schema " +
                                      detail::schema_text(oldest) + ")") != std::string::npos,
          "the refusal says the schema predates the oldest release: " + result.error().message);
}

// 1.47 -> 1.48. A 1.47 convolution insert: its managed IR, as 1.47 saved it.
constexpr SchemaVersion schema_1_47{1, 47};
constexpr std::uint32_t cast_engine_version = 1;
constexpr std::uint64_t convolution_generation = 7;
constexpr double convolution_wet = 0.35;
constexpr std::uint64_t ir_bytes = 4'096;
constexpr std::uint32_t ir_rate = 48'000;
constexpr std::uint32_t ir_channels = 2;
constexpr std::uint64_t ir_frames = 2'000;
constexpr double cast_normalise_off = 0.0;

Json managed_ir() {
  return {{"source", "00000000-0000-0000-0000-0000000000aa"},
          {"sha256", std::string(foundation::sha256_bytes * 2, generation_digit)},
          {"encoded_bytes", ir_bytes},
          {"sample_rate", ir_rate},
          {"channels", ir_channels},
          {"frames", ir_frames}};
}

Json convolution_insert(std::string_view id, bool enabled) {
  return {
      {"id", id},
      {"enabled", enabled},
      {"kind", "native_convolution"},
      {"payload",
       {{"generation", convolution_generation}, {"wet", convolution_wet}, {"ir", managed_ir()}}}};
}

Json manifest_1_47(Json track_effects, Json master_effects) {
  return {{"schema", {{"major", schema_1_47.major}, {"minor", schema_1_47.minor}}},
          {"project",
           {{"tracks", Json::array({{{"name", "A"}, {"effects", std::move(track_effects)}}})},
            {"master_effects", std::move(master_effects)}}}};
}

void check_cast(const Json& insert, std::string_view id, bool enabled) {
  REQUIRE_EQ(insert["id"], Json(id), "the insert keeps its slot id");
  REQUIRE_EQ(insert["enabled"], Json(enabled), "the insert keeps its bypass state");
  REQUIRE_EQ(insert["kind"], Json("native_effect"), "a convolution becomes a built-in effect");
  const auto& payload = insert["payload"];
  REQUIRE_EQ(payload["engine"], Json("cast"), "a convolution becomes Cast");
  REQUIRE_EQ(payload["engine_version"], Json(cast_engine_version), "Cast's first engine version");
  REQUIRE_EQ(payload["generation"], Json(convolution_generation), "the generation carries over");
  REQUIRE_EQ(payload["link"], Json(""), "a converted Cast is not linked");
  const Json parameters = Json::array({{{"id", "cast.mix"}, {"value", convolution_wet}},
                                       {{"id", "cast.normalise"}, {"value", cast_normalise_off}}});
  REQUIRE_EQ(payload["parameters"], parameters, "wet becomes the mix, at the IR's own level");
  const Json media = Json::array(
      {{{"id", "ir"}, {"frame_length", 0}, {"builtin", nullptr}, {"managed", managed_ir()}}});
  REQUIRE_EQ(payload["document"]["media"], media, "the IR moves to Cast's ir slot");
  // The decoder reads counts as unsigned integers only.
  require(payload["document"]["media"][0]["frame_length"].is_number_unsigned() &&
              payload["engine_version"].is_number_unsigned(),
          "the step writes counts as unsigned integers");
  for (const auto* empty : {"routes", "curves", "chains", "sequences", "labels"})
    REQUIRE_EQ(payload["document"][empty], Json::array(), "Cast's document holds only its IR");
}

void convolution_inserts_become_cast() {
  const auto other = Json{{"id", "kept"}, {"kind", "native_effect"}, {"payload", Json::object()}};
  const auto migrated = detail::migrate_1_47_to_1_48(
      manifest_1_47(Json::array({other, convolution_insert("track", false)}),
                    Json::array({convolution_insert("master", true)})));
  const auto& track_effects = migrated["project"]["tracks"][0]["effects"];
  REQUIRE_EQ(track_effects.size(), std::size_t{2}, "no insert is dropped");
  REQUIRE_EQ(track_effects[0], other, "other inserts are untouched");
  check_cast(track_effects[1], "track", false);
  check_cast(migrated["project"]["master_effects"][0], "master", true);
}

// 1.47 rejected a convolution payload with unknown fields; the step leaves it
// for the decoder to reject rather than converting it.
void malformed_convolution_is_not_converted() {
  auto extra = convolution_insert("track", true);
  extra["payload"]["unknown"] = true;
  const auto manifest = manifest_1_47(Json::array({extra}), Json::array());
  REQUIRE_EQ(detail::migrate_1_47_to_1_48(manifest), manifest,
             "a convolution payload with unknown fields was converted");
}

void manifest_without_convolution_is_unchanged() {
  const auto before = Json::parse(current_manifest());
  REQUIRE_EQ(detail::migrate_1_47_to_1_48(before), before,
             "a 1.47 manifest without convolution needs no change");
  const auto empty = manifest_1_47(Json::array(), Json::array());
  REQUIRE_EQ(detail::migrate_1_47_to_1_48(empty), empty, "empty effect lists stay empty");
}

// The real chain takes a 1.47 project to the current schema and decodes it.
void schema_1_47_opens_through_the_chain() {
  const auto opened = take(detail::decode_manifest(with_schema(schema_1_47)));
  REQUIRE_EQ(encode(opened), current_manifest(), "a 1.47 manifest opens as the current schema");
}

// A synthetic chain: each step appends its own mark, so order and coverage show.
constexpr SchemaVersion first_release{1, 1};
constexpr SchemaVersion second_release{1, 2};
constexpr SchemaVersion latest{1, 3};
constexpr SchemaVersion before_first_release{1, 0};

Json mark(Json manifest, std::string_view value) {
  manifest["marks"].push_back(value);
  return manifest;
}
Json to_second(Json manifest) { return mark(std::move(manifest), "second"); }
Json to_latest(Json manifest) { return mark(std::move(manifest), "latest"); }

constexpr std::array<SchemaVersion, 2> synthetic_releases{first_release, second_release};
constexpr std::array<MigrationStep, 2> synthetic_steps{
    MigrationStep{first_release, second_release, to_second},
    MigrationStep{second_release, latest, to_latest}};

Json synthetic_manifest(SchemaVersion version) {
  return {{"schema", {{"major", version.major}, {"minor", version.minor}}},
          {"marks", Json::array()}};
}

void chain_applies_steps_in_order() {
  const auto from_first =
      take(detail::migrate_manifest(synthetic_manifest(first_release), first_release, latest,
                                    synthetic_releases, synthetic_steps));
  REQUIRE_EQ(from_first["marks"], Json::array({"second", "latest"}), "every step runs, in order");
  REQUIRE_EQ(from_first["schema"], synthetic_manifest(latest)["schema"],
             "the migrated manifest names the current schema");

  const auto from_second =
      take(detail::migrate_manifest(synthetic_manifest(second_release), second_release, latest,
                                    synthetic_releases, synthetic_steps));
  REQUIRE_EQ(from_second["marks"], Json::array({"latest"}), "a later release skips earlier steps");

  const auto current = take(detail::migrate_manifest(synthetic_manifest(latest), latest, latest,
                                                     synthetic_releases, synthetic_steps));
  REQUIRE_EQ(current, synthetic_manifest(latest), "the current schema passes through untouched");

  const auto too_old =
      detail::migrate_manifest(synthetic_manifest(before_first_release), before_first_release,
                               latest, synthetic_releases, synthetic_steps);
  require(!too_old && too_old.error().code == ErrorCode::unsupported,
          "a schema before the first release is refused");

  const auto broken =
      detail::migrate_manifest(synthetic_manifest(first_release), first_release, latest,
                               synthetic_releases, std::span(synthetic_steps).first(1));
  require(!broken && broken.error().code == ErrorCode::internal,
          "a gap in the chain fails instead of decoding a stale layout");
}

// ADR-0095 point 8, through the real store: a project opened from an older
// release keeps that generation as a named revision on its first save, and
// pruning keeps it however many saves follow.
constexpr SchemaVersion previous_release{current_schema.major, current_schema.minor - 1};
Json unchanged(Json manifest) { return manifest; }
constexpr std::array<SchemaVersion, 1> previous_releases{previous_release};
constexpr std::array<MigrationStep, 1> previous_steps{
    MigrationStep{previous_release, current_schema, unchanged}};
constexpr std::uint64_t pre_upgrade_revision = 1;
// More saves than pruning keeps, so the pre-upgrade generation would go.
constexpr std::size_t saves_after_upgrade = detail::retained_recent_generations + 2;

void write_file(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  require(out.good(), "fixture file is writable: " + path.string());
}

void pre_upgrade_generation_survives_pruning() {
  const oscillate::test::TempDir directory("oscillate-pre-upgrade");
  const auto root = directory / "project";
  std::string pre_upgrade;
  {
    auto writer = take(ProjectStore::open_writer(root, true));
    pre_upgrade =
        take(writer.save(project_state(pre_upgrade_revision), SaveKind::explicit_save)).generation;
  }
  // Rewrite that save as the previous release would have written it.
  const auto generation = root / "state" / "generations" / pre_upgrade;
  auto manifest = Json::parse(read_file(generation / "manifest.json"));
  manifest["schema"] = {{"major", previous_release.major}, {"minor", previous_release.minor}};
  const auto bytes = manifest.dump() + '\n';
  const auto digest = take(foundation::sha256(bytes));
  const auto hash = foundation::to_hex(std::span(digest));
  write_file(generation / "manifest.json", bytes);
  write_file(generation / "manifest.sha256", hash + '\n');
  write_file(root / "state" / "CURRENT", detail::encode_pointer(pre_upgrade, hash));

  const detail::ScopedMigrationChain chain(previous_releases, previous_steps);
  const auto schema = detail::schema_text(previous_release);
  {
    // A revision whose durability is uncertain stops the save before CURRENT
    // moves past the generation it protects.
    bool revision_flush_failed = false;
    auto uncertain = take(ProjectStore::open_writer(root, false, [&](SaveStage stage) {
      revision_flush_failed |= stage == SaveStage::revision_directory_flush_failed;
      return stage == SaveStage::revision_directory_flush_failed;
    }));
    REQUIRE_FALSE(uncertain.save(project_state(pre_upgrade_revision + 1), SaveKind::explicit_save)
                      .has_value(),
                  "a save went ahead with an uncertain pre-upgrade revision");
    REQUIRE_EQ(take(uncertain.load_current()).generation, pre_upgrade,
               "CURRENT moved past the pre-upgrade generation");
    REQUIRE_TRUE(revision_flush_failed, "the save did not reach the revision flush");
    REQUIRE_EQ(take(uncertain.list_named_revisions()).revisions.size(), std::size_t{1},
               "the uncertain revision was not published");
  }
  auto writer = take(ProjectStore::open_writer(root, false));
  REQUIRE_EQ(take(writer.load_current()).migrated_from, std::optional<std::string>(schema),
             "the store reports the schema it migrated from");
  for (std::size_t save = 1; save <= saves_after_upgrade; ++save) {
    REQUIRE_EQ(
        take(writer.save(project_state(pre_upgrade_revision + save), SaveKind::explicit_save))
            .status,
        SaveStatus::committed, "a save after the upgrade was not durable");
  }
  REQUIRE_EQ(take(writer.load_current()).migrated_from, std::optional<std::string>(),
             "later saves are in the current schema");
  const auto listing = take(writer.list_named_revisions());
  REQUIRE_EQ(listing.revisions.size(), std::size_t{1},
             "only the first save after the upgrade names a revision");
  const auto& kept = listing.revisions.front();
  REQUIRE_EQ(kept.generation, pre_upgrade, "the revision names the pre-upgrade generation");
  REQUIRE_EQ(kept.name, ProjectStore::pre_upgrade_revision_name(schema, pre_upgrade),
             "the revision says which schema it predates");
  const auto reopened = take(writer.load_named_revision(kept.name));
  REQUIRE_EQ(reopened.snapshot.revision, pre_upgrade_revision,
             "the pre-upgrade generation still loads after pruning");
  REQUIRE_EQ(reopened.migrated_from, std::optional<std::string>(schema),
             "the kept generation is still the one the previous release wrote");
}

} // namespace

int main() {
  try {
    golden_manifests_open_and_round_trip();
    chain_covers_every_released_schema();
    current_schema_opens();
    newer_schema_is_refused();
    pre_release_schema_is_refused();
    convolution_inserts_become_cast();
    malformed_convolution_is_not_converted();
    manifest_without_convolution_is_unchanged();
    schema_1_47_opens_through_the_chain();
    chain_applies_steps_in_order();
    pre_upgrade_generation_survives_pruning();
    std::cout << "Schema migration contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
