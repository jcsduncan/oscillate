// Crate's chopping (spec 09 §3): transient positions on synthetic clicks,
// grid and equal arithmetic, normalisation, manual edits, classification of
// synthesised drums, root detection, slot preservation on re-chop and Chop to
// MIDI (spec 09 §4).

#include "oscillate/application/crate_chops.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace crate = oscillate::application::crate;
namespace project = oscillate::project;
using oscillate::engine::StereoFrame;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double two_pi = 2.0 * std::numbers::pi;
// Spec 09 §6: transient starts land within 2 ms of the click.
constexpr std::uint64_t transient_tolerance = rate * 2 / 1'000;
constexpr std::uint64_t merge_frames = rate / 100; // 10 ms

using oscillate::test::require;

// Deterministic white noise in [-1, 1] (xorshift32).
class Noise {
public:
  double next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    return static_cast<double>(state_) / static_cast<double>(UINT32_MAX) * 2.0 - 1.0;
  }

private:
  std::uint32_t state_ = 0x9E3779B9U;
};

// Independent RBJ high-pass for the hat oracle.
class HighPass {
public:
  explicit HighPass(double hz) {
    const double omega = two_pi * hz / rate;
    const double alpha = std::sin(omega) / std::numbers::sqrt2;
    const double cosine = std::cos(omega);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 + cosine) / 2.0 / a0;
    b1_ = -(1.0 + cosine) / a0;
    b2_ = b0_;
    a1_ = -2.0 * cosine / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  double process(double x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return y;
  }

private:
  double b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

crate::SampleAudio silence(double seconds) {
  crate::SampleAudio audio;
  audio.source_rate = rate;
  audio.frames.assign(static_cast<std::size_t>(seconds * rate), StereoFrame{0.0F, 0.0F});
  return audio;
}

void add(crate::SampleAudio& audio, std::uint64_t at, const std::vector<double>& signal) {
  for (std::size_t index = 0; index < signal.size() && at + index < audio.frames.size(); ++index) {
    auto& frame = audio.frames[at + index];
    frame.left += static_cast<float>(signal[index]);
    frame.right += static_cast<float>(signal[index]);
  }
}

std::size_t frames_of(double seconds) { return static_cast<std::size_t>(seconds * rate); }

// A 5 ms decaying noise burst.
std::vector<double> click(Noise& noise, double amplitude) {
  std::vector<double> signal(frames_of(0.005));
  for (std::size_t index = 0; index < signal.size(); ++index)
    signal[index] =
        amplitude * noise.next() * std::exp(-static_cast<double>(index) / (rate * 0.001));
  return signal;
}

// A decaying 55 Hz sine whose pitch drops from 150 Hz.
std::vector<double> kick() {
  std::vector<double> signal(frames_of(0.4));
  double phase = 0.0;
  for (std::size_t index = 0; index < signal.size(); ++index) {
    const double time = static_cast<double>(index) / rate;
    phase += two_pi * (55.0 + 95.0 * std::exp(-time / 0.02)) / rate;
    signal[index] = 0.9 * std::sin(phase) * std::exp(-time / 0.12);
  }
  return signal;
}

// A noise burst over a 200 Hz body.
std::vector<double> snare(Noise& noise) {
  std::vector<double> signal(frames_of(0.3));
  for (std::size_t index = 0; index < signal.size(); ++index) {
    const double time = static_cast<double>(index) / rate;
    signal[index] = 0.5 * std::sin(two_pi * 200.0 * time) * std::exp(-time / 0.08) +
                    0.6 * noise.next() * std::exp(-time / 0.06);
  }
  return signal;
}

// Short noise high-passed at 8 kHz.
std::vector<double> hat(Noise& noise) {
  std::vector<double> signal(frames_of(0.1));
  HighPass first(8'000.0);
  HighPass second(8'000.0);
  for (std::size_t index = 0; index < signal.size(); ++index) {
    const double time = static_cast<double>(index) / rate;
    signal[index] = 0.8 * second.process(first.process(noise.next())) * std::exp(-time / 0.015);
  }
  return signal;
}

std::vector<double> tone(double hz, double seconds, double amplitude) {
  std::vector<double> signal(frames_of(seconds));
  for (std::size_t index = 0; index < signal.size(); ++index)
    signal[index] = amplitude * std::sin(two_pi * hz * static_cast<double>(index) / rate);
  return signal;
}

bool normalised(const crate::ChopStarts& starts, const crate::SampleAudio& audio) {
  if (starts.empty() || starts.size() > crate::max_crate_chops || starts.front() != 0 ||
      starts.back() >= audio.frames.size())
    return false;
  for (std::size_t index = 1; index < starts.size(); ++index) {
    if (starts[index] < starts[index - 1] + merge_frames)
      return false;
  }
  return true;
}

bool near(std::uint64_t value, std::uint64_t expected) {
  const auto distance = value > expected ? value - expected : expected - value;
  return distance <= transient_tolerance;
}

void transients_land_on_clicks() {
  auto audio = silence(2.0);
  Noise noise;
  const std::vector<std::uint64_t> clicks{frames_of(0.25), frames_of(0.75), frames_of(1.25),
                                          frames_of(1.6)};
  for (const auto at : clicks)
    add(audio, at, click(noise, 0.8));
  const auto starts = crate::chop_transients(audio, 0.5F);
  REQUIRE_OK(starts, "transient chop succeeds");
  require(normalised(starts.value(), audio), "transient starts are normalised");
  REQUIRE_EQ(starts.value().size(), clicks.size() + 1, "one start per click plus frame 0");
  for (std::size_t index = 0; index < clicks.size(); ++index)
    require(near(starts.value()[index + 1], clicks[index]), "start lands within 2 ms of its click");
  const auto again = crate::chop_transients(audio, 0.5F);
  REQUIRE_OK(again, "chopping is deterministic");
  REQUIRE_EQ(again.value(), starts.value(), "chopping is deterministic");
  require(!crate::chop_transients(audio, 1.5F).has_value(), "sensitivity above 1 is refused");
  require(!crate::chop_transients(silence(0.0), 0.5F).has_value(), "an empty sample is refused");
}

void transient_cap_keeps_the_strongest() {
  auto audio = silence(4.0);
  Noise noise;
  // 37 clicks every 100 ms from 0.1 s; four of them quiet.
  std::vector<std::uint64_t> quiet;
  for (std::size_t index = 0; index < 37; ++index) {
    const auto at = frames_of(0.1 * static_cast<double>(index + 1));
    const bool soft = index % 9 == 4;
    if (soft)
      quiet.push_back(at);
    add(audio, at, click(noise, soft ? 0.1 : 0.9));
  }
  const auto starts = crate::chop_transients(audio, 1.0F);
  REQUIRE_OK(starts, "dense transient chop succeeds");
  REQUIRE_EQ(starts.value().size(), crate::max_crate_chops, "chops cap at 32");
  require(normalised(starts.value(), audio), "capped starts stay normalised");
  for (const auto at : quiet) {
    require(
        std::ranges::none_of(starts.value(), [at](std::uint64_t start) { return near(start, at); }),
        "the cap drops the quietest onsets");
  }
}

void grid_and_equal_are_exact() {
  const auto audio = silence(4.0); // two bars at 120 bpm
  const auto sixteenths = crate::chop_grid(audio, 120.0, crate::grid_division_sixteenth, 0);
  REQUIRE_OK(sixteenths, "two bars of sixteenths");
  REQUIRE_EQ(sixteenths.value().size(), 32, "two bars of sixteenths");
  for (std::size_t index = 0; index < 32; ++index)
    REQUIRE_EQ(sixteenths.value()[index], index * 6'000, "a sixteenth at 120 bpm is 6000 frames");

  // A pickup before the first beat, then eighths (12000 frames) from 13000.
  const auto eighths = crate::chop_grid(audio, 120.0, crate::grid_division_eighth, 13'000);
  REQUIRE_OK(eighths, "eighth grid succeeds");
  const crate::ChopStarts eighth_head{0, 1'000, 13'000, 25'000};
  require(std::equal(eighth_head.begin(), eighth_head.end(), eighths.value().begin()),
          "grid lines run back through the pickup and frame 0 is added");
  REQUIRE_EQ(eighths.value().size(), 17, "eighths run to the sample's end");
  REQUIRE_EQ(eighths.value().back(), 181'000, "eighths run to the sample's end");

  // 33 lines with frame 0 added: the earliest 32 are kept.
  const auto offset = crate::chop_grid(audio, 120.0, crate::grid_division_sixteenth, 1'000);
  REQUIRE_OK(offset, "the grid cap keeps the earliest lines");
  REQUIRE_EQ(offset.value().size(), 32, "the grid cap keeps the earliest lines");
  REQUIRE_EQ(offset.value()[1], 1'000, "the grid cap keeps the earliest lines");
  REQUIRE_EQ(offset.value().back(), 1'000 + 30 * 6'000, "the grid cap keeps the earliest lines");
  require(!crate::chop_grid(audio, 120.0, 3, 0).has_value(), "triplet divisions are refused");
  require(!crate::chop_grid(audio, 0.0, 4, 0).has_value(), "zero tempo is refused");
  require(!crate::chop_grid(audio, 120.0, 4, audio.frames.size()).has_value(),
          "a first beat outside the sample is refused");

  auto odd = silence(0.0);
  odd.frames.assign(100'000, StereoFrame{0.0F, 0.0F});
  const auto thirds = crate::chop_equal(odd, 3);
  REQUIRE_OK(thirds, "equal chops floor k * frames / n");
  REQUIRE_EQ(thirds.value(), (crate::ChopStarts{0, 33'333, 66'666}),
             "equal chops floor k * frames / n");
  REQUIRE_TRUE(!crate::chop_equal(odd, 0).has_value(), "equal chop counts are 1 to 32");
  REQUIRE_TRUE(!crate::chop_equal(odd, 33).has_value(), "equal chop counts are 1 to 32");
}

void close_starts_merge() {
  // 32 equal chops of 100 ms are 150 frames apart: they merge to 10 ms.
  const auto audio = silence(0.1);
  const auto merged = crate::chop_equal(audio, 32);
  REQUIRE_OK(merged, "short equal chop succeeds");
  crate::ChopStarts expected;
  for (std::uint64_t frame = 0; frame < audio.frames.size(); frame += 600)
    expected.push_back(frame);
  REQUIRE_EQ(merged.value(), expected, "starts closer than 10 ms merge into the earlier one");
}

void manual_edits_clamp() {
  const auto audio = silence(1.0);
  const crate::ChopStarts starts{0, 10'000, 20'000};
  REQUIRE_EQ(crate::add_chop_start(starts, 15'000, audio),
             (crate::ChopStarts{0, 10'000, 15'000, 20'000}), "add inserts in order");
  REQUIRE_EQ(crate::add_chop_start(starts, 10'100, audio), starts,
             "add next to a start is ignored");
  REQUIRE_EQ(crate::add_chop_start(starts, 999'999, audio),
             (crate::ChopStarts{0, 10'000, 20'000, audio.frames.size() - 1}),
             "add clamps into the sample");
  crate::ChopStarts full;
  for (std::uint64_t index = 0; index < crate::max_crate_chops; ++index)
    full.push_back(index * 1'000);
  REQUIRE_EQ(crate::add_chop_start(full, 40'000, audio), full, "add is ignored at 32 chops");

  REQUIRE_EQ(crate::move_chop_start(starts, 1, 12'000, audio),
             (crate::ChopStarts{0, 12'000, 20'000}), "move places a start");
  REQUIRE_EQ(crate::move_chop_start(starts, 1, 30'000, audio),
             (crate::ChopStarts{0, 20'000 - merge_frames, 20'000}),
             "move clamps before the next start");
  REQUIRE_EQ(crate::move_chop_start(starts, 1, 0, audio),
             (crate::ChopStarts{0, merge_frames, 20'000}), "move clamps after the previous start");
  REQUIRE_EQ(crate::move_chop_start(starts, 2, 999'999, audio),
             (crate::ChopStarts{0, 10'000, audio.frames.size() - 1}),
             "the last start clamps into the sample");
  REQUIRE_EQ(crate::move_chop_start(starts, 0, 500, audio), starts, "the first start stays at 0");

  REQUIRE_EQ(crate::remove_chop_start(starts, 1), (crate::ChopStarts{0, 20'000}), "remove a start");
  REQUIRE_EQ(crate::remove_chop_start(starts, 0), starts, "the first start cannot be removed");
  REQUIRE_EQ(crate::remove_chop_start(starts, 3), starts, "removing past the end is ignored");
}

struct Kit {
  crate::SampleAudio audio;
  crate::ChopStarts starts;
};

// Kick, snare, hat and a sustained tone, half a second (a second for the
// tone) apart.
Kit kit() {
  Kit result{silence(3.0), {0, frames_of(0.5), frames_of(1.0), frames_of(1.5)}};
  Noise noise;
  add(result.audio, result.starts[0], kick());
  add(result.audio, result.starts[1], snare(noise));
  add(result.audio, result.starts[2], hat(noise));
  add(result.audio, result.starts[3], tone(440.0, 1.0, 0.5));
  return result;
}

void drums_are_classified() {
  const auto drums = kit();
  const auto& audio = drums.audio;
  const auto& at = drums.starts;
  REQUIRE_EQ(crate::classify_chop(audio, at[0], at[1]), crate::ChopTag::kick, "kick is a kick");
  REQUIRE_EQ(crate::classify_chop(audio, at[1], at[2]), crate::ChopTag::snare, "snare is a snare");
  REQUIRE_EQ(crate::classify_chop(audio, at[2], at[3]), crate::ChopTag::hat, "hat is a hat");
  REQUIRE_EQ(crate::classify_chop(audio, at[3], audio.frames.size()), crate::ChopTag::other,
             "a sustained tone is other");
  REQUIRE_EQ(crate::classify_chop(silence(0.5), 0, frames_of(0.5)), crate::ChopTag::other,
             "silence is other");
  REQUIRE_EQ(crate::classify_chop(audio, 10, 10), crate::ChopTag::other, "an empty chop is other");
}

void root_is_detected() {
  auto audio = silence(1.0);
  add(audio, 0, tone(220.0, 1.0, 0.5));
  REQUIRE_EQ(crate::detect_root(audio), 57, "220 Hz is A3, key 57");
  REQUIRE_EQ(crate::detect_root(silence(1.0)), crate::default_root_key,
             "an unpitched sample roots at 60");
}

project::NativeRegion region(std::uint32_t slot, std::uint64_t start, std::string name) {
  return {slot, std::string(crate::crate_sample_media), start, "other", std::move(name)};
}

void rechop_keeps_slots() {
  const auto drums = kit();
  const auto& at = drums.starts;
  const auto first = crate::make_regions({}, at, drums.audio);
  REQUIRE_OK(first, "first chop makes regions");
  REQUIRE_EQ(first.value().regions.size(), 4, "first chop makes regions");
  const auto& made = first.value().regions;
  REQUIRE_EQ(made[0].slot, 0, "a first chop takes slots in order");
  REQUIRE_EQ(made[1].slot, 1, "a first chop takes slots in order");
  REQUIRE_EQ(made[2].slot, 2, "a first chop takes slots in order");
  REQUIRE_EQ(made[3].slot, 3, "a first chop takes slots in order");
  REQUIRE_EQ(made[0].tag, "kick", "regions are tagged");
  REQUIRE_EQ(made[1].tag, "snare", "regions are tagged");
  REQUIRE_EQ(made[2].tag, "hat", "regions are tagged");
  REQUIRE_EQ(made[3].tag, "other", "regions are tagged");
  REQUIRE_EQ(made[0].name, "Kick", "regions are named for their tag in media 'sample'");
  REQUIRE_EQ(made[3].name, "Chop", "regions are named for their tag in media 'sample'");
  REQUIRE_EQ(made[0].media, "sample", "regions are named for their tag in media 'sample'");
  require(first.value().freed_slots.empty(), "nothing is freed by a first chop");

  // The kick keeps slot 5, the renamed snare keeps slot 2 and its name; the
  // hat's slot 9 is freed; the new start takes the lowest free slot.
  const std::vector<project::NativeRegion> previous{
      region(5, at[0], "Kick"), region(2, at[1], "Crack"), region(9, at[2], "Hat")};
  const crate::ChopStarts again{at[0], at[1], at[3]};
  const auto rechop = crate::make_regions(previous, again, drums.audio);
  REQUIRE_OK(rechop, "re-chop succeeds");
  const auto& regions = rechop.value().regions;
  REQUIRE_EQ(regions.size(), 3, "unchanged starts keep their slots; new ones take the lowest free");
  REQUIRE_EQ(regions[0].slot, 5,
             "unchanged starts keep their slots; new ones take the lowest free");
  REQUIRE_EQ(regions[1].slot, 2,
             "unchanged starts keep their slots; new ones take the lowest free");
  REQUIRE_EQ(regions[2].slot, 0,
             "unchanged starts keep their slots; new ones take the lowest free");
  REQUIRE_EQ(regions[1].name, "Crack", "a renamed chop keeps its name and is re-tagged");
  REQUIRE_EQ(regions[0].name, "Kick", "a renamed chop keeps its name and is re-tagged");
  REQUIRE_EQ(regions[1].tag, "snare", "a renamed chop keeps its name and is re-tagged");
  REQUIRE_EQ(rechop.value().freed_slots, std::vector<std::uint32_t>{9},
             "the dropped slot is freed");
  for (std::size_t index = 0; index < regions.size(); ++index)
    REQUIRE_EQ(regions[index].start_frame, again[index], "regions follow the starts in order");

  require(!crate::make_regions({}, crate::ChopStarts{}, drums.audio).has_value(),
          "no starts are refused");
  require(!crate::make_regions({}, crate::ChopStarts{10, 20}, drums.audio).has_value(),
          "a first start after 0 is refused");
  require(!crate::make_regions({}, crate::ChopStarts{0, 20, 20}, drums.audio).has_value(),
          "repeated starts are refused");
}

void moved_regions_keep_their_slots() {
  const auto drums = kit();
  const auto& at = drums.starts;
  // Out of time order on purpose: the result is in time order.
  const std::vector<project::NativeRegion> regions{
      region(1, at[3], "Chop"), region(9, at[2], "Hat"), region(2, at[1], "Crack"),
      region(5, at[0], "Kick")};
  REQUIRE_EQ(crate::region_starts(regions), crate::ChopStarts(at.begin(), at.end()),
             "region starts are in time order");
  const auto moved = crate::move_region(regions, 2, frames_of(1.2), drums.audio);
  REQUIRE_EQ(moved.size(), 4, "a moved chop keeps its slot");
  REQUIRE_EQ(moved[2].slot, 9, "a moved chop keeps its slot");
  REQUIRE_EQ(moved[2].start_frame, frames_of(1.2), "a moved chop keeps its slot");
  REQUIRE_EQ(moved[2].tag, "other", "a moved chop with its tag's name is re-tagged and renamed");
  REQUIRE_EQ(moved[2].name, "Chop", "a moved chop with its tag's name is re-tagged and renamed");
  REQUIRE_EQ(moved[1].slot, 2, "the chop before keeps its slot and the user's name");
  REQUIRE_EQ(moved[1].name, "Crack", "the chop before keeps its slot and the user's name");
  REQUIRE_EQ(moved[0].slot, 5, "other chops are untouched");
  REQUIRE_EQ(moved[3].slot, 1, "other chops are untouched");
  REQUIRE_EQ(moved[3].start_frame, at[3], "other chops are untouched");
  const auto clamped = crate::move_region(regions, 2, frames_of(2.9), drums.audio);
  REQUIRE_EQ(clamped[2].start_frame, at[3] - merge_frames, "a move clamps before the next chop");
  const auto first = crate::move_region(regions, 0, frames_of(0.2), drums.audio);
  REQUIRE_EQ(first[0].start_frame, 0, "the first chop stays at frame 0");
  REQUIRE_EQ(first[0].slot, 5, "the first chop stays at frame 0");
}

void chop_to_midi_plays_pads_in_order() {
  // 120 BPM at 48 kHz: a quarter is 24 000 frames.
  constexpr std::uint64_t quarter_frames = rate / 2;
  constexpr auto quarter = oscillate::timeline::ticks_per_quarter;
  const std::vector<project::NativeRegion> regions{region(4, 0, "Kick"),
                                                   region(0, quarter_frames, "Snare"),
                                                   region(7, quarter_frames * 3 / 2, "Hat")};
  const auto clip = crate::chop_to_midi(regions, quarter_frames * 4, rate, 120.0);
  REQUIRE_OK(clip, "one note per chop");
  REQUIRE_EQ(clip.value().notes.size(), 3, "one note per chop");
  const auto& notes = clip.value().notes;
  REQUIRE_EQ(notes[0].start.value, 0,
             "the first chop is key 36 from the clip start to the next chop");
  REQUIRE_EQ(notes[0].end.value, quarter,
             "the first chop is key 36 from the clip start to the next chop");
  REQUIRE_EQ(notes[0].key, 36, "the first chop is key 36 from the clip start to the next chop");
  REQUIRE_EQ(notes[1].start.value, quarter,
             "the second chop is key 37 at its start in ticks, whatever its slot");
  REQUIRE_EQ(notes[1].end.value, quarter * 3 / 2,
             "the second chop is key 37 at its start in ticks, whatever its slot");
  REQUIRE_EQ(notes[1].key, 37,
             "the second chop is key 37 at its start in ticks, whatever its slot");
  REQUIRE_EQ(notes[2].start.value, quarter * 3 / 2, "the last chop lasts to the sample's end");
  REQUIRE_EQ(notes[2].end.value, 4 * quarter, "the last chop lasts to the sample's end");
  REQUIRE_EQ(notes[2].key, 38, "the last chop lasts to the sample's end");
  require(std::ranges::all_of(notes,
                              [](const project::MidiNote& note) {
                                return note.velocity == crate::chop_note_velocity;
                              }),
          "notes play at velocity 100");
  REQUIRE_EQ(clip.value().length.value, 4 * quarter, "an exact bar stays one bar");
  const auto longer = crate::chop_to_midi(regions, quarter_frames * 4 + 1, rate, 120.0);
  REQUIRE_OK(longer, "a sample just past a bar rounds up to two");
  REQUIRE_EQ(longer.value().length.value, 8 * quarter, "a sample just past a bar rounds up to two");
  // Half the tempo doubles every position.
  const auto slower = crate::chop_to_midi(regions, quarter_frames * 4, rate, 60.0);
  REQUIRE_OK(slower, "ticks follow the sample's tempo");
  REQUIRE_EQ(slower.value().notes[1].start.value, quarter / 2, "ticks follow the sample's tempo");

  require(!crate::chop_to_midi({}, quarter_frames, rate, 120.0).has_value(),
          "no chops are refused");
  require(!crate::chop_to_midi(regions, quarter_frames * 4, rate, 0.0).has_value(),
          "no tempo is refused");
  require(!crate::chop_to_midi(regions, quarter_frames * 4, 0, 120.0).has_value(),
          "no rate is refused");
  const std::vector<project::NativeRegion> unordered{region(0, quarter_frames, "A"),
                                                     region(1, 0, "B")};
  require(!crate::chop_to_midi(unordered, quarter_frames * 4, rate, 120.0).has_value(),
          "chops out of time order are refused");
  require(!crate::chop_to_midi(regions, quarter_frames, rate, 120.0).has_value(),
          "chops beyond the sample's end are refused");
}

} // namespace

int main() {
  try {
    transients_land_on_clicks();
    transient_cap_keeps_the_strongest();
    grid_and_equal_are_exact();
    close_starts_merge();
    manual_edits_clamp();
    drums_are_classified();
    root_is_detected();
    rechop_keeps_slots();
    moved_regions_keep_their_slots();
    chop_to_midi_plays_pads_in_order();
  } catch (const std::exception& error) {
    std::cerr << "crate chops: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
