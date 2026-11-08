#include "oscillate/clay_dsp/render.hpp"

#include "detect.hpp"
#include "spectrum.hpp"
#include "synth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::clay_dsp {
namespace {

using foundation::ErrorCode;

// An envelope correction never raises a partial by more than this (natural
// log), so a noisy envelope edge cannot blow a partial up.
constexpr double max_envelope_gain = 3.0;

struct TrackVoice {
  detail::PartialTrack track;
  std::array<double, 2> gain{};
};

struct SegmentVoice {
  std::array<detail::GrainSegment, 2> channel;
  double gain = 1.0;
};

[[nodiscard]] double lerp(double a, double b, double t) noexcept { return a + (b - a) * t; }

// The note's envelope (natural log) at fractional hop `tau`, at `hz`.
// A point on a note's envelope: fractional hop `tau` at `hz`.
struct EnvelopePoint {
  double tau = 0.0;
  double hz = 0.0;
};
[[nodiscard]] double envelope(const Note& note, EnvelopePoint point) {
  const double tau = point.tau;
  const double hz = point.hz;
  const auto rows = note.envelope.size() / envelope_bands;
  if (rows == 0)
    return 0.0;
  const double row =
      std::clamp(tau / static_cast<double>(envelope_stride), 0.0, static_cast<double>(rows - 1));
  const auto r0 = std::min(static_cast<std::size_t>(row), rows - 1);
  const auto r1 = std::min(r0 + 1, rows - 1);
  const double band = envelope_band_of(hz);
  const auto b0 = std::min(static_cast<std::size_t>(band), envelope_bands - 1);
  const auto b1 = std::min(b0 + 1, envelope_bands - 1);
  const double tb = band - static_cast<double>(b0);
  const auto at = [&](std::size_t r) {
    return lerp(note.envelope[r * envelope_bands + b0], note.envelope[r * envelope_bands + b1], tb);
  };
  return lerp(at(r0), at(r1), row - static_cast<double>(r0));
}

[[nodiscard]] double read(const std::vector<float>& values, double tau) {
  if (values.empty())
    return 0.0;
  const double clamped = std::clamp(tau, 0.0, static_cast<double>(values.size() - 1));
  const auto k0 = std::min(static_cast<std::size_t>(clamped), values.size() - 1);
  const auto k1 = std::min(k0 + 1, values.size() - 1);
  return lerp(values[k0], values[k1], clamped - static_cast<double>(k0));
}

[[nodiscard]] bool pitch_kept(const NoteEdit& edit) noexcept {
  return edit.pitch_cents == 0.0 && edit.modulation == 1.0 && edit.drift == 1.0 && !edit.start;
}

// The note as edited: its tracks resampled onto the new placement, its pitch
// curve rebuilt from centre, drift and vibrato, and each partial's level
// moved along the (possibly shifted) envelope.
[[nodiscard]] detail::PartialTrack edited_track(const Note& note, const NoteEdit& edit,
                                                std::int64_t hop, std::uint32_t rate, double fade) {
  if (note.hops() == 0 || note.partial_count == 0)
    return {};
  const auto start = edit.start.value_or(note.start);
  const auto end = edit.end.value_or(note.end);
  const double scale = static_cast<double>(end - start) /
                       static_cast<double>(std::max<std::int64_t>(1, note.end - note.start));
  const auto source_first = static_cast<double>(note.first_hop * hop);
  // Output hop 0 sits where the note's first hop lands after the move.
  const auto first_centre = start - static_cast<std::int64_t>(std::llround(
                                        (static_cast<double>(note.start) - source_first) * scale));
  const auto extent = static_cast<double>(note.hops() - 1) * static_cast<double>(hop) * scale;
  detail::PartialTrack track;
  track.first_centre = first_centre;
  track.hop = hop;
  track.hops = static_cast<std::size_t>(std::ceil(extent / static_cast<double>(hop))) + 1;
  track.partials = note.partial_count;
  track.start = static_cast<double>(start);
  track.end = static_cast<double>(end);
  track.fade = fade;
  const auto size = track.hops * track.partials;
  track.omega.assign(size, 0.0);
  track.amplitude.assign(size, 0.0);
  track.phase.assign(size, 0.0);
  const double gain = std::pow(10.0, edit.gain_db / 20.0);
  const double formant = std::exp2(-edit.formant_cents / cents_per_octave);
  const double ceiling = detail::partial_ceiling * rate;
  const double to_omega = detail::two_pi / static_cast<double>(rate);
  const bool locked = pitch_kept(edit);
  for (std::size_t j = 0; j < track.hops; ++j) {
    const double centre = static_cast<double>(first_centre + static_cast<std::int64_t>(j) * hop);
    const double source_frame =
        static_cast<double>(note.start) + (centre - static_cast<double>(start)) / scale;
    const double tau = std::clamp((source_frame - source_first) / static_cast<double>(hop), 0.0,
                                  static_cast<double>(note.hops() - 1));
    const auto k0 = std::min(static_cast<std::size_t>(tau), note.hops() - 1);
    const auto k1 = std::min(k0 + 1, note.hops() - 1);
    const double t = tau - static_cast<double>(k0);
    const double pitch = read(note.pitch, tau);
    const double original = pitch > 0.0 ? hz_to_cents(pitch) : note.centre;
    const double target = note.centre + edit.pitch_cents + read(note.drift, tau) * edit.drift +
                          read(note.modulation, tau) * edit.modulation;
    // A locked edit keeps the measured pitch exactly: rounding drift and
    // vibrato separately must not nudge a gain-only edit's partials.
    const double ratio =
        !locked && pitch > 0.0 ? std::exp2((target - original) / cents_per_octave) : 1.0;
    for (std::size_t h = 0; h < track.partials; ++h) {
      const auto& p0 = note.partials[k0 * note.partial_count + h];
      const auto& p1 = note.partials[k1 * note.partial_count + h];
      const double f = lerp(p0.frequency, p1.frequency, t);
      const double a = lerp(p0.amplitude, p1.amplitude, t);
      const double moved = f * ratio;
      const auto i = j * track.partials + h;
      track.omega[i] = moved * to_omega;
      if (moved >= ceiling || f <= 0.0)
        continue;
      double correction = 0.0;
      if (moved != f || formant != 1.0)
        correction =
            std::min(max_envelope_gain, envelope(note, {.tau = tau, .hz = moved * formant}) -
                                            envelope(note, {.tau = tau, .hz = f}));
      track.amplitude[i] = a * gain * std::exp(correction);
      if (locked)
        track.phase[i] = t < 0.5 ? p0.phase : p1.phase;
    }
  }
  if (!locked) {
    // Integrate the new frequencies from the note's measured starting phase.
    for (std::size_t h = 0; h < track.partials; ++h) {
      track.phase[h] = note.partials.empty() ? 0.0 : note.partials[h].phase;
      for (std::size_t j = 1; j < track.hops; ++j) {
        const auto i = j * track.partials + h;
        const auto previous = i - track.partials;
        track.phase[i] =
            std::remainder(track.phase[previous] + 0.5 * (track.omega[previous] + track.omega[i]) *
                                                       static_cast<double>(hop),
                           detail::two_pi);
      }
    }
  }
  return track;
}

} // namespace

struct Renderer::State {
  std::uint32_t rate = 0;
  std::int64_t frames = 0;
  bool stereo = false;
  std::vector<float> left;
  std::vector<float> right;
  std::vector<TrackVoice> tracks;
  std::vector<SegmentVoice> segments;
  std::int64_t length = 0;
};

Renderer::Renderer(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
Renderer::~Renderer() = default;

foundation::Result<Renderer> Renderer::create(const Take& take, std::span<const float> left,
                                              std::span<const float> right,
                                              std::span<const NoteEdit> edits) {
  using R = foundation::Result<Renderer>;
  if (static_cast<std::int64_t>(left.size()) != take.frames ||
      (!right.empty() && right.size() != left.size()) || take.hop <= 0 || take.sample_rate == 0)
    return R::failure({ErrorCode::invalid_argument, "The audio is not the analysed audio"});
  if (auto valid = validate(take, edits); !valid)
    return R::failure(valid.error());
  // Every sinusoidal note is synthesised (as a residual or an edit): its
  // tracks must cover all its hops.
  for (const auto& note : take.notes) {
    if (note.kind != NoteKind::sinusoidal)
      continue;
    const auto hops = note.hops();
    if (note.amplitude.size() < hops || note.drift.size() < hops || note.modulation.size() < hops ||
        (note.partial_count != 0 && note.partials.size() / note.partial_count < hops))
      return R::failure({ErrorCode::invalid_argument, "A note's tracks are shorter than its hops"});
  }
  auto state = std::make_unique<State>();
  state->rate = take.sample_rate;
  state->frames = take.frames;
  state->stereo = !right.empty();
  state->left.assign(left.begin(), left.end());
  state->right.assign(right.begin(), right.end());
  state->length = take.frames;
  const auto rate = take.sample_rate;
  const auto hop = take.hop;
  const double fade = note_fade_seconds * rate;
  const auto grain = detail::hop_frames(grain_seconds / 2.0, rate);
  const auto channels = state->stereo ? std::size_t{2} : std::size_t{1};
  const std::array<const std::vector<float>*, 2> audio{&state->left, &state->right};

  // A slice of each channel of the original, from `from` for `count` frames.
  const auto slice = [&](std::size_t channel, std::int64_t from, std::int64_t to) {
    std::vector<float> out(static_cast<std::size_t>(std::max<std::int64_t>(0, to - from)), 0.0F);
    const auto& source = *audio[channel];
    for (auto i = std::max<std::int64_t>(0, from); i < std::min(to, take.frames); ++i)
      out[static_cast<std::size_t>(i - from)] = source[static_cast<std::size_t>(i)];
    return out;
  };
  // A segment on this render's grain hop and fade.
  const auto segment = [&](detail::GrainSegment s) {
    s.grain_hop = grain;
    s.fade = fade;
    return s;
  };
  // What is left of the original over [from, to) once every note's partials
  // are taken out.
  const auto residual = [&](std::size_t channel, std::int64_t from, std::int64_t to) {
    auto out = slice(channel, from, to);
    for (const auto& other : take.notes) {
      if (other.kind != NoteKind::sinusoidal)
        continue;
      const auto track = detail::original_track(other, hop, rate, fade);
      if (track.last_frame() <= from || track.first_frame() >= to)
        continue;
      std::vector<float> synth(out.size(), 0.0F);
      detail::add_track(track, from, synth, 1.0);
      for (std::size_t i = 0; i < out.size(); ++i)
        out[i] -= other.pan[channel] * synth[i];
    }
    return out;
  };

  for (const auto& edit : edits) {
    if (edit.identity())
      continue;
    const auto& note = *take.find(edit.note);
    const auto start = edit.start.value_or(note.start);
    const auto end = edit.end.value_or(note.end);
    const double time_rate = static_cast<double>(note.end - note.start) /
                             static_cast<double>(std::max<std::int64_t>(1, end - start));
    const double gain = std::pow(10.0, edit.gain_db / 20.0);
    const auto reach = static_cast<std::int64_t>(std::ceil(fade)) + grain;
    state->length = std::max(state->length, end + reach);
    if (note.kind == NoteKind::segment) {
      const auto from = note.start - reach;
      const auto to = note.end + reach;
      if (!edit.duplicate) {
        SegmentVoice removal;
        removal.gain = -1.0;
        for (std::size_t c = 0; c < channels; ++c)
          removal.channel[c] = segment({.source = slice(c, from, to),
                                        .source_at_start = static_cast<double>(note.start - from),
                                        .out_start = note.start,
                                        .out_end = note.end});
        state->segments.push_back(std::move(removal));
      }
      SegmentVoice addition;
      addition.gain = gain;
      for (std::size_t c = 0; c < channels; ++c)
        addition.channel[c] = segment({.source = slice(c, from, to),
                                       .source_at_start = static_cast<double>(note.start - from),
                                       .time_rate = time_rate,
                                       .pitch_rate = std::exp2(edit.pitch_cents / cents_per_octave),
                                       .out_start = start,
                                       .out_end = end});
      state->segments.push_back(std::move(addition));
      continue;
    }
    if (!edit.duplicate) {
      TrackVoice removal;
      removal.track = detail::original_track(note, hop, rate, fade);
      removal.gain = {-note.pan[0], -note.pan[1]};
      state->tracks.push_back(std::move(removal));
      // A melodic note that moves takes its breath and noise with it.
      if (edit.start && take.mode == Mode::melodic) {
        const auto from = note.start - reach;
        const auto to = note.end + reach;
        SegmentVoice taken;
        SegmentVoice placed;
        taken.gain = -1.0;
        placed.gain = gain;
        for (std::size_t c = 0; c < channels; ++c) {
          auto rest = residual(c, from, to);
          taken.channel[c] = segment({.source = rest,
                                      .source_at_start = static_cast<double>(note.start - from),
                                      .out_start = note.start,
                                      .out_end = note.end});
          placed.channel[c] = segment({.source = std::move(rest),
                                       .source_at_start = static_cast<double>(note.start - from),
                                       .time_rate = time_rate,
                                       .out_start = start,
                                       .out_end = end});
        }
        state->segments.push_back(std::move(taken));
        state->segments.push_back(std::move(placed));
      }
    }
    TrackVoice addition;
    addition.track = edited_track(note, edit, hop, rate, fade);
    addition.gain = {note.pan[0], note.pan[1]};
    state->tracks.push_back(std::move(addition));
  }
  return R::success(Renderer(std::move(state)));
}

void Renderer::render(std::int64_t first, std::span<float> left, std::span<float> right) const {
  const auto& s = *state_;
  const auto count = static_cast<std::int64_t>(left.size());
  const auto copy = [&](const std::vector<float>& source, std::span<float> out) {
    for (std::int64_t i = 0; i < count; ++i) {
      const auto at = first + i;
      out[static_cast<std::size_t>(i)] =
          at >= 0 && at < s.frames ? source[static_cast<std::size_t>(at)] : 0.0F;
    }
  };
  copy(s.left, left);
  const bool stereo = s.stereo && right.size() == left.size();
  if (stereo)
    copy(s.right, right);
  std::vector<float> voice;
  for (const auto& v : s.tracks) {
    if (v.track.last_frame() <= first || v.track.first_frame() >= first + count)
      continue;
    voice.assign(left.size(), 0.0F);
    detail::add_track(v.track, first, voice, 1.0);
    for (std::size_t i = 0; i < voice.size(); ++i) {
      left[i] += static_cast<float>(v.gain[0] * voice[i]);
      if (stereo)
        right[i] += static_cast<float>(v.gain[1] * voice[i]);
    }
  }
  for (const auto& v : s.segments) {
    detail::add_segment(v.channel[0], first, left, v.gain);
    if (stereo)
      detail::add_segment(v.channel[1], first, right, v.gain);
  }
}

std::int64_t Renderer::length() const noexcept { return state_->length; }

} // namespace oscillate::clay_dsp
