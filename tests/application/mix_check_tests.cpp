// Mix Check (ADR-0074) on small projects with planted problems: each is
// found on the right track, its fix is one undo step, and the re-check
// shows it gone. Ids are stable across checks; history diffs; targets.
//   oscillate_application_mix_check_tests [case...]
#include "oscillate/application/mix_check.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace check = oscillate::application::mix_check;
namespace foundation = oscillate::foundation;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double seconds = 8.0;
constexpr std::int64_t bar = 4 * timeline::ticks_per_quarter;
constexpr std::int16_t full_scale = 32'767;

using oscillate::test::require;

using oscillate::test::take;

void accepted(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-mix-check") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

using Signal = std::function<std::pair<double, double>(double t, std::size_t i)>;

// A 16-bit stereo WAV of `signal` (left, right) for `length` seconds.
fs::path write_wav(const fs::path& path, double length, const Signal& signal) {
  const auto frames = static_cast<std::uint32_t>(length * rate);
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + frames * 4);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 2);
  put32(bytes, rate);
  put32(bytes, rate * 4);
  put16(bytes, 4);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto [l, r] = signal(static_cast<double>(i) / rate, i);
    for (const double x : {l, r})
      put16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                       std::lround(std::clamp(x, -1.0, 1.0) * full_scale))));
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

struct Song {
  Directory directory;
  fs::path root = directory.path() / "song";
  std::unique_ptr<app::ProjectSession> session = take(app::ProjectSession::create(root));

  project::TrackId import(std::string_view name, double length, const Signal& signal) {
    const auto wav = write_wav(directory.path() / (std::string(name) + ".wav"), length, signal);
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    const auto id = take(std::move(track));
    accepted(session->tracks().set_track_name(id, std::string(name)));
    return id;
  }

  check::Measurements measure() {
    accepted(session->saving().save());
    return take(check::measure(take(session->exports().pin_snapshot()), {}));
  }
};

const check::Finding* find(const check::Report& report, std::string_view prefix) {
  const auto found = std::ranges::find_if(
      report.findings, [&](const check::Finding& f) { return f.id.starts_with(prefix); });
  return found == report.findings.end() ? nullptr : &*found;
}

// A 55 Hz bass with a 30 Hz rumble 6 dB under it.
Signal rumbling_bass() {
  return [](double t, std::size_t) {
    const double x = 0.5 * std::sin(two_pi * 55.0 * t) + 0.25 * std::sin(two_pi * 30.0 * t);
    return std::pair{x, x};
  };
}

// The rumble is found and blamed on the bass; Contour is one undo step and
// the re-check finds it gone, with the rest kept. Ids are stable.
void sub_rumble_is_fixed_in_one_step() {
  Song song;
  const auto bass = song.import("Bass", seconds, rumbling_bass());
  const auto before = song.measure();
  const auto report = check::report(before, "club");
  const auto* sub = find(report, "sub:");
  REQUIRE_NE(sub, nullptr, "the rumble is a finding");
  REQUIRE_EQ(sub->tracks.size(), 1, "blamed on the bass");
  REQUIRE_EQ(sub->tracks.front().id, bass, "blamed on the bass");
  REQUIRE_TRUE(sub->band, "at 30 Hz");
  REQUIRE_LE(sub->band->low, 30.0, "at 30 Hz");
  REQUIRE_GE(sub->band->high, 30.0, "at 30 Hz");
  REQUIRE_TRUE(sub->fix, "Contour fixes it");
  REQUIRE_EQ(sub->fix->device, "Contour", "Contour fixes it");
  const auto again = check::report(song.measure(), "club");
  REQUIRE_NE(find(again, "sub:"), nullptr, "the same problem keeps its id");
  REQUIRE_EQ(find(again, "sub:")->id, sub->id, "the same problem keeps its id");

  const auto steps = song.session->history().undo_steps().size();
  // Assigned rather than copy-constructed: GCC 15 -O2 misreads the variant
  // copy constructor's unwind path as reading an uninitialized Fix.
  check::Finding finding;
  finding = *sub;
  accepted(check::apply(*song.session, finding, *before.snapshot));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "the fix is one undo step");
  const auto& track =
      *std::ranges::find(song.session->snapshot().tracks, bass, &project::Track::id);
  const auto* contour = std::get_if<project::NativeEffectPatch>(&track.effects.front().processor);
  REQUIRE_NE(contour, nullptr, "Contour is first on the bass");
  REQUIRE_EQ(contour->engine, "contour", "Contour is first on the bass");

  const auto after = song.measure();
  const auto rechecked = check::report(after, "club");
  REQUIRE_EQ(find(rechecked, "sub:"), nullptr, "the re-check finds no rumble");
  check::judge(finding, report, rechecked, after);
  require(finding.applied && finding.result.find("gone") != std::string::npos,
          "the result says so: " + finding.result);
  REQUIRE_EQ(report.project_name, "song", "the report names the project: " + report.project_name);

  accepted(song.session->history().undo());
  require(std::ranges::find(song.session->snapshot().tracks, bass, &project::Track::id)
              ->effects.empty(),
          "one undo removes the fix");
}

// An audio clip starting mid-wave clicks; the fades fix removes it.
void click_is_faded() {
  Song song;
  const auto track = song.import("Chop", 4.0, [](double t, std::size_t) {
    const double x = 0.5 * std::cos(two_pi * 200.0 * t);
    return std::pair{x, x};
  });
  const auto clip = std::ranges::find(song.session->snapshot().tracks, track, &project::Track::id)
                        ->clips.front()
                        .id;
  accepted(song.session->audio_clips().move_audio_clip(clip, {bar}));
  const auto before = song.measure();
  const auto report = check::report(before, "club");
  const auto* click = find(report, "click:");
  REQUIRE_NE(click, nullptr, "the clip's start clicks");
  REQUIRE_EQ(click->tracks.front().id, track, "the clip's start clicks");
  REQUIRE_TRUE(click->fix, "fades fix it");
  REQUIRE_EQ(click->fix->device, "Fades", "fades fix it");
  const auto steps = song.session->history().undo_steps().size();
  accepted(check::apply(*song.session, *click, *before.snapshot));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1,
             "one undo step for every edge");
  const auto& faded =
      std::ranges::find(song.session->snapshot().tracks, track, &project::Track::id)->clips.front();
  REQUIRE_GT(faded.fades.in_fraction, 0.0, "the start fades in");
  REQUIRE_EQ(faded.fades.in_curve, project::FadeCurve::smoothstep, "the start fades in");
  REQUIRE_EQ(find(check::report(song.measure(), "club"), "click:"), nullptr,
             "no click on the re-check");
}

// A pad whose 600 Hz band is anticorrelated loses it in mono; a Vista
// narrows that band and the loss falls under the threshold.
void mono_loss_is_narrowed() {
  Song song;
  const auto pad = song.import("Pad", seconds, [](double t, std::size_t) {
    double mid = 0.0;
    for (const double hz : {300.0, 450.0, 600.0, 1'000.0})
      mid += 0.1 * std::sin(two_pi * hz * t);
    const double side = 0.12 * std::sin(two_pi * 600.0 * t + 0.3);
    return std::pair{mid + side, mid - side};
  });
  const auto before = song.measure();
  const auto report = check::report(before, "club");
  const auto* mono = find(report, "mono:");
  REQUIRE_NE(mono, nullptr, "the pad loses its mids in mono");
  REQUIRE_EQ(mono->tracks.front().id, pad, "the pad loses its mids in mono");
  REQUIRE_TRUE(mono->fix, "Vista fixes it");
  REQUIRE_EQ(mono->fix->device, "Vista", "Vista fixes it");
  accepted(check::apply(*song.session, *mono, *before.snapshot));
  const auto after = check::report(song.measure(), "club");
  REQUIRE_EQ(find(after, "mono:"), nullptr, "the re-check keeps the band in mono");
}

// A hat whose three modes outlast the rest of its sound gets three Temper
// cuts at their pitches; its partials that die with it get none.
constexpr std::array<double, 3> hat_modes{9'680.0, 10'320.0, 11'010.0};
void ringing_gets_temper_cuts() {
  Song song;
  std::uint32_t state = 0x9E3779B9U;
  song.import("Hat", seconds, [&](double, std::size_t i) {
    const auto period = static_cast<std::size_t>(0.5 * rate);
    const double local = static_cast<double>(i % period) / rate;
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    const double noise = static_cast<double>(state) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
    double x = 0.6 * noise * std::exp(-local / 0.004);
    for (const double hz : {4'100.0, 5'300.0, 6'700.0, 7'900.0, 12'400.0, 13'600.0})
      x += 0.1 * std::sin(two_pi * hz * local) * std::exp(-local / 0.015);
    for (const double hz : hat_modes)
      x += 0.04 * std::sin(two_pi * hz * local) * std::exp(-local / 0.15);
    return std::pair{x, x};
  });
  const auto report = check::report(song.measure(), "club");
  const auto* ringing = find(report, "ringing:");
  REQUIRE_NE(ringing, nullptr, "the hat rings and Temper fixes it");
  REQUIRE_TRUE(ringing->fix, "the hat rings and Temper fixes it");
  REQUIRE_EQ(ringing->fix->device, "Temper", "the hat rings and Temper fixes it");
  const auto& edits = std::get<check::EffectFix>(ringing->fix->action).edits;
  const auto cuts = std::ranges::count_if(edits.front().values, [](const auto& value) {
    return value.first.starts_with("temper.band_freq_");
  });
  REQUIRE_EQ(cuts, static_cast<std::ptrdiff_t>(hat_modes.size()),
             "one cut per mode, none for the partials: " + std::to_string(cuts));
  for (const double hz : hat_modes)
    require(std::ranges::any_of(edits.front().values,
                                [&](const auto& value) {
                                  return value.first.starts_with("temper.band_freq_") &&
                                         std::fabs(value.second - hz) / hz < 0.01;
                                }),
            "a cut at " + std::to_string(hz) + " Hz");
}

// Nova playing C2 with its sub an octave down: the sub's C1 is the bass's
// lowest fundamental, so it is never rumble, and any Contour stays under it.
void nova_sub_is_not_rumble() {
  constexpr std::uint8_t c2 = 36;
  constexpr double c1_hz = 32.703;
  constexpr double hz_tolerance = 0.05; // A share of the fundamental.
  Song song;
  const auto bass =
      take(song.session->instruments().add_native_instrument_track("nova", std::nullopt));
  accepted(song.session->tracks().set_track_name(bass, "Bass"));
  const auto* engine = oscillate::native_dsp::synth::find_engine("nova");
  REQUIRE_NE(engine, nullptr, "Nova is built in");
  auto values = take(song.session->instruments().native_instrument_values(bass));
  const auto set = [&](std::string_view id, double value) {
    const auto index = oscillate::native_dsp::synth::find_parameter(*engine->descriptor, id);
    REQUIRE_OK(index, "Nova has " + std::string(id));
    values[*index] = value;
  };
  set("nova.osc.a.type", 0.0);  // Classic.
  set("nova.osc.a.shape", 0.0); // Sine.
  set("nova.sub.on", 1.0);
  set("nova.sub.octave", 1.0); // -1 octave.
  set("nova.sub.level", 1.0);
  accepted(song.session->instruments().set_native_instrument_values(bass, values));
  const auto placeholder = project::NoteId::from_bits({0, 1}).value();
  (void)take(song.session->midi_clips().add_midi_clip_with_notes(
      bass, {0}, {4 * bar},
      {{placeholder, {0}, {4 * bar - timeline::ticks_per_quarter}, c2, 100}}));
  const auto measured = song.measure();
  const auto& track = measured.tracks.at(bass);
  require(track.bass, "a bass");
  REQUIRE_LT(std::fabs(track.lowest_fundamental_hz - c1_hz), c1_hz * hz_tolerance,
             "its lowest fundamental is the sub's C1: " +
                 std::to_string(track.lowest_fundamental_hz));
  const auto report = check::report(measured, "club");
  const auto* sub = find(report, "sub:");
  if (sub == nullptr)
    return;
  REQUIRE_TRUE(sub->band, "no rumble at the sub's own fundamental");
  REQUIRE_LT(sub->band->high, c1_hz, "no rumble at the sub's own fundamental");
  if (!sub->fix)
    return;
  for (const auto& edit : std::get<check::EffectFix>(sub->fix->action).edits)
    for (const auto& [id, value] : edit.values)
      if (id == "contour.cutoff")
        REQUIRE_LT(value, c1_hz, "the Contour stays under the sub: " + std::to_string(value));
}

// True peak over the target with a limiter on the master: the fix lowers
// its ceiling by at least the overshoot and the margin, never raises it.
void true_peak_lowers_the_ceiling() {
  constexpr double limit_db = -1.0;
  constexpr double fader = 1.25; // About +1.9 dB after the limiter.
  constexpr double rounding_db = 0.01;
  const double margin = check::ceiling_true_peak_margin_db;
  // Funky's case: a -1.0 dB ceiling and -0.2 dBTP against -0.3: the peaks
  // run 0.8 dB over the ceiling, so -0.3 - 0.8 - margin (-1.6 dB).
  constexpr double club_db = -0.3;
  constexpr double funky_peak_db = -0.2;
  REQUIRE_LT(std::fabs(check::lowered_ceiling_db(limit_db, club_db, funky_peak_db) -
                       (club_db - (funky_peak_db - limit_db) - margin)),
             rounding_db, "a ceiling the peaks run over is lowered by that and the margin");
  // A ceiling above a stricter target: down by at least the overshoot and
  // the margin.
  constexpr double loose_db = -0.1;
  constexpr double strict_db = -1.0;
  constexpr double loose_peak_db = -0.05;
  REQUIRE_LE(check::lowered_ceiling_db(loose_db, strict_db, loose_peak_db),
             loose_db - (loose_peak_db - strict_db) - margin + rounding_db,
             "by at least the overshoot and the margin");
  REQUIRE_EQ(check::lowered_ceiling_db(std::nullopt, club_db, funky_peak_db), club_db - margin,
             "a new limiter sits the margin under the target");

  Song song;
  song.import("Loud", seconds, [](double t, std::size_t) {
    const double x = 0.95 * std::sin(two_pi * 100.0 * t);
    return std::pair{x, x};
  });
  (void)take(song.session->effects().apply_effect_edits(
      std::vector<app::EffectEdit>{{project::MasterSignalPath{},
                                    std::nullopt,
                                    std::nullopt,
                                    "ceiling",
                                    {{"ceiling.ceiling", limit_db}}}}));
  accepted(song.session->tracks().set_master_level(fader));
  const auto measured = song.measure();
  const auto& goal = check::target("club");
  const auto report = check::report(measured, goal.id);
  const auto* peak = find(report, "loudness:true_peak");
  REQUIRE_NE(peak, nullptr, "the true peak is over the target");
  REQUIRE_TRUE(peak->fix, "the true peak is over the target");
  REQUIRE_EQ(peak->fix->device, "Ceiling", "the true peak is over the target");
  REQUIRE_NE(peak->text.find("master fader"), std::string::npos,
             "the gain after the limiter is named: " + peak->text);
  const auto& edit = std::get<check::EffectFix>(peak->fix->action).edits.front();
  REQUIRE_OK(edit.slot, "the existing limiter is edited");
  const double lowered = edit.values.front().second;
  const double overshoot = measured.loudness.true_peak_db - goal.true_peak;
  REQUIRE_LT(lowered, limit_db, "the ceiling goes down");
  REQUIRE_LE(lowered, limit_db - overshoot - check::ceiling_true_peak_margin_db + rounding_db,
             "by at least the overshoot and the margin: " + std::to_string(lowered));
}

// Targets, history round trip and the since-last diff.
void targets_and_history() {
  REQUIRE_EQ(check::target("stream").lufs_low, -15.0, "the targets table");
  REQUIRE_EQ(check::target("dj").true_peak, -2.0, "the targets table");
  REQUIRE_EQ(check::target("nonsense").id, check::default_target, "unknown targets fall back");

  check::History previous{1'000,
                          {-8.0, 0.4, 6.0, 9.0},
                          {{"ringing:a", "The hat rings", 11.0, "Strongest peak"},
                           {"mono:b", "The mids disappear", 3.6, "Mono loss"},
                           {"sub:c", "Rumble", 5.0, "Sub excess"}}};
  check::History current{2'000,
                         {-8.0, 1.0, 6.0, 9.0},
                         {{"mono:b", "The mids disappear", 2.1, "Mono loss"},
                          {"sub:c", "Rumble", 5.0, "Sub excess"},
                          {"click:d", "A click", 1.0, "Clicks"}}};
  const auto changes = check::since_last(previous, current, "club");
  const auto has = [&](std::string_view direction, std::string_view words) {
    return std::ranges::any_of(changes, [&](const check::Change& c) {
      return c.direction == direction && c.text.find(words) != std::string::npos;
    });
  };
  require(has("fixed", "The hat rings"), "a gone finding is fixed");
  require(has("better", "3.6"), "a smaller loss is better");
  require(has("new", "A click"), "a new finding is new");
  require(has("worse", "True peak"), "a higher true peak is worse");
  require(has("same", "within"), "the rest is the same");

  const auto round = check::from_json(check::to_json(current));
  REQUIRE_TRUE(round, "history survives JSON");
  REQUIRE_EQ(round->checked_at, 2'000, "history survives JSON");
  REQUIRE_EQ(round->findings.size(), 3, "history survives JSON");
  REQUIRE_EQ(round->findings[1].metric, 5.0, "history survives JSON");
  require(!check::from_json("{not json"), "garbage is no history");

  Song song;
  accepted(song.session->saving().save());
  const auto store = take(persistence::ProjectStore::open_reader(song.root));
  require(!check::load_history(store), "a new project has no history");
  accepted(check::save_history(store, current));
  const auto loaded = check::load_history(store);
  REQUIRE_TRUE(loaded, "the history is kept in the cache directory");
  REQUIRE_EQ(loaded->findings.size(), 3, "the history is kept in the cache directory");
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string_view, void (*)()>> cases{
      {"sub", sub_rumble_is_fixed_in_one_step}, {"click", click_is_faded},
      {"mono", mono_loss_is_narrowed},          {"ringing", ringing_gets_temper_cuts},
      {"nova_sub", nova_sub_is_not_rumble},     {"true_peak", true_peak_lowers_the_ceiling},
      {"history", targets_and_history},
  };
  const std::vector<std::string_view> wanted(argv + 1, argv + argc);
  try {
    for (const auto& [name, run] : cases)
      if (wanted.empty() || std::ranges::find(wanted, name) != wanted.end())
        run();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mix check application contract passed\n";
  return 0;
}
