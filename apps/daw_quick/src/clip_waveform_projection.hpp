#pragma once

#include "oscillate/analysis/waveform.hpp"
#include "oscillate/project/model.hpp"

#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::quick {

// The arrangement waveform of a processed (warped, stretched or pitched)
// clip, as it plays. The visible part of the clip, [first_bar, last_bar) in
// zero-based timeline bars, is split into `columns` equal bar spans. Each
// span is converted to project samples through the shared meter and tempo
// maps, then to the source interval it plays through the authoritative clip
// frame plan (the warp map), so markers, stretch and tempo changes all show.
// Work is O(columns × log markers) plus the summary buckets in the visible
// source interval at the level matching the average column width, so it
// stays bounded at any zoom.
//
// The ascending absolute source frame at each of the columns + 1 column
// edges. Nullopt if the clip has no frame plan, the bars are outside it, or
// columns is 0 or above 4,096.
[[nodiscard]] std::optional<std::vector<std::uint64_t>>
clip_waveform_edges(const project::Snapshot& snapshot, const project::Clip& clip, double first_bar,
                    double last_bar, std::size_t columns);

// One {min, max} envelope per column between consecutive `edges` (absolute
// source frames, ascending). A column narrower than one source frame shows
// the frame it plays. Empty for invalid edges or more than 4,096 columns.
[[nodiscard]] QVariantList waveform_columns(const analysis::WaveformSummary& summary,
                                            std::span<const std::uint64_t> edges);

[[nodiscard]] QVariantList clip_waveform(const analysis::WaveformSummary& summary,
                                         const project::Snapshot& snapshot,
                                         const project::Clip& clip, double first_bar,
                                         double last_bar, std::size_t columns);

} // namespace oscillate::quick
