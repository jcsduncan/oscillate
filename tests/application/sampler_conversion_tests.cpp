// Convert to sampler (ADR-0063): an audio clip becomes Crate on a new
// instrument track below it, chopped at the clip's hits, with a MIDI clip
// that plays the chops where the audio plays them, as one undo step.
#include "oscillate/application/crate_chops.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/temp_dir.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {
namespace app = oscillate::application;
namespace chops = oscillate::application::crate;
namespace facts = oscillate::native_dsp::synth::crate;
namespace foundation = oscillate::foundation;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;
namespace fs = std::filesystem;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::uint32_t source_frames = rate * 2;
constexpr std::int16_t full_scale = 32'767;
constexpr double click_amplitude = 0.8;
// Spec 09 §6: transient starts land within 2 ms of the click.
constexpr std::uint64_t transient_tolerance = rate * 2 / 1'000;
// Clicks at 0.25, 0.75, 1.25 and 1.6 s.
const std::vector<std::uint64_t> clicks{rate / 4, rate * 3 / 4, rate * 5 / 4, rate * 8 / 5};

using oscillate::test::require;

using oscillate::test::take;

class Directory : public oscillate::test::TempDir {
public:
  Directory() : TempDir("oscillate-sampler") {}
};

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// Two seconds of mono 16-bit silence with a 5 ms decaying noise click at
// each of `clicks`.
fs::path make_wav(const fs::path& directory) {
  std::vector<double> samples(source_frames, 0.0);
  std::uint32_t state = 0x9E3779B9U; // xorshift32
  const auto click_frames = rate / 200;
  for (const auto at : clicks) {
    for (std::uint32_t index = 0; index < click_frames; ++index) {
      state ^= state << 13U;
      state ^= state >> 17U;
      state ^= state << 5U;
      const double noise = static_cast<double>(state) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
      const double decay_frames = static_cast<double>(rate) / 1'000; // 1 ms
      samples[at + index] =
          click_amplitude * noise * std::exp(-static_cast<double>(index) / decay_frames);
    }
  }
  const auto path = directory / "hits.wav";
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, 36 + source_frames * 2);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, 16);
  put16(bytes, 1);
  put16(bytes, 1);
  put32(bytes, rate);
  put32(bytes, rate * 2);
  put16(bytes, 2);
  put16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, source_frames * 2);
  for (const double sample : samples)
    put16(bytes,
          static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(sample * full_scale))));
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "WAV fixture write failed");
  return path;
}

struct Song {
  Directory directory;
  std::unique_ptr<app::ProjectSession> session =
      take(app::ProjectSession::create(directory.path() / "song"));
  project::TrackId audio_track = import_audio();
  project::ClipId clip = session->snapshot().tracks.front().clips.front().id;

  [[nodiscard]] project::TrackId import_audio() {
    const auto wav = make_wav(directory.path());
    const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE_GE(fd, 0, "fixture open failed");
    auto track = session->imports().import_wav_audio(fd);
    (void)::close(fd);
    return take(std::move(track));
  }
  [[nodiscard]] const project::Track& track(project::TrackId id) const {
    const auto& tracks = session->snapshot().tracks;
    const auto found = std::ranges::find(tracks, id, &project::Track::id);
    REQUIRE_NE(found, tracks.end(), "track exists");
    return *found;
  }
  // Project ticks per project sample at the song's (single) tempo: an
  // oracle independent of the session's mapping.
  [[nodiscard]] double ticks_per_frame() const {
    const auto& state = session->snapshot();
    const double bpm = state.tempo.events().front().bpm;
    return bpm / 60.0 / static_cast<double>(state.sample_rate) *
           static_cast<double>(timeline::ticks_per_quarter);
  }
};

bool near_tick(std::int64_t tick, std::uint64_t frame, double ticks_per_frame) {
  const double expected = static_cast<double>(frame) * ticks_per_frame;
  return std::fabs(static_cast<double>(tick) - expected) <=
         static_cast<double>(transient_tolerance) * ticks_per_frame;
}

bool near_frame(std::uint64_t value, std::uint64_t expected) {
  const auto distance = value > expected ? value - expected : expected - value;
  return distance <= transient_tolerance;
}

const project::NativeInstrumentPatch& crate_patch(const project::Track& track) {
  const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
  REQUIRE_NE(patch, nullptr, "the new track plays Crate");
  REQUIRE_EQ(patch->engine, facts::engine_id, "the new track plays Crate");
  return *patch;
}

void whole_clip_becomes_crate_below_it_in_one_undo_step() {
  Song song;
  const auto before = song.session->snapshot().tracks;
  const auto revision = song.session->snapshot().revision;
  const auto made = take(song.session->audio_clips().convert_audio_clip_to_sampler(song.clip));
  const auto& state = song.session->snapshot();
  REQUIRE_EQ(state.revision, revision + 1, "the conversion is one revision");
  REQUIRE_EQ(state.tracks.size(), before.size() + 1, "one track is added");
  REQUIRE_EQ(song.session->tracks().track_index(made.track),
             song.session->tracks().track_index(song.audio_track).value() + 1,
             "the sampler sits right below the audio");
  const auto& audio = song.track(song.audio_track);
  REQUIRE_TRUE(!audio.muted, "the audio track is left as is");
  REQUIRE_EQ(audio.clips, before.front().clips, "the audio track is left as is");

  const auto& track = song.track(made.track);
  REQUIRE_EQ(track.kind, project::TrackKind::instrument, "the new track is an instrument track");
  const auto& patch = crate_patch(track);
  const auto& clip = audio.clips.front();
  const auto& source = *std::ranges::find(state.sources, clip.source, &project::SourceRecord::id);
  REQUIRE_EQ(patch.document.media.size(), 1, "Crate has one sample slot");
  REQUIRE_EQ(patch.document.media.front().id, chops::crate_sample_media,
             "Crate has one sample slot");
  const auto* reference =
      std::get_if<project::ManagedAudioReference>(&patch.document.media.front().media);
  REQUIRE_NE(reference, nullptr, "the slot references the clip's own source");
  REQUIRE_EQ(reference->source, source.id, "the slot references the clip's own source");
  REQUIRE_EQ(reference->sha256, source.sha256, "the slot references the clip's own source");
  REQUIRE_EQ(reference->frames, source.frames, "the slot references the clip's own source");
  REQUIRE_EQ(state.sources.size(), 1, "no audio is copied");

  const auto starts = chops::region_starts(patch.document.regions);
  REQUIRE_EQ(starts.size(), clicks.size() + 1, "regions start at 0 with one chop per hit");
  REQUIRE_EQ(starts.front(), 0, "regions start at 0 with one chop per hit");
  for (std::size_t index = 0; index < clicks.size(); ++index)
    require(near_frame(starts[index + 1], clicks[index]), "a chop starts at each hit");

  REQUIRE_EQ(track.midi_clips.size(), 1, "one MIDI clip is written");
  REQUIRE_EQ(track.midi_clips.front().id, made.clip, "one MIDI clip is written");
  const auto& midi = track.midi_clips.front();
  REQUIRE_EQ(midi.placement, clip.placement, "the MIDI clip sits where the audio clip does");
  REQUIRE_EQ(made.chops, starts.size(), "every chop of a whole clip plays");
  REQUIRE_EQ(midi.notes.size(), starts.size(), "every chop of a whole clip plays");
  const double per_frame = song.ticks_per_frame();
  for (std::size_t index = 0; index < midi.notes.size(); ++index) {
    const auto& note = midi.notes[index];
    REQUIRE_EQ(static_cast<std::size_t>(note.key), facts::first_pad_key + index,
               "chop i plays pad i");
    REQUIRE_EQ(note.velocity, chops::chop_note_velocity, "notes play at the chop velocity");
    require(near_tick(note.start.value, starts[index], per_frame), "a note starts at its chop");
    const auto next = index + 1 < starts.size() ? starts[index + 1] : source_frames;
    require(near_tick(note.end.value, next, per_frame), "a note lasts until the next chop");
  }

  REQUIRE_OK(song.session->history().undo(), "undo succeeds");
  REQUIRE_EQ(song.session->snapshot().tracks, before, "one undo removes the track and clip");
}

void a_trimmed_clip_plays_only_its_range() {
  Song song;
  // Plays 0.5 s to 1.5 s of the source: the hits at 0.75 and 1.25 s.
  const std::uint64_t first = rate / 2;
  const std::uint64_t end = rate * 3 / 2;
  require(
      song.session->audio_clips()
          .trim_audio_clip(song.clip, app::AudioClipEdge::right, {static_cast<std::int64_t>(end)})
          .has_value(),
      "right trim");
  require(
      song.session->audio_clips()
          .trim_audio_clip(song.clip, app::AudioClipEdge::left, {static_cast<std::int64_t>(first)})
          .has_value(),
      "left trim");
  const auto& clip = song.track(song.audio_track).clips.front();
  REQUIRE_EQ(clip.source_range.start.value, static_cast<std::int64_t>(first),
             "the clip plays its sub-range");
  REQUIRE_EQ(clip.source_range.end.value, static_cast<std::int64_t>(end),
             "the clip plays its sub-range");

  const auto made = take(song.session->audio_clips().convert_audio_clip_to_sampler(song.clip));
  const auto& track = song.track(made.track);
  const auto starts = chops::region_starts(crate_patch(track).document.regions);
  // A lead-in chop, the clip's start, its two hits and the tail after it.
  require(starts.size() == 5 && starts.front() == 0 && starts[1] == first &&
              near_frame(starts[2], clicks[1]) && near_frame(starts[3], clicks[2]) &&
              starts[4] == end,
          "chops cover the clip's range, with a lead-in and a tail");

  const auto& midi = track.midi_clips.front();
  REQUIRE_EQ(midi.placement, clip.placement, "the MIDI clip sits where the audio clip does");
  REQUIRE_EQ(made.chops, 3, "only chops inside the range play");
  REQUIRE_EQ(midi.notes.size(), 3, "only chops inside the range play");
  const auto placed = std::get<project::ElapsedInterval>(clip.placement).start.value;
  REQUIRE_EQ(placed, static_cast<std::int64_t>(first), "a left trim keeps the audio in place");
  const double per_frame = song.ticks_per_frame();
  // Clip-relative: the source frame f sounds at f - first after the clip start.
  for (std::size_t index = 0; index < midi.notes.size(); ++index) {
    const auto& note = midi.notes[index];
    REQUIRE_EQ(static_cast<std::size_t>(note.key), facts::first_pad_key + index + 1,
               "the lead-in pad stays silent");
    require(near_tick(note.start.value, starts[index + 1] - first, per_frame),
            "a note starts where its chop sounds");
    require(near_tick(note.end.value, starts[index + 2] - first, per_frame),
            "a note ends at the next chop or the clip's end");
  }
}

void refusals_are_clear() {
  Song song;
  const auto instrument = take(song.session->tracks().add_instrument_track());
  REQUIRE_OK(song.session->midi_clips().add_midi_clip(instrument, {0}), "MIDI clip fixture");
  const auto midi = song.track(instrument).midi_clips.front().id;
  const auto revision = song.session->snapshot().revision;
  const auto refused = song.session->audio_clips().convert_audio_clip_to_sampler(midi);
  REQUIRE_TRUE(!refused, "a MIDI clip is refused");
  REQUIRE_EQ(refused.error().message, "Convert to sampler takes an audio clip",
             "a MIDI clip is refused");
  REQUIRE_EQ(song.session->snapshot().revision, revision, "a refusal changes nothing");
}

} // namespace

int main() {
  try {
    whole_clip_becomes_crate_below_it_in_one_undo_step();
    a_trimmed_clip_plays_only_its_range();
    refusals_are_clear();
  } catch (const std::exception& error) {
    std::cerr << "sampler conversion: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
