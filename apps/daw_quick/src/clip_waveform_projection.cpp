#include "clip_waveform_projection.hpp"

#include "oscillate/processing/clip_frame_plan.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace oscillate::quick {
namespace {

constexpr std::size_t maximum_columns = 4096;

// Zero-based timeline bar (with fraction) to its tick, as the arrangement
// draws bars: linear in ticks within each meter bar.
[[nodiscard]] std::optional<timeline::TickPosition> bar_tick(const project::Snapshot& snapshot,
                                                             double bar) {
  if (!std::isfinite(bar) || bar < 0.0 || bar > 1'000'000.0)
    return std::nullopt;
  const auto index = static_cast<std::int32_t>(std::floor(bar));
  const auto first = snapshot.meter.tick_at({index + 1, 1, 0});
  const auto next = snapshot.meter.tick_at({index + 2, 1, 0});
  if (!first || !next)
    return std::nullopt;
  const auto span = static_cast<double>(next.value().value - first.value().value);
  return timeline::TickPosition{first.value().value +
                                static_cast<std::int64_t>(std::llround((bar - index) * span))};
}

} // namespace

std::optional<std::vector<std::uint64_t>> clip_waveform_edges(const project::Snapshot& snapshot,
                                                              const project::Clip& clip,
                                                              double first_bar, double last_bar,
                                                              std::size_t columns) {
  if (columns == 0 || columns > maximum_columns || !(last_bar > first_bar))
    return std::nullopt;
  const auto source = std::ranges::find(snapshot.sources, clip.source, &project::SourceRecord::id);
  if (source == snapshot.sources.end())
    return std::nullopt;
  auto plan =
      processing::prepare_clip_frame_plan(clip, *source, snapshot.tempo, snapshot.sample_rate);
  if (!plan)
    return std::nullopt;
  const auto& value = plan.value();
  const auto output_frames = value.warp.output_frames();
  std::vector<std::uint64_t> edges;
  edges.reserve(columns + 1);
  for (std::size_t index = 0; index <= columns; ++index) {
    const auto bar = first_bar + (last_bar - first_bar) * static_cast<double>(index) /
                                     static_cast<double>(columns);
    const auto tick = bar_tick(snapshot, bar);
    if (!tick)
      return std::nullopt;
    const auto sample = snapshot.tempo.tick_to_sample(tick.value(), snapshot.sample_rate);
    if (!sample)
      return std::nullopt;
    // Clamp to the clip: bar rounding may land a frame outside it.
    const auto local = std::clamp<std::int64_t>(
        sample.value().value - static_cast<std::int64_t>(value.project_start_frame), 0,
        static_cast<std::int64_t>(output_frames));
    const auto mapped = value.warp.source_at_output(static_cast<std::uint64_t>(local));
    if (!mapped)
      return std::nullopt;
    edges.push_back(value.source_start_frame + *mapped);
  }
  return edges;
}

QVariantList waveform_columns(const analysis::WaveformSummary& summary,
                              std::span<const std::uint64_t> edges) {
  QVariantList output;
  if (edges.size() < 2 || edges.size() - 1 > maximum_columns || summary.levels.empty() ||
      !std::ranges::is_sorted(edges) || edges.back() > summary.frames ||
      edges.front() >= summary.frames)
    return output;
  const auto columns = edges.size() - 1;
  const auto target = std::max<std::uint64_t>(1, (edges.back() - edges.front()) / columns);
  auto level = std::ranges::find_if(summary.levels, [target](const auto& candidate) {
    return candidate.frames_per_bucket > target;
  });
  if (level != summary.levels.begin())
    --level;
  const auto& buckets = level->buckets;
  output.reserve(static_cast<qsizetype>(columns));
  for (std::size_t index = 0; index < columns; ++index) {
    const auto start = std::min(edges[index], summary.frames - 1);
    const auto end = std::max(edges[index + 1], start + 1);
    float minimum = 1.0F;
    float maximum = -1.0F;
    bool occupied = false;
    for (auto bucket =
             std::ranges::upper_bound(buckets, start, {}, &analysis::WaveformBucket::end_frame);
         bucket != buckets.end() && bucket->begin_frame < end; ++bucket) {
      const auto& left = bucket->channels[0];
      const auto& right = summary.channels == 2 ? bucket->channels[1] : left;
      minimum = std::min({minimum, left.minimum, right.minimum});
      maximum = std::max({maximum, left.maximum, right.maximum});
      occupied = true;
    }
    output.push_back(
        QVariantMap{{"min", occupied ? minimum : 0.0F}, {"max", occupied ? maximum : 0.0F}});
  }
  return output;
}

QVariantList clip_waveform(const analysis::WaveformSummary& summary,
                           const project::Snapshot& snapshot, const project::Clip& clip,
                           double first_bar, double last_bar, std::size_t columns) {
  const auto edges = clip_waveform_edges(snapshot, clip, first_bar, last_bar, columns);
  return edges ? waveform_columns(summary, *edges) : QVariantList{};
}

} // namespace oscillate::quick
