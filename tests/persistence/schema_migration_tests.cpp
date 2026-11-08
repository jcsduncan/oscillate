#include "migration.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/check.hpp"
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

std::string current_manifest() {
  auto tempo =
      take(timeline::TempoMap::create({{{0}, project_bpm, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, beats_per_bar, beat_note}}, 1));
  const project::Snapshot state{take(project::ProjectId::from_bits({0, 1})),
                                project_revision,
                                project_sample_rate,
                                std::move(tempo),
                                std::move(meter),
                                {},
                                {}};
  return detail::encode_manifest(state, std::string(generation_length, generation_digit),
                                 std::nullopt, SaveKind::explicit_save);
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
  if (!detail::released_schemas.empty()) {
    return;
  }
  static_assert(current_schema.minor > 0, "a development schema precedes the current one");
  constexpr SchemaVersion development{current_schema.major, current_schema.minor - 1};
  const auto result = detail::decode_manifest(with_schema(development));
  require(!result, "a development schema does not open");
  REQUIRE_EQ(result.error().code, ErrorCode::unsupported, "a development schema is unsupported");
  require(result.error().message.find("predates") != std::string::npos,
          "the refusal says the schema predates the first release: " + result.error().message);
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

} // namespace

int main() {
  try {
    golden_manifests_open_and_round_trip();
    chain_covers_every_released_schema();
    current_schema_opens();
    newer_schema_is_refused();
    pre_release_schema_is_refused();
    chain_applies_steps_in_order();
    std::cout << "Schema migration contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
