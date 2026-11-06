#include "oscillate/application/browser_preview.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace timeline = oscillate::timeline;

using oscillate::test::require;

template <typename Id> Id id(std::uint64_t low) { return Id::from_bits({0, low}).value(); }

// Each sound gets the phrase that shows it off, in time.
void phrases_suit_the_sound() {
  using application::PreviewStyle;
  REQUIRE_EQ(application::preview_style("pulse", "Kit", "808 Classic"), PreviewStyle::drums,
             "Pulse previews a beat");
  REQUIRE_EQ(application::preview_style("prism", "Bass", "Wobble"), PreviewStyle::bass,
             "a bass previews a riff");
  REQUIRE_EQ(application::preview_style("nova", "Character", "Reese Growl"), PreviewStyle::bass,
             "a bass previews a riff");
  REQUIRE_EQ(application::preview_style("halo", "Mallets", "Marimba"), PreviewStyle::line,
             "leads and mallets preview a line, case aside");
  REQUIRE_EQ(application::preview_style("prism", "LEAD", "Supersaw"), PreviewStyle::line,
             "leads and mallets preview a line, case aside");
  REQUIRE_EQ(application::preview_style("drift", "Clouds", "Nebula"), PreviewStyle::texture,
             "a granular texture holds; a granular pad plays chords");
  REQUIRE_EQ(application::preview_style("drift", "Pad", "Aurora"), PreviewStyle::chord,
             "a granular texture holds; a granular pad plays chords");
  REQUIRE_EQ(application::preview_style("concert", "Grand", "Hall"), PreviewStyle::chord,
             "a piano plays chords");
  REQUIRE_EQ(application::preview_style("knock", "Kit", "Microhouse Clicks"),
             PreviewStyle::percussion, "Knock previews a top loop");

  // 120 BPM at 48 kHz: a beat is 24 000 frames.
  const auto drums = application::preview_phrase(PreviewStyle::drums, 120.0, 48'000);
  require(std::ranges::is_sorted(drums, {}, &application::PreviewNote::start_frame),
          "notes are in order");
  const auto kicks = std::ranges::count_if(drums, [](const auto& note) { return note.key == 36; });
  const auto snares = std::ranges::count_if(drums, [](const auto& note) { return note.key == 38; });
  REQUIRE_EQ(kicks, 3, "the beat has three kicks and two snares on Pulse's pads");
  REQUIRE_EQ(snares, 2, "the beat has three kicks and two snares on Pulse's pads");
  require(std::ranges::any_of(
              drums, [](const auto& note) { return note.key == 38 && note.start_frame == 24'000; }),
          "the snare lands on beat two");
  REQUIRE_LT(drums.back().start_frame, 4 * 24'000, "the beat is one bar");

  // Knock's top loop plays its eight pads from the first key, for two bars.
  namespace knock = oscillate::native_dsp::synth::knock;
  const auto top =
      application::preview_phrase(PreviewStyle::percussion, 120.0, 48'000, knock::first_key);
  require(std::ranges::is_sorted(top, {}, &application::PreviewNote::start_frame),
          "the loop is in order");
  require(std::ranges::all_of(top,
                              [](const auto& note) {
                                return note.key >= knock::first_key &&
                                       static_cast<std::size_t>(note.key) <
                                           knock::first_key + knock::pad_count;
                              }),
          "the loop stays on the eight pads");
  require(std::ranges::any_of(top, [](const auto& note) { return note.start_frame >= 4 * 24'000; }),
          "the loop plays a second bar");

  // Tempo scales every start: at 60 BPM the chord's second voicing starts
  // on beat five, 4 s in.
  const auto slow = application::preview_phrase(PreviewStyle::chord, 60.0, 48'000);
  require(std::ranges::any_of(slow, [](const auto& note) { return note.start_frame == 192'000; }),
          "the second chord starts on beat five");
  // Out-of-range tempos clamp: 400 BPM plays at 180.
  const auto fast = application::preview_phrase(PreviewStyle::chord, 400.0, 48'000);
  require(std::ranges::any_of(fast, [](const auto& note) { return note.start_frame == 64'000; }),
          "tempo clamps to 180 BPM");
  for (const auto style : {PreviewStyle::drums, PreviewStyle::percussion, PreviewStyle::bass,
                           PreviewStyle::line, PreviewStyle::chord, PreviewStyle::texture}) {
    for (const auto& note : application::preview_phrase(style, 90.0, 44'100)) {
      REQUIRE_LE(note.key, 127, "every note is valid and inside two bars");
      REQUIRE_GE(note.velocity, 1, "every note is valid and inside two bars");
      REQUIRE_LE(note.velocity, 127, "every note is valid and inside two bars");
      REQUIRE_GT(note.length_frames, 0, "every note is valid and inside two bars");
      REQUIRE_LE(note.start_frame + note.length_frames, 8 * 29'400,
                 "every note is valid and inside two bars");
    }
  }
}

void sample_tempos_come_from_names_and_lengths() {
  const auto name = [](std::string_view stem) { return application::tempo_from_file_name(stem); };
  REQUIRE_EQ(name("Drum Loop 124 BPM"), 124.0, "a spaced BPM tag");
  REQUIRE_EQ(name("drums_128bpm_Am"), 128.0, "an attached bpm tag");
  REQUIRE_EQ(name("BPM 90 - groove"), 90.0, "a leading BPM tag");
  REQUIRE_EQ(name("Bass-Am-90"), 90.0, "a lone number set off by separators");
  REQUIRE_EQ(name("120_Kick"), 120.0, "a lone number at the start");
  require(!name("TR-808 Kick 01"), "a drum machine's name and an index are not tempos");
  require(!name("Loop 100 120"), "two candidates are ambiguous");
  require(!name("Pad v2 long"), "no number, no tempo");

  // 4 bars of 124 BPM at 48 kHz: 16 beats in 371 612.9 frames, so a cut
  // to the nearest frame still reads 124 BPM.
  const auto bars = application::tempo_from_loop_length(371'613, 48'000, 120.0);
  REQUIRE_TRUE(bars, "a four-bar loop reads its tempo");
  REQUIRE_LT(std::abs(*bars - 124.0), 0.01, "a four-bar loop reads its tempo");
  // 4 s fits 60 BPM (4 beats, out of range), 120 BPM (8 beats) and 240.
  const auto even = application::tempo_from_loop_length(192'000, 48'000, 100.0);
  REQUIRE_TRUE(even, "only in-range tempos qualify");
  REQUIRE_LT(std::abs(*even - 120.0), tolerance::double_accumulation,
             "only in-range tempos qualify");
  require(!application::tempo_from_loop_length(30'000, 48'000, 120.0), "a one-shot has none");
  require(!application::tempo_from_loop_length(100'003, 48'000, 120.0),
          "a length that fits no whole tempo has none");

  // The next beat at 120 BPM, by the project's tempo map.
  const auto tempo =
      timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1).value();
  REQUIRE_EQ(application::next_beat_sample(tempo, 0, 48'000), 0, "a beat starts now");
  REQUIRE_EQ(application::next_beat_sample(tempo, 1, 48'000), 1, "a sample late is on the beat");
  REQUIRE_EQ(application::next_beat_sample(tempo, 2, 48'000), 24'000, "the next beat waits");
  REQUIRE_EQ(application::next_beat_sample(tempo, 23'999, 48'000), 24'000,
             "a frame early still waits for the beat");
  REQUIRE_EQ(application::next_beat_sample(tempo, 48'000, 48'000), 48'000, "on a beat");
}

void dropped_clips_move_into_the_song_key() {
  using project::KeyMode;
  const auto review = [](project::MusicalKey key, double confidence) {
    project::SourceKeyReview out{id<project::SourceId>(1), key, confidence, {}, std::nullopt};
    return out;
  };
  const project::MusicalKey c_major{0, KeyMode::major};
  const project::MusicalKey a_minor{9, KeyMode::minor};
  // D major into C major: two down.
  auto match = application::key_match(review({2, KeyMode::major}, 0.9), 0.0, c_major);
  REQUIRE_TRUE(match, "D major moves down a tone");
  REQUIRE_EQ(match->semitones, -2, "D major moves down a tone");
  REQUIRE_EQ(match->to, c_major, "D major moves down a tone");
  // G major into C major: five up, not seven down.
  match = application::key_match(review({7, KeyMode::major}, 0.9), 0.0, c_major);
  REQUIRE_TRUE(match, "the nearest way wins");
  REQUIRE_EQ(match->semitones, 5, "the nearest way wins");
  // F# major into C major: a tritone either way goes down.
  match = application::key_match(review({6, KeyMode::major}, 0.9), 0.0, c_major);
  REQUIRE_TRUE(match, "a tie goes down");
  REQUIRE_EQ(match->semitones, -6, "a tie goes down");
  // A minor material in C major is already at home (relative minor).
  require(!application::key_match(review(a_minor, 0.9), 0.0, c_major),
          "the relative minor needs no move");
  // B minor in C major moves to A minor.
  match = application::key_match(review({11, KeyMode::minor}, 0.9), 0.0, c_major);
  REQUIRE_TRUE(match, "minor goes to the relative");
  REQUIRE_EQ(match->semitones, -2, "minor goes to the relative");
  REQUIRE_EQ(match->to, a_minor, "minor goes to the relative");
  // A clip already pitched +2 from C major hears D major, and moves back.
  match = application::key_match(review(c_major, 0.9), 2.0, c_major);
  REQUIRE_TRUE(match, "the clip's own pitch counts");
  REQUIRE_EQ(match->semitones, -2, "the clip's own pitch counts");
  REQUIRE_EQ(match->from.tonic, 2, "the clip's own pitch counts");
  // Weak evidence is ignored, a correction is trusted.
  require(!application::key_match(review({2, KeyMode::major}, 0.2), 0.0, c_major),
          "an unsure detection offers nothing");
  auto corrected = review({2, KeyMode::major}, 0.0);
  corrected.correction = project::MusicalKey{4, KeyMode::major};
  match = application::key_match(corrected, 0.0, c_major);
  REQUIRE_TRUE(match, "a correction overrides the detection");
  REQUIRE_EQ(match->semitones, -4, "a correction overrides the detection");
  // The clip's pitch range is never exceeded.
  require(!application::key_match(review({2, KeyMode::major}, 0.9), -47.0, c_major),
          "a shift past -48 semitones is not offered");
}

void kicks_are_recognised() {
  project::Track named(id<project::TrackId>(1), {}, project::TrackKind::audio);
  named.name = "Kick 2";
  require(application::looks_like_kick(named), "a track called Kick");
  named.name = "BD hard";
  require(application::looks_like_kick(named), "BD is a kick");
  named.name = "Bass Drum";
  require(application::looks_like_kick(named), "so is a bass drum");
  named.name = "Bass";
  require(!application::looks_like_kick(named), "a bass is not");
  named.name = "Kickback Vox";
  require(application::looks_like_kick(named), "kick-prefixed words count (a cheap rule)");
  named.name = "Drums";
  require(!application::looks_like_kick(named), "a drum bus is not a kick");
  require(application::looks_like_kick(named, "Kick"), "the bundled kick sound is");

  project::Track bus(id<project::TrackId>(2), {}, project::TrackKind::return_bus);
  bus.name = "Kick verb";
  require(!application::looks_like_kick(bus), "buses never suggest ducking");

  // A Pulse kit that plays only its kick pads.
  const auto note = [](std::uint64_t serial, std::uint8_t key) {
    return project::MidiNote{id<project::NoteId>(serial), {0}, {960}, key, 100};
  };
  project::NativeInstrumentPatch pulse;
  pulse.engine = "pulse";
  project::Track kit(id<project::TrackId>(3), {}, project::TrackKind::instrument, pulse);
  require(!application::looks_like_kick(kit), "an empty kit is not a kick yet");
  kit.midi_clips.emplace_back(id<project::ClipId>(4), project::MusicalInterval{{0}, {960'000'000}},
                              timeline::TickPosition{960'000'000}, false,
                              std::vector{note(1, 36), note(2, 37)});
  require(application::looks_like_kick(kit), "a kit on its kick pads is a kick");
  kit.midi_clips.front().notes.push_back(note(3, 38));
  require(!application::looks_like_kick(kit), "a snare makes it a beat");

  require(!application::keys_a_sidechain(kit), "nothing keyed yet");
  kit.sends.emplace_back(id<project::SendId>(5), id<project::TrackId>(1), 1.0,
                         project::SendPurpose::sidechain, id<project::InsertSlotId>(6));
  require(application::keys_a_sidechain(kit), "a key send counts");
}

} // namespace

int main() {
  phrases_suit_the_sound();
  sample_tempos_come_from_names_and_lengths();
  dropped_clips_move_into_the_song_key();
  kicks_are_recognised();
}
