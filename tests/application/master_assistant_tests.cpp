// The master assistant on a project (ADR-0124): it measures the mix as it
// enters the master chain; proposing changes nothing; accepting is one undo
// step, all of it or part of it, editing the master's own devices before
// adding any; a changed mix is refused; the rendered master meets the goals
// the proposal predicted; nudges render again and land in the rebalance;
// the A/B is level-matched, gapless and allocation-free.
//   oscillate_application_master_assistant_tests [case...]
#include "oscillate/application/master_assistant.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace app = oscillate::application;
namespace assistant = oscillate::application::master_assistant;
namespace core = oscillate::native_dsp::assistant;
namespace project = oscillate::project;
namespace fs = std::filesystem;
using oscillate::test::take;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double rate_hz = oscillate::test::sample_rate_48k_hz;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double seconds = 6.0;
constexpr std::int16_t full_scale = 32'767;
constexpr std::uint64_t seed = 0x6d61'7374'6572'0124ULL;
constexpr std::string_view target_id = "all";
// The rendered master against the plan's prediction: the song also
// carries the chain's tail past the programme.
constexpr double prediction_tolerance_lu = 0.25;
constexpr double level_tolerance_db = 0.1;
// The largest step between frames while the A/B switches, against the
// signal's own largest.
constexpr double switch_jump_ratio = 1.5;

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

using Signal = std::function<std::pair<double, double>(double t)>;

fs::path write_wav(const fs::path& path, const Signal& signal) {
  constexpr std::uint32_t header_bytes = 36;
  constexpr std::uint32_t format_bytes = 16;
  constexpr std::uint16_t pcm = 1;
  constexpr std::uint16_t stereo = 2;
  constexpr std::uint16_t frame_bytes = 4;
  constexpr std::uint16_t bits = 16;
  const auto frames = static_cast<std::uint32_t>(seconds * rate);
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, header_bytes + frames * frame_bytes);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, format_bytes);
  put16(bytes, pcm);
  put16(bytes, stereo);
  put32(bytes, rate);
  put32(bytes, rate * frame_bytes);
  put16(bytes, frame_bytes);
  put16(bytes, bits);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * frame_bytes);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto [l, r] = signal(static_cast<double>(i) / rate_hz);
    for (const double x : {l, r})
      put16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                       std::lround(std::clamp(x, -1.0, 1.0) * full_scale))));
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  REQUIRE_TRUE(output.good(), "WAV fixture write failed");
  return path;
}

// Kicks on the beat over a noisy pad: a mix that wants limiting.
Signal drums_and_pad() {
  constexpr double beat = 0.5;
  constexpr double kick_hz = 55.0;
  constexpr double decay = 0.12;
  constexpr double kick_level = 0.5;
  constexpr double pad_level = 0.05;
  auto random = std::make_shared<oscillate::foundation::SplitMix64>(seed);
  return [=](double t) {
    const double since = std::fmod(t, beat);
    const double kick = kick_level * std::exp(-since / decay) * std::sin(two_pi * kick_hz * since);
    const double l = pad_level * (2.0 * random->unit() - 1.0);
    const double r = pad_level * (2.0 * random->unit() - 1.0);
    return std::pair{kick + l, kick + r};
  };
}

// A loud bass filling the lows: partials across the region under 150 Hz.
Signal bass_line() {
  constexpr std::array partials_hz{42.0, 55.0, 70.0, 88.0, 110.0, 135.0};
  constexpr double level = 0.12;
  return [=](double t) {
    double x = 0.0;
    for (const double hz : partials_hz)
      x += level * std::sin(two_pi * hz * t);
    return std::pair{x, x};
  };
}

// A quiet airy pad.
Signal quiet_pad() {
  constexpr double level = 0.02;
  auto random = std::make_shared<oscillate::foundation::SplitMix64>(seed + 1);
  return [=](double) {
    return std::pair{level * (2.0 * random->unit() - 1.0), level * (2.0 * random->unit() - 1.0)};
  };
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-master-assistant") {}
};

struct Song {
  Directory directory;
  fs::path root = directory.path() / "song";
  std::unique_ptr<app::ProjectSession> session = take(app::ProjectSession::create(root));

  project::TrackId import(std::string_view name, const Signal& signal) {
    const auto wav = write_wav(directory.path() / (std::string(name) + ".wav"), signal);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    const auto id = take(std::move(track));
    REQUIRE_OK(session->tracks().set_track_name(id, std::string(name)));
    return id;
  }

  assistant::Measured measure() {
    REQUIRE_OK(session->saving().save());
    return take(assistant::measure(take(session->exports().pin_snapshot()), std::nullopt, {}));
  }
};

const core::Reference& reference() {
  static const auto r =
      core::reference_from(*oscillate::native_dsp::tonal::find_factory_target(target_id));
  return r;
}

const core::Goals club{core::loudness_presets[1].lufs, core::loudness_presets[1].ceiling_dbtp,
                       core::Character::punchy, 0.0};

std::vector<std::string> master_engines(const project::Snapshot& snapshot) {
  std::vector<std::string> out;
  for (const auto& insert : snapshot.master_effects) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor);
    out.push_back(patch != nullptr ? patch->engine : std::string());
  }
  return out;
}

void add_master_trim(Song& song, double gain_db) {
  const app::EffectEdit trim{
      project::MasterSignalPath{}, std::nullopt, std::nullopt, "trim", {{"trim.gain", gain_db}}};
  (void)take(song.session->effects().apply_effect_edits(std::span(&trim, 1)));
}

// ---- Cases ---------------------------------------------------------------------------

// The measurement is of the mix entering the master chain: a trim on the
// master does not move it.
void measure_hears_the_mix_before_the_chain() {
  Song song;
  (void)song.import("Drums", drums_and_pad());
  const auto plain = song.measure();
  constexpr double trim_db = -6.0;
  add_master_trim(song, trim_db);
  const auto trimmed = song.measure();
  REQUIRE_NEAR(trimmed.mix.lufs(), plain.mix.lufs(), level_tolerance_db, "the chain is left out");
  REQUIRE_EQ(trimmed.programme->frames(), plain.programme->frames(), "the same programme");
}

// Proposing changes nothing; accepting is one undo step that edits the
// master's own Trim and adds the rest in chain order; one undo takes it all
// back.
void accept_is_one_step_and_proposing_changes_nothing() {
  Song song;
  (void)song.import("Drums", drums_and_pad());
  add_master_trim(song, 0.0);
  const auto measured = song.measure();
  const auto revision = song.session->snapshot().revision;
  const auto proposed = take(assistant::propose(measured, reference(), club, {}));
  REQUIRE_EQ(song.session->snapshot().revision, revision, "proposing changes nothing");
  const auto steps = song.session->history().undo_steps().size();
  const auto before = song.session->snapshot().master_effects;
  REQUIRE_OK(assistant::accept(*song.session, proposed.proposal, {}, *measured.pinned->snapshot));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "one undo step");
  const auto engines = master_engines(song.session->snapshot());
  std::vector<std::string> proposed_engines;
  for (const auto& d : proposed.proposal.devices)
    proposed_engines.push_back(d.engine);
  if (std::ranges::find(proposed_engines, "trim") == proposed_engines.end())
    proposed_engines.insert(proposed_engines.begin(), "trim");
  REQUIRE_TRUE(engines == proposed_engines, "the chain in order, the Trim kept in place");
  REQUIRE_OK(song.session->history().undo());
  REQUIRE_TRUE(song.session->snapshot().master_effects == before, "one undo takes it back");
}

// Accepting part of it: only the chosen devices, and an existing Ceiling
// is edited rather than doubled.
void accept_part_of_it() {
  Song song;
  (void)song.import("Drums", drums_and_pad());
  const app::EffectEdit ceiling{
      project::MasterSignalPath{}, std::nullopt, std::nullopt, "ceiling", {}};
  (void)take(song.session->effects().apply_effect_edits(std::span(&ceiling, 1)));
  const auto measured = song.measure();
  const auto proposed = take(assistant::propose(measured, reference(), club, {}));
  assistant::Selection only_limiter;
  for (const auto& d : proposed.proposal.devices)
    only_limiter.devices.push_back(d.engine == "ceiling");
  REQUIRE_OK(assistant::accept(*song.session, proposed.proposal, only_limiter,
                               *measured.pinned->snapshot));
  const auto engines = master_engines(song.session->snapshot());
  REQUIRE_EQ(engines.size(), 1, "one device");
  REQUIRE_EQ(engines.front(), "ceiling", "the Ceiling, edited");
}

// After the proposal is accepted the master renders on target under the
// ceiling, as predicted; a mix changed since the measurement is refused.
void the_master_meets_what_was_predicted() {
  Song song;
  const auto track = song.import("Drums", drums_and_pad());
  const auto measured = song.measure();
  const auto proposed = take(assistant::propose(measured, reference(), club, {}));
  REQUIRE_TRUE(core::meets(proposed.predicted, club), "the prediction meets the goals");

  constexpr double louder = 1.5;
  REQUIRE_OK(song.session->tracks().set_track_level_pan(track, louder, 0.0));
  const auto refused =
      assistant::accept(*song.session, proposed.proposal, {}, *measured.pinned->snapshot);
  REQUIRE_FALSE(refused.has_value(), "a changed mix is refused");
  REQUIRE_OK(song.session->history().undo());

  REQUIRE_OK(assistant::accept(*song.session, proposed.proposal, {}, *measured.pinned->snapshot));
  REQUIRE_OK(song.session->saving().save());
  const auto after =
      take(assistant::verify(take(song.session->exports().pin_snapshot()), std::nullopt, {}));
  const auto outcome = core::outcome(after, reference());
  REQUIRE_TRUE(core::meets(outcome, club),
               "the master meets the goals: " + std::to_string(outcome.lufs) + " LUFS, " +
                   std::to_string(outcome.true_peak_dbtp) + " dBTP");
  REQUIRE_NEAR(outcome.lufs, proposed.predicted.lufs, prediction_tolerance_lu, "as predicted");
}

// A bass that makes nearly all of the lows, far over the reference there:
// its role is nudged down, the mix rendered again with it, and accepting
// writes it into the rebalance in the same step.
void nudges_land_in_the_rebalance() {
  Song song;
  const auto bass = song.import("Bass", bass_line());
  (void)song.import("Pad", quiet_pad());
  const auto measured = song.measure();
  const auto bass_role = static_cast<std::size_t>(project::MixRole::bass);
  REQUIRE_TRUE(measured.roles[bass_role].has_value(), "the bass is heard as bass");
  const auto proposed = take(assistant::propose(measured, reference(), club, {}));
  REQUIRE_FALSE(proposed.proposal.nudges.empty(), "a nudge");
  REQUIRE_EQ(proposed.proposal.nudges.front().role, bass_role, "on the bass");
  const auto steps = song.session->history().undo_steps().size();
  REQUIRE_OK(assistant::accept(*song.session, proposed.proposal, {}, *measured.pinned->snapshot));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "one undo step");
  const auto& rebalance = song.session->snapshot().rebalance;
  REQUIRE_TRUE(rebalance.enabled, "the rebalance is on");
  REQUIRE_LT(rebalance.roles[bass_role].gain_db, 0.0, "the bass comes down");
  (void)bass;
}

// The A/B: the louder side is turned down to the quieter; switching to a
// side of opposite polarity, which a hard switch would jump across,
// crossfades with no step steeper than the tone's own; the callback
// allocates nothing.
void audition_is_level_matched_and_gapless() {
  constexpr double tone_hz = 440.0;
  constexpr double quiet = 0.1;
  constexpr double louder_db = 6.0;
  constexpr std::size_t block = oscillate::test::block_64;
  const auto frames = static_cast<std::size_t>(seconds * rate_hz);
  core::Capture a;
  core::Capture b;
  const double loud = quiet * oscillate::native_dsp::dsp::db_to_gain(louder_db);
  for (std::size_t i = 0; i < frames; ++i) {
    const double s = std::sin(two_pi * tone_hz * static_cast<double>(i) / rate_hz);
    a.left.push_back(static_cast<float>(quiet * s));
    a.right.push_back(static_cast<float>(quiet * s));
    b.left.push_back(static_cast<float>(-loud * s));
    b.right.push_back(static_cast<float>(-loud * s));
  }
  const double a_lufs = core::analyse(a.left, a.right, rate).lufs();
  const double b_lufs = core::analyse(b.left, b.right, rate).lufs();
  assistant::Audition audition(a, core::matched_gain_db(a_lufs, b_lufs), b,
                               core::matched_gain_db(b_lufs, a_lufs), rate);
  std::vector<oscillate::engine::StereoFrame> out(block);
  const auto rms_of = [&](std::size_t blocks) {
    double sum = 0.0;
    for (std::size_t k = 0; k < blocks; ++k) {
      audition.render(out);
      for (const auto& f : out)
        sum += static_cast<double>(f.left) * f.left;
    }
    return std::sqrt(sum / static_cast<double>(blocks * block));
  };
  constexpr std::size_t measure_blocks = 200;
  const double rms_a = rms_of(measure_blocks);
  audition.choose_proposal(true);
  const auto fade_blocks = static_cast<std::size_t>(
      std::ceil(assistant::Audition::crossfade_seconds * rate_hz / static_cast<double>(block)));
  // Through the crossfade, counting allocations and the largest step.
  float previous = out.back().left;
  double jump = 0.0;
  oscillate::test::counted_allocations = 0;
  oscillate::test::counting_allocations = true;
  for (std::size_t k = 0; k <= fade_blocks; ++k) {
    audition.render(out);
    for (const auto& f : out) {
      jump = std::max(jump, static_cast<double>(std::fabs(f.left - previous)));
      previous = f.left;
    }
  }
  oscillate::test::counting_allocations = false;
  REQUIRE_EQ(oscillate::test::counted_allocations, 0, "the callback allocates nothing");
  const double rms_b = rms_of(measure_blocks);
  REQUIRE_NEAR(20.0 * std::log10(rms_b / rms_a), 0.0, level_tolerance_db, "level-matched");
  const double steepest = quiet * two_pi * tone_hz / rate_hz;
  REQUIRE_LT(jump, switch_jump_ratio * steepest, "the switch crossfades");
  REQUIRE_TRUE(audition.proposal_chosen(), "the proposal is heard");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "master assistant session contract passed",
      {{"measure_hears_the_mix_before_the_chain", measure_hears_the_mix_before_the_chain},
       {"accept_is_one_step_and_proposing_changes_nothing",
        accept_is_one_step_and_proposing_changes_nothing},
       {"accept_part_of_it", accept_part_of_it},
       {"the_master_meets_what_was_predicted", the_master_meets_what_was_predicted},
       {"nudges_land_in_the_rebalance", nudges_land_in_the_rebalance},
       {"audition_is_level_matched_and_gapless", audition_is_level_matched_and_gapless}});
}
