#include "detect.hpp"
#include "spectrum.hpp"

#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Percussive analysis: a note per transient (the warp engine's detector),
// running to the next transient or until it fades, placed at the pitch of
// its strongest early peak. Notes are segments of the original audio.
namespace oscillate::clay_dsp::detail {
namespace {

// The pitch is read from this long after the onset.
constexpr double pitch_window_seconds = 0.05;
// A hit ends when its level falls this far under its peak.
constexpr double fade_out_db = -50.0;
// Level is measured on blocks this long.
constexpr double level_block_seconds = 0.005;

} // namespace

bool detect_percussive(const Context& context, std::span<const float> left,
                       std::span<const float> right, std::int64_t hop, std::vector<Note>& notes) {
  const auto rate = context.sample_rate;
  const auto mono = context.mono;
  const auto length = static_cast<std::int64_t>(mono.size());
  warp_dsp::MemorySourceView view(left, right.empty() ? left : right);
  warp_dsp::analysis::TransientOptions options;
  options.sample_rate = rate;
  auto found = warp_dsp::analysis::detect_transients(view, options, context.cancelled);
  if (!found)
    return !context.stopped();
  const auto& transients = found.value();
  FrameAnalyser analyser(fft_size_at_least(pitch_window_seconds * rate));
  const double bin_hz = static_cast<double>(rate) / static_cast<double>(analyser.size());
  const auto block = std::max<std::int64_t>(1, std::llround(level_block_seconds * rate));
  const auto minimum = static_cast<std::int64_t>(context.options->minimum_note_seconds * rate);
  for (std::size_t t = 0; t < transients.size(); ++t) {
    if (context.stopped())
      return false;
    const auto start = static_cast<std::int64_t>(transients[t].frame);
    const auto limit =
        t + 1 < transients.size() ? static_cast<std::int64_t>(transients[t + 1].frame) : length;
    // Peak block level, then the last block above the fade-out level.
    double peak = 0.0;
    std::vector<double> levels;
    for (auto b = start; b < limit; b += block) {
      double sum = 0.0;
      const auto to = std::min(limit, b + block);
      for (auto i = b; i < to; ++i)
        sum += static_cast<double>(mono[static_cast<std::size_t>(i)]) *
               mono[static_cast<std::size_t>(i)];
      levels.push_back(std::sqrt(sum / static_cast<double>(to - b)));
      peak = std::max(peak, levels.back());
    }
    if (peak <= 0.0)
      continue;
    auto end = start;
    for (std::size_t b = 0; b < levels.size(); ++b) {
      if (levels[b] >= peak * db_to_ratio(fade_out_db))
        end = std::min(limit, start + static_cast<std::int64_t>(b + 1) * block);
    }
    if (end - start < minimum)
      continue;
    analyser.analyse(mono, start + static_cast<std::int64_t>(analyser.size() / 2));
    const auto magnitude = analyser.magnitude();
    std::size_t loudest = 1;
    for (std::size_t k = 1; k + 1 < magnitude.size(); ++k) {
      if (magnitude[k] > magnitude[loudest])
        loudest = k;
    }
    const double pitch = std::max(bin_hz, analyser.peak(loudest).bin * bin_hz);
    Note note;
    note.kind = NoteKind::segment;
    note.start = start;
    note.end = end;
    note.first_hop = start / hop;
    const auto last_hop = (end + hop - 1) / hop;
    const auto hops = static_cast<std::size_t>(last_hop - note.first_hop + 1);
    note.pitch.assign(hops, static_cast<float>(pitch));
    note.amplitude.assign(hops, 0.0F);
    for (std::size_t k = 0; k < hops; ++k) {
      const auto centre = (note.first_hop + static_cast<std::int64_t>(k)) * hop;
      const auto b = std::clamp<std::int64_t>((centre - start) / block, 0,
                                              static_cast<std::int64_t>(levels.size()) - 1);
      note.amplitude[k] = static_cast<float>(levels[static_cast<std::size_t>(b)]);
    }
    notes.push_back(std::move(note));
  }
  return true;
}

} // namespace oscillate::clay_dsp::detail
