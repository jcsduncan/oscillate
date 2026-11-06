#pragma once

#include "oscillate/clay_dsp/limits.hpp"
#include "oscillate/foundation/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Clay: Melodyne-class note editing of audio (ADR-0081). A take is one
// analysed audio clip: its notes, each a sinusoidal model (or, for
// percussive takes, a segment of the original), and everything needed to
// render edits as original - note + edited note.
namespace oscillate::clay_dsp {

enum class Mode : std::uint8_t { melodic, polyphonic, percussive };

// How a note sounds back: a bank of partials, or the original samples.
enum class NoteKind : std::uint8_t { sinusoidal, segment };

// Pitch in cents on the MIDI scale: 100 per semitone, A4 (440 Hz) = 6900.
inline constexpr double cents_per_semitone = 100.0;
inline constexpr double cents_per_octave = 1'200.0;
inline constexpr double reference_hz = 440.0;
inline constexpr double reference_cents = 6'900.0;

[[nodiscard]] double hz_to_cents(double hz) noexcept;
[[nodiscard]] double cents_to_hz(double cents) noexcept;

// The spectral envelope is kept as natural-log amplitudes on fixed bands,
// log-spaced from envelope_low_hz to envelope_high_hz (clipped at Nyquist
// when read), every envelope_stride hops.
inline constexpr std::size_t envelope_bands = 240;
inline constexpr double envelope_low_hz = 40.0;
inline constexpr double envelope_high_hz = 20'000.0;
inline constexpr std::size_t envelope_stride = 4;
// Fractional band index of a frequency, clamped to the band range.
[[nodiscard]] double envelope_band_of(double hz) noexcept;

// One partial at one hop: frequency (Hz), sinusoid amplitude (linear) and
// phase (radians) at the hop's centre frame.
struct PartialFrame {
  float frequency = 0.0F;
  float amplitude = 0.0F;
  float phase = 0.0F;
};

// A note. Hop k of the take is centred on frame k * take.hop; a note covers
// hops [first_hop, first_hop + hops()), which span [start, end).
struct Note {
  std::uint32_t id = 0;
  NoteKind kind = NoteKind::sinusoidal;
  std::int64_t start = 0; // first frame
  std::int64_t end = 0;   // one past the last frame
  std::int64_t first_hop = 0;
  std::vector<float> pitch;     // Hz per hop
  std::vector<float> amplitude; // linear per hop (sum of partial amplitudes, or RMS)
  // Amplitude-weighted median of the pitch, in cents.
  double centre = 0.0;
  // cents(t) = centre + drift(t) + modulation(t): drift is the slow part,
  // modulation the vibrato.
  std::vector<float> drift;
  std::vector<float> modulation;
  std::size_t partial_count = 0;
  std::vector<PartialFrame> partials; // hop-major: [hop * partial_count + h]
  // ceil(hops / envelope_stride) rows of envelope_bands, hop-major.
  std::vector<float> envelope;
  // Share of the note's energy the partials do not explain (0..1).
  float noise_share = 0.0F;
  // How much of the analysed mono note each channel holds.
  std::array<float, 2> pan{1.0F, 1.0F};

  [[nodiscard]] std::size_t hops() const noexcept { return pitch.size(); }
  [[nodiscard]] double mean_amplitude() const noexcept;
};

struct Take {
  Mode mode = Mode::melodic;
  std::uint32_t sample_rate = 0;
  std::int64_t frames = 0;
  std::int64_t hop = 0;
  std::vector<Note> notes; // sorted by start, then id
  std::uint32_t next_id = 1;

  [[nodiscard]] const Note* find(std::uint32_t id) const noexcept;
};

// One note's edit. Defaults change nothing. A duplicate is a new voice made
// from the note's model with its own edit; the note itself still plays.
struct NoteEdit {
  std::uint32_t note = 0;
  bool duplicate = false;
  double pitch_cents = 0.0;   // added to the centre
  double modulation = 1.0;    // scales the vibrato
  double drift = 1.0;         // scales the slow pitch drift
  double formant_cents = 0.0; // 0 keeps the envelope where it is
  double gain_db = 0.0;
  std::optional<std::int64_t> start{}; // new placement; the note's tracks stretch
  std::optional<std::int64_t> end{};

  [[nodiscard]] bool identity() const noexcept;
  bool operator==(const NoteEdit&) const = default;
};

// Bounds edits and placements are checked against: limits.hpp.

[[nodiscard]] foundation::Result<void> validate(const Take& take, std::span<const NoteEdit> edits);

// Splits a note at `frame` (strictly inside it) into two notes; the second
// gets a new id. Returns that id.
[[nodiscard]] foundation::Result<std::uint32_t> split_note(Take& take, std::uint32_t id,
                                                           std::int64_t frame);
// Joins two notes that touch or overlap (`second` starts within a hop of
// `first`'s end) into `first`.
[[nodiscard]] foundation::Result<void> join_notes(Take& take, std::uint32_t first,
                                                  std::uint32_t second);

// Recomputes centre, drift and modulation from the pitch curve.
void measure_pitch(Note& note, std::int64_t hop, std::uint32_t sample_rate);

} // namespace oscillate::clay_dsp
