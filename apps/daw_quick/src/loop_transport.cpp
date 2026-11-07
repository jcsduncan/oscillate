#include "loop_transport.hpp"

#include "routed_playback.hpp"

#include <utility>

namespace oscillate::quick {

LoopTransport::LoopTransport(Snapshot snapshot, Playback playback, CanSeek can_seek, Seek seek,
                             QObject* parent)
    : QObject(parent), snapshot_(std::move(snapshot)), playback_(std::move(playback)),
      can_seek_(std::move(can_seek)), seek_(std::move(seek)) {}

std::optional<LoopTransport::Samples> LoopTransport::samples() const {
  const auto* snapshot = snapshot_ ? snapshot_() : nullptr;
  if (!snapshot)
    return std::nullopt;
  const auto at_bar = [snapshot](int bar) -> std::optional<std::uint64_t> {
    const auto tick = snapshot->meter.tick_at({bar, 1, 0});
    if (!tick)
      return std::nullopt;
    const auto sample = snapshot->tempo.tick_to_sample(tick.value(), snapshot->sample_rate);
    if (!sample || sample.value().value < 0)
      return std::nullopt;
    return static_cast<std::uint64_t>(sample.value().value);
  };
  const auto start = at_bar(start_bar_);
  const auto end = at_bar(end_bar_);
  if (!start || !end || *end <= *start)
    return std::nullopt;
  return Samples{*start, *end};
}

std::optional<std::uint64_t> LoopTransport::start_sample() const {
  if (!enabled_)
    return std::nullopt;
  const auto range = samples();
  return range ? std::optional{range->start} : std::nullopt;
}

std::optional<application::LoopSamples> LoopTransport::loop_samples() const {
  if (!enabled_)
    return std::nullopt;
  const auto range = samples();
  return range ? std::optional{application::LoopSamples{range->start, range->end}} : std::nullopt;
}

void LoopTransport::set_status(QString status) {
  if (status_.set(std::move(status)))
    emit changed();
}

void LoopTransport::set_range(int start_bar, int end_bar) {
  if (start_bar < 1 || end_bar <= start_bar || end_bar > 100'000) {
    set_status(QStringLiteral("A loop needs a start bar before its end bar"));
    return;
  }
  start_bar_ = start_bar;
  end_bar_ = end_bar;
  emit changed();
  poll();
}

void LoopTransport::set_enabled(bool enabled) {
  if (enabled == enabled_)
    return;
  enabled_ = enabled;
  emit changed();
  poll();
}

void LoopTransport::poll() {
  auto* playback = playback_ ? playback_() : nullptr;
  if (playback != armed_playback_) {
    // A new run (or none): its cursor counts wraps from zero.
    armed_playback_ = playback;
    armed_.reset();
    wraps_seen_ = {};
  }
  if (!enabled_) {
    if (playback && armed_) {
      playback->clear_loop(); // withdraws a queued wrap
      armed_.reset();
    }
    // Keep servicing: frames queued for the withdrawn pass are discarded by
    // an in-place restart before playback reaches the old loop end.
    const auto progress = playback ? playback->service_loop() : RoutedPlayback::LoopProgress{};
    count_wraps(progress.wraps);
    set_status(progress.restarting
                   ? QStringLiteral("Loop off: discarding the primed loop pass (short hold)")
                   : QStringLiteral("Loop off"));
    return;
  }
  const auto range = samples();
  if (!range) {
    set_status(
        QStringLiteral("Loop bars %1–%2 are outside the tempo map").arg(start_bar_).arg(end_bar_));
    return;
  }
  if (!playback) {
    set_status(QStringLiteral("Loop bars %1–%2: armed when routed playback starts")
                   .arg(start_bar_)
                   .arg(end_bar_));
    return;
  }
  const auto position = playback->completed_position();
  if (!armed_ || armed_->start != range->start || armed_->end != range->end) {
    // Re-arming another range withdraws a wrap queued for the old one.
    if (!playback->set_loop(range->start, range->end)) {
      set_status(QStringLiteral("Loop could not be armed on this playback run"));
      return;
    }
    armed_ = range;
    last_position_ = position.next_sample;
    last_generation_ = position.generation;
  }
  const auto progress = playback->service_loop();
  count_wraps(progress.wraps);
  const bool inside = position.next_sample >= range->start && position.next_sample < range->end;
  const bool was_inside = last_position_ >= range->start && last_position_ < range->end;
  if (progress.scheduled) {
    set_status(QStringLiteral("Looping bars %1–%2: next pass primed; wraps exactly at the loop "
                              "end")
                   .arg(start_bar_)
                   .arg(end_bar_));
  } else if (progress.restarting) {
    set_status(QStringLiteral("Looping bars %1–%2: re-priming in place (short hold) so the "
                              "next pass wraps exactly")
                   .arg(start_bar_)
                   .arg(end_bar_));
  } else if (inside) {
    set_status(progress.missed
                   ? QStringLiteral("Looping bars %1–%2: the next pass could not be primed; "
                                    "the loop will restart by seek with a short gap")
                         .arg(start_bar_)
                         .arg(end_bar_)
                   : QStringLiteral("Looping bars %1–%2: priming the next pass")
                         .arg(start_bar_)
                         .arg(end_bar_));
  } else if (was_inside && position.generation == last_generation_ &&
             position.next_sample >= range->end) {
    // Crossed the loop end without a primed wrap: restart by seek.
    if (can_seek_ && can_seek_() && seek_) {
      seek_(range->start);
      ++seek_restarts_;
      set_status(QStringLiteral("Loop restarted by seek: the next pass was not primed in time "
                                "(short gap)"));
      emit changed();
    }
  } else if (position.next_sample >= range->end) {
    set_status(QStringLiteral("Loop bars %1–%2: the playhead is past the loop; it loops once "
                              "playback is moved inside it")
                   .arg(start_bar_)
                   .arg(end_bar_));
  }
  last_position_ = position.next_sample;
  last_generation_ = position.generation;
}

void LoopTransport::count_wraps(audio_io::TransportCursor::LoopWrapCounts wraps) {
  if (wraps.applied < wraps_seen_.applied || wraps.late < wraps_seen_.late)
    wraps_seen_ = {}; // another run's cursor
  if (wraps.applied == wraps_seen_.applied && wraps.late == wraps_seen_.late)
    return;
  const auto late = wraps.late - wraps_seen_.late;
  gapless_wraps_ += static_cast<int>(wraps.applied - wraps_seen_.applied - late);
  late_wraps_ += static_cast<int>(late);
  wraps_seen_ = wraps;
  emit changed();
}

} // namespace oscillate::quick
