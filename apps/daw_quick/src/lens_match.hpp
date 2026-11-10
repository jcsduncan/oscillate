#pragma once

#include "oscillate/native_dsp/match_eq.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::quick {

class NativeSynthDeviceController;
class NativeSynthPanelAdapter;

// Lens's match EQ and tonal balance (ADR-0120), off the audio thread. It
// keeps its own long-term average of Lens's tonal meter (the input's mid and
// the sidechain's, per fine band) from each telemetry frame the panel polls,
// and from it: the correction to the sidechain or a target with Lens's
// match settings, the fewest Shape bands that draw it, and the mix against
// the chosen target's range. Apply writes the bands into the Shape right
// after Lens (a new one when the next device is not one) as one undo step.
class LensMatch final : public QObject {
  Q_OBJECT
  // Bumps whenever anything below changes.
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  // How long the average covers, in seconds; ready once it covers
  // ready_seconds and there is something to match to.
  Q_PROPERTY(double seconds READ seconds NOTIFY changed)
  Q_PROPERTY(bool ready READ ready NOTIFY changed)
  Q_PROPERTY(bool matchesTarget READ matches_target NOTIFY changed)
  // The sidechain has carried signal over the average.
  Q_PROPERTY(bool hasSidechain READ has_sidechain NOTIFY changed)
  // The fine bands' centres, the correction and the fitted response there
  // (dB), and the bands [{kind ("Peak", "Low Shelf", "High Shelf"), hz,
  // shape, gain}] with the fit's error (dB, RMS and largest).
  Q_PROPERTY(QVariantList fineHz READ fine_hz CONSTANT)
  Q_PROPERTY(QVariantList correction READ correction NOTIFY changed)
  Q_PROPERTY(QVariantList response READ response NOTIFY changed)
  Q_PROPERTY(QVariantList bands READ bands NOTIFY changed)
  Q_PROPERTY(double fitRms READ fit_rms NOTIFY changed)
  Q_PROPERTY(double fitMax READ fit_max NOTIFY changed)
  // The targets [{id, name, references, user}], factory first, and the
  // chosen one.
  Q_PROPERTY(QVariantList targets READ targets NOTIFY targets_changed)
  Q_PROPERTY(QString targetId READ target_id WRITE set_target_id NOTIFY changed)
  Q_PROPERTY(QString targetName READ target_name NOTIFY changed)
  // The grid's centres; the mix's normalised curve on it (empty before any
  // average); the chosen target's range and median; each band's placement
  // (-1 below, 0 inside, 1 above) and how many are inside.
  Q_PROPERTY(QVariantList bandHz READ band_hz CONSTANT)
  Q_PROPERTY(QVariantList tonal READ tonal NOTIFY changed)
  Q_PROPERTY(QVariantList targetLow READ target_low NOTIFY changed)
  Q_PROPERTY(QVariantList targetMedian READ target_median NOTIFY changed)
  Q_PROPERTY(QVariantList targetHigh READ target_high NOTIFY changed)
  Q_PROPERTY(QVariantList placements READ placements NOTIFY changed)
  Q_PROPERTY(int inRange READ in_range NOTIFY changed)
  // What the last Apply did, or why it did nothing; and the same for the
  // last Save as target.
  Q_PROPERTY(QString applyMessage READ apply_message NOTIFY changed)
  Q_PROPERTY(QString message READ message NOTIFY changed)

public:
  // The average must cover this long before a match is offered.
  static constexpr double ready_seconds = 2.0;
  // While the average grows, the fit is redrawn at most this often.
  static constexpr int refit_ms = 1'000;
  // A saved target's range is its reference's middle half over chunks this
  // long, kept for up to max_chunks; it needs min_chunks.
  static constexpr double chunk_seconds = 3.0;
  static constexpr std::size_t max_chunks = 1'200;
  static constexpr std::size_t min_chunks = 4;
  // User targets: at most this many files are read.
  static constexpr std::size_t max_user_targets = 200;

  // `targets_directory` holds the user's own targets (*.json).
  LensMatch(NativeSynthDeviceController& controller, NativeSynthPanelAdapter& adapter,
            QString targets_directory, QObject* parent = nullptr);

  // $XDG_DATA_HOME/oscillate/tonal-targets.
  [[nodiscard]] static QString default_targets_directory();
  // The user's targets in `directory` (*.json, by name), at most
  // max_user_targets; files that are too large or do not parse are skipped.
  [[nodiscard]] static std::vector<native_dsp::tonal::Target>
  read_user_targets(const QString& directory);

  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] double seconds() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool matches_target() const;
  [[nodiscard]] bool has_sidechain() const noexcept;
  [[nodiscard]] QVariantList fine_hz() const;
  [[nodiscard]] QVariantList correction() const;
  [[nodiscard]] QVariantList response() const;
  [[nodiscard]] QVariantList bands() const;
  [[nodiscard]] double fit_rms() const noexcept { return fit_.rms_db; }
  [[nodiscard]] double fit_max() const noexcept { return fit_.max_db; }
  [[nodiscard]] QVariantList targets() const;
  [[nodiscard]] QString target_id() const { return target_id_; }
  void set_target_id(const QString& id);
  [[nodiscard]] QString target_name() const;
  [[nodiscard]] QVariantList band_hz() const;
  [[nodiscard]] QVariantList tonal() const;
  [[nodiscard]] QVariantList target_low() const;
  [[nodiscard]] QVariantList target_median() const;
  [[nodiscard]] QVariantList target_high() const;
  [[nodiscard]] QVariantList placements() const;
  [[nodiscard]] int in_range() const;
  [[nodiscard]] QString apply_message() const { return apply_message_; }
  [[nodiscard]] QString message() const { return message_; }

  // Reads Lens's newest telemetry frame (the panel calls it from its poll).
  Q_INVOKABLE void poll();
  // Starts the average again.
  Q_INVOKABLE void reset();
  // The next or previous target.
  Q_INVOKABLE void step_target(int direction);
  // Writes the fitted bands into the Shape after Lens; false when there is
  // nothing to write or it is refused (message says why).
  Q_INVOKABLE bool apply();
  // Saves what the sidechain carried (the input when it carried nothing)
  // as a user target named after now; false when the average is too short
  // or the file cannot be written.
  Q_INVOKABLE bool save_target();

  // A Lens telemetry frame, as poll() reads it (tests feed frames here).
  void ingest(std::span<const float> frame);

signals:
  void changed();
  void targets_changed();

private:
  void load_targets();
  void refit();
  [[nodiscard]] const native_dsp::tonal::Target* target() const;
  void bump();

  NativeSynthDeviceController& controller_;
  NativeSynthPanelAdapter& adapter_;
  QString directory_;
  std::vector<native_dsp::tonal::Target> user_targets_;
  QString target_id_;

  // The tonal meter's mean and window count at the last frame, so each
  // frame adds only what is new.
  std::array<native_dsp::tonal::FineCurve, 2> last_mean_{};
  std::uint64_t last_windows_ = 0;
  // This model's own average: summed band powers and the windows they cover.
  std::array<native_dsp::tonal::FineCurve, 2> sum_{};
  std::uint64_t windows_ = 0;
  // The current chunk, and the finished chunks' normalised curves.
  std::array<native_dsp::tonal::FineCurve, 2> chunk_{};
  std::uint64_t chunk_windows_ = 0;
  std::array<std::vector<native_dsp::tonal::Curve>, 2> chunks_;

  native_dsp::match_eq::FineCurve correction_{};
  native_dsp::match_eq::Fit fit_{};
  bool fitted_ = false;
  std::vector<double> fitted_values_;
  QElapsedTimer since_fit_;
  // The match settings and source the fit was drawn with: other Lens
  // values changing do not redraw it.
  native_dsp::match_eq::Settings fit_settings_{};
  bool fit_to_target_ = false;
  QString apply_message_;
  QString message_;
  int revision_ = 0;
};

} // namespace oscillate::quick
