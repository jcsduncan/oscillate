#include "oscillate/application/midi_pedal_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <vector>

namespace oscillate::application {
namespace {

using Point = project::MidiPedalPoint;

// Sorted, and inside the pattern without points that repeat the value in
// force (0 before the first point). Points kept after the pattern's end are
// silent (a shortened clip keeps them) and pass through untouched, as do
// those before tick 0 (a clip trimmed from its left edge).
std::vector<Point> normalised(std::vector<Point> points, std::int64_t pattern_length) {
  std::ranges::stable_sort(points, {}, [](const Point& p) { return p.position.value; });
  std::vector<Point> out;
  std::vector<Point> kept;
  std::vector<Point> before;
  for (const auto& point : points) {
    if (point.position.value >= 0) {
      break;
    }
    if (before.empty() || before.back().position != point.position) {
      before.push_back(point);
    } else {
      before.back() = point;
    }
  }
  double current = before.empty() ? 0.0 : before.back().value;
  for (const auto& point : points) {
    if (point.position.value >= pattern_length) {
      if (kept.empty() || kept.back().position != point.position) {
        kept.push_back(point);
      } else {
        kept.back() = point;
      }
      continue;
    }
    if (point.position.value < 0) {
      continue;
    }
    if (!out.empty() && out.back().position == point.position) {
      out.back() = point; // A later write at the same tick wins.
      current = point.value;
      continue;
    }
    if (point.value == current) {
      continue;
    }
    out.push_back(point);
    current = point.value;
  }
  // A same-tick overwrite can leave a point repeating its predecessor.
  std::vector<Point> clean = std::move(before);
  current = clean.empty() ? 0.0 : clean.back().value;
  for (const auto& point : out) {
    if (point.value != current) {
      clean.push_back(point);
      current = point.value;
    }
  }
  clean.insert(clean.end(), kept.begin(), kept.end());
  return clean;
}

const project::MidiPedalLane* lane_of(const project::MidiClip& clip, project::MidiPedal pedal) {
  const auto found = std::ranges::find(clip.pedals, pedal, &project::MidiPedalLane::pedal);
  return found == clip.pedals.end() ? nullptr : &*found;
}

// The first tick at or after `from` where the lane is below `threshold`.
std::int64_t release_after(std::span<const Point> points, std::int64_t from, double threshold,
                           std::int64_t pattern_length) {
  for (const auto& point : points) {
    if (point.position.value > from && point.value < threshold) {
      return point.position.value;
    }
  }
  return pattern_length;
}

} // namespace

double pedal_value_at(std::span<const Point> points, std::int64_t tick) noexcept {
  double value = 0.0;
  for (const auto& point : points) {
    if (point.position.value > tick) {
      break;
    }
    value = point.value;
  }
  return value;
}

std::vector<PedalSpan> pedal_spans(std::span<const Point> points, std::int64_t pattern_length) {
  std::vector<PedalSpan> spans;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (points[i].position.value >= pattern_length) {
      break; // Kept past the pattern's end, silent.
    }
    if (points[i].value <= 0.0) {
      continue;
    }
    // A point before tick 0 (trimmed off the clip's left edge) holds from 0.
    const std::int64_t start = std::max<std::int64_t>(0, points[i].position.value);
    const std::int64_t end = i + 1 < points.size() ? points[i + 1].position.value : pattern_length;
    if (end > start) {
      spans.push_back({start, std::min(end, pattern_length), points[i].value});
    }
  }
  return spans;
}

std::vector<Point> paint_pedal(std::span<const Point> points, std::int64_t start, std::int64_t end,
                               double depth, std::int64_t pattern_length) {
  start = std::clamp<std::int64_t>(start, 0, pattern_length);
  end = std::clamp<std::int64_t>(end, 0, pattern_length);
  depth = std::clamp(depth, 0.0, 1.0);
  std::vector<Point> out(points.begin(), points.end());
  if (end <= start) {
    return normalised(std::move(out), pattern_length);
  }
  const double after = pedal_value_at(points, end);
  std::erase_if(out, [&](const Point& p) {
    return p.position.value >= start && p.position.value <= end &&
           p.position.value < pattern_length;
  });
  out.push_back({{start}, depth});
  if (end < pattern_length) {
    out.push_back({{end}, after});
  }
  return normalised(std::move(out), pattern_length);
}

std::vector<Point> remove_pedal_span(std::span<const Point> points, timeline::TickPosition tick,
                                     std::int64_t pattern_length) {
  for (const auto& span : pedal_spans(points, pattern_length)) {
    if (tick.value >= span.start && tick.value < span.end) {
      return paint_pedal(points, span.start, span.end, 0.0, pattern_length);
    }
  }
  return normalised({points.begin(), points.end()}, pattern_length);
}

std::vector<Point> pedal_per_chord(std::span<const project::MidiNote> notes,
                                   std::int64_t pattern_length, ChordPedalling pedalling) {
  const std::int64_t repress = pedalling.repress;
  const double depth = pedalling.depth;
  std::set<std::int64_t> onsets;
  for (const auto& note : notes) {
    if (note.start.value >= 0 && note.start.value < pattern_length) {
      onsets.insert(note.start.value);
    }
  }
  std::vector<Point> points;
  const std::vector<std::int64_t> chords(onsets.begin(), onsets.end());
  for (std::size_t i = 0; i < chords.size(); ++i) {
    const std::int64_t press = chords[i] + repress;
    const std::int64_t lift = i + 1 < chords.size() ? chords[i + 1] : pattern_length;
    if (press >= lift) {
      continue; // Too close to the next chord to re-pedal.
    }
    points.push_back({{press}, depth});
    if (lift < pattern_length) {
      points.push_back({{lift}, 0.0});
    }
  }
  return normalised(std::move(points), pattern_length);
}

std::int64_t sounding_end(const project::MidiNote& note, const project::MidiClip& clip) noexcept {
  const std::int64_t pattern = clip.pattern_length.value;
  std::int64_t end = note.end.value;
  if (const auto* sustain = lane_of(clip, project::MidiPedal::sustain)) {
    if (pedal_value_at(sustain->points, note.end.value) >= 0.5) {
      end = std::max(end, release_after(sustain->points, note.end.value, 0.5, pattern));
    }
  }
  if (const auto* sostenuto = lane_of(clip, project::MidiPedal::sostenuto)) {
    // Sostenuto holds a note only if it went down while the note was held.
    double before = 0.0;
    for (const auto& point : sostenuto->points) {
      const bool rises = before < 0.5 && point.value >= 0.5;
      if (rises && point.position.value >= note.start.value &&
          point.position.value < note.end.value) {
        end = std::max(end, release_after(sostenuto->points, point.position.value, 0.5, pattern));
        break;
      }
      before = point.value;
    }
    // Already down when the note started: it is not captured.
  }
  return std::min(end, pattern);
}

} // namespace oscillate::application
