#include "oscillate/clay_dsp/analysis.hpp"
#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/clay_dsp/render.hpp"
#include "oscillate/clay_dsp/take.hpp"
#include "oscillate/foundation/midi.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include "signals.hpp"
#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {

using namespace oscillate::clay_dsp;
namespace sig = clay_signals;

using oscillate::test::require;

// Tolerances the oracles hold the analysis and render to.
constexpr double onset_tolerance_seconds = 0.010;
constexpr double centre_tolerance_cents = 5.0;
constexpr double chord_tolerance_cents = 30.0;
constexpr double ghost_db = -24.0;
constexpr double untouched_db = 1.5;
constexpr double removed_db = -20.0;
constexpr double formant_tolerance = 0.10;
constexpr double envelope_tolerance_db = 2.0;
constexpr double preserved_top_hz = 3'500.0;
constexpr double length_tolerance_seconds = 0.015;

[[nodiscard]] double ratio_db(double db) { return std::pow(10.0, db / 20.0); }

[[nodiscard]] Take analyse_or_throw(std::span<const float> mono, Mode mode) {
  AnalysisOptions options;
  options.mode = mode;
  auto take = analyse({mono, {}, sig::rate}, options);
  REQUIRE_OK(take, take.has_value() ? "" : "analysis failed: " + take.error().message);
  return std::move(take).value();
}

[[nodiscard]] std::vector<float> render_all(const Take& take, std::span<const float> mono,
                                            std::span<const NoteEdit> edits) {
  auto renderer = Renderer::create(take, mono, {}, edits);
  REQUIRE_OK(renderer, renderer.has_value() ? "" : "render: " + renderer.error().message);
  std::vector<float> out(static_cast<std::size_t>(renderer.value().length()), 0.0F);
  renderer.value().render(0, out, {});
  return out;
}

[[nodiscard]] double seconds(std::int64_t frames) {
  return static_cast<double>(frames) / sig::rate;
}

[[nodiscard]] const Note& longest(const Take& take) {
  require(!take.notes.empty(), "no notes found");
  return *std::ranges::max_element(take.notes, {}, [](const Note& n) { return n.end - n.start; });
}

// Hann-windowed amplitude of one frequency around `centre`.
[[nodiscard]] double amplitude_at(std::span<const float> x, std::size_t centre, double hz) {
  constexpr std::size_t window = 8'192;
  std::complex<double> sum{};
  double weight = 0.0;
  for (std::size_t n = 0; n < window; ++n) {
    const auto at = centre - window / 2 + n;
    const double w = 0.5 - 0.5 * std::cos(sig::two_pi * static_cast<double>(n) / window);
    weight += w;
    if (at < x.size())
      sum += static_cast<double>(x[at]) * w *
             std::polar(1.0, -sig::two_pi * hz * static_cast<double>(n) / sig::rate);
  }
  return 2.0 * std::abs(sum) / weight;
}

// ---- melodic ------------------------------------------------------------

[[nodiscard]] std::vector<float> phrase() {
  const std::vector<sig::SungNote> notes{
      {0.10, 0.50, 5'700.0, 30.0},
      {0.58, 1.00, 6'000.0, 0.0, 5.5, 0.06, 6'400.0},
      {1.00, 1.40, 6'400.0},
      {1.50, 1.80, 6'200.0},
  };
  return sig::sing(notes, 2.0);
}

void melodic_finds_sung_notes() {
  const auto audio = phrase();
  const auto take = analyse_or_throw(audio, Mode::melodic);
  const std::array<std::pair<double, double>, 4> truth{
      {{0.10, 5'700.0}, {0.58, 6'000.0}, {1.00, 6'400.0}, {1.50, 6'200.0}}};
  std::vector<const Note*> notes;
  for (const auto& n : take.notes) {
    if (seconds(n.end - n.start) > 0.1)
      notes.push_back(&n);
  }
  REQUIRE_EQ(notes.size(), truth.size(), "four sung notes, found " + std::to_string(notes.size()));
  for (std::size_t i = 0; i < truth.size(); ++i) {
    const auto& n = *notes[i];
    REQUIRE_NEAR(seconds(n.start), truth[i].first, onset_tolerance_seconds,
                 "note " + std::to_string(i) + " starts at " + std::to_string(seconds(n.start)) +
                     ", expected " + std::to_string(truth[i].first));
    REQUIRE_NEAR(n.centre, truth[i].second, centre_tolerance_cents,
                 "note " + std::to_string(i) + " centre " + std::to_string(n.centre));
  }
  // The vibrato (30 cents at 5.5 Hz) is the first note's modulation.
  const auto& first = *notes[0];
  const auto from = first.hops() / 5;
  const auto to = first.hops() - first.hops() / 5;
  double square = 0.0;
  for (auto k = from; k < to; ++k)
    square += static_cast<double>(first.modulation[k]) * first.modulation[k];
  const double depth = std::sqrt(2.0 * square / static_cast<double>(to - from));
  require(depth > 22.0 && depth < 36.0, "vibrato depth " + std::to_string(depth));
  REQUIRE_GT(first.partial_count, 10, "the voice is mostly partials");
  REQUIRE_LT(first.noise_share, 0.2F, "the voice is mostly partials");
}

// ---- polyphonic ---------------------------------------------------------

constexpr std::array<double, 4> chord_cents{4'500.0, 5'500.0, 6'000.0, 7'100.0}; // A2 G3 C4 B4

[[nodiscard]] std::vector<float> chord(double inharmonicity) {
  std::vector<float> out(sig::frames(1.5), 0.0F);
  for (const double cents : chord_cents)
    sig::add_tone(out, sig::cents_to_hz(cents), 0.1, 1.3, inharmonicity);
  return out;
}

void check_chord(const Take& take) {
  std::vector<double> levels;
  std::vector<bool> real(take.notes.size(), false);
  for (const double cents : chord_cents) {
    bool found = false;
    for (std::size_t i = 0; i < take.notes.size(); ++i) {
      const auto& n = take.notes[i];
      if (std::abs(n.centre - cents) <= chord_tolerance_cents && seconds(n.end - n.start) > 0.8) {
        found = true;
        real[i] = true;
        levels.push_back(n.mean_amplitude());
      }
    }
    require(found, "chord note " + std::to_string(cents) + " not found");
  }
  std::ranges::sort(levels);
  const double reference = levels[levels.size() / 2];
  for (std::size_t i = 0; i < take.notes.size(); ++i) {
    if (!real[i])
      REQUIRE_LT(take.notes[i].mean_amplitude(), reference * ratio_db(ghost_db),
                 "ghost note at " + std::to_string(take.notes[i].centre) + " cents, level " +
                     std::to_string(take.notes[i].mean_amplitude()) + " against " +
                     std::to_string(reference));
  }
}

void polyphonic_finds_the_chord() { check_chord(analyse_or_throw(chord(0.0), Mode::polyphonic)); }

void polyphonic_finds_a_stretched_chord() {
  constexpr double string_inharmonicity = 4e-5;
  check_chord(analyse_or_throw(chord(string_inharmonicity), Mode::polyphonic));
}

// Exactly the notes played: one note per pitch, unbroken over `held`
// seconds from `from`, nothing else longer than a fragment, and partials
// that explain the notes.
constexpr double fragment_seconds = 0.05;
constexpr double held_share = 0.9;
constexpr float clean_noise_share = 0.2F;

void check_exact_notes(const Take& take, std::span<const double> cents, double from, double held,
                       const std::string& what) {
  std::vector<bool> played(take.notes.size(), false);
  std::string listing;
  for (const auto& n : take.notes)
    listing += " " + std::to_string(static_cast<int>(std::lround(n.centre))) + "@" +
               std::to_string(seconds(n.start)).substr(0, 5) + "-" +
               std::to_string(seconds(n.end)).substr(0, 5);
  for (const double c : cents) {
    int count = 0;
    for (std::size_t i = 0; i < take.notes.size(); ++i) {
      const auto& n = take.notes[i];
      if (std::abs(n.centre - c) > chord_tolerance_cents ||
          seconds(n.end - n.start) <= fragment_seconds)
        continue;
      ++count;
      played[i] = true;
      require(seconds(n.start) <= from + (1.0 - held_share) * held &&
                  seconds(n.end) >= from + held_share * held,
              what + ": note " + std::to_string(c) + " is not held whole:" + listing);
      REQUIRE_LT(n.noise_share, clean_noise_share,
                 what + ": note " + std::to_string(c) + " noise share " +
                     std::to_string(n.noise_share));
    }
    REQUIRE_EQ(count, 1,
               what + ": note " + std::to_string(c) + " found " + std::to_string(count) +
                   " times:" + listing);
  }
  for (std::size_t i = 0; i < take.notes.size(); ++i)
    require(played[i] || seconds(take.notes[i].end - take.notes[i].start) <= fragment_seconds,
            what + ": an extra note:" + listing);
}

// The Notes tab's fixture: an Am7 stab (A3 C4 E4 G4) struck together, eight
// in-phase harmonics each, decaying.
void polyphonic_finds_a_struck_stab() {
  constexpr std::array<double, 4> stab_cents{5'700.0, 6'000.0, 6'400.0, 6'700.0};
  constexpr double length = 1.6;
  std::vector<float> audio(sig::frames(length + 0.12), 0.0F);
  for (std::size_t k = 0; k < sig::frames(length); ++k) {
    const double t = static_cast<double>(k) / sig::rate;
    const double edge = std::min(1.0, std::min(t / 0.01, (length - t) / 0.05));
    double x = 0.0;
    for (const double c : stab_cents)
      for (int h = 1; h <= 8; ++h)
        x += std::sin(sig::two_pi * sig::cents_to_hz(c) * h * t) / h;
    audio[k] = static_cast<float>(0.09 * edge * std::exp(-0.8 * t) * x);
  }
  const auto take = analyse_or_throw(audio, Mode::polyphonic);
  check_exact_notes(take, stab_cents, 0.0, length, "stab");
}

// ---- rendering ----------------------------------------------------------

void unedited_render_is_the_original() {
  const auto audio = phrase();
  const auto take = analyse_or_throw(audio, Mode::melodic);
  REQUIRE_EQ(render_all(take, audio, {}), audio, "no edits renders the original");
  std::vector<NoteEdit> edits{NoteEdit{take.notes.front().id}};
  REQUIRE_EQ(render_all(take, audio, edits), audio, "an identity edit renders the original");
  // An edited note changes nothing outside its reach.
  NoteEdit shift{take.notes.back().id};
  shift.pitch_cents = 200.0;
  const auto out = render_all(take, audio, std::span(&shift, 1));
  const auto edge = take.notes.back().start - static_cast<std::int64_t>(sig::frames(0.05));
  for (std::size_t i = 0; i < static_cast<std::size_t>(edge); ++i)
    REQUIRE_EQ(out[i], audio[i], "audio before the edited note is untouched");
}

void shifted_note_sounds_at_its_new_pitch() {
  const std::vector<sig::SungNote> notes{{0.10, 0.70, 6'000.0}};
  const auto audio = sig::sing(notes, 0.9);
  const auto take = analyse_or_throw(audio, Mode::melodic);
  NoteEdit edit{longest(take).id};
  edit.pitch_cents = 300.0;
  const auto out = render_all(take, audio, std::span(&edit, 1));
  const auto again = analyse_or_throw(out, Mode::melodic);
  REQUIRE_NEAR(longest(again).centre, 6'300.0, centre_tolerance_cents,
               "shifted note at " + std::to_string(longest(again).centre));
}

void shifted_chord_note_leaves_the_others() {
  const auto audio = chord(0.0);
  const auto take = analyse_or_throw(audio, Mode::polyphonic);
  const Note* c4 = nullptr;
  for (const auto& n : take.notes) {
    if (std::abs(n.centre - chord_cents[2]) <= chord_tolerance_cents)
      c4 = &n;
  }
  REQUIRE_NE(c4, nullptr, "C4 found");
  NoteEdit edit{c4->id};
  edit.pitch_cents = 300.0;
  const auto out = render_all(take, audio, std::span(&edit, 1));
  const auto centre = sig::frames(0.7);
  const double old_hz = sig::cents_to_hz(chord_cents[2]);
  const double new_hz = sig::cents_to_hz(chord_cents[2] + 300.0);
  const auto near_moved = [&](double hz) {
    for (int h = 1; h <= 20; ++h) {
      if (std::abs(hz - h * old_hz) < 40.0 || std::abs(hz - h * new_hz) < 40.0)
        return true;
    }
    return false;
  };
  for (const double cents : {chord_cents[0], chord_cents[1], chord_cents[3]}) {
    for (int h = 1; h <= 6; ++h) {
      const double hz = h * sig::cents_to_hz(cents);
      if (near_moved(hz))
        continue;
      const double before = amplitude_at(audio, centre, hz);
      const double after = amplitude_at(out, centre, hz);
      REQUIRE_LE(std::abs(20.0 * std::log10(after / before)), untouched_db,
                 "partial at " + std::to_string(hz) + " Hz moved");
    }
  }
  REQUIRE_LT(amplitude_at(out, centre, old_hz),
             amplitude_at(audio, centre, old_hz) * ratio_db(removed_db), "the old C4 is gone");
  const auto again = analyse_or_throw(out, Mode::polyphonic);
  bool moved = false;
  for (const auto& n : again.notes)
    moved = moved || std::abs(n.centre - (chord_cents[2] + 300.0)) <= centre_tolerance_cents;
  require(moved, "E-flat 4 sounds");
}

// The true spectral envelope (Röbel & Rodet 2005: iterated cepstral
// smoothing that rides the harmonic peaks) of the Hann window centred at
// `centre`, for a sound with fundamental `f0`, as natural-log amplitudes at
// `frequencies`: the formant oracle, through Clay's own estimator.
[[nodiscard]] std::vector<double> spectral_envelope(std::span<const float> signal,
                                                    std::uint32_t sample_rate, std::int64_t centre,
                                                    double f0,
                                                    std::span<const double> frequencies) {
  constexpr double envelope_periods = 4.0;
  namespace detail = oscillate::clay_dsp::detail;
  const auto size = detail::fft_size_at_least(envelope_periods * sample_rate / std::max(1.0, f0));
  detail::FrameAnalyser analyser(size);
  detail::EnvelopeEstimator estimator(size);
  analyser.analyse(signal, centre);
  std::vector<double> bins(size / 2 + 1);
  estimator.estimate(analyser.magnitude(), f0, sample_rate, bins);
  const double bin_hz = static_cast<double>(sample_rate) / static_cast<double>(size);
  std::vector<double> out;
  out.reserve(frequencies.size());
  for (const double hz : frequencies)
    out.push_back(detail::read_bins(bins, hz, bin_hz));
  return out;
}

[[nodiscard]] std::vector<double> formant_peaks(std::span<const float> x, std::size_t centre,
                                                double f0, double scale) {
  constexpr double step = 5.0;
  constexpr double top = 4'500.0;
  std::vector<double> grid;
  for (double hz = step; hz <= top; hz += step)
    grid.push_back(hz);
  const auto envelope =
      spectral_envelope(x, sig::rate, static_cast<std::int64_t>(centre), f0, grid);
  // Search windows around each formant, scaled with any formant shift.
  const std::array<std::pair<double, double>, 3> windows{
      {{550.0, 900.0}, {1'000.0, 1'500.0}, {2'300.0, 3'000.0}}};
  std::vector<double> peaks;
  for (const auto& [lo, hi] : windows) {
    double best = -1e300;
    double at = 0.0;
    for (std::size_t i = 0; i < grid.size(); ++i) {
      if (grid[i] >= lo * scale && grid[i] <= hi * scale && envelope[i] > best) {
        best = envelope[i];
        at = grid[i];
      }
    }
    peaks.push_back(at);
  }
  return peaks;
}

void formants_stay_unless_shifted() {
  const std::vector<sig::SungNote> notes{{0.10, 0.80, 5'100.0}}; // about 156 Hz
  const auto audio = sig::sing(notes, 1.0);
  const auto take = analyse_or_throw(audio, Mode::melodic);
  const auto& note = longest(take);
  const auto centre = sig::frames(0.45);
  const double source_hz = sig::cents_to_hz(notes.front().cents);
  // The true envelope finds the vowel's formants.
  const auto peaks = formant_peaks(audio, centre, cents_to_hz(note.centre), 1.0);
  for (std::size_t i = 0; i < peaks.size(); ++i)
    REQUIRE_NEAR(peaks[i], sig::vowel_a[i].hz, formant_tolerance * sig::vowel_a[i].hz,
                 "formant " + std::to_string(i) + " found at " + std::to_string(peaks[i]));
  const double up = std::exp2(500.0 / 1'200.0);
  const auto check_harmonics = [&](std::span<const float> out, const auto& expected,
                                   const std::string& what) {
    for (int h = 1; h * source_hz * up < preserved_top_hz; ++h) {
      const double hz = h * source_hz * up;
      const double error = 20.0 * std::log10(amplitude_at(out, centre, hz) / expected(h, hz));
      REQUIRE_LE(std::abs(error), envelope_tolerance_db,
                 what + ": harmonic " + std::to_string(h) + " at " + std::to_string(hz) +
                     " Hz is " + std::to_string(error) + " dB off");
    }
  };
  // Shifted up a fourth with the envelope kept: every new harmonic sits on
  // the source's own envelope (its 1/h tilt, f0 / f, through the resonators).
  NoteEdit kept{note.id};
  kept.pitch_cents = 500.0;
  const auto preserved = render_all(take, audio, std::span(&kept, 1));
  check_harmonics(
      preserved,
      [&](int, double hz) {
        return sig::default_level * source_hz / hz * sig::formant_gain(hz, sig::vowel_a);
      },
      "kept envelope");
  // Shifted with the formants by the same amount: harmonic h is as loud as
  // the original's harmonic h, because the whole spectrum moved together.
  NoteEdit moved = kept;
  moved.formant_cents = 500.0;
  const auto shifted = render_all(take, audio, std::span(&moved, 1));
  check_harmonics(
      shifted, [&](int h, double) { return amplitude_at(audio, centre, h * source_hz); },
      "moved envelope");
}

void stretched_note_keeps_its_pitch() {
  const std::vector<sig::SungNote> notes{{0.10, 0.50, 6'000.0}};
  const auto audio = sig::sing(notes, 1.2);
  const auto take = analyse_or_throw(audio, Mode::melodic);
  const auto& note = longest(take);
  constexpr double stretch = 1.5;
  NoteEdit edit{note.id};
  edit.start = note.start;
  edit.end = note.start + static_cast<std::int64_t>(
                              std::llround(stretch * static_cast<double>(note.end - note.start)));
  const auto out = render_all(take, audio, std::span(&edit, 1));
  const auto again = analyse_or_throw(out, Mode::melodic);
  const auto& stretched = longest(again);
  REQUIRE_NEAR(seconds(stretched.end - stretched.start), seconds(*edit.end - *edit.start),
               length_tolerance_seconds,
               "stretched length " + std::to_string(seconds(stretched.end - stretched.start)));
  REQUIRE_NEAR(stretched.centre, 6'000.0, centre_tolerance_cents,
               "stretched pitch " + std::to_string(stretched.centre));
}

void renders_are_block_split_invariant() {
  const auto audio = chord(0.0);
  const auto take = analyse_or_throw(audio, Mode::polyphonic);
  std::vector<NoteEdit> edits;
  NoteEdit shift{take.notes.front().id};
  shift.pitch_cents = -150.0;
  shift.formant_cents = 200.0;
  edits.push_back(shift);
  NoteEdit moved{take.notes.back().id};
  moved.start = take.notes.back().start + static_cast<std::int64_t>(sig::frames(0.05));
  moved.end = take.notes.back().end + static_cast<std::int64_t>(sig::frames(0.2));
  edits.push_back(moved);
  NoteEdit copy{take.notes.back().id};
  copy.duplicate = true;
  copy.pitch_cents = 700.0;
  edits.push_back(copy);
  auto renderer = Renderer::create(take, audio, {}, edits);
  REQUIRE_OK(renderer, "render");
  const auto length = static_cast<std::size_t>(renderer.value().length());
  std::vector<float> whole(length);
  renderer.value().render(0, whole, {});
  std::vector<float> pieces(length);
  std::uint32_t state = 12'345;
  std::size_t at = 0;
  while (at < length) {
    state = state * 1'664'525U + 1'013'904'223U;
    const auto block = std::min<std::size_t>(length - at, 1 + state % 3'000);
    renderer.value().render(static_cast<std::int64_t>(at), std::span(pieces).subspan(at, block),
                            {});
    at += block;
  }
  REQUIRE_EQ(whole, pieces, "every block split renders the same samples");
}

void analysis_is_deterministic() {
  const auto audio = phrase();
  const auto a = analyse_or_throw(audio, Mode::melodic);
  const auto b = analyse_or_throw(audio, Mode::melodic);
  REQUIRE_EQ(a.notes.size(), b.notes.size(), "same notes");
  for (std::size_t i = 0; i < a.notes.size(); ++i) {
    require(a.notes[i].start == b.notes[i].start && a.notes[i].centre == b.notes[i].centre &&
                a.notes[i].pitch == b.notes[i].pitch,
            "same note " + std::to_string(i));
    for (std::size_t p = 0; p < a.notes[i].partials.size(); ++p)
      REQUIRE_EQ(a.notes[i].partials[p].amplitude, b.notes[i].partials[p].amplitude,
                 "same partials");
  }
}

void percussive_hits_become_notes() {
  std::vector<float> audio(sig::frames(1.4), 0.0F);
  const std::array<double, 4> times{0.1, 0.4, 0.7, 1.0};
  const std::array<double, 4> pitches{200.0, 300.0, 400.0, 500.0};
  for (std::size_t i = 0; i < times.size(); ++i)
    sig::add_hit(audio, times[i], pitches[i], 0.04, static_cast<std::uint32_t>(i + 1));
  const auto take = analyse_or_throw(audio, Mode::percussive);
  REQUIRE_EQ(take.notes.size(), times.size(),
             "four hits, found " + std::to_string(take.notes.size()));
  for (std::size_t i = 0; i < times.size(); ++i) {
    REQUIRE_NEAR(seconds(take.notes[i].start), times[i], onset_tolerance_seconds,
                 "hit " + std::to_string(i) + " at " +
                     std::to_string(seconds(take.notes[i].start)));
    REQUIRE_NEAR(take.notes[i].centre, hz_to_cents(pitches[i]), 50.0,
                 "hit " + std::to_string(i) + " pitch");
  }
  NoteEdit quieter{take.notes[1].id};
  quieter.gain_db = -6.0;
  const auto out = render_all(take, audio, std::span(&quieter, 1));
  double before = 0.0;
  double after = 0.0;
  const auto from = static_cast<std::size_t>(take.notes[1].start) + sig::frames(0.01);
  const auto to = static_cast<std::size_t>(take.notes[1].end) - sig::frames(0.01);
  for (auto i = from; i < to; ++i) {
    before += static_cast<double>(audio[i]) * audio[i];
    after += static_cast<double>(out[i]) * out[i];
  }
  const double change = 10.0 * std::log10(after / before);
  REQUIRE_LE(std::abs(change + 6.0), 1.0, "hit 2 is 6 dB down: " + std::to_string(change));
  const auto third = static_cast<std::size_t>(take.notes[2].start) + sig::frames(0.03);
  for (auto i = third; i < third + sig::frames(0.05); ++i)
    REQUIRE_EQ(out[i], audio[i], "the third hit is untouched");
}

void split_and_join_keep_the_audio() {
  const auto audio = phrase();
  auto take = analyse_or_throw(audio, Mode::melodic);
  const auto& note = take.notes.front();
  const auto id = note.id;
  const auto hops = note.hops();
  const auto middle = (note.start + note.end) / 2;
  auto second = split_note(take, id, middle);
  REQUIRE_OK(second, "split");
  REQUIRE_EQ(take.find(id)->end, middle, "split edges");
  REQUIRE_EQ(take.find(second.value())->start, middle, "split edges");
  REQUIRE_EQ(render_all(take, audio, {}), audio, "a split take renders the original");
  REQUIRE_OK(join_notes(take, id, second.value()), "join");
  REQUIRE_EQ(take.find(id)->hops(), hops, "joined back");
  REQUIRE_EQ(take.find(second.value()), nullptr, "joined back");
}

// ---- macros -------------------------------------------------------------

// A hand-made take: notes at the given centres, levels and starts.
[[nodiscard]] Take toy_take(std::span<const double> centres, std::span<const double> levels) {
  Take take;
  take.mode = Mode::melodic;
  take.sample_rate = sig::rate;
  take.hop = 256;
  take.frames = static_cast<std::int64_t>(sig::frames(4.0));
  for (std::size_t i = 0; i < centres.size(); ++i) {
    Note n;
    n.id = take.next_id++;
    n.start = static_cast<std::int64_t>(sig::frames(0.5 * static_cast<double>(i) + 0.13));
    n.end = n.start + static_cast<std::int64_t>(sig::frames(0.3));
    n.first_hop = n.start / take.hop;
    const auto hops = static_cast<std::size_t>((n.end - n.start) / take.hop + 2);
    n.pitch.assign(hops, static_cast<float>(cents_to_hz(centres[i])));
    n.amplitude.assign(hops, static_cast<float>(levels[i]));
    n.centre = centres[i];
    n.drift.assign(hops, 0.0F);
    n.modulation.assign(hops, 0.0F);
    for (std::size_t k = 0; k < hops; ++k)
      n.modulation[k] = static_cast<float>(20.0 * std::sin(0.3 * static_cast<double>(k)));
    take.notes.push_back(std::move(n));
  }
  return take;
}

void macros_correct_pitch_to_the_scale() {
  const std::array<double, 3> centres{6'040.0, 6'460.0, 6'930.0};
  const std::array<double, 3> levels{0.1, 0.1, 0.1};
  const auto take = toy_take(centres, levels);
  const auto scale = Scale::major(0);
  const auto edits = correct_pitch(take, {}, 1.0, 1.0, scale);
  const std::array<double, 3> expected{6'000.0, 6'500.0, 6'900.0};
  for (std::size_t i = 0; i < centres.size(); ++i) {
    const auto e = edit_of(edits, take.notes[i].id);
    REQUIRE_LT(std::abs(edited_centre(take.notes[i], e) - expected[i]),
               tolerance::double_accumulation,
               "corrected to " + std::to_string(edited_centre(take.notes[i], e)));
    REQUIRE_EQ(e.drift, 0.0, "drift removed");
  }
  const auto half = correct_pitch(take, {}, 0.5, 0.0, scale);
  REQUIRE_LT(std::abs(edited_centre(take.notes[0], edit_of(half, take.notes[0].id)) - 6'020.0),
             tolerance::double_accumulation, "half way");
}

void macros_quantize_level_and_vary() {
  const std::array<double, 2> centres{6'000.0, 6'200.0};
  const std::array<double, 2> levels{0.2, 0.05};
  const auto take = toy_take(centres, levels);
  TimeGrid grid;
  grid.step_frames = static_cast<double>(sig::frames(0.125));
  const auto snapped = quantize_time(take, {}, grid, 1.0);
  for (const auto& n : take.notes) {
    const auto e = edit_of(snapped, n.id);
    REQUIRE_TRUE(e.start, "on the grid");
    REQUIRE_EQ(*e.start % static_cast<std::int64_t>(grid.step_frames), 0, "on the grid");
    REQUIRE_EQ(*e.end - *e.start, n.end - n.start, "length kept");
  }
  const auto levelled = level_amplitude(take, {}, 1.0);
  const auto l0 = 20.0 * std::log10(0.2) + edit_of(levelled, 1).gain_db;
  const auto l1 = 20.0 * std::log10(0.05) + edit_of(levelled, 2).gain_db;
  REQUIRE_LT(std::abs(l0 - l1), tolerance::double_accumulation, "levels equal");
  const auto a = variation(take, {}, 7, 1.0);
  const auto b = variation(take, {}, 7, 1.0);
  const auto c = variation(take, {}, 8, 1.0);
  REQUIRE_EQ(a, b, "variation follows its seed");
  REQUIRE_NE(a, c, "variation follows its seed");
  for (const auto& e : a)
    require(std::abs(e.pitch_cents) <= variation_cents && std::abs(e.gain_db) <= variation_db,
            "variation stays small");
}

void macros_to_midi() {
  const std::array<double, 2> centres{6'010.0, 6'690.0};
  const std::array<double, 2> levels{0.2, 0.02};
  const auto take = toy_take(centres, levels);
  const auto midi = to_midi(take, {});
  REQUIRE_EQ(midi.notes.size(), 2, "two MIDI notes");
  REQUIRE_EQ(midi.notes[0].key, 60, "keys");
  REQUIRE_EQ(midi.notes[1].key, 67, "keys");
  REQUIRE_EQ(midi.notes[0].velocity, oscillate::foundation::max_note_velocity,
             "velocities follow level");
  REQUIRE_LT(midi.notes[1].velocity, midi.notes[0].velocity, "velocities follow level");
  REQUIRE_TRUE(!midi.bends.empty(), "bends follow the vibrato");
  REQUIRE_GE(midi.bend_range, 1.0, "bends follow the vibrato");
}

void macros_harmonise_and_fit_to_key() {
  const std::array<double, 2> centres{6'000.0, 6'400.0}; // C4, E4
  const std::array<double, 2> levels{0.1, 0.1};
  const auto take = toy_take(centres, levels);
  const auto voices = harmonise(take, {}, 2, Scale::major(0));
  REQUIRE_EQ(voices.size(), 2, "two harmony voices");
  REQUIRE_TRUE(voices[0].duplicate, "two harmony voices");
  REQUIRE_TRUE(voices[1].duplicate, "two harmony voices");
  REQUIRE_LT(std::abs(voices[0].pitch_cents - 400.0), tolerance::double_accumulation,
             "C4 up a third is E4");
  REQUIRE_LT(std::abs(voices[1].pitch_cents - 300.0), tolerance::double_accumulation,
             "E4 up a third is G4");
  // A minor to A major: C (the minor third) becomes C sharp; E stays.
  const auto major = fit_to_key(take, {}, Scale::minor(9), Scale::major(9), true);
  REQUIRE_LT(std::abs(edit_of(major, 1).pitch_cents - 100.0), tolerance::double_accumulation,
             "C becomes C sharp");
  REQUIRE_EQ(edit_of(major, 2).pitch_cents, 0.0, "E stays");
  const auto same = fit_to_key(take, {}, Scale::minor(9), Scale::major(9), false);
  REQUIRE_EQ(edit_of(same, 1).pitch_cents, 0.0, "without quality nothing moves");
  // Up a tone: C minor to D minor.
  const auto up = fit_to_key(take, {}, Scale::minor(0), Scale::minor(2), true);
  REQUIRE_LT(std::abs(edit_of(up, 1).pitch_cents - 200.0), tolerance::double_accumulation,
             "up a tone");
}

void macros_follow_notes() {
  const std::array<double, 2> centres{6'000.0, 6'400.0};
  const std::array<double, 2> levels{0.1, 0.1};
  const auto take = toy_take(centres, levels);
  const std::vector<TargetNote> targets{
      {take.notes[0].start + 100, take.notes[0].end + 400, 62},
      {take.notes[1].start - 300, take.notes[1].end, 65},
  };
  const auto edits = follow_notes(take, {}, targets, 1.0);
  REQUIRE_LT(std::abs(edited_centre(take.notes[0], edit_of(edits, 1)) - 6'200.0),
             tolerance::double_accumulation, "to D4");
  REQUIRE_LT(std::abs(edited_centre(take.notes[1], edit_of(edits, 2)) - 6'500.0),
             tolerance::double_accumulation, "to F4");
  REQUIRE_EQ(edit_of(edits, 1).start, targets[0].start, "to the target's time");
  REQUIRE_EQ(edit_of(edits, 1).end, targets[0].end, "to the target's time");
}

// Macros never hand back an edit list validate() refuses: shifts past
// max_edit_cents and stretches past max_time_scale are not made, and bad
// grids or empty scales change nothing they cannot.
void macros_stay_within_edit_bounds() {
  constexpr int lowest_key = 0;
  constexpr double far_frames = 1e300;
  const std::array<double, 2> centres{6'000.0, 6'400.0};
  const std::array<double, 2> levels{0.1, 0.1};
  const auto take = toy_take(centres, levels);
  const auto length = take.notes[0].end - take.notes[0].start;
  const auto stretch = static_cast<std::int64_t>(std::ceil(max_time_scale)) + 1;
  const std::vector<TargetNote> far{
      {take.notes[0].start, take.notes[0].start + length * stretch, lowest_key}};
  const auto followed = follow_notes(take, {}, far, 1.0);
  REQUIRE_OK(validate(take, followed), "follow_notes returned an edit list it cannot render");
  REQUIRE_EQ(edit_of(followed, take.notes[0].id).pitch_cents, 0.0,
             "a shift past the edit limit was made");
  REQUIRE_TRUE(!edit_of(followed, take.notes[0].id).start,
               "a stretch past the time limit was made");
  for (const auto& grid : {TimeGrid{0, std::numeric_limits<double>::quiet_NaN(), {}},
                           TimeGrid{0, std::numeric_limits<double>::infinity(), {}},
                           TimeGrid{0, 1.0, {std::numeric_limits<double>::infinity()}},
                           TimeGrid{0, 1.0 / far_frames, {}}}) {
    const auto snapped = quantize_time(take, {}, grid, 1.0);
    REQUIRE_TRUE(snapped.empty(), "an unusable grid moved a note");
  }
  const auto empty = fit_to_key(take, {}, Scale{}, Scale{}, true);
  REQUIRE_OK(validate(take, empty), "fitting between empty scales failed");
  std::vector<NoteEdit> at_limit{NoteEdit{}};
  at_limit[0].note = take.notes[0].id;
  at_limit[0].pitch_cents = max_edit_cents;
  REQUIRE_OK(validate(take, variation(take, at_limit, 1, 1.0)),
             "variation pushed a shift past the edit limit");
}

// Overlapping voices keep their own bends, and a voice placed where
// validate() refuses is left out of the MIDI.
void macros_to_midi_routes_bends_by_voice() {
  const std::array<double, 2> centres{6'010.0, 6'690.0};
  const std::array<double, 2> levels{0.2, 0.1};
  const auto take = toy_take(centres, levels);
  auto edits = harmonise(take, {}, 2, Scale::major(0));
  REQUIRE_TRUE(!edits.empty(), "harmony voices for the bend fixture");
  const auto midi = to_midi(take, edits);
  REQUIRE_EQ(midi.notes.size(), take.notes.size() + edits.size(), "every voice reaches the MIDI");
  for (const auto& bend : midi.bends) {
    REQUIRE_LT(bend.note, midi.notes.size(), "a bend names no voice");
    const auto& voice = midi.notes[bend.note];
    REQUIRE_TRUE(bend.frame >= voice.start && bend.frame < voice.end,
                 "a bend was routed to a voice that is not sounding");
  }
  NoteEdit reversed;
  reversed.note = take.notes[0].id;
  reversed.start = take.notes[0].end;
  reversed.end = take.notes[0].start;
  const std::vector<NoteEdit> invalid{reversed};
  const auto kept = to_midi(take, invalid);
  REQUIRE_EQ(kept.notes.size(), take.notes.size() - 1, "an invalid placement reached the MIDI");
  REQUIRE_EQ(kept.notes.front().velocity, oscillate::foundation::max_note_velocity,
             "a left-out loud voice lowered the kept voices' velocities");
  // A high note shifted up by the full edit range passes validate() but
  // lands above the MIDI keys.
  const std::array<double, 1> high_centre{8'200.0};
  const std::array<double, 1> high_level{0.1};
  const auto high_take = toy_take(high_centre, high_level);
  NoteEdit high;
  high.note = high_take.notes[0].id;
  high.pitch_cents = max_edit_cents;
  REQUIRE_OK(validate(high_take, std::vector<NoteEdit>{high}), "high shift fixture is valid");
  REQUIRE_TRUE(to_midi(high_take, std::vector<NoteEdit>{high}).notes.empty(),
               "a key outside the MIDI range was emitted");
  NoteEdit far;
  far.note = take.notes[0].id;
  far.start = max_placement_frame - (take.notes[0].end - take.notes[0].start);
  far.end = max_placement_frame + 1;
  REQUIRE_TRUE(!validate(take, std::vector<NoteEdit>{far}),
               "a placement past the render limit was accepted");
}

// A second note nested inside the first keeps the first's tail on join; a
// take with no hop refuses both edits.
void join_keeps_a_nested_notes_tail() {
  constexpr std::size_t nested_from = 2;
  constexpr std::size_t nested_hops = 3;
  constexpr float nested_hz = 330.0F;
  const std::array<double, 1> centres{6'000.0};
  const std::array<double, 1> levels{0.1};
  auto take = toy_take(centres, levels);
  const auto& outer = take.notes.front();
  REQUIRE_GT(outer.hops(), nested_from + nested_hops + 1, "nested fixture too short");
  Note inner = outer;
  inner.id = take.next_id++;
  inner.first_hop = outer.first_hop + static_cast<std::int64_t>(nested_from);
  inner.start = inner.first_hop * take.hop;
  inner.end = inner.start + static_cast<std::int64_t>(nested_hops) * take.hop;
  inner.pitch.assign(nested_hops, nested_hz);
  inner.amplitude.assign(nested_hops, 0.1F);
  inner.drift.assign(nested_hops, 0.0F);
  inner.modulation.assign(nested_hops, 0.0F);
  const auto outer_id = outer.id;
  const auto outer_hz = outer.pitch.back();
  const auto outer_hops = outer.hops();
  take.notes.push_back(inner);
  auto no_hop = take;
  no_hop.hop = 0;
  REQUIRE_TRUE(!join_notes(no_hop, outer_id, inner.id), "a take with no hop was joined");
  REQUIRE_TRUE(!split_note(no_hop, outer_id, (outer.start + outer.end) / 2),
               "a take with no hop was split");
  REQUIRE_OK(join_notes(take, outer_id, inner.id), "nested join");
  const auto& joined = *take.find(outer_id);
  REQUIRE_EQ(joined.hops(), outer_hops, "the nested join changed the note's hops");
  REQUIRE_EQ(joined.pitch[nested_from], nested_hz, "the nested note did not play inside");
  REQUIRE_EQ(joined.pitch.back(), outer_hz, "the first note's tail was replaced");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "clay dsp passed",
      {
          {"melodic_finds_sung_notes", melodic_finds_sung_notes},
          {"polyphonic_finds_the_chord", polyphonic_finds_the_chord},
          {"polyphonic_finds_a_stretched_chord", polyphonic_finds_a_stretched_chord},
          {"polyphonic_finds_a_struck_stab", polyphonic_finds_a_struck_stab},
          {"unedited_render_is_the_original", unedited_render_is_the_original},
          {"shifted_note_sounds_at_its_new_pitch", shifted_note_sounds_at_its_new_pitch},
          {"shifted_chord_note_leaves_the_others", shifted_chord_note_leaves_the_others},
          {"formants_stay_unless_shifted", formants_stay_unless_shifted},
          {"stretched_note_keeps_its_pitch", stretched_note_keeps_its_pitch},
          {"renders_are_block_split_invariant", renders_are_block_split_invariant},
          {"analysis_is_deterministic", analysis_is_deterministic},
          {"percussive_hits_become_notes", percussive_hits_become_notes},
          {"split_and_join_keep_the_audio", split_and_join_keep_the_audio},
          {"macros_correct_pitch_to_the_scale", macros_correct_pitch_to_the_scale},
          {"macros_quantize_level_and_vary", macros_quantize_level_and_vary},
          {"macros_to_midi", macros_to_midi},
          {"macros_harmonise_and_fit_to_key", macros_harmonise_and_fit_to_key},
          {"macros_follow_notes", macros_follow_notes},
          {"macros_stay_within_edit_bounds", macros_stay_within_edit_bounds},
          {"macros_to_midi_routes_bends_by_voice", macros_to_midi_routes_bends_by_voice},
          {"join_keeps_a_nested_notes_tail", join_keeps_a_nested_notes_tail},
      });
}
