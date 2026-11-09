// "Harbour Lights": the project the documentation screenshots show. Built as
// model operations and saved through the project store, so it opens like any
// project a user saved.
#include "screenshot_demo.hpp"

#include "oscillate/analysis/tempo.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oscillate::quick::screenshot {
namespace {

namespace synth = native_dsp::synth;
namespace effects = native_dsp::effects;

constexpr std::int64_t beat = timeline::ticks_per_quarter;
constexpr int beats_per_bar = 4;
constexpr int beat_unit = 4; // 4/4: the beat is a quarter note.
constexpr std::int64_t bar = beat * beats_per_bar;
constexpr std::uint32_t sample_rate = 48'000;
constexpr double tempo_bpm = 124.0;
constexpr int song_bars = 32;
constexpr int chord_bars = 2;
constexpr int section_bars = 8;
constexpr std::uint8_t a_minor_root = 9;

// Sections, as zero-based bars.
constexpr int groove_bar = 8;
constexpr int lift_bar = 16;
constexpr int break_bar = 24;
constexpr int drop_bar = 28;

// General MIDI drum keys, which Anvil and Pulse follow.
constexpr int kick_key = 36;
constexpr int clap_key = 39;
constexpr int closed_hat_key = 42;
constexpr int open_hat_key = 46;

constexpr int semitones_per_octave = 12;
constexpr std::size_t chords_in_cycle = 4;

// Am9, Fmaj7, Cmaj7/G, G6: four voices each, around middle C.
constexpr std::array<std::array<int, 4>, chords_in_cycle> chords{{
    {57, 60, 64, 71},
    {53, 57, 60, 64},
    {55, 59, 60, 64},
    {55, 59, 62, 64},
}};
// The chords' roots, an octave and a half below them: A2, F2, C3, G2.
constexpr std::array<int, chords_in_cycle> bass_roots{45, 41, 48, 43};

// Track colours, warm drums to cool atmosphere.
constexpr std::uint32_t kick_colour = 0xE69561;
constexpr std::uint32_t drums_colour = 0xE4BC66;
constexpr std::uint32_t bass_colour = 0xD9776B;
constexpr std::uint32_t chords_colour = 0xB47AC7;
constexpr std::uint32_t arp_colour = 0x6ABDC1;
constexpr std::uint32_t bells_colour = 0x78A9DB;
constexpr std::uint32_t pad_colour = 0x8B93C9;

// Fader levels and pans.
constexpr double kick_level = 0.8;
constexpr double drums_level = 0.55;
constexpr double bass_level = 0.5;
constexpr double chords_level = 0.36;
constexpr double chords_pan = -0.1;
constexpr double arp_level = 0.3;
constexpr double arp_pan = 0.18;
constexpr double bells_level = 0.28;
constexpr double bells_pan = -0.15;
constexpr double pad_level = 0.26;
// The Chords, Arp and Pad tracks play through the Music group bus; the
// Chords and Pad also send to the Hall reverb return.
constexpr std::uint32_t music_bus_colour = 0xA98CE6;
constexpr std::uint32_t hall_return_colour = 0x5FC7C0;
constexpr double hall_send_gain = 0.3;
constexpr double hall_reverb_mix = 1.0;
constexpr double hall_reverb_decay = 5.0;

// The Top Loop (ADR-0112's screenshots): an audio loop recorded at its own
// tempo, following the project's from the Lift with two warp markers, and a
// tempo ramp into the Drop.
constexpr std::uint32_t top_loop_colour = 0x9DCB7A;
constexpr double top_loop_bpm = 122.0;
constexpr int top_loop_bars = 8;
constexpr int top_loop_bar = lift_bar;
constexpr double top_loop_confidence = 0.9;
constexpr std::uint64_t top_loop_generation = 1;
// The two warp markers: on these beats of the loop, the second dragged a
// sixteenth late so its segment plays a little slower.
constexpr int first_warp_beat = 9;
constexpr int second_warp_beat = 21;
constexpr double warp_drag_beats = 0.25;
// The loop's voices: a kick each beat, a clap on two and four, hats on the
// off-beats; amplitudes, decays (seconds) and the kick's pitch (Hz).
constexpr double kick_level_peak = 0.7;
constexpr double kick_decay = 0.09;
constexpr double kick_hz = 55.0;
constexpr double clap_level_peak = 0.35;
constexpr double clap_decay = 0.05;
constexpr double hat_level_peak = 0.2;
constexpr double hat_decay = 0.02;
constexpr int beats_per_clap = 2;
constexpr std::uint32_t noise_multiplier = 1'664'525U;
constexpr std::uint32_t noise_increment = 1'013'904'223U;
constexpr double pcm_full_scale = 32'767.0;
// The ramp into the Drop.
constexpr double drop_bpm = 126.0;

template <typename T> T take(foundation::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}

template <typename T> T id(std::uint64_t serial) { return take(T::from_bits({0, serial})); }

class Gate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

std::size_t preset_index(const synth::SynthEngine& engine, std::string_view name) {
  const auto presets = engine.descriptor->presets;
  const auto found = std::ranges::find(
      presets, name, [](const native_dsp::Preset& preset) { return preset.name; });
  if (found == presets.end())
    throw std::runtime_error("missing preset: " + std::string(name));
  return static_cast<std::size_t>(found - presets.begin());
}

const std::array<int, 4>& chord_at(int absolute_bar) {
  return chords[static_cast<std::size_t>(absolute_bar / chord_bars) % chords_in_cycle];
}

int root_at(int absolute_bar) {
  return bass_roots[static_cast<std::size_t>(absolute_bar / chord_bars) % chords_in_cycle];
}

// One instrument track: its engine and factory preset, name, colour and fader.
struct Voice {
  std::string_view engine;
  std::string_view preset;
  std::string name;
  std::uint32_t colour;
  double level;
  double pan = 0.0;
};

// One note; `bar` counts from the clip's start, beat and length are in beats.
struct NoteAt {
  int bar;
  double beat;
  double length;
  int key;
  int velocity;
};

struct Song {
  std::vector<project::Operation> operations;
  std::uint64_t next_track = 1;
  std::uint64_t next_clip = 1;
  std::uint64_t next_note = 1;
  std::uint64_t next_marker = 1;
  std::uint64_t next_send = 1;

  project::TrackId instrument(Voice voice) {
    const auto* engine = synth::find_engine(voice.engine);
    if (engine == nullptr)
      throw std::runtime_error("missing native synth: " + std::string(voice.engine));
    const auto track = id<project::TrackId>(next_track++);
    operations.emplace_back(project::AddTrack{track, project::TrackKind::instrument});
    operations.emplace_back(project::SetNativeInstrumentPatch{
        track, take(native_inserts::native_patch_for_preset(*engine,
                                                            preset_index(*engine, voice.preset)))});
    operations.emplace_back(project::SetTrackName{track, std::move(voice.name)});
    operations.emplace_back(project::SetTrackColor{track, voice.colour});
    operations.emplace_back(project::SetTrackLevelPan{track, voice.level, voice.pan});
    return track;
  }

  project::TrackId bus(project::TrackKind kind, std::string name, std::uint32_t colour) {
    const auto track = id<project::TrackId>(next_track++);
    operations.emplace_back(project::AddTrack{track, kind});
    operations.emplace_back(project::SetTrackName{track, std::move(name)});
    operations.emplace_back(project::SetTrackColor{track, colour});
    return track;
  }

  void send(project::TrackId from, project::TrackId to, double gain) {
    operations.emplace_back(
        project::AddSendRoute{from, {id<project::SendId>(next_send++), to, gain}});
  }

  void effect(project::SignalPathId path, std::size_t index, std::string_view engine,
              std::vector<project::NativeInstrumentParameter> parameters = {}) {
    if (effects::find_engine(engine) == nullptr)
      throw std::runtime_error("missing native effect: " + std::string(engine));
    operations.emplace_back(project::AddNativeEffect{
        path, index, project::NativeEffectPatch{std::string(engine), 1, std::move(parameters), 1}});
  }

  void marker(int at_bar, std::string name) {
    operations.emplace_back(project::AddMarker{
        {id<project::MarkerId>(next_marker++), {at_bar * bar}, std::move(name)}});
  }

  struct Clip {
    Song& song;
    project::TrackId track;
    int start_bar;
    int bars;
    std::vector<project::MidiNote> notes;

    void note(const NoteAt& at) {
      const auto start =
          static_cast<std::int64_t>(at.bar) * bar + static_cast<std::int64_t>(at.beat * beat);
      const auto length = static_cast<std::int64_t>(at.length * beat);
      notes.push_back({id<project::NoteId>(song.next_note++),
                       {start},
                       {start + length},
                       static_cast<std::uint8_t>(at.key),
                       static_cast<std::uint8_t>(at.velocity)});
    }

    void finish() {
      std::ranges::sort(notes, {}, [](const project::MidiNote& note) { return note.start.value; });
      song.operations.emplace_back(project::AddMidiClip{
          track,
          {id<project::ClipId>(song.next_clip++),
           project::MusicalInterval{{start_bar * bar}, {(start_bar + bars) * bar}},
           {bars * bar},
           false,
           std::move(notes)}});
    }
  };

  Clip clip(project::TrackId track, int start_bar, int bars) {
    return {*this, track, start_bar, bars, {}};
  }
};

// Four on the floor, resting through the break.
void kick(Song& song, project::TrackId track) {
  constexpr int accent = 112;
  constexpr int pulse = 100;
  constexpr double length = 0.25;
  for (const auto& [start, bars] : {std::pair{0, lift_bar}, std::pair{lift_bar, section_bars},
                                    std::pair{drop_bar, song_bars - drop_bar}}) {
    auto clip = song.clip(track, start, bars);
    for (int b = 0; b < bars; ++b) {
      for (int quarter = 0; quarter < beats_per_bar; ++quarter)
        clip.note(
            {b, static_cast<double>(quarter), length, kick_key, quarter == 0 ? accent : pulse});
    }
    clip.finish();
  }
}

// Offbeat hats from the top; claps from the groove; open hats in the lift.
void drums(Song& song, project::TrackId track) {
  constexpr double hat_length = 0.1;
  constexpr double clap_length = 0.2;
  constexpr double offbeat = 0.5;
  constexpr int hat_velocity = 70;
  constexpr int ghost_velocity = 46;
  constexpr int clap_velocity = 92;
  constexpr int open_hat_velocity = 58;
  constexpr int sixteenths_per_beat = 4;
  for (const auto& [start, bars] :
       {std::pair{0, section_bars}, std::pair{groove_bar, section_bars},
        std::pair{lift_bar, section_bars}, std::pair{drop_bar, song_bars - drop_bar}}) {
    auto clip = song.clip(track, start, bars);
    for (int b = 0; b < bars; ++b) {
      for (int quarter = 0; quarter < beats_per_bar; ++quarter) {
        clip.note({b, quarter + offbeat, hat_length, closed_hat_key, hat_velocity});
        if (start >= lift_bar && quarter % 2 == 1)
          clip.note({b, quarter + offbeat + 1.0 / sixteenths_per_beat, hat_length, closed_hat_key,
                     ghost_velocity});
      }
      if (start >= groove_bar) {
        clip.note({b, 1, clap_length, clap_key, clap_velocity});
        clip.note({b, 3, clap_length, clap_key, clap_velocity});
      }
      if (start >= lift_bar && b % 2 == 1)
        clip.note({b, 3 + offbeat, hat_length, open_hat_key, open_hat_velocity});
    }
    clip.finish();
  }
}

// A rolling offbeat line on the chord roots, lifting to the fifth.
void bass(Song& song, project::TrackId track) {
  constexpr int fifth = 7;
  constexpr int velocity = 92;
  constexpr int soft = 74;
  constexpr double length = 0.4;
  for (const auto& [start, bars] :
       {std::pair{groove_bar, section_bars}, std::pair{lift_bar, section_bars},
        std::pair{drop_bar, song_bars - drop_bar}}) {
    auto clip = song.clip(track, start, bars);
    for (int b = 0; b < bars; ++b) {
      const int root = root_at(start + b);
      clip.note({b, 0.5, length, root, velocity});
      clip.note({b, 1.5, length, root, soft});
      clip.note({b, 2.5, length, root, velocity});
      clip.note({b, 3.5, length, b % 2 == 1 ? root + fifth : root, soft});
    }
    clip.finish();
  }
}

// One held chord per two bars, slightly short so they breathe.
void chord_part(Song& song, project::TrackId track) {
  constexpr double held = chord_bars * beats_per_bar - 0.25;
  constexpr int velocity = 64;
  constexpr int lifted = 76;
  for (const int start : {0, lift_bar}) {
    auto clip = song.clip(track, start, lift_bar);
    for (int b = 0; b < lift_bar; b += chord_bars) {
      for (const int key : chord_at(start + b))
        clip.note({b, 0, held, key, start >= lift_bar ? lifted : velocity});
    }
    clip.finish();
  }
}

// A displaced eighth-note arpeggio an octave up, with gaps.
void arp(Song& song, project::TrackId track) {
  constexpr std::array<std::size_t, 7> order{0, 2, 1, 3, 2, 0, 3};
  constexpr double step = 0.5;
  constexpr double length = 0.3;
  constexpr int steps_per_bar = 8;
  constexpr int rest_every = 5;
  constexpr int velocity = 58;
  constexpr int accent = 74;
  for (const int start : {0, groove_bar, lift_bar, break_bar}) {
    auto clip = song.clip(track, start, section_bars);
    int position = 0;
    for (int b = 0; b < section_bars; ++b) {
      const auto& chord = chord_at(start + b);
      for (int s = 0; s < steps_per_bar; ++s, ++position) {
        if (position % rest_every == rest_every - 1)
          continue;
        const auto key = chord[order[static_cast<std::size_t>(position) % order.size()]];
        clip.note(
            {b, s * step, length, key + semitones_per_octave, s % 3 == 0 ? accent : velocity});
      }
    }
    clip.finish();
  }
}

// A sparse bell motif off the beat, answered an octave down.
void bells(Song& song, project::TrackId track) {
  struct Hit {
    int bar;
    double beat;
    double length;
    int key;
  };
  constexpr std::array<Hit, 6> motif{{{0, 1.5, 1.0, 76},
                                      {0, 3.0, 0.75, 72},
                                      {1, 0.75, 1.5, 74},
                                      {2, 2.5, 1.0, 71},
                                      {3, 0.5, 0.75, 72},
                                      {3, 2.0, 1.5, 69}}};
  constexpr int phrase_bars = 4;
  constexpr int velocity = 70;
  constexpr int answer_velocity = 52;
  auto clip = song.clip(track, lift_bar, song_bars - lift_bar);
  for (int phrase = 0; phrase < (song_bars - lift_bar) / phrase_bars; ++phrase) {
    const bool answer = phrase % 2 == 1;
    for (const auto& hit : motif) {
      clip.note({phrase * phrase_bars + hit.bar, hit.beat, hit.length,
                 answer ? hit.key - semitones_per_octave : hit.key,
                 answer ? answer_velocity : velocity});
    }
  }
  clip.finish();
}

// Long low roots and fifths under everything.
void pad(Song& song, project::TrackId track) {
  constexpr int fifth_voice = 2;
  constexpr int velocity = 52;
  constexpr double held = section_bars * beats_per_bar - 0.5;
  for (const int start : {0, lift_bar}) {
    auto clip = song.clip(track, start, lift_bar);
    for (int b = 0; b < lift_bar; b += section_bars) {
      const auto& chord = chord_at(start + b);
      clip.note({b, 0, held, chord[0] - semitones_per_octave, velocity});
      clip.note({b, 0, held, chord[fifth_voice], velocity});
    }
    clip.finish();
  }
}

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// The Top Loop as a mono 16-bit WAV at its own tempo.
void write_top_loop(const std::filesystem::path& path) {
  const auto period = 60.0 * sample_rate / top_loop_bpm;
  const auto beats = top_loop_bars * beats_per_bar;
  const auto frames = static_cast<std::uint32_t>(std::lround(beats * period));
  constexpr std::uint16_t pcm_format = 1;
  constexpr std::uint16_t mono = 1;
  constexpr std::uint16_t bytes_per_sample = 2;
  constexpr std::uint16_t bits_per_sample = 16;
  constexpr std::uint32_t format_chunk_bytes = 16;
  constexpr std::uint32_t header_bytes = 36;
  std::vector<std::uint8_t> wav{'R', 'I', 'F', 'F'};
  put32(wav, header_bytes + frames * bytes_per_sample);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(wav, format_chunk_bytes);
  put16(wav, pcm_format);
  put16(wav, mono);
  put32(wav, sample_rate);
  put32(wav, sample_rate * bytes_per_sample);
  put16(wav, bytes_per_sample);
  put16(wav, bits_per_sample);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(wav, frames * bytes_per_sample);
  std::uint32_t noise = 1;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto beat_position = frame / period;
    const auto beat = static_cast<int>(beat_position);
    const auto since_beat = (beat_position - beat) * period / sample_rate;
    const auto half = beat_position * 2.0;
    const auto since_half = (half - std::floor(half)) * period / 2.0 / sample_rate;
    noise = noise * noise_multiplier + noise_increment;
    const auto white =
        static_cast<double>(noise) / std::numeric_limits<std::uint32_t>::max() * 2.0 - 1.0;
    double value = kick_level_peak * std::exp(-since_beat / kick_decay) *
                   std::sin(2.0 * std::numbers::pi * kick_hz * since_beat);
    if (beat % beats_per_clap == 1)
      value += clap_level_peak * std::exp(-since_beat / clap_decay) * white;
    if (static_cast<int>(half) % 2 == 1)
      value += hat_level_peak * std::exp(-since_half / hat_decay) * white;
    put16(wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                   std::lround(std::clamp(value, -1.0, 1.0) * pcm_full_scale))));
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  if (!output.good())
    throw std::runtime_error("the Top Loop did not write");
}

void must(const foundation::Result<void>& result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}

// Imports the Top Loop with its reviewed tempo, beats and transients, sets
// it to follow the project from the Lift with two warp markers, and draws
// the ramp into the Drop.
void add_top_loop(const std::filesystem::path& root) {
  auto session = take(application::ProjectSession::open(root));
  const auto wav = root.parent_path() / (root.filename().string() + "-top-loop.wav");
  write_top_loop(wav);
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("the Top Loop did not open");
  const auto imported = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  std::filesystem::remove(wav);
  const auto track = take(imported);
  must(session->tracks().set_track_name(track, "Top Loop"));
  must(session->tracks().set_track_color(track, top_loop_colour));
  // First in the list, where the screenshots show it beside the ruler.
  must(session->tracks().move_track(track, 0));
  const auto& snapshot = session->snapshot();
  const auto& source = snapshot.sources.back();
  const auto clip =
      std::ranges::find(snapshot.tracks, track, &project::Track::id)->clips.front().id;
  must(session->audio_clips().set_audio_clip_name(clip, "Top Loop"));

  const auto period = 60.0 * source.sample_rate / top_loop_bpm;
  analysis::TempoEstimate estimate;
  estimate.identity = {take(application::tempo_cache_key(source)).hex(), top_loop_generation};
  estimate.sample_rate = source.sample_rate;
  estimate.frames = source.frames;
  estimate.bpm = top_loop_bpm;
  estimate.confidence = top_loop_confidence;
  std::vector<std::uint64_t> onsets;
  for (int beat = 0; beat < top_loop_bars * beats_per_bar; ++beat) {
    const auto at = static_cast<std::uint64_t>(std::lround(beat * period));
    estimate.suggested_beat_frames.push_back(at);
    onsets.push_back(at);
    onsets.push_back(static_cast<std::uint64_t>(std::lround((beat + 0.5) * period)));
  }
  must(session->source_analysis().accept_source_tempo_analysis(
      source.id, session->snapshot().revision, estimate));
  must(session->source_analysis().accept_source_onset_analysis(
      session->snapshot().sources.back(), session->snapshot().revision, onsets, false));
  must(session->warp().set_audio_clip_follow(clip, true));
  must(session->audio_clips().move_audio_clip(clip, {top_loop_bar * bar}));

  // Two markers on beats, the second dragged a sixteenth later.
  const auto layout = take(application::clip_warp_layout(session->snapshot(), clip));
  const auto beat_frames =
      static_cast<double>(layout.output_frames) / (top_loop_bars * beats_per_bar);
  for (const int at : {first_warp_beat, second_warp_beat})
    must(session->audio_clips().set_audio_clip_processing(
        clip, take(application::add_warp_marker(
                  session->snapshot(), clip,
                  static_cast<std::uint64_t>(std::lround(at * beat_frames))))));
  must(session->audio_clips().set_audio_clip_processing(
      clip, take(application::move_warp_marker(
                session->snapshot(), clip,
                {.index = 1,
                 .output_frame = static_cast<std::uint64_t>(
                     std::lround((second_warp_beat + warp_drag_beats) * beat_frames))}))));

  must(session->song().set_tempo_ramp({break_bar * bar}, {drop_bar * bar}, drop_bpm,
                                      timeline::TempoShape::ease_in_out));
  must(session->saving().save());
}

} // namespace

void create_demo_project(const std::filesystem::path& root) {
  if (std::filesystem::exists(root))
    throw std::runtime_error("the demo project folder already exists");
  Gate gate;
  auto model = take(project::ProjectModel::create(
      id<project::ProjectId>(1), sample_rate,
      take(timeline::TempoMap::create({{{0}, tempo_bpm, timeline::TempoShape::step}}, 1)),
      take(timeline::MeterMap::create({{{0}, beats_per_bar, beat_unit}}, 1)), gate));
  Song song;
  const auto kick_track =
      song.instrument({"anvil", "Tech House Thump", "Kick", kick_colour, kick_level});
  const auto drum_track =
      song.instrument({"pulse", "909 House", "Drums", drums_colour, drums_level});
  const auto bass_track = song.instrument({"torque", "Moog Bass", "Bass", bass_colour, bass_level});
  const auto chord_track =
      song.instrument({"ember", "Juno Strings", "Chords", chords_colour, chords_level, chords_pan});
  const auto arp_track = song.instrument({"nova", "Pluck", "Arp", arp_colour, arp_level, arp_pan});
  const auto bell_track =
      song.instrument({"halo", "Bells", "Bells", bells_colour, bells_level, bells_pan});
  const auto pad_track = song.instrument({"drift", "Aurora", "Pad", pad_colour, pad_level});

  // The chords' chain shows live displays while playing: Shape's output
  // spectrum and Press's gain reduction.
  constexpr double chords_press_threshold_db = -30.0;
  constexpr double chords_press_ratio = 3.0;
  constexpr double arp_delay_mix = 0.24;
  constexpr double arp_delay_feedback = 0.4;
  constexpr double bells_chorus_mix = 0.35;
  constexpr double bells_reverb_mix = 0.3;
  constexpr double bells_reverb_decay = 4.0;
  constexpr double master_ceiling_db = -1.0;
  song.effect(chord_track, 0, "shape");
  song.effect(
      chord_track, 1, "press",
      {{"press.threshold", chords_press_threshold_db}, {"press.ratio", chords_press_ratio}});
  song.effect(arp_track, 0, "relay",
              {{"relay.mix", arp_delay_mix}, {"relay.feedback", arp_delay_feedback}});
  song.effect(bell_track, 0, "orbit", {{"orbit.mix", bells_chorus_mix}});
  song.effect(bell_track, 1, "expanse",
              {{"expanse.mix", bells_reverb_mix}, {"expanse.decay", bells_reverb_decay}});
  song.effect(project::MasterSignalPath{}, 0, "ceiling", {{"ceiling.ceiling", master_ceiling_db}});

  // After the instruments in the saved order, so their indices stay put;
  // the track list shows the members under their bus.
  const auto music_bus = song.bus(project::TrackKind::group_bus, "Music", music_bus_colour);
  for (const auto member : {chord_track, arp_track, pad_track})
    song.operations.emplace_back(project::SetTrackMainOutput{member, music_bus});
  const auto hall_return = song.bus(project::TrackKind::return_bus, "Hall", hall_return_colour);
  song.effect(hall_return, 0, "expanse",
              {{"expanse.mix", hall_reverb_mix}, {"expanse.decay", hall_reverb_decay}});
  for (const auto source : {chord_track, pad_track})
    song.send(source, hall_return, hall_send_gain);

  kick(song, kick_track);
  drums(song, drum_track);
  bass(song, bass_track);
  chord_part(song, chord_track);
  arp(song, arp_track);
  bells(song, bell_track);
  pad(song, pad_track);
  song.marker(0, "Intro");
  song.marker(groove_bar, "Groove");
  song.marker(lift_bar, "Lift");
  song.marker(break_bar, "Break");
  song.operations.emplace_back(
      project::SetProjectKey{project::MusicalKey{a_minor_root, project::KeyMode::minor}});

  auto applied = take(model.apply(song.operations));
  {
    auto store = take(persistence::ProjectStore::open_writer(root, true));
    const auto saved =
        take(store.save(*applied.snapshot, persistence::SaveKind::explicit_save, {}));
    if (saved.status != persistence::SaveStatus::committed)
      throw std::runtime_error("the demo project did not save");
  }
  // The writer is closed, so the session can open the project.
  add_top_loop(root);
}

} // namespace oscillate::quick::screenshot
