// Clay in the project (ADR-0081 phase 2): a take saves and loads, every edit
// is one undo step, bad takes are refused, an unedited take renders the
// source exactly, an edited one changes it and export plays the same render,
// and To MIDI writes the notes with their bends.
//   oscillate_application_clay_session_tests [case...]
#include "oscillate/application/clay_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/clay_dsp/limits.hpp"
#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/clay_source.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
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
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace clay = oscillate::application::clay;
namespace dsp = oscillate::clay_dsp;
namespace foundation = oscillate::foundation;
namespace media = oscillate::media;
namespace persistence = oscillate::persistence;
namespace project = oscillate::project;
namespace source_clay = oscillate::processing::clay;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::int16_t full_scale = 32'767;
// A sung line: three notes with vibrato and gaps between them.
constexpr std::array<double, 3> sung_hz{220.0, 261.63, 329.63};
constexpr double note_seconds = 0.4;
constexpr double gap_seconds = 0.15;
constexpr double vibrato_hz = 5.5;
constexpr double vibrato_cents = 20.0;
constexpr int harmonics = 8;
constexpr double shift_cents = 300.0;
constexpr std::size_t read_chunk = 4'096;

using oscillate::test::require;

using oscillate::test::take;

void accepted(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-clay") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// A 16-bit stereo WAV of the sung line.
fs::path write_sung(const fs::path& path) {
  const double length =
      static_cast<double>(sung_hz.size()) * (note_seconds + gap_seconds) + gap_seconds;
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
  double phase = 0.0;
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double slot = t - gap_seconds;
    const auto index = static_cast<std::size_t>(std::max(0.0, slot) / (note_seconds + gap_seconds));
    const double within = slot - static_cast<double>(index) * (note_seconds + gap_seconds);
    double x = 0.0;
    if (slot >= 0.0 && index < sung_hz.size() && within < note_seconds) {
      const double cents = vibrato_cents * std::sin(two_pi * vibrato_hz * t);
      const double hz = sung_hz[index] * std::exp2(cents / 1'200.0);
      phase += two_pi * hz / rate;
      // A soft attack and release keep the edges clean.
      const double edge = std::min({1.0, within / 0.02, (note_seconds - within) / 0.02});
      for (int h = 1; h <= harmonics; ++h)
        x += std::sin(phase * h) / h;
      x *= 0.25 * edge;
    }
    for (int channel = 0; channel < 2; ++channel)
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
  project::TrackId track = import();
  project::TrackId import() {
    const auto wav = write_sung(directory.path() / "vocal.wav");
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture open failed");
    auto imported = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    return take(std::move(imported));
  }
  [[nodiscard]] const project::Clip& clip() const {
    const auto& tracks = session->snapshot().tracks;
    return std::ranges::find(tracks, track, &project::Track::id)->clips.front();
  }
  [[nodiscard]] const project::SourceRecord& source() const {
    const auto& sources = session->snapshot().sources;
    return *std::ranges::find(sources, clip().source, &project::SourceRecord::id);
  }
  [[nodiscard]] project::ClayTake fresh() const {
    return clay::new_take(session->snapshot(), clip(), project::ClayMode::melodic);
  }
  // The analysis the take's ids refer to.
  [[nodiscard]] dsp::Take analysed() {
    accepted(session->saving().save());
    auto pinned = take(session->exports().pin_snapshot());
    auto audio = take(source_clay::decode_source(pinned.reader, source()));
    return take(source_clay::analysed_take(nullptr, source(), project::ClayMode::melodic, audio));
  }
};

// All of a render, read back as planar floats.
std::pair<std::vector<float>, std::vector<float>> read_all(int fd,
                                                           const project::SourceRecord& record) {
  auto reader = take(media::AudioReader::open_fd(fd, record));
  std::vector<float> left;
  std::vector<float> right;
  std::vector<media::StereoFrame> chunk(read_chunk);
  std::uint64_t at = 0;
  while (at < reader.frame_count()) {
    const auto got = take(reader.read_at(at, chunk));
    REQUIRE_GT(got, 0, "the render reads to its end");
    for (std::size_t i = 0; i < got; ++i) {
      left.push_back(chunk[i].left);
      right.push_back(chunk[i].right);
    }
    at += got;
  }
  return {std::move(left), std::move(right)};
}

std::pair<std::vector<float>, std::vector<float>> rendered(Song& song) {
  accepted(song.session->saving().save());
  auto pinned = take(song.session->exports().pin_snapshot());
  const auto& clip = song.clip();
  auto audio = take(source_clay::open_rendered(pinned.reader, song.source(), *clip.clay));
  return read_all(audio.fd(), audio.record());
}

// A take saves with the project and loads unchanged.
void a_take_saves_and_loads() {
  Song song;
  auto made = song.fresh();
  made.key = project::MusicalKey{9, project::KeyMode::minor};
  made.ops.push_back({project::ClayOpKind::split, 1, 0, rate / 2});
  made.edits.push_back({1, 0, 120.0, 0.5, 0.25, -150.0, -3.0, false, rate / 4, rate / 2});
  made.edits.push_back({1, 1, 400.0, 1.0, 1.0, 200.0, -6.0, true, std::nullopt, std::nullopt});
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, made));
  accepted(song.session->saving().save());
  const auto clip = song.clip().id;
  song.session.reset();
  auto reopened = take(app::ProjectSession::open(song.root));
  const auto& tracks = reopened->snapshot().tracks;
  const auto& loaded = std::ranges::find(tracks, song.track, &project::Track::id)->clips.front();
  REQUIRE_EQ(loaded.id, clip, "the take loads as it was saved");
  REQUIRE_EQ(loaded.clay, made, "the take loads as it was saved");
}

// Setting a take is one undo step, and undo plays the source again.
void every_edit_is_one_undo_step() {
  Song song;
  const auto steps = song.session->history().undo_steps().size();
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, song.fresh()));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "detecting is one step");
  auto edited = *song.clip().clay;
  edited.edits.push_back({1, 0, shift_cents});
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, edited));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 2, "an edit is one step");
  accepted(song.session->history().undo());
  REQUIRE_TRUE(song.clip().clay, "undo takes the edit back");
  REQUIRE_TRUE(song.clip().clay->edits.empty(), "undo takes the edit back");
  accepted(song.session->history().undo());
  require(!song.clip().clay, "and the take");
}

// Out-of-range or malformed takes never reach the project.
void bad_takes_are_refused() {
  Song song;
  const auto refused = [&](project::ClayTake bad, std::string_view what) {
    require(!song.session->audio_clips().set_clip_clay(song.clip().id, std::move(bad)), what);
  };
  auto stale = song.fresh();
  stale.analysis_version = dsp::analysis_version + 1;
  refused(stale, "another analysis version is refused");
  auto twice = song.fresh();
  twice.edits.push_back({1, 0, 100.0});
  twice.edits.push_back({1, 0, 200.0});
  refused(twice, "two edits of one voice are refused");
  auto backwards = song.fresh();
  backwards.edits.push_back({1, 0, 0.0, 1.0, 1.0, 0.0, 0.0, false, 500, 400});
  refused(backwards, "a placement ending before it starts is refused");
  auto loud = song.fresh();
  loud.edits.push_back({1, 0, 0.0, 1.0, 1.0, 0.0, dsp::max_edit_gain_db + 1.0});
  refused(loud, "a gain over the limit is refused");
  auto voices = song.fresh();
  voices.edits.push_back({1, project::max_clay_voices + 1, 0.0});
  refused(voices, "too many voices are refused");
  auto join = song.fresh();
  join.ops.push_back({project::ClayOpKind::join, 2, 2, 0});
  refused(join, "a note joined to itself is refused");
  require(!song.clip().clay, "nothing was saved");
}

// An unedited take renders the decoded source sample for sample.
void unedited_render_is_the_source() {
  Song song;
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, song.fresh()));
  const auto [left, right] = rendered(song);
  accepted(song.session->saving().save());
  auto pinned = take(song.session->exports().pin_snapshot());
  const auto audio = take(source_clay::decode_source(pinned.reader, song.source()));
  REQUIRE_EQ(left, audio.left, "the render is the source exactly");
  REQUIRE_EQ(right, audio.right, "the render is the source exactly");
}

// A shifted note changes the render; the export of the clip's track is that
// render, so export plays what playback plays.
void edited_render_reaches_export() {
  Song song;
  const auto analysed = song.analysed();
  REQUIRE_EQ(analysed.notes.size(), sung_hz.size(),
             "the sung line has three notes: " + std::to_string(analysed.notes.size()));
  auto edited = song.fresh();
  edited.edits.push_back({analysed.notes[1].id, 0, shift_cents});
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, edited));
  const auto [left, right] = rendered(song);
  accepted(song.session->saving().save());
  auto pinned = take(song.session->exports().pin_snapshot());
  const auto audio = take(source_clay::decode_source(pinned.reader, song.source()));
  REQUIRE_NE(left, audio.left, "the edit changes the render");
  const auto target = song.directory.path() / "track.wav";
  auto export_store = take(persistence::ProjectStore::open_reader(song.root));
  const auto render = take(oscillate::exporting::render_track(
      *pinned.snapshot, export_store,
      {target, song.track, std::pair<std::uint64_t, std::uint64_t>{0, left.size()}}));
  REQUIRE_EQ(render.first_frame, 0, "the render starts with the clip");
  const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE_GE(fd, 0, "the export opens");
  const project::SourceRecord exported{
      song.source().id, project::SourceEncoding::wav, std::string(64, '0'), 0, rate, 2,
      left.size()};
  auto [export_left, export_right] = read_all(fd, exported);
  (void)::close(fd);
  double worst = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i)
    worst = std::max({worst, std::abs(static_cast<double>(export_left[i] - left[i])),
                      std::abs(static_cast<double>(export_right[i] - right[i]))});
  REQUIRE_LT(worst, 1e-6, "export plays the Clay render: worst " + std::to_string(worst));
}

// To MIDI: the notes on a new Nova track, their bends as clip automation of
// its pitch bend, in one undo step.
void to_midi_writes_notes_and_bends() {
  Song song;
  const auto analysed = song.analysed();
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, song.fresh()));
  const auto time = take(clay::ClipTime::of(song.session->snapshot(), song.clip()));
  const auto midi = dsp::to_midi(analysed, {});
  REQUIRE_EQ(midi.notes.size(), sung_hz.size(), "three MIDI notes");
  const auto out = take(clay::midi_clip_out(time, midi));
  const auto steps = song.session->history().undo_steps().size();
  const auto tracks = song.session->snapshot().tracks.size();
  const auto made = take(song.session->audio_clips().add_clay_midi_clip(std::nullopt, out));
  REQUIRE_EQ(song.session->history().undo_steps().size(), steps + 1, "To MIDI is one step");
  const auto& snapshot = song.session->snapshot();
  REQUIRE_EQ(snapshot.tracks.size(), tracks + 1, "a Nova track was added");
  const auto& synth = snapshot.tracks.back();
  REQUIRE_EQ(synth.midi_clips.size(), 1, "with the clip");
  REQUIRE_EQ(synth.midi_clips.front().id, made, "with the clip");
  const auto& clip = synth.midi_clips.front();
  REQUIRE_EQ(clip.notes.size(), sung_hz.size(), "holding the notes");
  // A3 C4 E4 = 57 60 64.
  require(clip.notes[0].key == 57 && clip.notes[1].key == 60 && clip.notes[2].key == 64,
          "at their keys");
  REQUIRE_EQ(clip.automation.size(), 1, "and a bend lane");
  const auto& lane = clip.automation.front();
  const auto* target = std::get_if<project::NativeParameterTarget>(&lane.target);
  REQUIRE_NE(target, nullptr, "on the pitch bend");
  REQUIRE_EQ(target->parameter_id, "nova.perf.bend", "on the pitch bend");
  REQUIRE_TRUE(!lane.points.empty(), "inside its range");
  REQUIRE_TRUE(std::ranges::all_of(lane.points,
                                   [](const auto& p) { return p.value >= -1.0 && p.value <= 1.0; }),
               "inside its range");
  require(std::ranges::any_of(lane.points, [](const auto& p) { return std::abs(p.value) > 0.0; }),
          "following the vibrato");
  accepted(song.session->history().undo());
  REQUIRE_EQ(song.session->snapshot().tracks.size(), tracks, "undo removes it all");
}

// Harmonies are voices of their note, numbered in order; fit to key moves
// notes degree by degree.
void macros_map_to_voices() {
  Song song;
  const auto analysed = song.analysed();
  auto made = song.fresh();
  made.key = project::MusicalKey{9, project::KeyMode::minor};
  const auto scale = source_clay::dsp_scale(made.key);
  auto edits = dsp::harmonise(analysed, source_clay::dsp_edits(made, false), 2, scale);
  edits = dsp::harmonise(analysed, edits, -3, scale);
  const auto voiced = source_clay::with_dsp_edits(made, edits);
  for (const auto& note : analysed.notes) {
    const auto count = std::ranges::count_if(
        voiced.edits, [&](const auto& edit) { return edit.note == note.id && edit.voice != 0; });
    REQUIRE_EQ(count, 2, "each note has two harmonies");
    require(std::ranges::any_of(voiced.edits,
                                [&](const auto& e) { return e.note == note.id && e.voice == 2; }),
            "numbered 1 and 2");
  }
  REQUIRE_EQ(source_clay::dsp_edits(voiced, false).size(), edits.size(),
             "the voices map back to the same edits");
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, voiced));
  const auto fitted = source_clay::with_dsp_edits(
      voiced, dsp::fit_to_key(
                  analysed, source_clay::dsp_edits(voiced, false), scale,
                  source_clay::dsp_scale(project::MusicalKey{5, project::KeyMode::minor}), true));
  const auto moved = std::ranges::find_if(fitted.edits, [&](const auto& edit) {
    return edit.note == analysed.notes.front().id && edit.voice == 0;
  });
  REQUIRE_NE(moved, fitted.edits.end(), "A minor to F minor moves the root down a third");
  REQUIRE_LT(std::abs(moved->pitch_cents + 400.0), 10.0,
             "A minor to F minor moves the root down a third");
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, fitted));
}

// A clip that also needs warp (here a pitch shift) plays its Clay render
// through the warp: export renders it, and the edit is heard through it.
void warp_plays_the_render() {
  Song song;
  const auto analysed = song.analysed();
  project::ClipProcessing processing = song.clip().processing;
  processing.pitch_semitones = 2.0;
  processing.warp.mode = project::WarpMode::complex;
  accepted(song.session->audio_clips().set_audio_clip_processing(song.clip().id, processing));
  const auto frames = song.source().frames;
  const auto export_track = [&](std::string_view name) {
    accepted(song.session->saving().save());
    auto pinned = take(song.session->exports().pin_snapshot());
    const auto target = song.directory.path() / std::string(name);
    auto store = take(persistence::ProjectStore::open_reader(song.root));
    (void)take(oscillate::exporting::render_track(
        *pinned.snapshot, store,
        {target, song.track, std::pair<std::uint64_t, std::uint64_t>{0, frames}}));
    const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "the export opens");
    const project::SourceRecord exported{
        song.source().id, project::SourceEncoding::wav, std::string(64, '0'), 0, rate, 2, frames};
    auto audio = read_all(fd, exported);
    (void)::close(fd);
    return audio.first;
  };
  const auto warped = export_track("warped.wav");
  auto edited = song.fresh();
  edited.edits.push_back({analysed.notes[1].id, 0, shift_cents});
  accepted(song.session->audio_clips().set_clip_clay(song.clip().id, edited));
  const auto snapshot = song.session->snapshot();
  auto request = take(oscillate::processing::warp_render_request(snapshot, song.clip().id));
  REQUIRE_TRUE(request.clay, "the warp reads the Clay render");
  REQUIRE_NE(request.source.sha256, song.source().sha256, "the warp reads the Clay render");
  const auto both = export_track("both.wav");
  REQUIRE_EQ(both.size(), warped.size(), "the edit is heard through the warp");
  REQUIRE_NE(both, warped, "the edit is heard through the warp");
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string_view, void (*)()>> cases{
      {"saves", a_take_saves_and_loads},        {"undo", every_edit_is_one_undo_step},
      {"refused", bad_takes_are_refused},       {"unedited", unedited_render_is_the_source},
      {"export", edited_render_reaches_export}, {"to_midi", to_midi_writes_notes_and_bends},
      {"voices", macros_map_to_voices},         {"warp", warp_plays_the_render},
  };
  const std::vector<std::string_view> wanted(argv + 1, argv + argc);
  // A misspelt or renamed case fails rather than silently running nothing.
  for (const auto name : wanted) {
    if (std::ranges::find(cases, name, &std::pair<std::string_view, void (*)()>::first) ==
        cases.end()) {
      std::cerr << "FAILED: unknown case " << name << '\n';
      return 1;
    }
  }
  try {
    for (const auto& [name, run] : cases)
      if (wanted.empty() || std::ranges::find(wanted, name) != wanted.end())
        run();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "clay session contract passed\n";
  return 0;
}
