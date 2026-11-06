#include "oscillate/clay_dsp/analysis.hpp"

#include "detect.hpp"
#include "spectrum.hpp"
#include "synth.hpp"

#include "oscillate/clay_dsp/render.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace oscillate::clay_dsp {
namespace detail {

std::int64_t hop_frames(double seconds, std::uint32_t sample_rate) noexcept {
  return std::max<std::int64_t>(1, std::llround(seconds * sample_rate));
}

double db_to_ratio(double db) noexcept { return std::pow(10.0, db / 20.0); }

} // namespace detail

namespace {

using foundation::ErrorCode;

constexpr double tiny = 1e-20;

[[nodiscard]] foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

// Each channel's share of the note, and how much of the sound under the
// note its partials leave unexplained: the residual (the clip minus every
// note's partials, so in a chord the other notes do not count as noise)
// against the clip, over the note's span.
void measure_mix(Note& note, const Audio& audio, std::span<const float> mono,
                 std::span<const float> residual, std::int64_t hop) {
  if (note.kind == NoteKind::segment) {
    note.noise_share = 0.0F;
    return;
  }
  const auto track =
      detail::original_track(note, hop, audio.sample_rate, note_fade_seconds * audio.sample_rate);
  const auto first = std::max<std::int64_t>(0, track.first_frame());
  const auto last = std::min(static_cast<std::int64_t>(mono.size()), track.last_frame());
  if (last <= first)
    return;
  std::vector<float> synth(static_cast<std::size_t>(last - first), 0.0F);
  detail::add_track(track, first, synth, 1.0);
  double ss = 0.0;
  double mm = 0.0;
  double rr = 0.0;
  double ls = 0.0;
  double rs = 0.0;
  for (std::size_t i = 0; i < synth.size(); ++i) {
    const auto at = static_cast<std::size_t>(first) + i;
    const double s = synth[i];
    ss += s * s;
    mm += static_cast<double>(mono[at]) * mono[at];
    rr += static_cast<double>(residual[at]) * residual[at];
    ls += static_cast<double>(audio.left[at]) * s;
    if (!audio.right.empty())
      rs += static_cast<double>(audio.right[at]) * s;
  }
  note.noise_share = static_cast<float>(mm > tiny ? std::clamp(rr / mm, 0.0, 1.0) : 0.0);
  if (ss > tiny) {
    note.pan[0] = static_cast<float>(ls / ss);
    note.pan[1] = static_cast<float>(audio.right.empty() ? ls / ss : rs / ss);
  }
}

} // namespace

foundation::Result<Take> analyse(const Audio& audio, const AnalysisOptions& options,
                                 const std::atomic<bool>* cancelled) {
  using R = foundation::Result<Take>;
  if (audio.sample_rate < min_sample_rate || audio.sample_rate > max_sample_rate)
    return R::failure(invalid("Unsupported sample rate"));
  if (!audio.right.empty() && audio.right.size() != audio.left.size())
    return R::failure(invalid("Channels differ in length"));
  if (static_cast<double>(audio.left.size()) > max_take_seconds * audio.sample_rate)
    return R::failure({ErrorCode::resource_exhausted, "Clips are analysed up to an hour"});
  if (!std::isfinite(options.minimum_hz) || !std::isfinite(options.maximum_hz) ||
      options.minimum_hz < envelope_low_hz || options.maximum_hz <= 2.0 * options.minimum_hz ||
      options.maximum_hz > detail::partial_ceiling * audio.sample_rate / 2.0 ||
      !(options.minimum_note_seconds > 0.0) || options.max_polyphony == 0)
    return R::failure(invalid("Invalid analysis options"));
  for (const float x : audio.left) {
    if (!std::isfinite(x))
      return R::failure(invalid("The audio holds a non-finite sample"));
  }
  std::vector<float> mono(audio.left.begin(), audio.left.end());
  if (!audio.right.empty()) {
    for (std::size_t i = 0; i < mono.size(); ++i) {
      if (!std::isfinite(audio.right[i]))
        return R::failure(invalid("The audio holds a non-finite sample"));
      mono[i] = 0.5F * (audio.left[i] + audio.right[i]);
    }
  }
  Take take;
  take.mode = options.mode;
  take.sample_rate = audio.sample_rate;
  take.frames = static_cast<std::int64_t>(mono.size());
  const detail::Context context{mono, audio.sample_rate, &options, cancelled};
  bool finished = false;
  switch (options.mode) {
  case Mode::melodic:
    take.hop = detail::hop_frames(detail::melodic_hop_seconds, audio.sample_rate);
    finished = detail::detect_melodic(context, take.hop, take.notes);
    break;
  case Mode::polyphonic:
    take.hop = detail::hop_frames(detail::polyphonic_hop_seconds, audio.sample_rate);
    finished = detail::detect_polyphonic(context, take.hop, take.notes);
    break;
  case Mode::percussive:
    take.hop = detail::hop_frames(detail::polyphonic_hop_seconds, audio.sample_rate);
    finished = detail::detect_percussive(context, audio.left, audio.right, take.hop, take.notes);
    break;
  }
  if (!finished)
    return R::failure({ErrorCode::cancelled, "Analysis was cancelled"});
  std::ranges::stable_sort(take.notes, {}, &Note::start);
  const double fade = note_fade_seconds * audio.sample_rate;
  std::vector<float> residual = mono;
  for (auto& note : take.notes) {
    note.id = take.next_id++;
    measure_pitch(note, take.hop, take.sample_rate);
    if (note.envelope.empty())
      note.envelope.assign(((note.hops() + envelope_stride - 1) / envelope_stride) * envelope_bands,
                           0.0F);
    if (note.kind == NoteKind::sinusoidal)
      detail::add_track(detail::original_track(note, take.hop, audio.sample_rate, fade), 0,
                        residual, -1.0);
  }
  for (auto& note : take.notes)
    measure_mix(note, audio, mono, residual, take.hop);
  return R::success(std::move(take));
}

} // namespace oscillate::clay_dsp
