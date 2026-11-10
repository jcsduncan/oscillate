// Master rebalance (schema 1.49, ADR-0123): the per-role settings and the
// tracks' chosen and heard roles round-trip exactly; malformed or
// out-of-range values are refused.

#include "oscillate/persistence/store.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "private.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace {
using namespace oscillate;
using namespace oscillate::project;
using namespace oscillate::persistence;
using Json = nlohmann::json;
using oscillate::test::take;

template <typename Id> Id id(std::uint64_t value) { return take(Id::from_bits({0, value})); }

constexpr double bpm = 120.0;
constexpr std::uint8_t beats = 4;
constexpr std::uint32_t rate = 48'000;
constexpr int schema_minor = 49;
constexpr double vocal_gain_db = 2.5;
constexpr double drum_low_db = -3.0;
constexpr double drum_low_hz = 80.0;
constexpr double bass_mid_db = 4.0;
constexpr double bass_mid_hz = 250.0;
constexpr double bass_mid_q = 1.4;
constexpr double music_high_db = -1.5;
constexpr double music_high_hz = 6'000.0;
constexpr double past_limit_db = rebalance_gain_limit_db + 1.0;

Snapshot snapshot() {
  auto tempo = take(timeline::TempoMap::create({{{0}, bpm, timeline::TempoShape::step}}, 1));
  auto meter = take(timeline::MeterMap::create({{{0}, beats, beats}}, 1));
  Snapshot state{id<ProjectId>(1), 2, rate, std::move(tempo), std::move(meter), {}, {}};
  Track chosen{id<TrackId>(2)};
  chosen.mix_role = MixRole::vocals;
  chosen.heard_role = MixRole::music;
  Track heard{id<TrackId>(3)};
  heard.heard_role = MixRole::drums;
  Track excluded{id<TrackId>(4), {}, TrackKind::group_bus};
  excluded.mix_role = MixRole::none;
  state.tracks = {chosen, heard, excluded};
  state.rebalance.enabled = true;
  auto& roles = state.rebalance.roles;
  roles[static_cast<std::size_t>(MixRole::vocals)].gain_db = vocal_gain_db;
  roles[static_cast<std::size_t>(MixRole::drums)].low_db = drum_low_db;
  roles[static_cast<std::size_t>(MixRole::drums)].low_hz = drum_low_hz;
  roles[static_cast<std::size_t>(MixRole::bass)].mid_db = bass_mid_db;
  roles[static_cast<std::size_t>(MixRole::bass)].mid_hz = bass_mid_hz;
  roles[static_cast<std::size_t>(MixRole::bass)].mid_q = bass_mid_q;
  roles[static_cast<std::size_t>(MixRole::music)].high_db = music_high_db;
  roles[static_cast<std::size_t>(MixRole::music)].high_hz = music_high_hz;
  return state;
}

std::string encode(const Snapshot& state) {
  return detail::encode_manifest(state, std::string(32, 'a'), std::nullopt,
                                 SaveKind::explicit_save);
}

void rebalance_round_trips() {
  const auto original = snapshot();
  const auto json = Json::parse(encode(original));
  REQUIRE_EQ(json["schema"]["minor"], schema_minor, "the rebalance saves in the current schema");
  REQUIRE_EQ(json["project"]["tracks"][0]["mix_role"], "vocals", "roles save by name");
  REQUIRE_EQ(json["project"]["tracks"][1]["mix_role"], nullptr, "an inferred role saves nothing");
  REQUIRE_EQ(json["project"]["tracks"][1]["heard_role"], "drums");
  REQUIRE_EQ(json["project"]["tracks"][2]["mix_role"], "none");
  REQUIRE_EQ(json["project"]["rebalance"]["roles"].size(), mix_role_count, "one setting per role");
  const auto decoded = take(detail::decode_manifest(json.dump()));
  REQUIRE_EQ(decoded.snapshot.rebalance, original.rebalance, "the settings round-trip exactly");
  for (std::size_t i = 0; i < original.tracks.size(); ++i) {
    REQUIRE_EQ(decoded.snapshot.tracks[i].mix_role, original.tracks[i].mix_role);
    REQUIRE_EQ(decoded.snapshot.tracks[i].heard_role, original.tracks[i].heard_role);
  }
}

void malformed_rebalance_is_refused() {
  const auto valid = Json::parse(encode(snapshot()));
  const auto refused = [&](const std::function<void(Json&)>& change, const char* why) {
    auto manifest = valid;
    change(manifest["project"]);
    REQUIRE_TRUE(!detail::decode_manifest(manifest.dump()), why);
  };
  refused([](Json& p) { p.erase("rebalance"); }, "a missing rebalance");
  refused([](Json& p) { p["tracks"][0].erase("mix_role"); }, "a missing role");
  refused([](Json& p) { p["tracks"][0]["mix_role"] = "lead"; }, "an unknown role");
  refused([](Json& p) { p["rebalance"]["roles"].erase(0); }, "a missing role setting");
  refused([](Json& p) { p["rebalance"]["roles"][0]["gain_db"] = past_limit_db; },
          "a gain past its limit");
  refused([](Json& p) { p["rebalance"]["roles"][1]["low_hz"] = rebalance_low_max_hz * 2.0; },
          "a frequency out of range");
  refused([](Json& p) { p["rebalance"]["roles"][2]["extra"] = 1; }, "an unknown field");
  refused([](Json& p) { p["rebalance"]["enabled"] = 1; }, "a non-boolean switch");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "rebalance schema passed",
      {{"rebalance_round_trips", rebalance_round_trips},
       {"malformed_rebalance_is_refused", malformed_rebalance_is_refused}});
}
