#include "oscillate/clay_dsp/take.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::clay_dsp {
namespace {

using foundation::ErrorCode;
using foundation::Result;

// Drift is the pitch averaged over this long; what is left is vibrato. A
// moving average this long has its first null at 5 Hz.
constexpr double drift_window_seconds = 0.2;

[[nodiscard]] foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

[[nodiscard]] Note* find_mutable(Take& take, std::uint32_t id) noexcept {
  for (auto& note : take.notes) {
    if (note.id == id)
      return &note;
  }
  return nullptr;
}

[[nodiscard]] std::size_t envelope_rows(std::size_t hops) noexcept {
  return (hops + envelope_stride - 1) / envelope_stride;
}

// The note's envelope at one of its hops, interpolated between rows.
void envelope_at(const Note& note, double hop_index, std::span<float> out) {
  const auto rows = note.envelope.size() / envelope_bands;
  if (rows == 0) {
    std::fill(out.begin(), out.end(), 0.0F);
    return;
  }
  const double row = std::clamp(hop_index / static_cast<double>(envelope_stride), 0.0,
                                static_cast<double>(rows - 1));
  const auto below = std::min(static_cast<std::size_t>(row), rows - 1);
  const auto above = std::min(below + 1, rows - 1);
  const auto t = static_cast<float>(row - static_cast<double>(below));
  for (std::size_t b = 0; b < envelope_bands; ++b)
    out[b] = (1.0F - t) * note.envelope[below * envelope_bands + b] +
             t * note.envelope[above * envelope_bands + b];
}

// Hops [from, to) of `note` as a note of their own (envelope resampled).
[[nodiscard]] Note slice(const Note& note, std::size_t from, std::size_t to) {
  Note part = note;
  part.first_hop = note.first_hop + static_cast<std::int64_t>(from);
  const auto take_range = [&](const std::vector<float>& values) {
    if (values.size() < to)
      return std::vector<float>(to - from, 0.0F);
    return std::vector<float>(values.begin() + static_cast<std::ptrdiff_t>(from),
                              values.begin() + static_cast<std::ptrdiff_t>(to));
  };
  part.pitch = take_range(note.pitch);
  part.amplitude = take_range(note.amplitude);
  part.drift = take_range(note.drift);
  part.modulation = take_range(note.modulation);
  part.partials.assign(
      note.partials.begin() + static_cast<std::ptrdiff_t>(from * note.partial_count),
      note.partials.begin() + static_cast<std::ptrdiff_t>(to * note.partial_count));
  const auto hops = to - from;
  part.envelope.assign(envelope_rows(hops) * envelope_bands, 0.0F);
  for (std::size_t r = 0; r * envelope_stride < hops; ++r)
    envelope_at(note, static_cast<double>(from + r * envelope_stride),
                std::span(part.envelope).subspan(r * envelope_bands, envelope_bands));
  return part;
}

void sort_notes(Take& take) {
  std::ranges::sort(take.notes, [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.id < b.id;
  });
}

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

} // namespace

double hz_to_cents(double hz) noexcept {
  return reference_cents + cents_per_octave * std::log2(hz / reference_hz);
}

double cents_to_hz(double cents) noexcept {
  return reference_hz * std::exp2((cents - reference_cents) / cents_per_octave);
}

double envelope_band_of(double hz) noexcept {
  const double clamped = std::clamp(hz, envelope_low_hz, envelope_high_hz);
  return std::log(clamped / envelope_low_hz) / std::log(envelope_high_hz / envelope_low_hz) *
         static_cast<double>(envelope_bands - 1);
}

double Note::mean_amplitude() const noexcept {
  if (amplitude.empty())
    return 0.0;
  const double sum = std::accumulate(amplitude.begin(), amplitude.end(), 0.0);
  return sum / static_cast<double>(amplitude.size());
}

const Note* Take::find(std::uint32_t id) const noexcept {
  for (const auto& note : notes) {
    if (note.id == id)
      return &note;
  }
  return nullptr;
}

bool NoteEdit::identity() const noexcept {
  return !duplicate && pitch_cents == 0.0 && modulation == 1.0 && drift == 1.0 &&
         formant_cents == 0.0 && gain_db == 0.0 && !start && !end;
}

foundation::Result<void> validate(const Take& take, std::span<const NoteEdit> edits) {
  std::set<std::uint32_t> edited;
  for (const auto& edit : edits) {
    const auto* note = take.find(edit.note);
    if (note == nullptr)
      return Result<void>::failure({ErrorCode::not_found, "An edit names a note the take lacks"});
    if (!edit.duplicate && !edited.insert(edit.note).second)
      return Result<void>::failure(invalid("A note has two edits"));
    if (!finite(edit.pitch_cents) || std::abs(edit.pitch_cents) > max_edit_cents ||
        !finite(edit.formant_cents) || std::abs(edit.formant_cents) > max_edit_cents)
      return Result<void>::failure(invalid("A pitch or formant shift is out of range"));
    if (!finite(edit.modulation) || edit.modulation < 0.0 ||
        edit.modulation > max_modulation_scale || !finite(edit.drift) || edit.drift < 0.0 ||
        edit.drift > max_drift_scale)
      return Result<void>::failure(invalid("A modulation or drift scale is out of range"));
    if (!finite(edit.gain_db) || edit.gain_db < min_edit_gain_db || edit.gain_db > max_edit_gain_db)
      return Result<void>::failure(invalid("A gain is out of range"));
    if (edit.start.has_value() != edit.end.has_value())
      return Result<void>::failure(invalid("A placement needs both a start and an end"));
    if (edit.start) {
      if (*edit.start < 0 || *edit.end <= *edit.start)
        return Result<void>::failure(invalid("A placement is empty or before the clip"));
      if (*edit.end > max_placement_frame)
        return Result<void>::failure(invalid("A placement ends too far out to render"));
      const double scale = static_cast<double>(*edit.end - *edit.start) /
                           static_cast<double>(std::max<std::int64_t>(1, note->end - note->start));
      if (scale > max_time_scale || scale < 1.0 / max_time_scale)
        return Result<void>::failure(invalid("A note is stretched too far"));
    }
  }
  return Result<void>::success();
}

void measure_pitch(Note& note, std::int64_t hop, std::uint32_t sample_rate) {
  const auto hops = note.hops();
  note.drift.assign(hops, 0.0F);
  note.modulation.assign(hops, 0.0F);
  if (hops == 0 || hop <= 0)
    return;
  std::vector<std::pair<double, double>> weighted;
  weighted.reserve(hops);
  for (std::size_t k = 0; k < hops; ++k) {
    if (note.pitch[k] > 0.0F)
      weighted.emplace_back(hz_to_cents(note.pitch[k]),
                            std::max(1e-12, static_cast<double>(note.amplitude[k])));
  }
  if (weighted.empty())
    return;
  auto sorted = weighted;
  std::ranges::sort(sorted);
  const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0,
                                       [](double s, const auto& p) { return s + p.second; });
  double running = 0.0;
  note.centre = sorted.back().first;
  for (const auto& [cents, weight] : sorted) {
    running += weight;
    if (running >= total / 2.0) {
      note.centre = cents;
      break;
    }
  }
  if (note.kind == NoteKind::segment)
    return;
  std::vector<double> cents(hops, note.centre);
  for (std::size_t k = 0; k < hops; ++k) {
    if (note.pitch[k] > 0.0F)
      cents[k] = hz_to_cents(note.pitch[k]);
  }
  const auto half = static_cast<std::size_t>(
      std::max(1.0, std::round(drift_window_seconds * static_cast<double>(sample_rate) /
                               static_cast<double>(hop) / 2.0)));
  std::vector<double> prefix(hops + 1, 0.0);
  for (std::size_t k = 0; k < hops; ++k)
    prefix[k + 1] = prefix[k] + cents[k];
  for (std::size_t k = 0; k < hops; ++k) {
    const auto lo = k >= half ? k - half : 0;
    const auto hi = std::min(hops, k + half + 1);
    const double slow = (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo);
    if (note.pitch[k] > 0.0F) {
      note.drift[k] = static_cast<float>(slow - note.centre);
      note.modulation[k] = static_cast<float>(cents[k] - slow);
    }
  }
}

foundation::Result<std::uint32_t> split_note(Take& take, std::uint32_t id, std::int64_t frame) {
  using R = Result<std::uint32_t>;
  if (take.hop <= 0)
    return R::failure(invalid("The take has no hop"));
  auto* note = find_mutable(take, id);
  if (note == nullptr)
    return R::failure({ErrorCode::not_found, "No such note"});
  if (frame <= note->start || frame >= note->end || note->hops() < 2)
    return R::failure(invalid("A split must fall inside the note"));
  const auto at =
      std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(
                                   static_cast<double>(frame) / static_cast<double>(take.hop))) -
                                   note->first_hop,
                               1, static_cast<std::int64_t>(note->hops()) - 1);
  const auto split = static_cast<std::size_t>(at);
  // Both halves keep the hop at the split so each has data at its edge.
  auto first = slice(*note, 0, split + 1);
  auto second = slice(*note, split, note->hops());
  first.end = frame;
  second.start = frame;
  second.id = take.next_id++;
  measure_pitch(first, take.hop, take.sample_rate);
  measure_pitch(second, take.hop, take.sample_rate);
  const auto new_id = second.id;
  *note = std::move(first);
  take.notes.push_back(std::move(second));
  sort_notes(take);
  return R::success(new_id);
}

foundation::Result<void> join_notes(Take& take, std::uint32_t first_id, std::uint32_t second_id) {
  using R = Result<void>;
  if (take.hop <= 0)
    return R::failure(invalid("The take has no hop"));
  auto* a = find_mutable(take, first_id);
  auto* b = find_mutable(take, second_id);
  if (a == nullptr || b == nullptr || a == b)
    return R::failure({ErrorCode::not_found, "No such notes"});
  if (a->kind != b->kind || a->hops() == 0 || b->hops() == 0)
    return R::failure(invalid("Only notes of one kind join"));
  const auto covered = [](const Note& note) {
    return note.amplitude.size() >= note.hops() &&
           (note.partial_count == 0 || note.partials.size() / note.partial_count >= note.hops());
  };
  if (!covered(*a) || !covered(*b))
    return R::failure(invalid("A note's tracks are shorter than its hops"));
  if (b->start < a->start || b->start - a->end > take.hop || b->first_hop < a->first_hop)
    return R::failure(invalid("Only touching notes join"));
  const auto first_hop = a->first_hop;
  const auto last_hop = std::max(a->first_hop + static_cast<std::int64_t>(a->hops()),
                                 b->first_hop + static_cast<std::int64_t>(b->hops()));
  const auto hops = static_cast<std::size_t>(last_hop - first_hop);
  Note joined = *a;
  joined.partial_count = std::max(a->partial_count, b->partial_count);
  joined.pitch.assign(hops, 0.0F);
  joined.amplitude.assign(hops, 0.0F);
  joined.partials.assign(hops * joined.partial_count, PartialFrame{});
  joined.envelope.assign(envelope_rows(hops) * envelope_bands, 0.0F);
  const auto a_end = a->first_hop + static_cast<std::int64_t>(a->hops());
  const auto b_end = b->first_hop + static_cast<std::int64_t>(b->hops());
  const auto source = [&](std::int64_t hop) -> std::pair<const Note*, std::size_t> {
    // Over the second note's hops the second note plays; before them the
    // first (holding its last hop across any gap), and after them the first
    // again while it lasts, so a second note nested inside keeps the first's
    // tail.
    if (hop >= b->first_hop && (hop < b_end || hop >= a_end)) {
      const auto index =
          std::min<std::int64_t>(hop - b->first_hop, static_cast<std::int64_t>(b->hops()) - 1);
      return {b, static_cast<std::size_t>(index)};
    }
    const auto index =
        std::min<std::int64_t>(hop - a->first_hop, static_cast<std::int64_t>(a->hops()) - 1);
    return {a, static_cast<std::size_t>(index)};
  };
  for (std::size_t k = 0; k < hops; ++k) {
    const auto [from, index] = source(first_hop + static_cast<std::int64_t>(k));
    joined.pitch[k] = from->pitch[index];
    joined.amplitude[k] = from->amplitude[index];
    for (std::size_t h = 0; h < from->partial_count; ++h)
      joined.partials[k * joined.partial_count + h] =
          from->partials[index * from->partial_count + h];
  }
  for (std::size_t r = 0; r * envelope_stride < hops; ++r) {
    const auto hop = first_hop + static_cast<std::int64_t>(r * envelope_stride);
    const auto [from, index] = source(hop);
    envelope_at(*from, static_cast<double>(index),
                std::span(joined.envelope).subspan(r * envelope_bands, envelope_bands));
  }
  joined.end = std::max(a->end, b->end);
  joined.noise_share = static_cast<float>((a->noise_share * static_cast<double>(a->hops()) +
                                           b->noise_share * static_cast<double>(b->hops())) /
                                          static_cast<double>(a->hops() + b->hops()));
  measure_pitch(joined, take.hop, take.sample_rate);
  *a = std::move(joined);
  std::erase_if(take.notes, [&](const Note& n) { return n.id == second_id; });
  sort_notes(take);
  return R::success();
}

} // namespace oscillate::clay_dsp
