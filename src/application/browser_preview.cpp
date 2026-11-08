#include "oscillate/application/browser_preview.hpp"
#include "oscillate/foundation/midi.hpp"

#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string>
#include <variant>

namespace oscillate::application {
namespace {

[[nodiscard]] std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Alphanumeric runs, lower-cased.
[[nodiscard]] std::vector<std::string> tokens(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (std::isalnum(c) != 0) {
      current.push_back(static_cast<char>(std::tolower(c)));
    } else if (!current.empty()) {
      out.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty())
    out.push_back(std::move(current));
  return out;
}

[[nodiscard]] bool contains_any(std::string_view text,
                                std::initializer_list<std::string_view> words) {
  return std::ranges::any_of(words,
                             [&](std::string_view word) { return text.find(word) != text.npos; });
}

[[nodiscard]] bool all_digits(std::string_view token) {
  return !token.empty() &&
         std::ranges::all_of(token, [](unsigned char c) { return std::isdigit(c) != 0; });
}

[[nodiscard]] std::optional<double> whole_number(std::string_view token) {
  if (!all_digits(token) || token.size() > 3 || token.front() == '0')
    return std::nullopt;
  double value = 0.0;
  for (const char c : token)
    value = value * 10.0 + (c - '0');
  return value;
}

struct Beat {
  double at;     // Beats from the phrase start.
  double length; // Beats.
  std::uint8_t key;
  std::uint8_t velocity;
};

} // namespace

PreviewStyle preview_style(const PresetWords& sound) noexcept {
  const std::string_view engine = sound.engine;
  const std::string_view category = sound.category;
  const std::string_view preset_name = sound.preset_name;
  if (engine == "pulse")
    return PreviewStyle::drums;
  if (engine == "knock")
    return PreviewStyle::percussion;
  if (engine == "torque")
    return PreviewStyle::bass;
  try {
    const auto words = lower(category) + " " + lower(preset_name);
    if (contains_any(words, {"bass", "reese", "sub", "acid", "wobble", "808"}))
      return PreviewStyle::bass;
    if (contains_any(words, {"lead", "pluck", "arp", "mallet", "bell", "seq", "harp", "marimba"}))
      return PreviewStyle::line;
    if (contains_any(words, {"texture", "drone", "atmos", "cloud", "ambient", "noise"}))
      return PreviewStyle::texture;
    if (engine == "drift" && !contains_any(words, {"pad", "keys", "string"}))
      return PreviewStyle::texture;
  } catch (...) {
    // Out of memory: fall back to a chord.
    return PreviewStyle::chord;
  }
  return PreviewStyle::chord;
}

std::vector<PreviewNote> preview_phrase(PreviewStyle style, double bpm, std::uint32_t sample_rate,
                                        std::uint8_t drum_first_key) {
  if (!std::isfinite(bpm))
    bpm = 120.0;
  bpm = std::clamp(bpm, 60.0, 180.0);
  std::vector<Beat> beats;
  const auto drum = [&](int pad) {
    return static_cast<std::uint8_t>(std::clamp(drum_first_key + pad, 0, foundation::max_midi_key));
  };
  switch (style) {
  case PreviewStyle::drums: {
    // One bar: kick on 1, the "and" of 2 and 3; snare on 2 and 4; closed
    // hats on the eighths, an open hat closing the bar.
    for (const double at : {0.0, 1.5, 2.0})
      beats.push_back({at, 0.25, drum(0), 118});
    for (const double at : {1.0, 3.0})
      beats.push_back({at, 0.25, drum(2), 110});
    for (int eighth = 0; eighth < 7; ++eighth)
      beats.push_back(
          {eighth * 0.5, 0.25, drum(8), eighth % 2 == 0 ? std::uint8_t{92} : std::uint8_t{70}});
    beats.push_back({3.5, 0.5, drum(9), 88});
    break;
  }
  case PreviewStyle::percussion: {
    // A minimal top loop, one bar played twice: a row of sixteenths per
    // pad, x a hit, X an accent and ? a ghost note.
    constexpr std::size_t steps_per_bar = 16;
    constexpr double beats_per_step = 0.25;
    constexpr double bar_beats = static_cast<double>(steps_per_bar) * beats_per_step;
    // One row per Knock pad.
    constexpr std::array<std::string_view, native_dsp::synth::knock::pad_count> loop{
        "......x.....?...", "...x.......x..x.", "x.?.x.x.?.x.x.?x", "..x...x...x...x.",
        "..........X.....", "................", "x.X.x.X.x.X.x.Xx", "...............?"};
    for (int bar = 0; bar < 2; ++bar) {
      for (std::size_t pad = 0; pad < loop.size(); ++pad) {
        for (std::size_t step = 0; step < steps_per_bar && step < loop[pad].size(); ++step) {
          const char hit = loop[pad][step];
          if (hit == '.')
            continue;
          const std::uint8_t velocity = hit == 'X'   ? std::uint8_t{118}
                                        : hit == '?' ? std::uint8_t{58}
                                                     : std::uint8_t{92};
          beats.push_back({bar * bar_beats + static_cast<double>(step) * beats_per_step,
                           beats_per_step, drum(static_cast<int>(pad)), velocity});
        }
      }
    }
    break;
  }
  case PreviewStyle::bass:
    beats = {{0.0, 0.5, 36, 112},  {0.75, 0.25, 36, 92}, {1.5, 0.25, 48, 100}, {2.0, 0.5, 43, 108},
             {2.75, 0.25, 39, 92}, {3.0, 0.75, 41, 104}, {4.0, 0.5, 36, 112},  {4.75, 0.25, 36, 92},
             {5.5, 0.25, 48, 100}, {6.0, 1.5, 34, 104}};
    break;
  case PreviewStyle::line:
    beats = {{0.0, 0.5, 60, 104}, {0.5, 0.5, 63, 92}, {1.0, 0.5, 67, 100}, {1.5, 0.5, 70, 92},
             {2.0, 1.0, 72, 108}, {3.0, 0.5, 70, 90}, {3.5, 0.5, 67, 90},  {4.0, 2.0, 65, 100}};
    break;
  case PreviewStyle::chord: {
    // Cmaj9, then Am9, each lightly strummed upwards.
    constexpr std::array first{48, 55, 59, 62, 64};
    constexpr std::array second{45, 52, 55, 59, 60};
    constexpr double strum = 0.03;
    for (std::size_t i = 0; i < first.size(); ++i) {
      const double offset = static_cast<double>(i) * strum;
      beats.push_back({offset, 3.5 - offset, static_cast<std::uint8_t>(first[i]), 96});
    }
    for (std::size_t i = 0; i < second.size(); ++i) {
      const double offset = static_cast<double>(i) * strum;
      beats.push_back({4.0 + offset, 3.5 - offset, static_cast<std::uint8_t>(second[i]), 92});
    }
    break;
  }
  case PreviewStyle::texture:
    for (const int key : {48, 55, 62, 67})
      beats.push_back({0.0, 6.0, static_cast<std::uint8_t>(key), 90});
    break;
  }
  const double frames_per_beat = 60.0 / bpm * sample_rate;
  std::vector<PreviewNote> notes;
  notes.reserve(beats.size());
  for (const auto& beat : beats) {
    notes.push_back({static_cast<std::uint64_t>(std::llround(beat.at * frames_per_beat)),
                     std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
                                                    std::llround(beat.length * frames_per_beat))),
                     beat.key, beat.velocity});
  }
  std::ranges::stable_sort(notes, {}, &PreviewNote::start_frame);
  return notes;
}

std::optional<double> tempo_from_file_name(std::string_view stem) noexcept {
  try {
    const auto parts = tokens(stem);
    const auto in_range = [](double value) { return value >= 40.0 && value <= 250.0; };
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const auto& token = parts[i];
      // "128bpm".
      if (token.size() > 3 && token.ends_with("bpm")) {
        const auto value = whole_number(std::string_view(token).substr(0, token.size() - 3));
        if (value && in_range(*value))
          return value;
      }
      if (token == "bpm") {
        // "128 BPM", or "BPM 128".
        if (i > 0) {
          const auto value = whole_number(parts[i - 1]);
          if (value && in_range(*value))
            return value;
        }
        if (i + 1 < parts.size()) {
          const auto value = whole_number(parts[i + 1]);
          if (value && in_range(*value))
            return value;
        }
      }
    }
    std::optional<double> lone;
    for (const auto& token : parts) {
      const auto value = whole_number(token);
      if (!value || *value < 60.0 || *value > 200.0)
        continue;
      if (lone)
        return std::nullopt; // Two candidates: ambiguous.
      lone = value;
    }
    return lone;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> tempo_from_loop_length(std::uint64_t frames, std::uint32_t sample_rate,
                                             double preferred) noexcept {
  if (sample_rate == 0)
    return std::nullopt;
  const double seconds = static_cast<double>(frames) / sample_rate;
  if (seconds < 1.0)
    return std::nullopt;
  std::optional<double> best;
  for (const double beat_count : {4.0, 8.0, 16.0, 32.0, 64.0}) {
    const double bpm = beat_count * 60.0 / seconds;
    if (bpm < 70.0 || bpm > 180.0)
      continue;
    const double step = std::round(bpm * 2.0) / 2.0;
    if (std::abs(bpm - step) > step * 0.001)
      continue;
    if (!best || std::abs(bpm - preferred) < std::abs(*best - preferred))
      best = bpm;
  }
  return best;
}

std::optional<std::int64_t> next_beat_sample(const timeline::TempoMap& tempo, std::int64_t now,
                                             std::uint32_t sample_rate) {
  const auto tick = tempo.sample_to_tick({now}, sample_rate);
  if (!tick)
    return std::nullopt;
  const auto quarter = timeline::ticks_per_quarter;
  const auto floor_beat = tick.value().value / quarter * quarter;
  const auto at_floor = tempo.tick_to_sample({floor_beat}, sample_rate);
  if (at_floor && std::abs(at_floor.value().value - now) <= 1)
    return now;
  const auto next = tempo.tick_to_sample({floor_beat + quarter}, sample_rate);
  if (!next)
    return std::nullopt;
  return std::max(next.value().value, now);
}

std::optional<KeyMatch> key_match(const project::SourceKeyReview& review,
                                  double current_pitch_semitones, project::MusicalKey song,
                                  double min_confidence) noexcept {
  std::optional<project::MusicalKey> source = review.correction;
  if (!source && review.detected && review.confidence >= min_confidence)
    source = review.detected;
  if (!source || source->tonic >= foundation::semitones_per_octave ||
      song.tonic >= foundation::semitones_per_octave || !std::isfinite(current_pitch_semitones))
    return std::nullopt;
  const auto pitch = static_cast<int>(std::lround(current_pitch_semitones));
  const int heard = foundation::pitch_class(source->tonic + pitch);
  int target = song.tonic;
  if (source->mode != song.mode)
    target = source->mode == project::KeyMode::minor
                 ? (song.tonic + 9) % foundation::semitones_per_octave  // Relative minor.
                 : (song.tonic + 3) % foundation::semitones_per_octave; // Relative major.
  const int up = foundation::pitch_class(target - heard);
  const int shift = up <= 5 ? up : up - foundation::semitones_per_octave;
  if (shift == 0 || std::abs(current_pitch_semitones + shift) > project::max_clip_pitch_semitones)
    return std::nullopt;
  return KeyMatch{{static_cast<std::uint8_t>(heard), source->mode},
                  {static_cast<std::uint8_t>(target), source->mode},
                  shift};
}

namespace {

[[nodiscard]] bool kick_words(std::string_view text) {
  const auto parts = tokens(text);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto& token = parts[i];
    if (token.starts_with("kick") || token == "kck" || token == "bd" || token == "bassdrum")
      return true;
    if (token == "bass" && i + 1 < parts.size() && parts[i + 1].starts_with("drum"))
      return true;
  }
  return false;
}

} // namespace

bool looks_like_kick(const project::Track& track, std::string_view sound_name) noexcept {
  if (track.kind == project::TrackKind::group_bus || track.kind == project::TrackKind::return_bus)
    return false;
  try {
    if (kick_words(track.name) || kick_words(sound_name))
      return true;
    if (std::ranges::any_of(track.clips,
                            [](const project::Clip& clip) { return kick_words(clip.name); }))
      return true;
    // A Pulse kit that only ever plays its two kick pads.
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    if (patch == nullptr || patch->engine != "pulse")
      return false;
    const auto first = native_dsp::synth::pulse::first_key;
    bool any = false;
    for (const auto& clip : track.midi_clips) {
      for (const auto& note : clip.notes) {
        if (note.key != first && note.key != first + 1)
          return false;
        any = true;
      }
    }
    return any;
  } catch (...) {
    return false;
  }
}

bool keys_a_sidechain(const project::Track& track) noexcept {
  return std::ranges::any_of(track.sends, [](const project::SendRoute& send) {
    return send.purpose == project::SendPurpose::sidechain;
  });
}

} // namespace oscillate::application
