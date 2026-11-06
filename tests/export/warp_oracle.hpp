#pragma once

// Independent oracle for exported processed clips: a direct Q0 warp stream
// opened through the project path (not the export's verified descriptor),
// pulled in fixed small blocks, so export PCM must agree across different
// partitioning and source-opening routes.
#include "oscillate/processing/verified_source_view.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/processing/warp_stream_map.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace warp_oracle {

struct Rendered {
  std::uint64_t first = 0; // project frame of output frame 0
  std::vector<float> left;
  std::vector<float> right;
};

inline Rendered render(const std::filesystem::path& root,
                       const oscillate::project::Snapshot& snapshot,
                       oscillate::project::ClipId clip, std::size_t pull) {
  namespace processing = oscillate::processing;
  const auto fail = [](const std::string& what) { throw std::runtime_error("oracle: " + what); };
  auto request = processing::warp_render_request(snapshot, clip);
  if (!request)
    fail(request.error().message);
  auto view = processing::ProjectRateSourceView::open(root, request.value().source,
                                                      request.value().project_rate);
  if (!view)
    fail(view.error().message);
  auto map = processing::make_warp_stream_map(
      request.value().plan, request.value().source.sample_rate, request.value().project_rate);
  if (!map)
    fail(map.error().message);
  auto analysis = processing::analyse_warp_source(request.value(), *view.value());
  if (!analysis)
    fail(analysis.error().message);
  auto stream = processing::create_warp_stream(request.value(), analysis.value(), map.value(),
                                               oscillate::warp_dsp::QualityTier::q0,
                                               static_cast<std::uint32_t>(pull));
  if (!stream)
    fail(stream.error().message);
  if (stream.value()->start(map.value(), *view.value(), 0) != oscillate::warp_dsp::StreamStatus::ok)
    fail("stream did not start");
  Rendered out{request.value().plan.project_start_frame, {}, {}};
  std::vector<float> left(pull), right(pull);
  while (stream.value()->next_output_frame() < map.value().output_frames()) {
    const auto count = stream.value()->render(left, right);
    if (count == 0)
      fail("stream stalled");
    out.left.insert(out.left.end(), left.begin(),
                    left.begin() + static_cast<std::ptrdiff_t>(count));
    out.right.insert(out.right.end(), right.begin(),
                     right.begin() + static_cast<std::ptrdiff_t>(count));
  }
  return out;
}

} // namespace warp_oracle
