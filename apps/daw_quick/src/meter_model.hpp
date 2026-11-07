#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace oscillate::quick {

// One engine meter window: post-fader linear peaks and RMS per side.
struct MeterReading {
  float left_peak = 0.0F;
  float right_peak = 0.0F;
  float left_rms = 0.0F;
  float right_rms = 0.0F;
  bool clipped = false;
};

// One channel's displayed level, with its ballistics run in C++ so QML only
// draws: the peak rises at once and falls at a steady rate, a held-peak
// line waits before falling, RMS is smoothed, and a clip light stays lit
// for a moment. Each channel signals alone, so only its own meter redraws.
class MeterChannel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool valid READ valid NOTIFY changed)
  Q_PROPERTY(double leftPeak READ left_peak NOTIFY changed)
  Q_PROPERTY(double rightPeak READ right_peak NOTIFY changed)
  Q_PROPERTY(double leftHold READ left_hold NOTIFY changed)
  Q_PROPERTY(double rightHold READ right_hold NOTIFY changed)
  Q_PROPERTY(double leftRms READ left_rms NOTIFY changed)
  Q_PROPERTY(double rightRms READ right_rms NOTIFY changed)
  Q_PROPERTY(bool clipped READ clipped NOTIFY changed)
public:
  // Display ballistics, in decibels and seconds.
  static constexpr double floor_db = -70.0;
  static constexpr double peak_fall_db_per_second = 26.0;
  static constexpr double hold_seconds = 1.2;
  static constexpr double hold_fall_db_per_second = 18.0;
  static constexpr double rms_seconds = 0.3;
  static constexpr double clip_seconds = 1.5;

  explicit MeterChannel(QObject* parent = nullptr) : QObject(parent) {}

  // Advances the display by `seconds` towards `reading` (none: falling to
  // silence). True while anything is still visible.
  bool advance(const std::optional<MeterReading>& reading, double seconds);

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] double left_peak() const noexcept { return left_.peak; }
  [[nodiscard]] double right_peak() const noexcept { return right_.peak; }
  [[nodiscard]] double left_hold() const noexcept { return left_.hold; }
  [[nodiscard]] double right_hold() const noexcept { return right_.hold; }
  [[nodiscard]] double left_rms() const noexcept { return left_.rms; }
  [[nodiscard]] double right_rms() const noexcept { return right_.rms; }
  [[nodiscard]] bool clipped() const noexcept { return clip_left_ > 0.0; }

signals:
  void changed();

private:
  // One side, displayed as linear amplitudes; the falls run in decibels.
  struct Side {
    double peak = 0.0;
    double hold = 0.0;
    double hold_left = 0.0;
    double power = 0.0;
    double rms = 0.0;
    [[nodiscard]] bool visible() const noexcept { return peak > 0.0 || hold > 0.0 || rms > 0.0; }
  };
  static void advance_side(Side& side, std::optional<float> peak, std::optional<float> rms,
                           double seconds);

  Side left_;
  Side right_;
  double clip_left_ = 0.0;
  bool valid_ = false;
};

// Every channel's meter, on its own display clock (about 60 Hz), never
// tied to edits or transport polling. `Source` reads a channel's newest
// engine window by track ID (empty for the master), or none.
class MeterModel final : public QObject {
  Q_OBJECT
public:
  using Source = std::function<std::optional<MeterReading>(const QString& id)>;
  static constexpr int interval_ms = 16;

  explicit MeterModel(Source source, QObject* parent = nullptr);

  // The channel's meter, made on first use and kept by this model. An empty
  // ID is the master.
  Q_INVOKABLE QObject* channel(const QString& id);
  // Starts the clock if it is idle: readings may have begun. It stops by
  // itself once nothing reads and every meter has fallen to silence.
  void wake();
  // One display step of `seconds`, as the clock runs it.
  void tick(double seconds);

private:
  Source source_;
  QHash<QString, MeterChannel*> channels_;
  QTimer timer_;
  QElapsedTimer clock_;
};

} // namespace oscillate::quick
