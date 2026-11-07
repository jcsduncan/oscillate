#include "waveform_view.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace oscillate::quick {
namespace {

constexpr std::size_t maximum_display_columns = 4096;

struct Column {
  float minimum = 1.0F;
  float maximum = -1.0F;
  bool occupied = false;
};

} // namespace

QVariantList waveform_view(const analysis::WaveformSummary& summary,
                           project::SourceRange source_range, std::size_t display_columns) {
  QVariantList output;
  if (display_columns == 0 || display_columns > maximum_display_columns ||
      source_range.start.value < 0 || source_range.end <= source_range.start ||
      summary.levels.empty()) {
    return output;
  }
  const auto start = static_cast<std::uint64_t>(source_range.start.value);
  const auto end = static_cast<std::uint64_t>(source_range.end.value);
  if (end > summary.frames) {
    return output;
  }
  const auto span = end - start;
  const auto target_frames_per_column = std::max<std::uint64_t>(1, span / display_columns);
  auto level =
      std::ranges::find_if(summary.levels, [target_frames_per_column](const auto& candidate) {
        return candidate.frames_per_bucket > target_frames_per_column;
      });
  if (level != summary.levels.begin())
    --level;
  const auto& buckets = level->buckets;
  std::vector<Column> columns(display_columns);
  const auto first_bucket =
      std::ranges::lower_bound(buckets, start + 1, {}, &analysis::WaveformBucket::end_frame);
  for (auto bucket_it = first_bucket; bucket_it != buckets.end() && bucket_it->begin_frame < end;
       ++bucket_it) {
    const auto& bucket = *bucket_it;
    const auto overlap_start = std::max(start, bucket.begin_frame);
    const auto overlap_end = std::min(end, bucket.end_frame);
    if (overlap_start >= overlap_end) {
      continue;
    }
    const auto first = static_cast<std::size_t>(
        (static_cast<long double>(overlap_start - start) * display_columns) / span);
    const auto last =
        std::min(display_columns - 1,
                 static_cast<std::size_t>(std::ceil(
                     (static_cast<long double>(overlap_end - start) * display_columns) / span)) -
                     1);
    for (auto index = std::min(first, display_columns - 1); index <= last; ++index) {
      auto& column = columns[index];
      const auto& left = bucket.channels[0];
      const auto& right = summary.channels == 2 ? bucket.channels[1] : left;
      column.minimum = std::min({column.minimum, left.minimum, right.minimum});
      column.maximum = std::max({column.maximum, left.maximum, right.maximum});
      column.occupied = true;
    }
  }
  output.reserve(static_cast<qsizetype>(display_columns));
  for (const auto& column : columns) {
    output.push_back(QVariantMap{{"min", column.occupied ? column.minimum : 0.0F},
                                 {"max", column.occupied ? column.maximum : 0.0F}});
  }
  return output;
}

} // namespace oscillate::quick
