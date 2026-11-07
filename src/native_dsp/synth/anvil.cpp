#include "oscillate/native_dsp/synth/anvil.hpp"

#include "anvil_presets.hpp"
#include "anvil_table.hpp"
#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/drum_dsp.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/synth/oversampler.hpp"
#include "oscillate/native_dsp/synth/shaping.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::native_dsp::synth::anvil {
namespace {

using anvil_table::index_of;
using foundation::ErrorCode;
using foundation::Result;
using Ramp = dsp::Ramp;

namespace p {
constexpr std::size_t length = index_of("anvil.length");
constexpr std::size_t tune = index_of("anvil.tune");
constexpr std::size_t key = index_of("anvil.key");
constexpr std::size_t velocity = index_of("anvil.velocity");
constexpr std::size_t velocity_drive = index_of("anvil.velocity.drive");
constexpr std::size_t body_level = index_of("anvil.body.level");
constexpr std::size_t body_wave = index_of("anvil.body.wave");
constexpr std::size_t harmonics = index_of("anvil.body.harmonics");
constexpr std::size_t phase = index_of("anvil.body.phase");
constexpr std::size_t body_drive = index_of("anvil.body.drive");
constexpr std::size_t click_type = index_of("anvil.click.type");
constexpr std::size_t click_level = index_of("anvil.click.level");
constexpr std::size_t click_tone = index_of("anvil.click.tone");
constexpr std::size_t click_length = index_of("anvil.click.length");
constexpr std::size_t click_pitch = index_of("anvil.click.pitch");
constexpr std::size_t click_start = index_of("anvil.click.start");
constexpr std::size_t click_velocity = index_of("anvil.click.velocity");
constexpr std::size_t sub_on = index_of("anvil.sub.on");
constexpr std::size_t sub_level = index_of("anvil.sub.level");
constexpr std::size_t sub_octave = index_of("anvil.sub.octave");
constexpr std::size_t sub_attack = index_of("anvil.sub.attack");
constexpr std::size_t rumble_on = index_of("anvil.rumble.on");
constexpr std::size_t rumble_amount = index_of("anvil.rumble.amount");
constexpr std::size_t rumble_decay = index_of("anvil.rumble.decay");
constexpr std::size_t rumble_duck = index_of("anvil.rumble.duck");
constexpr std::size_t rumble_tone = index_of("anvil.rumble.tone");
constexpr std::size_t rumble_drive = index_of("anvil.rumble.drive");
constexpr std::size_t rumble_space = index_of("anvil.rumble.space");
constexpr std::size_t rumble_width = index_of("anvil.rumble.width");
constexpr std::size_t drive_type = index_of("anvil.drive.type");
constexpr std::size_t drive_amount = index_of("anvil.drive.amount");
constexpr std::size_t tone = index_of("anvil.tone");
constexpr std::size_t punch = index_of("anvil.punch");
constexpr std::size_t glue = index_of("anvil.glue");
constexpr std::size_t output = index_of("anvil.output");
constexpr std::size_t lock = index_of(lock_kick_parameter);
constexpr std::size_t length_mode = index_of("anvil.length.mode");
constexpr std::size_t length_division = index_of("anvil.length.division");
} // namespace p

constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::size_t oversampling = 4; // The body and the master drive.
constexpr double oversampling_rate = 4.0;
constexpr double attack_seconds = 0.00025;  // A hit's body starts over a quarter millisecond.
constexpr double end_seconds = 0.002;       // ...and ends over two.
constexpr double retrigger_seconds = 0.003; // An outgoing hit fades under the new one.
constexpr double gate_seconds = 0.005;      // Note length: a key let go ends its hit.
// On/off switches and wave changes, raised-cosine.
constexpr double switch_seconds = native_dsp::dsp::switch_fade_seconds;
// A drive type change crossfades.
constexpr double type_fade_seconds = native_dsp::dsp::switch_fade_seconds;
constexpr double reset_seconds = 64.0 / 48'000.0;
constexpr double duck_release_seconds = 0.06;
constexpr double punch_seconds = 0.015;
constexpr double glue_release_seconds = 0.12;
constexpr double max_sample_seconds = 2.0;
// Lock: a bass note starting this soon after a hit still counts as under it.
constexpr double lock_lookahead_beats = 0.25;
constexpr double lock_lookahead_seconds = 0.125; // With no tempo.
constexpr int lock_fifth = 7;
constexpr std::size_t max_partials = 24;
constexpr double ceiling = 0.97;       // The limiter's soft ceiling, at 2x...
constexpr double knee = 0.85;          // ...from this level...
constexpr double guard_knee = 0.97;    // ...then the half-band's overshoot is caught softly
constexpr double guard_ceiling = 0.99; // at the base rate...
constexpr double hard_limit = 0.995;   // ...and a last hard guard.
// The rumble's space: four lines of a small feedback delay network.
constexpr std::array<double, 4> space_ms{23.3, 29.9, 36.7, 41.3};

// An on/off switch that glides over a raised cosine, so neither end of the
// move bends the sound sharply.
struct SwitchFade {
  double from = 0.0;
  double to = 0.0;
  double progress = 1.0;
  void set(double target, bool jump) noexcept {
    if (jump) {
      from = to = target;
      progress = 1.0;
    } else if (target != to) {
      from = value();
      to = target;
      progress = 0.0;
    }
  }
  [[nodiscard]] double value() const noexcept {
    return from + (to - from) * native_dsp::dsp::raised_cosine(progress);
  }
  double next(double step) noexcept {
    progress = std::min(1.0, progress + step);
    return value();
  }
};

using native_dsp::dsp::raised_cosine;

[[nodiscard]] std::size_t choice(double value) noexcept {
  return static_cast<std::size_t>(std::max(0.0, std::round(value)));
}
using native_dsp::dsp::db_to_gain;
[[nodiscard]] double midi_hz(double key) noexcept {
  return 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
}

// Padé [7/6] tanh, within 1e-4 of tanh where it is used and exactly +/-1
// beyond 4.97. The drive stages call it several times a frame.
[[nodiscard]] double fast_tanh(double x) noexcept {
  if (x > 4.97)
    return 1.0;
  if (x < -4.97)
    return -1.0;
  const double s = x * x;
  const double y = x * (135'135.0 + s * (17'325.0 + s * (378.0 + s))) /
                   (135'135.0 + s * (62'370.0 + s * (3'150.0 + 28.0 * s)));
  return y > 1.0 ? 1.0 : (y < -1.0 ? -1.0 : y);
}

// The one bend every drawn curve uses (Nova's, Pump's).
[[nodiscard]] double bend(double t, double tension) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  if (tension > 0.0)
    return std::pow(t, 1.0 + 4.0 * tension);
  if (tension < 0.0)
    return 1.0 - std::pow(1.0 - t, 1.0 - 4.0 * tension);
  return t;
}

// A compiled drawn curve.
struct Curve {
  std::vector<project::NativeCurvePoint> points;
};

// Evaluates a curve at a rising position, walking its segments once.
struct CurveCursor {
  std::size_t segment = 0;
  void reset() noexcept { segment = 0; }
  [[nodiscard]] double at(const Curve& curve, double x) noexcept {
    const auto& pts = curve.points;
    const std::size_t n = pts.size();
    if (x <= pts.front().x)
      return pts.front().y;
    if (x >= pts.back().x)
      return pts.back().y;
    if (segment + 1 >= n || x < pts[segment].x)
      segment = 0;
    while (segment + 2 < n && x >= pts[segment + 1].x)
      ++segment;
    const auto& a = pts[segment];
    const auto& b = pts[segment + 1];
    if (a.segment == project::NativeCurveSegment::step)
      return a.y;
    const double t = (x - a.x) / std::max(1e-12, b.x - a.x);
    return a.y + (b.y - a.y) * bend(t, a.tension);
  }
};

// The compiled document: the three body curves and the user's transient,
// resampled to the engine's rate and summed to mono.
struct AnvilState final : SynthDocumentState {
  Curve pitch;
  Curve amp;
  Curve drive;
  double tail_hz = 50.0; // The drawn pitch's end, untuned.
  std::string click_sha;
  std::shared_ptr<const std::vector<float>> click;
};

[[nodiscard]] Curve curve_of(std::span<const anvil_presets::Point> points, bool hz) {
  Curve curve;
  for (const auto& point : points)
    curve.points.push_back({point.x, hz ? y_of_hz(point.value) : point.value, point.tension,
                            project::NativeCurveSegment::curve});
  return curve;
}

[[nodiscard]] project::NativeCurve document_curve(std::string_view id, const Curve& curve) {
  return {std::string(id), curve.points};
}

[[nodiscard]] const AnvilState& default_state() {
  static const auto state = [] {
    auto made = std::make_unique<AnvilState>();
    const auto& body = anvil_presets::bodies.front();
    made->pitch = curve_of(body.pitch, true);
    made->amp = curve_of(body.amp, false);
    made->drive = curve_of(body.drive, false);
    made->tail_hz = hz_of_y(made->pitch.points.back().y);
    return made;
  }();
  return *state;
}

// Windowed-sinc resampling of one channel, on control.
[[nodiscard]] std::vector<float> resample(const std::vector<double>& in, double from, double to,
                                          std::size_t limit) {
  constexpr double half_width = 16.0;
  const double ratio = from / to;
  const double cutoff = std::min(1.0, to / from);
  const double half = half_width / cutoff;
  const auto length =
      std::min(limit, static_cast<std::size_t>(std::floor(static_cast<double>(in.size()) / ratio)));
  std::vector<float> out(length, 0.0F);
  for (std::size_t n = 0; n < length; ++n) {
    const double t = static_cast<double>(n) * ratio;
    if (from == to) {
      out[n] = static_cast<float>(in[n]);
      continue;
    }
    const auto first = static_cast<std::ptrdiff_t>(std::ceil(t - half));
    const auto last = static_cast<std::ptrdiff_t>(std::floor(t + half));
    double sum = 0.0;
    for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, first);
         k <= last && k < static_cast<std::ptrdiff_t>(in.size()); ++k) {
      const double d = t - static_cast<double>(k);
      const double x = pi * cutoff * d;
      const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
      const double w = 0.42 + 0.5 * std::cos(pi * d / half) + 0.08 * std::cos(two_pi * d / half);
      sum += in[static_cast<std::size_t>(k)] * cutoff * sinc * w;
    }
    out[n] = static_cast<float>(sum);
  }
  return out;
}

Result<void> validate_document(const project::NativeSynthDocument& document) {
  using R = Result<void>;
  const auto invalid = [](std::string message) {
    return R::failure({ErrorCode::invalid_argument, "Anvil: " + std::move(message)});
  };
  if (!document.routes.empty() || !document.chains.empty() || !document.sequences.empty() ||
      !document.labels.empty() || !document.voice_lanes.empty())
    return invalid("the document holds only the body's curves and the click sample");
  std::array<bool, 3> seen{};
  for (const auto& curve : document.curves) {
    const std::size_t index = curve.id == pitch_curve   ? 0
                              : curve.id == amp_curve   ? 1
                              : curve.id == drive_curve ? 2
                                                        : 3;
    if (index == 3 || seen[index])
      return invalid("unknown or repeated curve '" + curve.id + "'");
    if (curve.points.size() < 2)
      return invalid("a curve needs two points");
    seen[index] = true;
  }
  if (document.media.size() > 1)
    return invalid("only the click slot holds media");
  for (const auto& slot : document.media) {
    if (slot.id != click_slot ||
        !std::holds_alternative<project::ManagedAudioReference>(slot.media) ||
        slot.frame_length != 0)
      return invalid("the click slot holds one managed sample");
  }
  return R::success();
}

Result<std::shared_ptr<const SynthDocumentState>>
prepare_document(const project::NativeSynthDocument& document, const SynthMedia& media,
                 double sample_rate, const SynthDocumentState* previous) {
  using R = Result<std::shared_ptr<const SynthDocumentState>>;
  if (auto valid = validate_document(document); !valid)
    return R::failure(valid.error());
  auto state = std::make_shared<AnvilState>();
  const auto& defaults = default_state();
  state->pitch = defaults.pitch;
  state->amp = defaults.amp;
  state->drive = defaults.drive;
  for (const auto& curve : document.curves) {
    Curve compiled{curve.points};
    if (curve.id == pitch_curve)
      state->pitch = std::move(compiled);
    else if (curve.id == amp_curve)
      state->amp = std::move(compiled);
    else
      state->drive = std::move(compiled);
  }
  state->tail_hz = hz_of_y(state->pitch.points.back().y);
  if (!document.media.empty()) {
    const auto& slot = document.media.front();
    const auto& reference = std::get<project::ManagedAudioReference>(slot.media);
    const auto* before = static_cast<const AnvilState*>(previous);
    if (before != nullptr && before->click && before->click_sha == reference.sha256) {
      state->click = before->click; // Unchanged media keep their resampling.
    } else {
      const auto found = std::ranges::find(media, std::string(click_slot), &SynthMediaEntry::slot);
      if (found == media.end() || !found->asset || found->asset->source_rate == 0)
        return R::failure({ErrorCode::unavailable, "Anvil: the click sample is not loaded"});
      const auto& asset = *found->asset;
      std::vector<double> mono(asset.frames.size());
      for (std::size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5 * (static_cast<double>(asset.frames[i].left) +
                         static_cast<double>(asset.frames[i].right));
      const auto limit = static_cast<std::size_t>(max_sample_seconds * sample_rate);
      state->click = std::make_shared<const std::vector<float>>(
          resample(mono, asset.source_rate, sample_rate, limit));
    }
    state->click_sha = reference.sha256;
  }
  return R::success(std::shared_ptr<const SynthDocumentState>(std::move(state)));
}

project::NativeSynthDocument preset_document(std::optional<std::size_t> preset) {
  const std::size_t index = preset.value_or(0);
  const auto& body = anvil_presets::bodies[index < anvil_presets::bodies.size() ? index : 0];
  project::NativeSynthDocument document;
  document.curves.push_back(document_curve(pitch_curve, curve_of(body.pitch, true)));
  document.curves.push_back(document_curve(amp_curve, curve_of(body.amp, false)));
  document.curves.push_back(document_curve(drive_curve, curve_of(body.drive, false)));
  return document;
}

// Partial amplitudes for the additive waves (index n = partial n, 1-based),
// and their running sums of magnitudes, the normalisation at each count.
struct WaveTables {
  std::array<std::array<double, max_partials + 1>, 4> amplitude{};
  std::array<std::array<double, max_partials + 1>, 4> norm{};
  void fixed() noexcept {
    for (std::size_t n = 1; n <= max_partials; ++n) {
      const double k = static_cast<double>(n);
      if (n % 2 == 1) {
        amplitude[1][n] = ((n / 2) % 2 == 0 ? 1.0 : -1.0) / (k * k); // Triangle.
        amplitude[2][n] = std::exp(-(k - 1.0) / 10.0) / k;           // Soft square.
      }
    }
    sum(1);
    sum(2);
  }
  // Harmonic: a falling series whose slope Harmonics sets.
  void harmonic(double harmonics) noexcept {
    const double ratio = 0.15 + 0.7 * harmonics;
    double a = 1.0;
    for (std::size_t n = 1; n <= max_partials; ++n) {
      amplitude[3][n] = a / static_cast<double>(n);
      a *= ratio;
    }
    sum(3);
  }
  void sum(std::size_t shape) noexcept {
    norm[shape][0] = 0.0;
    for (std::size_t n = 1; n <= max_partials; ++n)
      norm[shape][n] = norm[shape][n - 1] + std::fabs(amplitude[shape][n]);
  }
};

// The body's wave at a phase whose sine and cosine are s1 and c,
// band-limited to `partials`.
[[nodiscard]] double wave(const WaveTables& tables, std::size_t shape, double s1, double c,
                          double harmonics, std::size_t partials) noexcept {
  if (shape == 0 && harmonics <= 0.0)
    return s1;
  // The 2nd and 3rd partials every wave can add ("Harmonics").
  const double s2 = 2.0 * c * s1;
  const double s3 = 2.0 * c * s2 - s1;
  const double warmth = partials >= 3 ? harmonics * (0.35 * s2 + 0.2 * s3) : 0.0;
  if (shape == 0 || partials < 3)
    return (s1 + warmth) / (1.0 + 0.55 * harmonics);
  // Higher partials by the Chebyshev recurrence sin((n+1)t) = 2cos(t)sin(nt) - sin((n-1)t).
  const auto& amplitude = tables.amplitude[shape];
  double sum = amplitude[1] * s1 + amplitude[2] * s2 + amplitude[3] * s3;
  double previous = s2;
  double current = s3;
  for (std::size_t n = 4; n <= partials; ++n) {
    const double next = 2.0 * c * current - previous;
    previous = current;
    current = next;
    sum += amplitude[n] * next;
  }
  const double base = sum / tables.norm[shape][partials];
  return shape == 3 ? base : (base + warmth) / (1.0 + 0.55 * harmonics);
}

// One stage of the master drive: unity gain for small signals, so the drive
// rounds the loud part of the hit and leaves its decay as drawn. (A shaper
// normalised to keep full scale, tanh(g x) / tanh(g), lifts everything quiet
// and flattens the decay into a held tone.)
[[nodiscard]] double drive_shape(std::size_t type, double x) noexcept {
  switch (type) {
  case 1: { // Tube: asymmetric, so it adds even partials (a DC blocker follows).
    constexpr double bias = 0.25;
    const double slope = 1.0 - std::tanh(bias) * std::tanh(bias);
    return (fast_tanh(x + bias) - fast_tanh(bias)) / slope;
  }
  case 2: // Clip.
    return std::clamp(x, -1.0, 1.0);
  case 3: { // Fold: straight up to full scale, reflected beyond it.
    const double t = std::fmod(std::fabs(x) + 1.0, 4.0);
    const double y = t < 2.0 ? t - 1.0 : 3.0 - t;
    return x < 0.0 ? -y : y;
  }
  default: // Tape.
    return fast_tanh(x);
  }
}

[[nodiscard]] double soft_ceiling(double x, double from = knee, double to = ceiling) noexcept {
  const double m = std::fabs(x);
  if (m <= from)
    return x;
  const double y = from + (to - from) * fast_tanh((m - from) / (to - from));
  return x < 0.0 ? -y : y;
}

// A one-pole low-pass's phase delay (in samples) and gain at w radians/sample.
struct OnePoleAt {
  double delay = 0.0;
  double gain = 1.0;
};
[[nodiscard]] OnePoleAt one_pole_at(double coefficient, double w) noexcept {
  const double a = 1.0 - coefficient;
  const double re = 1.0 - a * std::cos(w);
  const double im = a * std::sin(w);
  return {std::atan2(im, re) / std::max(1e-9, w), coefficient / std::sqrt(re * re + im * im)};
}

[[nodiscard]] double one_pole(double hz, double rate) noexcept {
  return native_dsp::dsp::one_pole_coefficient(std::min(hz, native_dsp::dsp::nyquist_guard * rate),
                                               rate);
}

[[nodiscard]] SvfCoefficients svf(double hz, double q, double rate) noexcept {
  return SvfCoefficients::make(
      prewarp(std::clamp(hz, 10.0, native_dsp::dsp::nyquist_guard * rate) / rate), 1.0 / q);
}

// A hit's length in seconds under the length mode: Length, the synced note
// value at the transport's tempo (tempo_sync's default when unknown), or the
// note's own length when known (else Length), always within the shortest
// and longest hit.
[[nodiscard]] double hit_seconds(std::span<const double> v, const SynthTransport& transport,
                                 std::uint32_t note_frames, double rate) noexcept {
  double seconds = v[p::length] * 1e-3;
  const auto mode = choice(v[p::length_mode]);
  if (mode == anvil_table::length_sync) {
    const auto division =
        std::min(choice(v[p::length_division]), anvil_table::length_divisions.size() - 1);
    seconds = sync_seconds(division, transport, rate);
  } else if (mode == anvil_table::length_note && note_frames > 0) {
    seconds = static_cast<double>(note_frames) / rate;
  }
  return std::clamp(seconds, anvil_table::shortest_hit_seconds, anvil_table::longest_hit_seconds);
}

// A transport at a steady tempo in 4/4, for work off the callback that
// knows only the tempo (0: unknown).
[[nodiscard]] SynthTransport steady(double bpm, double rate) noexcept {
  constexpr double seconds_per_minute = 60.0;
  return {0.0, bpm > 0.0 ? bpm / seconds_per_minute / rate : 0.0, 0.0,
          SynthTransport{}.beats_per_bar};
}

// One hit: the body (synthesised at 4x and decimated), the click and the sub.
struct Hit {
  bool active = false;
  bool body = false;   // Until the body's length ends.
  double frames = 0.0; // The hit's own length (Note mode, a note's length); 0: the kernel's.
  bool gated = false;  // Note mode with no known length: its key's release ends it.
  foundation::IdBits clip{};
  foundation::IdBits note{};
  std::uint8_t key = 0;
  double x = 0.0;     // Position, 0..1 of the length.
  double theta = 0.0; // Body phase.
  double sub_theta = 0.0;
  double start_phase = 0.0;
  double follow = 1.0;   // Follow note's transposition.
  double velocity = 1.0; // 0..1.
  double gain = 1.0;     // Velocity's level.
  double drive_scale = 1.0;
  double click_gain = 1.0;
  std::size_t wave = 0;
  std::size_t old_wave = 0;
  double wave_mix = 1.0; // 1: wave alone; below, crossfading from old_wave.
  double f = 0.0;        // Body frequency and envelopes at the frame start.
  double a = 0.0;
  double d = 0.0;
  double sub_amp = 0.0;
  std::uint32_t age = 0; // Frames since the hit.
  double fade = 1.0;     // Retrigger fade-out.
  double fade_step = 0.0;
  CurveCursor pitch{};
  CurveCursor amp{};
  CurveCursor drive{};
  std::array<Downsampler2x, 2> down{}; // 4x -> 2x -> 1x.
  // Click.
  std::size_t click_type = 0;
  double click_env = 0.0;
  double click_env2 = 0.0;
  double click_theta = 0.0;
  double click_theta2 = 0.0;
  double sample_pos = 0.0;
  NoiseSource noise{};
  BasicZdfSvf<double> click_filter{};
  double last_raw = 0.0; // For the rumble: the undriven body.
  double last_amp = 0.0; // For the duck.
};

class AnvilKernel final : public SynthKernel {
public:
  explicit AnvilKernel(double sample_rate) : rate_(sample_rate) {
    comb_.assign(static_cast<std::size_t>(sample_rate / 15.0) + 8, 0.0);
    for (std::size_t i = 0; i < space_.size(); ++i) {
      space_length_[i] = static_cast<std::size_t>(space_ms[i] * 1e-3 * sample_rate);
      space_[i].assign(space_length_[i] + 1, 0.0);
    }
    glue_.prepare(sample_rate);
    for (auto& dc : dc_)
      dc.prepare(sample_rate);
    state_ = &default_state();
    drawn_tail_ = state_->tail_hz;
    tables_.fixed();
    switch_step_ = 1.0 / (switch_seconds * rate_);
    comb_glide_ = 1.0 - std::exp(-1.0 / (0.02 * rate_));
    duck_release_ = std::exp(-1.0 / (duck_release_seconds * rate_));
    duck_smooth_ = 1.0 - std::exp(-1.0 / (0.002 * rate_));
    punch_decay_ = std::exp(-1.0 / (punch_seconds * rate_));
    punch_attack_ = 1.0 - std::exp(-1.0 / (0.0003 * rate_ / 4.6));
    peak_release_ = std::exp(-1.0 / (0.3 * rate_));
    clear();
  }

  void adopt_document(const SynthDocumentState& state) noexcept override {
    state_ = &static_cast<const AnvilState&>(state);
    drawn_tail_ = state_->tail_hz;
  }

  void adopt_lock(const LockState& state) noexcept override { lock_ = &state; }
  void locate(std::uint64_t project_frame) noexcept override { frame_ = project_frame; }

  void control(std::span<const double> v, std::uint32_t frames_to_next_tick,
               const SynthTransport& transport) noexcept override {
    values_ = v;
    // Only Bass note retunes; Keep tone pairs without touching the tail.
    lock_retunes_ = choice(v[p::lock]) == static_cast<std::size_t>(lock_kick_bass_note);
    lock_lookahead_ = transport.beats_per_frame > 0.0
                          ? lock_lookahead_beats / transport.beats_per_frame
                          : lock_lookahead_seconds * rate_;
    const double steps = frames_to_next_tick;
    const bool jump = !primed_;
    note_mode_ = choice(v[p::length_mode]) == anvil_table::length_note;
    // Sync follows the tempo here; Time and Note glide on Length (Note's
    // hits with a known length carry their own).
    length_.to(hit_seconds(v, transport, 0, rate_) * 1e3, steps, jump);
    tune_.to(v[p::tune], steps, jump);
    if (jump) {
      tune_value_ = v[p::tune];
      tune_ratio_ = std::pow(2.0, tune_value_ / 12.0);
    }
    body_level_.to(v[p::body_level], steps, jump);
    harmonics_.to(v[p::harmonics], steps, jump);
    body_drive_.to(v[p::body_drive], steps, jump);
    click_level_.to(v[p::click_level], steps, jump);
    sub_level_.to(v[p::sub_level], steps, jump);
    sub_on_.set(choice(v[p::sub_on]) == 1 ? 1.0 : 0.0, jump);
    rumble_on_.set(choice(v[p::rumble_on]) == 1 ? 1.0 : 0.0, jump);
    amount_.to(v[p::rumble_amount], steps, jump);
    duck_.to(v[p::rumble_duck], steps, jump);
    rumble_drive_.to(v[p::rumble_drive], steps, jump);
    space_mix_.to(v[p::rumble_space], steps, jump);
    width_.to(v[p::rumble_width], steps, jump);
    drive_.to(v[p::drive_amount], steps, jump);
    punch_.to(v[p::punch], steps, jump);
    out_gain_.to(db_to_gain(v[p::output]), steps, jump);

    // A wave change crossfades in the sounding hit.
    const std::size_t wave = std::min<std::size_t>(choice(v[p::body_wave]), 3);
    for (auto& hit : hits_) {
      if (hit.active && hit.wave != wave) {
        hit.old_wave = hit.wave;
        hit.wave = wave;
        hit.wave_mix = 0.0;
      }
    }
    wave_ = wave;
    // A drive type change crossfades from the old shape.
    const std::size_t type = std::min<std::size_t>(choice(v[p::drive_type]), 3);
    if (jump) {
      drive_type_ = type;
      old_drive_type_ = type;
      type_mix_ = 1.0;
    } else if (type != drive_type_) {
      old_drive_type_ = drive_type_;
      drive_type_ = type;
      type_mix_ = 0.0;
    }
    octave_ratio_ = std::pow(2.0, std::clamp(std::round(v[p::sub_octave]), -1.0, 1.0));
    tables_.harmonic(v[p::harmonics]);
    sub_attack_ = 1.0 - std::exp(-1.0 / (std::max(0.25, v[p::sub_attack]) * 1e-3 * rate_));

    // The rumble follows the sounding hit's tail note.
    const double tail = tail_hz(current());
    const double tone_hz = 60.0 * std::pow(20.0, v[p::rumble_tone]);
    const double loop_c = one_pole(3.0 * tone_hz, rate_);
    const double w = two_pi * tail / rate_;
    const auto loop = one_pole_at(loop_c, w);
    comb_target_ =
        std::clamp(rate_ / tail - loop.delay, 2.0, static_cast<double>(comb_.size()) - 3.0);
    if (jump)
      comb_delay_ = comb_target_;
    const double decay = v[p::rumble_decay];
    comb_c_ = loop_c;
    comb_g_ = std::min(0.9995, std::pow(10.0, -3.0 * (rate_ / tail) / (decay * rate_)) / loop.gain);
    comb_in_ = (1.0 - comb_g_ * loop.gain) * (0.6 + 0.55 * decay);
    for (std::size_t i = 0; i < space_.size(); ++i)
      space_g_[i] = std::pow(10.0, -3.0 * static_cast<double>(space_length_[i]) / (decay * rate_));
    space_c_ = loop_c;
    rumble_lp_ = svf(tone_hz, 0.707, rate_);
    rumble_norm_ = std::tanh(1.0 + 10.0 * v[p::rumble_drive]);

    tone_lp_ = svf(200.0 * std::pow(100.0, v[p::tone]), 0.707, rate_);
    glue_.set(v[p::glue], glue_release_seconds);
    primed_ = true;
  }

  void note_on(const NoteEvent& event) noexcept override {
    if (values_.empty())
      return;
    const auto& v = values_;
    // The sounding hit fades out under the new one.
    Hit& old = hits_[current_];
    if (old.active) {
      old.fade_step = 1.0 / std::max(1.0, retrigger_seconds * rate_);
    }
    current_ = 1 - current_;
    Hit& hit = hits_[current_];
    const std::uint64_t seed = ++hit_count_;
    hit = Hit{};
    hit.active = true;
    hit.body = true;
    hit.start_phase = v[p::phase] * pi / 180.0;
    hit.velocity = std::clamp(event.velocity / 127.0, 0.0, 1.0);
    hit.gain = 1.0 - v[p::velocity] * (1.0 - std::pow(hit.velocity, 1.6));
    hit.drive_scale = 1.0 - v[p::velocity_drive] * (1.0 - hit.velocity);
    hit.click_gain = 1.0 - v[p::click_velocity] * (1.0 - hit.velocity);
    hit.wave = wave_;
    hit.old_wave = wave_;
    hit.follow = 1.0;
    if (choice(v[p::key]) == 1) {
      hit.follow = midi_hz(event.key) / drawn_tail_;
    }
    if (const LockNote* bass = lock_retunes_ ? bass_under_hit() : nullptr) {
      const double drawn = drawn_tail_ * tune_ratio();
      hit.follow = lock_target(bass->key, drawn) / drawn;
    }
    if (note_mode_) {
      if (event.frames > 0)
        hit.frames = hit_seconds(v, SynthTransport{}, event.frames, rate_) * rate_;
      hit.gated = event.frames == 0;
      hit.clip = event.clip;
      hit.note = event.note;
      hit.key = event.key;
    }
    hit.click_type = std::min<std::size_t>(choice(v[p::click_type]), 4);
    hit.click_env = 1.0;
    hit.click_env2 = 1.0;
    hit.sample_pos = v[p::click_start] * 1e-3 * rate_;
    hit.noise.reset(static_cast<std::uint32_t>(0x9E3779B9U ^ (seed * 2654435761U)));
    hit.f = frequency(hit, 0.0);
    hit.a = hit.amp.at(state_->amp, 0.0);
    hit.d = hit.drive.at(state_->drive, 0.0);
    punch_rising_ = true; // Punch rises over a third of a millisecond, never in a step.
    last_note_ = event.key;
    last_velocity_ = event.velocity;
  }

  // Kicks are one-shots: a note-off changes nothing, except in Note mode
  // for a hit whose length was not known (a key played live), which it ends.
  void note_off(const NoteEvent& event) noexcept override {
    if (!note_mode_)
      return;
    for (auto& hit : hits_) {
      if (hit.active && hit.gated && hit.fade_step == 0.0 && hit.key == event.key &&
          hit.clip == event.clip && hit.note == event.note)
        hit.fade_step = 1.0 / std::max(1.0, gate_seconds * rate_);
    }
  }

  void render(std::span<engine::StereoFrame> out) noexcept override {
    if (values_.empty())
      return;
    for (auto& frame : out) {
      double left = 0.0;
      double right = 0.0;
      render_frame(left, right);
      if (reset_remaining_ > 0) {
        const double g = static_cast<double>(reset_remaining_) / reset_frames_;
        left += reset_left_ * g;
        right += reset_right_ * g;
        --reset_remaining_;
      }
      last_left_ = left;
      last_right_ = right;
      peak_ = std::max(std::max(std::fabs(left), std::fabs(right)), peak_ * peak_release_);
      frame.left += static_cast<float>(left);
      frame.right += static_cast<float>(right);
    }
  }

  void reset() noexcept override {
    // Fade the last output out while everything restarts from silence.
    reset_left_ = last_left_;
    reset_right_ = last_right_;
    reset_frames_ = std::max(1.0, reset_seconds * rate_);
    reset_remaining_ = static_cast<std::uint32_t>(reset_frames_);
    clear();
  }

  [[nodiscard]] KernelStatus status() const noexcept override {
    std::uint32_t voices = 0;
    for (const auto& hit : hits_)
      voices += hit.active ? 1U : 0U;
    return {voices, 0, false};
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size)
      return 0;
    const Hit& hit = hits_[current_];
    out[telemetry::position] = hit.active && hit.body ? static_cast<float>(hit.x) : -1.0F;
    out[telemetry::last_note] = static_cast<float>(last_note_);
    out[telemetry::last_velocity] = static_cast<float>(last_velocity_);
    out[telemetry::output_peak_db] =
        static_cast<float>(peak_ > 1e-6 ? 20.0 * std::log10(peak_) : -120.0);
    return telemetry::size;
  }

  // For render_preview: the rumble layer alone is written here, mono.
  void tap_rumble(std::vector<float>* tap) noexcept { rumble_tap_ = tap; }
  [[nodiscard]] double sounding_tail_hz() const noexcept { return tail_hz(current()); }

private:
  // The bass note a hit at frame_ lands on: the latest one still sounding,
  // else one starting within the lookahead. Null when there is none.
  [[nodiscard]] const LockNote* bass_under_hit() const noexcept {
    if (lock_ == nullptr || lock_->notes.empty())
      return nullptr;
    const auto& notes = lock_->notes;
    const auto after = std::ranges::upper_bound(notes, frame_, {}, &LockNote::start);
    for (auto it = after; it != notes.begin();) {
      --it;
      if (it->end > frame_)
        return &*it;
    }
    if (after != notes.end() && static_cast<double>(after->start - frame_) <= lock_lookahead_)
      return &*after;
    return nullptr;
  }

  // The root or fifth of `key`, in whichever octave lies nearest `drawn` Hz.
  [[nodiscard]] static double lock_target(std::uint8_t key, double drawn) noexcept {
    double best = drawn;
    double distance = std::numeric_limits<double>::infinity();
    for (const int step : {0, lock_fifth}) {
      const double hz = midi_hz(static_cast<double>(key + step));
      const double octaves = std::round(std::log2(drawn / hz));
      const double candidate = hz * std::exp2(octaves);
      const double away = std::fabs(std::log2(candidate / drawn));
      if (away < distance) {
        distance = away;
        best = candidate;
      }
    }
    return best;
  }

  [[nodiscard]] const Hit* current() const noexcept {
    return hits_[current_].active ? &hits_[current_] : nullptr;
  }

  [[nodiscard]] double tune_ratio() const noexcept { return tune_ratio_; }

  [[nodiscard]] double tail_hz(const Hit* hit) const noexcept {
    const double follow = hit != nullptr ? hit->follow : last_follow_;
    return std::clamp(drawn_tail_ * follow * tune_ratio(), 15.0,
                      native_dsp::dsp::nyquist_guard * rate_);
  }

  [[nodiscard]] double frequency(Hit& hit, double x) noexcept {
    return hz_of_y(hit.pitch.at(state_->pitch, x)) * hit.follow * tune_ratio();
  }

  void clear() noexcept {
    for (auto& hit : hits_)
      hit = Hit{};
    std::ranges::fill(comb_, 0.0);
    comb_write_ = 0;
    comb_lp_ = 0.0;
    for (std::size_t i = 0; i < space_.size(); ++i) {
      std::fill(space_[i].begin(), space_[i].end(), 0.0);
      space_lp_[i] = 0.0;
    }
    space_write_ = {};
    rumble_filter_.reset();
    duck_env_ = 0.0;
    duck_gain_ = 1.0;
    for (auto& stage : up_)
      stage.reset();
    for (auto& stage : down_)
      stage.reset();
    for (auto& stage : limit_up_)
      stage.reset();
    for (auto& stage : limit_down_)
      stage.reset();
    tone_filter_.reset();
    for (auto& dc : dc_)
      dc.reset();
    glue_.reset();
    punch_env_ = 0.0;
    punch_rising_ = false;
    peak_ = 0.0;
    last_follow_ = 1.0;
  }

  // One base-rate frame of the hit: the body at 4x, the click and the sub.
  // Returns the mono sum; raw (undriven body) and amp feed the rumble.
  [[nodiscard]] double render_hit(Hit& hit, double body_level, double harmonics, double body_drive,
                                  double click_level, double sub_level,
                                  double length_frames) noexcept {
    double out = 0.0;
    const double inv_os = 1.0 / oversampling_rate;
    if (hit.body) {
      const double dx = 1.0 / length_frames;
      const double x1 = hit.x + dx;
      const double f1 = frequency(hit, std::min(1.0, x1));
      const double a1 = hit.amp.at(state_->amp, std::min(1.0, x1));
      const double d1 = hit.drive.at(state_->drive, std::min(1.0, x1));
      const double drive_amount = body_drive * hit.drive_scale;
      std::array<double, oversampling> fine{};
      const double fine_rate = rate_ * oversampling_rate;
      const double attack_frames = attack_seconds * fine_rate;
      const double end_frames = end_seconds * rate_;
      double raw_last = 0.0;
      double amp_last = 0.0;
      // The frame's four phases: one exact sine and cosine at its start, then
      // a rotation by the frame's mean step (the sweep barely moves in one
      // frame), so the 4x body costs two sincos a frame.
      const double f_mean = 0.5 * (hit.f + f1);
      const double step = two_pi * f_mean / fine_rate;
      const double start = hit.theta + hit.start_phase;
      double sine = std::sin(start);
      double cosine = std::cos(start);
      const double rotate_sine = std::sin(step);
      const double rotate_cosine = std::cos(step);
      const auto partials = static_cast<std::size_t>(
          std::clamp(std::floor(native_dsp::dsp::nyquist_guard * rate_ / std::max(1.0, f_mean)),
                     1.0, static_cast<double>(max_partials)));
      hit.theta += oversampling_rate * step;
      if (hit.theta > two_pi)
        hit.theta -= two_pi * std::floor(hit.theta / two_pi);
      for (std::size_t k = 0; k < oversampling; ++k) {
        const double u = (static_cast<double>(k) + 0.5) * inv_os;
        const double a = hit.a + (a1 - hit.a) * u;
        const double d = (hit.d + (d1 - hit.d) * u) * drive_amount;
        const double next_sine = sine * rotate_cosine + cosine * rotate_sine;
        cosine = cosine * rotate_cosine - sine * rotate_sine;
        sine = next_sine;
        double osc = wave(tables_, hit.wave, sine, cosine, harmonics, partials);
        if (hit.wave_mix < 1.0) {
          const double other = wave(tables_, hit.old_wave, sine, cosine, harmonics, partials);
          osc = other + (osc - other) * raised_cosine(hit.wave_mix);
          hit.wave_mix = std::min(1.0, hit.wave_mix + 1.0 / (switch_seconds * fine_rate));
        }
        // A quarter-millisecond rise, and a 2 ms fall into the length's end.
        const double fine_age =
            static_cast<double>(hit.age) * oversampling_rate + static_cast<double>(k);
        const double rise =
            fine_age < attack_frames ? 0.5 - 0.5 * std::cos(pi * fine_age / attack_frames) : 1.0;
        const double remaining = (1.0 - (hit.x + dx * u)) * length_frames;
        const double fall = remaining < end_frames ? std::max(0.0, remaining / end_frames) : 1.0;
        const double envelope = a * rise * fall;
        const double raw = osc * envelope;
        // The drive shapes the wave, not the level: its curve adds harmonics
        // over the hit and the amp curve alone sets the decay.
        // Grows with the drive across the whole knob: at 5 % a real kick's
        // third harmonic (about -35 dB), not a square wave.
        double shaped = osc;
        if (d > 0.0) {
          const double g = 1.0 + 10.0 * d;
          shaped = osc + d * (fast_tanh(g * osc) / fast_tanh(g) - osc);
        }
        fine[k] = shaped * envelope;
        raw_last = raw;
        amp_last = envelope;
      }
      const double half0 = hit.down[0].process(fine[0], fine[1]);
      const double half1 = hit.down[0].process(fine[2], fine[3]);
      const double body = hit.down[1].process(half0, half1);
      out += body * body_level * hit.gain;
      hit.last_raw = raw_last * body_level * hit.gain;
      hit.last_amp = amp_last * body_level * hit.gain;
      // The sub: a sine at the tail note, following the body's envelope.
      hit.sub_amp += (amp_last - hit.sub_amp) * sub_attack_;
      const double sub_hz = tail_hz(&hit) * octave_ratio_;
      hit.sub_theta += two_pi * sub_hz / rate_;
      if (hit.sub_theta > two_pi)
        hit.sub_theta -= two_pi;
      const double sub = std::sin(hit.sub_theta) * hit.sub_amp * sub_level * hit.gain;
      out += sub;
      hit.last_raw += sub;
      hit.f = f1;
      hit.a = a1;
      hit.d = d1;
      hit.x = x1;
      if (hit.x >= 1.0) {
        hit.body = false;
        hit.last_raw = 0.0;
        hit.last_amp = 0.0;
      }
    } else {
      hit.last_raw = 0.0;
      hit.last_amp = 0.0;
    }
    out += click(hit) * click_level * hit.click_gain * hit.gain;
    ++hit.age;
    if (!hit.body && hit.click_env < 1e-5 && hit.click_env2 < 1e-5)
      hit.active = false;
    return out;
  }

  [[nodiscard]] double click(Hit& hit) noexcept {
    if (hit.click_env < 1e-5 && hit.click_env2 < 1e-5)
      return 0.0;
    const auto& v = values_;
    const double pitch = std::pow(2.0, v[p::click_pitch] / 12.0);
    const double length = v[p::click_length] * 1e-3;
    const double tone = v[p::click_tone];
    const double decay = std::exp(-6.9 / (length * rate_)); // -60 dB at the length.
    const double nyquist = native_dsp::dsp::nyquist_guard * rate_;
    double y = 0.0;
    switch (hit.click_type) {
    case 0: { // Tick: a short sine burst.
      const double f = std::min(nyquist, (1'500.0 + 6'500.0 * tone) * pitch);
      hit.click_theta += two_pi * f / rate_;
      y = std::sin(hit.click_theta) * hit.click_env;
      break;
    }
    case 1: { // Noise, high-passed.
      const auto c = svf(400.0 * std::pow(20.0, tone) * pitch, 0.707, rate_);
      y = hit.click_filter.process(hit.noise.white(), c).high * hit.click_env;
      break;
    }
    case 2: { // Beater: band-passed noise and a felt thump.
      const auto c = svf(600.0 * std::pow(8.0, tone) * pitch, 1.2, rate_);
      hit.click_theta += two_pi * std::min(nyquist, 150.0 * pitch) / rate_;
      y = 1.8 * hit.click_filter.process(hit.noise.white(), c).band * hit.click_env +
          0.4 * std::sin(hit.click_theta) * hit.click_env2;
      hit.click_env2 *= std::pow(decay, 1.0 / 3.0);
      break;
    }
    case 3: { // FM snap: a falling index, kept under Nyquist.
      const double fc = std::min(nyquist, (800.0 + 3'200.0 * tone) * pitch);
      const double fm = fc * 1.41;
      const double limit = std::max(0.0, (nyquist - fc) / fm - 1.0);
      hit.click_theta += two_pi * fc / rate_;
      hit.click_theta2 += two_pi * fm / rate_;
      const double index = std::min(6.0 * hit.click_env, limit);
      y = std::sin(hit.click_theta + index * std::sin(hit.click_theta2)) * hit.click_env;
      break;
    }
    default: { // Sample: the user's transient, then a fast fade after the length.
      const auto* sample = state_->click.get();
      if (sample == nullptr || sample->empty()) {
        hit.click_env = 0.0;
        break;
      }
      const double age = static_cast<double>(hit.age) / rate_;
      const auto i = static_cast<std::size_t>(hit.sample_pos);
      if (i + 1 >= sample->size()) {
        hit.click_env = 0.0;
        break;
      }
      const double frac = hit.sample_pos - static_cast<double>(i);
      const double s = (*sample)[i] + ((*sample)[i + 1] - (*sample)[i]) * frac;
      const double in = std::min(1.0, age / 0.0003); // A 0.3 ms fade-in.
      const double gate = age < length ? 1.0 : hit.click_env;
      y = s * in * gate;
      hit.sample_pos += pitch;
      if (age >= length)
        hit.click_env *= std::exp(-6.9 / (std::max(0.002, 0.5 * length) * rate_));
      hit.click_env2 = 0.0;
      return y;
    }
    }
    hit.click_env *= decay;
    if (hit.click_type != 2)
      hit.click_env2 = 0.0;
    return y;
  }

  // The tuned rumble: a comb at the tail note with a low-pass in its loop,
  // into a small network for space, driven, low-passed and ducked by the kick.
  void render_rumble(double input, double duck_input, double& left, double& right) noexcept {
    comb_delay_ += (comb_target_ - comb_delay_) * comb_glide_;
    const auto size = comb_.size();
    double read = static_cast<double>(comb_write_) - comb_delay_;
    if (read < 0.0)
      read += static_cast<double>(size);
    const auto i0 = std::min(static_cast<std::size_t>(read), size - 1);
    const std::size_t i1 = i0 + 1 == size ? 0 : i0 + 1;
    const double frac = read - static_cast<double>(i0);
    const double delayed = comb_[i0] + (comb_[i1] - comb_[i0]) * frac;
    comb_lp_ = comb_lp_ + comb_c_ * (delayed - comb_lp_);
    comb_[comb_write_] = input * comb_in_ + comb_g_ * comb_lp_;
    comb_write_ = comb_write_ + 1 == size ? 0 : comb_write_ + 1;
    const double comb = comb_lp_;

    // Space: four damped lines through a Householder matrix.
    std::array<double, 4> taps{};
    double sum = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
      const auto at = space_write_[i];
      const std::size_t from = at == space_length_[i] ? 0 : at + 1; // Written len frames ago.
      space_lp_[i] = space_lp_[i] + space_c_ * (space_[i][from] - space_lp_[i]);
      taps[i] = space_lp_[i] * space_g_[i];
      sum += taps[i];
    }
    for (std::size_t i = 0; i < 4; ++i) {
      const auto at = space_write_[i];
      const double sign = (i % 2 == 0) ? 1.0 : -1.0;
      space_[i][at] = taps[i] - 0.5 * sum + sign * 0.35 * comb;
      space_write_[i] = at == space_length_[i] ? 0 : at + 1;
    }
    const double space = space_mix_.next();
    const double wide = width_.next();
    const double space_left = 0.7 * (space_lp_[0] + space_lp_[2]);
    const double space_right = 0.7 * (space_lp_[1] + space_lp_[3]);
    double l = comb * (1.0 - space) + space_left * space;
    double r = comb * (1.0 - space) + space_right * space;
    const double mid = 0.5 * (l + r);
    const double side = 0.5 * (l - r) * wide;
    l = mid + side;
    r = mid - side;

    // Drive, then the tone low-pass.
    const double amount = rumble_drive_.next();
    if (amount > 0.0) {
      const double g = 1.0 + 10.0 * amount;
      const double w = std::min(1.0, 8.0 * amount);
      l += w * (fast_tanh(g * l) / rumble_norm_ - l);
      r += w * (fast_tanh(g * r) / rumble_norm_ - r);
    }
    const auto filtered = rumble_filter_.process(StereoSample{l, r}, rumble_lp_).low;

    // Duck under the kick: a peak follower of the body's envelope.
    duck_env_ = std::max(duck_input, duck_env_ * duck_release_);
    const double target = 1.0 - duck_.next() * std::min(1.0, 1.25 * duck_env_);
    duck_gain_ += (target - duck_gain_) * duck_smooth_;
    // Scaled so a bed under the kick sits mid-knob; full is a wall of rumble.
    const double gain = duck_gain_ * amount_.next() * 0.6 * rumble_on_.next(switch_step_);
    left = filtered.left * gain;
    right = filtered.right * gain;
  }

  void render_frame(double& left, double& right) noexcept {
    const double length_frames = std::max(1.0, length_.next() * 1e-3 * rate_);
    const double tune = tune_.next();
    if (tune != tune_value_) {
      tune_value_ = tune;
      tune_ratio_ = std::pow(2.0, tune / 12.0);
    }
    const double body_level = body_level_.next();
    const double harmonics = harmonics_.next();
    const double body_drive = body_drive_.next();
    const double click_level = click_level_.next();
    const double sub_level = sub_level_.next() * sub_on_.next(switch_step_);

    double mono = 0.0;
    double rumble_in = 0.0;
    double duck_in = 0.0;
    for (auto& hit : hits_) {
      if (!hit.active)
        continue;
      double y = render_hit(hit, body_level, harmonics, body_drive, click_level, sub_level,
                            hit.frames > 0.0 ? hit.frames : length_frames);
      double raw = hit.last_raw;
      double amp = hit.last_amp;
      if (hit.fade_step > 0.0) {
        hit.fade = std::max(0.0, hit.fade - hit.fade_step);
        const double g = raised_cosine(hit.fade);
        y *= g;
        raw *= g;
        amp *= g;
        if (hit.fade <= 0.0)
          hit.active = false;
      }
      mono += y;
      rumble_in += raw;
      duck_in = std::max(duck_in, amp);
    }
    if (const Hit* hit = current(); hit != nullptr)
      last_follow_ = hit->follow;

    double rumble_left = 0.0;
    double rumble_right = 0.0;
    render_rumble(rumble_in, duck_in, rumble_left, rumble_right);
    if (rumble_tap_ != nullptr && rumble_tap_->size() < rumble_tap_->capacity())
      rumble_tap_->push_back(static_cast<float>(0.5 * (rumble_left + rumble_right)));

    // The master: drive (4x), tone, punch, glue, output and the ceiling (2x).
    StereoSample x{mono + rumble_left, mono + rumble_right};
    const double amount = drive_.next();
    std::array<StereoSample, 4> fine{};
    StereoSample a{};
    StereoSample b{};
    up_[0].process(x, a, b);
    up_[1].process(a, fine[0], fine[1]);
    up_[1].process(b, fine[2], fine[3]);
    if (amount > 0.0 || type_mix_ < 1.0) {
      // Up to 18 dB into the shaper, half of it taken back after: more drive
      // is more saturation, not a louder tail.
      const double g = std::pow(10.0, 18.0 * amount / 20.0);
      const double makeup = 1.0 / std::sqrt(g);
      const double w = std::min(1.0, 6.0 * amount);
      const double fade_step = 1.0 / (type_fade_seconds * rate_ * oversampling_rate);
      for (auto& s : fine) {
        const auto shape = [&](std::size_t type, double v) {
          return v + w * (drive_shape(type, g * v) * makeup - v);
        };
        double l = shape(drive_type_, s.left);
        double r = shape(drive_type_, s.right);
        if (type_mix_ < 1.0) {
          const double ol = shape(old_drive_type_, s.left);
          const double orr = shape(old_drive_type_, s.right);
          l = ol + (l - ol) * raised_cosine(type_mix_);
          r = orr + (r - orr) * raised_cosine(type_mix_);
          type_mix_ = std::min(1.0, type_mix_ + fade_step);
        }
        s = {l, r};
      }
    }
    a = down_[1].process(fine[0], fine[1]);
    b = down_[1].process(fine[2], fine[3]);
    x = down_[0].process(a, b);
    x = {dc_[0].process(x.left), dc_[1].process(x.right)};
    x = tone_filter_.process(x, tone_lp_).low;
    if (punch_rising_) {
      punch_env_ += (1.0 - punch_env_) * punch_attack_;
      punch_rising_ = punch_env_ < 0.99;
    } else {
      punch_env_ = punch_env_ * punch_decay_;
    }
    x = x * (1.0 + 1.2 * punch_.next() * punch_env_);
    const auto glued = glue_.process(x.left, x.right);
    const double gain = out_gain_.next();
    x = {glued.left * gain, glued.right * gain};
    StereoSample c{};
    StereoSample d{};
    limit_up_[0].process(x, c, d);
    c = {soft_ceiling(c.left), soft_ceiling(c.right)};
    d = {soft_ceiling(d.left), soft_ceiling(d.right)};
    x = limit_down_[0].process(c, d);
    left = std::clamp(soft_ceiling(x.left, guard_knee, guard_ceiling), -hard_limit, hard_limit);
    right = std::clamp(soft_ceiling(x.right, guard_knee, guard_ceiling), -hard_limit, hard_limit);
  }

  double rate_;
  std::span<const double> values_{};
  const AnvilState* state_ = nullptr;
  bool primed_ = false;
  std::array<Hit, 2> hits_{};
  std::size_t current_ = 0;
  std::uint64_t hit_count_ = 0;
  std::size_t wave_ = 0;
  bool note_mode_ = false;
  Ramp length_{}, tune_{}, body_level_{}, harmonics_{}, body_drive_{}, click_level_{}, sub_level_{},
      amount_{}, duck_{}, rumble_drive_{}, space_mix_{}, width_{}, drive_{}, punch_{}, out_gain_{};
  SwitchFade sub_on_{};
  SwitchFade rumble_on_{};
  double switch_step_ = 0.0;
  double octave_ratio_ = 1.0;
  double tune_value_ = 0.0;
  double tune_ratio_ = 1.0;
  double drawn_tail_ = 50.0;
  WaveTables tables_{};
  double sub_attack_ = 1.0;
  double last_follow_ = 1.0;
  const LockState* lock_ = nullptr;
  std::uint64_t frame_ = 0;
  bool lock_retunes_ = false;
  double lock_lookahead_ = 0.0;
  // Rumble.
  std::vector<double> comb_;
  std::size_t comb_write_ = 0;
  double comb_delay_ = 100.0;
  double comb_target_ = 100.0;
  double comb_glide_ = 0.0;
  double comb_lp_ = 0.0;
  double comb_c_ = 0.1;
  double comb_g_ = 0.0;
  double comb_in_ = 0.0;
  std::array<std::vector<double>, 4> space_{};
  std::array<std::size_t, 4> space_length_{};
  std::array<double, 4> space_lp_{};
  std::array<double, 4> space_g_{};
  double space_c_ = 0.1;
  std::array<std::size_t, 4> space_write_{};
  SvfCoefficients rumble_lp_{};
  BasicZdfSvf<StereoSample> rumble_filter_{};
  double rumble_norm_ = 1.0;
  double duck_env_ = 0.0;
  double duck_gain_ = 1.0;
  double duck_release_ = 0.0;
  double duck_smooth_ = 0.0;
  std::vector<float>* rumble_tap_ = nullptr;
  // Master.
  std::size_t drive_type_ = 0;
  std::size_t old_drive_type_ = 0;
  double type_mix_ = 1.0;
  std::array<StereoUpsampler2x, 2> up_{};
  std::array<StereoDownsampler2x, 2> down_{};
  std::array<StereoUpsampler2x, 1> limit_up_{};
  std::array<StereoDownsampler2x, 1> limit_down_{};
  std::array<DcBlocker, 2> dc_{};
  SvfCoefficients tone_lp_{};
  BasicZdfSvf<StereoSample> tone_filter_{};
  double punch_env_ = 0.0;
  double punch_decay_ = 0.0;
  double punch_attack_ = 1.0;
  bool punch_rising_ = false;
  GlueCompressor glue_{};
  double peak_ = 0.0;
  double peak_release_ = 0.0;
  double last_left_ = 0.0;
  double last_right_ = 0.0;
  double reset_left_ = 0.0;
  double reset_right_ = 0.0;
  double reset_frames_ = 1.0;
  std::uint32_t reset_remaining_ = 0;
  int last_note_ = -1;
  int last_velocity_ = 0;
};

std::unique_ptr<SynthKernel> create_kernel(double sample_rate) {
  return std::make_unique<AnvilKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  // The hit, its click, then the rumble: a comb and a network that share its
  // decay fall to -100 dB within 2.4 decays; then the glue's release.
  // Sync and Note lengths are known only when played: their longest.
  double seconds = (choice(values[p::length_mode]) == anvil_table::length_time
                        ? values[p::length] * 1e-3
                        : anvil_table::longest_hit_seconds) +
                   0.05;
  if (choice(values[p::rumble_on]) == 1)
    seconds += 2.4 * values[p::rumble_decay];
  seconds += glue_release_seconds + 0.05;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

// Bass note and Keep tone both lock: Nova follows the hits either way.
bool lock_enabled(std::span<const double> values) {
  return values.size() == anvil_table::parameters.size() &&
         choice(values[p::lock]) != static_cast<std::size_t>(lock_choice_off);
}

// A hit's body amplitude for a velocity, as the kernel shapes it: the drawn
// amp curve over the hit's length, scaled by the velocity's level.
std::vector<float> lock_envelope(std::span<const double> values,
                                 const project::NativeSynthDocument& document,
                                 std::uint8_t velocity, std::uint32_t note_frames, double bpm,
                                 double sample_rate) {
  if (values.size() != anvil_table::parameters.size() || sample_rate <= 0.0)
    return {};
  Curve amp = default_state().amp;
  for (const auto& curve : document.curves) {
    if (curve.id == amp_curve)
      amp = Curve{curve.points};
  }
  const double v = std::clamp(velocity / 127.0, 0.0, 1.0);
  const double gain = 1.0 - values[p::velocity] * (1.0 - std::pow(v, 1.6));
  const double length =
      hit_seconds(values, steady(bpm, sample_rate), note_frames, sample_rate) * sample_rate;
  const auto count = static_cast<std::size_t>(std::ceil(length / lock_envelope_frames)) + 1;
  std::vector<float> envelope(count);
  CurveCursor cursor;
  for (std::size_t i = 0; i < count; ++i) {
    const double x = std::min(1.0, static_cast<double>(i * lock_envelope_frames) / length);
    envelope[i] = static_cast<float>(gain * std::clamp(cursor.at(amp, x), 0.0, 1.0));
  }
  envelope.back() = 0.0F;
  return envelope;
}

const EngineDescriptor descriptor{"anvil", "Anvil", 1, anvil_table::parameters,
                                  anvil_presets::presets};
// Kicks ignore the pedals: the kernel models them by doing nothing. Lock
// follows Nova's notes (the bass) and gives Nova each hit's envelope.
const SynthEngine anvil_engine{
    &descriptor,       &create_kernel,   &tail_frames,     true,          &validate_document,
    &prepare_document, &preset_document, lock_bass_engine, &lock_enabled, &lock_envelope};

} // namespace

Preview render_preview(std::span<const double> values, const project::NativeSynthDocument& document,
                       const SynthMedia* media, std::uint32_t sample_rate, double bpm,
                       std::uint8_t key, std::uint8_t velocity) {
  Preview preview;
  if (values.size() != anvil_table::parameters.size() || sample_rate == 0)
    return preview;
  const double rate = sample_rate;
  AnvilKernel kernel(rate);
  const SynthMedia none;
  auto state = prepare_document(document, media != nullptr ? *media : none, rate, nullptr);
  std::shared_ptr<const SynthDocumentState> held;
  if (state) {
    held = state.value();
    kernel.adopt_document(*held);
  }
  const double length = hit_seconds(values, steady(bpm, rate), 0, rate) * rate;
  const double beat = 60.0 / std::clamp(bpm, 20.0, 999.0) * rate;
  const auto total = static_cast<std::size_t>(std::ceil(std::max(length, beat)));
  preview.kick_frames = static_cast<std::size_t>(std::ceil(length));
  preview.master.resize(total);
  preview.rumble.reserve(total);
  kernel.tap_rumble(&preview.rumble);
  std::vector<engine::StereoFrame> block(control_interval_frames);
  const SynthTransport transport = steady(std::clamp(bpm, 20.0, 999.0), rate);
  double peak = 0.0;
  for (std::size_t frame = 0; frame < total; frame += control_interval_frames) {
    const std::size_t count = std::min<std::size_t>(control_interval_frames, total - frame);
    kernel.control(values, control_interval_frames, transport);
    if (frame == 0) {
      kernel.note_on({{1, 1}, {0, 1}, key, velocity});
      preview.tail_hz = kernel.sounding_tail_hz();
    }
    std::ranges::fill(block, engine::StereoFrame{0.0F, 0.0F});
    kernel.render(std::span(block).first(count));
    for (std::size_t i = 0; i < count; ++i) {
      preview.master[frame + i] = 0.5F * (block[i].left + block[i].right);
      peak = std::max({peak, std::fabs(static_cast<double>(block[i].left)),
                       std::fabs(static_cast<double>(block[i].right))});
    }
  }
  kernel.tap_rumble(nullptr);
  preview.peak_db = peak > 1e-6 ? 20.0 * std::log10(peak) : -120.0;
  return preview;
}

const SynthEngine& engine() { return anvil_engine; }

} // namespace oscillate::native_dsp::synth::anvil
