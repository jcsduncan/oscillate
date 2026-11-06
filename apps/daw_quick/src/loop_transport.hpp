#pragma once

#include "oscillate/application/midi_recording.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/project/model.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

namespace oscillate::quick {

class RoutedPlayback;

// Control-sequencer loop playback for the routed path. The user range is in
// bars; it is armed on the playing source (RoutedPlayback::set_loop), and
// every poll services it (RoutedPlayback::service_loop): the next pass is
// queued as a planned wrap at the loop end once the source reports it primed,
// and the transport splits the device block there, so a wrap is
// sample-exact at any bar. Changing or disabling the range withdraws a
// queued wrap. If priming missed, playback crosses the loop end unprimed, or
// a wrap was queued only after the loop end had started playing, the status
// and counters say so (seek restarts, late wraps). Nothing here runs on the
// audio callback.
class LoopTransport final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
  Q_PROPERTY(int startBar READ start_bar NOTIFY changed)
  Q_PROPERTY(int endBar READ end_bar NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(int lateWraps READ late_wraps NOTIFY changed)

public:
  using Snapshot = std::function<const project::Snapshot*()>;
  using Playback = std::function<RoutedPlayback*()>;
  // Whether a transport seek can start now (no transport change in flight).
  using CanSeek = std::function<bool()>;
  using Seek = std::function<void(std::uint64_t sample)>;

  LoopTransport(Snapshot snapshot, Playback playback, CanSeek can_seek, Seek seek,
                QObject* parent = nullptr);

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  [[nodiscard]] int start_bar() const noexcept { return start_bar_; }
  [[nodiscard]] int end_bar() const noexcept { return end_bar_; }
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] int gapless_wraps() const noexcept { return gapless_wraps_; }
  [[nodiscard]] int seek_restarts() const noexcept { return seek_restarts_; }
  // Wraps queued after the in-flight block had already passed the loop end:
  // they applied at the next block start, so that pass ran long.
  [[nodiscard]] int late_wraps() const noexcept { return late_wraps_; }

  // One-based bars; the loop covers [start, end) bars.
  Q_INVOKABLE void set_range(int start_bar, int end_bar);
  Q_INVOKABLE void set_enabled(bool enabled);
  // Control sequencer: called on each playback poll and when playback stops.
  void poll();
  // Where the loop starts while it is on: Stop goes back there instead of bar 1.
  [[nodiscard]] std::optional<std::uint64_t> start_sample() const;
  // The loop's samples while it is on (a recording wraps presses into it).
  [[nodiscard]] std::optional<application::LoopSamples> loop_samples() const;

signals:
  void changed();

private:
  struct Samples {
    std::uint64_t start;
    std::uint64_t end;
  };
  [[nodiscard]] std::optional<Samples> samples() const;
  void set_status(QString status);
  void count_wraps(audio_io::TransportCursor::LoopWrapCounts wraps);

  Snapshot snapshot_;
  Playback playback_;
  CanSeek can_seek_;
  Seek seek_;
  bool enabled_ = false;
  int start_bar_ = 1;
  int end_bar_ = 5;
  StatusText status_{QStringLiteral("Loop off")};
  int gapless_wraps_ = 0;
  int seek_restarts_ = 0;
  int late_wraps_ = 0;
  audio_io::TransportCursor::LoopWrapCounts wraps_seen_;
  RoutedPlayback* armed_playback_ = nullptr;
  std::optional<Samples> armed_;
  std::uint64_t last_position_ = 0;
  std::uint64_t last_generation_ = 0;
};

} // namespace oscillate::quick
