#include "waveform_controller.hpp"

#include "clip_waveform_projection.hpp"
#include "waveform_view.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace oscillate::quick {
namespace {
std::optional<project::SourceRange>
visible_source_window(project::SourceRange source, double clip_start_bar, double clip_end_bar,
                      double viewport_first_bar, double viewport_visible_bars) {
  if (source.start.value < 0 || source.end <= source.start || clip_end_bar <= clip_start_bar)
    return std::nullopt;
  const double visible_start = std::max(clip_start_bar, viewport_first_bar);
  const double visible_end = std::min(clip_end_bar, viewport_first_bar + viewport_visible_bars);
  if (visible_end <= visible_start)
    return std::nullopt;
  const auto source_start = source.start.value;
  const auto source_span = source.end.value - source_start;
  const auto clip_span = clip_end_bar - clip_start_bar;
  const auto begin_ratio = (visible_start - clip_start_bar) / clip_span;
  const auto end_ratio = (visible_end - clip_start_bar) / clip_span;
  const auto begin =
      source_start +
      static_cast<std::int64_t>(std::floor(static_cast<long double>(source_span) * begin_ratio));
  const auto end = source_start + static_cast<std::int64_t>(
                                      std::ceil(static_cast<long double>(source_span) * end_ratio));
  return project::SourceRange{{std::clamp(begin, source.start.value, source.end.value - 1)},
                              {std::clamp(end, source.start.value + 1, source.end.value)}};
}
} // namespace

WaveformController::WaveformController(Host host, QObject* parent)
    : QObject(parent), host_(std::move(host)), timer_(this), detail_timer_(this) {
  timer_.setInterval(50);
  connect(&timer_, &QTimer::timeout, this, &WaveformController::poll);
  detail_timer_.setInterval(30);
  connect(&detail_timer_, &QTimer::timeout, this, &WaveformController::poll_details);
}

void WaveformController::invalidate() {
  sources_.clear();
  summaries_.clear();
  failures_.clear();
  cache_degraded_.clear();
  rebuild_source_id_.clear();
  details_.clear();
  detail_order_.clear();
  cancel_work();
}

void WaveformController::cancel_work() {
  worker_.submit({}, {}, ++generation_);
  timer_.stop();
  detail_timer_.stop();
  for (auto& [id, detail] : details_)
    detail.waiting = false;
}

void WaveformController::set_project(std::filesystem::path root) {
  root_ = std::move(root);
  invalidate();
}

void WaveformController::refresh_sources(const project::Snapshot& snapshot) {
  std::vector<project::SourceRecord> visible_sources;
  std::size_t inspected_audio_clips = 0;
  for (const auto& track : snapshot.tracks) {
    for (const auto& clip : track.clips) {
      if (++inspected_audio_clips > max_clips || visible_sources.size() == max_sources)
        break;
      const auto source = std::ranges::find_if(
          snapshot.sources, [&clip](const auto& item) { return item.id == clip.source; });
      if (source != snapshot.sources.end() &&
          std::ranges::none_of(visible_sources,
                               [source](const auto& item) { return item.id == source->id; }))
        visible_sources.push_back(*source);
    }
  }
  if (visible_sources == sources_)
    return;
  sources_ = std::move(visible_sources);
  summaries_.clear();
  details_.clear();
  detail_order_.clear();
  failures_.clear();
  cache_degraded_.clear();
  worker_.submit(root_, sources_, ++generation_);
  if (sources_.empty())
    timer_.stop();
  else
    timer_.start();
}

void WaveformController::retry_failures() {
  if (failures_.empty())
    return;
  failures_.clear();
  worker_.submit(root_, sources_, ++generation_);
  timer_.start();
  host_.refresh();
}

bool WaveformController::set_viewport(double first_bar, double visible_bars, int pixel_width) {
  if (!host_.session() || !std::isfinite(first_bar) || !std::isfinite(visible_bars) ||
      first_bar < 0.0 || visible_bars < 0.0625 || visible_bars > 1'000'000.0 || pixel_width < 1 ||
      pixel_width > 16'384 ||
      (first_bar == first_bar_ && visible_bars == visible_bars_ && pixel_width == view_width_))
    return false;
  first_bar_ = first_bar;
  visible_bars_ = visible_bars;
  view_width_ = pixel_width;
  return true;
}

void WaveformController::rebuild_clip(const QString& clip_id) {
  const auto* session = host_.session();
  if (!session)
    return;
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  if (!parsed) {
    host_.status(QStringLiteral("Select an audio clip to rebuild its waveform"));
    return;
  }
  for (const auto& track : session->snapshot().tracks) {
    const auto clip = std::ranges::find(track.clips, parsed.value(), &project::Clip::id);
    if (clip == track.clips.end())
      continue;
    const auto source = std::ranges::find(sources_, clip->source, &project::SourceRecord::id);
    if (source == sources_.end()) {
      host_.status(QStringLiteral("Waveform preview is unavailable for this source"));
      return;
    }
    const auto id = source->id.to_string();
    summaries_.erase(id);
    failures_.erase(id);
    cache_degraded_.erase(id);
    rebuild_source_id_ = id;
    worker_.submit(root_, sources_, ++generation_, source->id);
    timer_.start();
    host_.refresh();
    host_.status(QStringLiteral("Rebuilding waveform from source audio…"));
    return;
  }
  host_.status(QStringLiteral("Audio clip is no longer in the project"));
}

const analysis::WaveformSummary* WaveformController::summary(const std::string& id) const noexcept {
  const auto found = summaries_.find(id);
  return found == summaries_.end() ? nullptr : &found->second;
}

const QString* WaveformController::failure(const std::string& id) const noexcept {
  const auto found = failures_.find(id);
  return found == failures_.end() ? nullptr : &found->second;
}

bool WaveformController::cache_degraded(const std::string& id) const noexcept {
  return cache_degraded_.contains(id);
}

QVariantList WaveformController::view_range(project::SourceId source, project::SourceRange range,
                                            std::size_t columns) const {
  const auto* found = summary(source.to_string());
  return found ? waveform_view(*found, range, columns) : QVariantList{};
}

QVariantList WaveformController::view_for_warp(const project::Clip& clip,
                                               std::size_t columns) const {
  const auto* found = summary(clip.source.to_string());
  return found ? waveform_view(*found, clip.source_range, columns) : QVariantList{};
}

QVariantList WaveformController::close_up_columns(const project::SourceRecord& source,
                                                  const analysis::WaveformSummary& summary,
                                                  std::span<const std::uint64_t> edges) {
  if (edges.size() < 2 || summary.levels.empty() || edges.back() <= edges.front())
    return {};
  const auto span = edges.back() - edges.front();
  // Wider columns than the finest summary bucket draw from the summary.
  if (span / (edges.size() - 1) >= summary.levels.front().frames_per_bucket)
    return {};
  const auto id = source.id.to_string();
  std::erase(detail_order_, id);
  detail_order_.push_back(id);
  while (detail_order_.size() > max_details) {
    details_.erase(detail_order_.front());
    detail_order_.erase(detail_order_.begin());
  }
  auto& detail = details_[id];
  if (detail.frames.covers(edges.front(), edges.back()))
    return detail_columns(detail.frames, edges);
  if (detail.unreadable ||
      (detail.waiting && detail.wanted_begin <= edges.front() && detail.wanted_end >= edges.back()))
    return {};
  // The view and a screen either side, so a pan stays sharp.
  detail.wanted_begin = edges.front() > span ? edges.front() - span : 0;
  detail.wanted_end = std::min(edges.back() + span, summary.frames);
  detail.waiting = true;
  detail_worker_.request(root_, source, detail.wanted_begin, detail.wanted_end, generation_);
  detail_timer_.start();
  return {};
}

void WaveformController::poll_details() {
  bool changed = false;
  for (auto& outcome : detail_worker_.collect()) {
    const auto detail = details_.find(outcome.source_id);
    if (outcome.generation != generation_ || detail == details_.end())
      continue;
    detail->second.waiting = false;
    if (outcome.detail.frames.empty())
      detail->second.unreadable = true;
    else
      detail->second.frames = std::move(outcome.detail);
    changed = true;
  }
  if (std::ranges::none_of(details_, [](const auto& item) { return item.second.waiting; }))
    detail_timer_.stop();
  if (changed && host_.refresh_waveforms)
    host_.refresh_waveforms();
}

QVariantMap WaveformController::project_clip_waveform(const project::Snapshot& snapshot,
                                                      const project::Clip& clip, ClipView view) {
  QVariantMap projected;
  const auto id = clip.source.to_string();
  const auto recorded_source = std::ranges::find_if(
      snapshot.sources, [&clip](const auto& item) { return item.id == clip.source; });
  projected.insert(QStringLiteral("sourceTotalFrames"),
                   recorded_source == snapshot.sources.end()
                       ? 0.0
                       : static_cast<double>(recorded_source->frames));
  const auto source =
      std::ranges::find_if(sources_, [&clip](const auto& item) { return item.id == clip.source; });
  if (view.audio_clip_ordinal > max_clips) {
    projected.insert(QStringLiteral("waveformState"), QStringLiteral("unavailable"));
    projected.insert(QStringLiteral("waveformMessage"),
                     QStringLiteral("Waveform preview limited to 256 clips"));
  } else if (source == sources_.end()) {
    projected.insert(QStringLiteral("waveformState"), QStringLiteral("unavailable"));
    projected.insert(QStringLiteral("waveformMessage"),
                     recorded_source == snapshot.sources.end()
                         ? QStringLiteral("Missing source record")
                         : QStringLiteral("Waveform preview limited to 64 sources"));
  } else if (const auto* found = summary(id)) {
    const auto window = visible_source_window(clip.source_range, view.start_bar, view.end_bar,
                                              first_bar_, visible_bars_);
    QVariantList waveform;
    if (window && !view.columns) {
      projected.insert(QStringLiteral("waveformState"), QStringLiteral("ready"));
      projected.insert(QStringLiteral("waveformMessage"),
                       QStringLiteral("Waveform source range unavailable"));
      projected.insert(QStringLiteral("waveformCacheDegraded"), cache_degraded(id));
      return projected;
    }
    if (window) {
      const double visible_start = std::max(view.start_bar, first_bar_);
      const double visible_end = std::min(view.end_bar, first_bar_ + visible_bars_);
      const auto columns = static_cast<std::size_t>(std::clamp(
          std::ceil((visible_end - visible_start) * view_width_ / visible_bars_), 1.0, 4096.0));
      // A processed clip follows the warp map rather than a linear source slice.
      std::optional<std::vector<std::uint64_t>> edges;
      if (view.processed) {
        edges = clip_waveform_edges(snapshot, clip, visible_start, visible_end, columns);
      } else {
        const auto begin = static_cast<std::uint64_t>(window->start.value);
        const auto span = static_cast<std::uint64_t>(window->end.value) - begin;
        edges.emplace();
        edges->reserve(columns + 1);
        for (std::size_t index = 0; index <= columns; ++index)
          edges->push_back(
              begin + static_cast<std::uint64_t>(static_cast<long double>(span) * index / columns));
      }
      if (edges)
        waveform = close_up_columns(*source, *found, *edges);
      if (waveform.isEmpty())
        waveform = !view.processed ? waveform_view(*found, *window, columns)
                   : edges         ? waveform_columns(*found, *edges)
                                   : QVariantList{};
    }
    projected.insert(QStringLiteral("waveformState"), !window || !waveform.isEmpty()
                                                          ? QStringLiteral("ready")
                                                          : QStringLiteral("unavailable"));
    projected.insert(QStringLiteral("waveformMessage"),
                     QStringLiteral("Waveform source range unavailable"));
    projected.insert(QStringLiteral("waveformCacheDegraded"), cache_degraded(id));
    projected.insert(QStringLiteral("waveform"), waveform);
  } else if (const auto* reason = failure(id)) {
    projected.insert(QStringLiteral("waveformState"), QStringLiteral("unavailable"));
    projected.insert(QStringLiteral("waveformMessage"), *reason);
  } else {
    projected.insert(QStringLiteral("waveformState"), QStringLiteral("loading"));
    projected.insert(QStringLiteral("waveformMessage"), QStringLiteral("Building waveform"));
  }
  return projected;
}

void WaveformController::poll() {
  bool changed = false;
  for (auto& outcome : worker_.collect()) {
    if (outcome.generation != generation_ ||
        !std::ranges::any_of(sources_,
                             [&outcome](const auto& source) { return source == outcome.source; }))
      continue;
    const auto id = outcome.source.id.to_string();
    if (outcome.result && outcome.result.value().identity.generation == generation_ &&
        !outcome.result.value().identity.source_key.empty()) {
      if (outcome.cache_degraded)
        cache_degraded_.insert_or_assign(id, true);
      summaries_.insert_or_assign(id, std::move(outcome.result.value()));
      if (id == rebuild_source_id_) {
        host_.status(QStringLiteral("Waveform rebuilt from source audio"));
        rebuild_source_id_.clear();
      }
    } else {
      const auto code =
          outcome.result ? foundation::ErrorCode::corrupt_data : outcome.result.error().code;
      const auto message = code == foundation::ErrorCode::not_found
                               ? QStringLiteral("Managed media missing")
                           : code == foundation::ErrorCode::unsupported
                               ? QStringLiteral("Decoder unavailable for this source")
                           : code == foundation::ErrorCode::corrupt_data
                               ? QStringLiteral("Managed media failed integrity check")
                               : QStringLiteral("Waveform unavailable; check project media");
      failures_.insert_or_assign(id, message);
      if (id == rebuild_source_id_) {
        host_.status(QStringLiteral("Waveform rebuild failed: %1").arg(message));
        rebuild_source_id_.clear();
      }
    }
    changed = true;
  }
  if (changed && host_.session())
    host_.refresh();
  if (summaries_.size() + failures_.size() == sources_.size())
    timer_.stop();
}

} // namespace oscillate::quick
