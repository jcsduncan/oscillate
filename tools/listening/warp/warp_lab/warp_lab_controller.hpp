#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "oscillate/warp_dsp/analysis.hpp"

namespace oscillate::warp_lab {

// Sandbox controller: loads one 48 kHz stereo WAV, analyses it once, renders
// it through any warp mode on a worker thread and plays source or render
// through PipeWire for A/B listening. Not part of the DAW.
class WarpLabController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
  Q_PROPERTY(bool hasSource READ has_source NOTIFY sourceChanged)
  Q_PROPERTY(QString sourceName READ source_name NOTIFY sourceChanged)
  Q_PROPERTY(QString sourceInfo READ source_info NOTIFY sourceChanged)
  Q_PROPERTY(QVariantList sourceWaveform READ source_waveform NOTIFY sourceChanged)
  Q_PROPERTY(QVariantList transients READ transients NOTIFY sourceChanged)
  Q_PROPERTY(bool needsConversion READ needs_conversion NOTIFY sourceChanged)
  Q_PROPERTY(bool hasRender READ has_render NOTIFY renderChanged)
  Q_PROPERTY(QString renderInfo READ render_info NOTIFY renderChanged)
  Q_PROPERTY(QVariantList renderWaveform READ render_waveform NOTIFY renderChanged)
  Q_PROPERTY(QString playing READ playing NOTIFY playingChanged)

  Q_PROPERTY(int mode MEMBER mode_ NOTIFY settingsChanged)
  Q_PROPERTY(double speed MEMBER speed_ NOTIFY settingsChanged)
  Q_PROPERTY(int semitones MEMBER semitones_ NOTIFY settingsChanged)
  Q_PROPERTY(int cents MEMBER cents_ NOTIFY settingsChanged)
  Q_PROPERTY(int tier MEMBER tier_ NOTIFY settingsChanged)
  Q_PROPERTY(bool tempoChange MEMBER tempo_change_ NOTIFY settingsChanged)
  Q_PROPERTY(int beatsLoop MEMBER beats_loop_ NOTIFY settingsChanged)
  Q_PROPERTY(double beatsEnvelope MEMBER beats_envelope_ NOTIFY settingsChanged)
  Q_PROPERTY(double formants MEMBER formants_ NOTIFY settingsChanged)
  Q_PROPERTY(double envelope MEMBER envelope_ NOTIFY settingsChanged)
  Q_PROPERTY(double tonesGrain MEMBER tones_grain_ NOTIFY settingsChanged)
  Q_PROPERTY(double textureGrain MEMBER texture_grain_ NOTIFY settingsChanged)
  Q_PROPERTY(double flux MEMBER flux_ NOTIFY settingsChanged)
  Q_PROPERTY(bool autoRender MEMBER auto_render_ NOTIFY settingsChanged)

public:
  explicit WarpLabController(QObject* parent = nullptr);
  ~WarpLabController() override;
  WarpLabController(const WarpLabController&) = delete;
  WarpLabController& operator=(const WarpLabController&) = delete;
  WarpLabController(WarpLabController&&) = delete;
  WarpLabController& operator=(WarpLabController&&) = delete;

  [[nodiscard]] QString status() const { return status_; }
  [[nodiscard]] bool busy() const { return busy_; }
  [[nodiscard]] bool has_source() const { return !left_.empty(); }
  [[nodiscard]] QString source_name() const { return source_name_; }
  [[nodiscard]] QString source_info() const { return source_info_; }
  [[nodiscard]] QVariantList source_waveform() const { return source_waveform_; }
  [[nodiscard]] QVariantList transients() const { return transient_view_; }
  [[nodiscard]] bool needs_conversion() const { return !conversion_input_.isEmpty(); }
  [[nodiscard]] bool has_render() const { return !render_path_.isEmpty(); }
  [[nodiscard]] QString render_info() const { return render_info_; }
  [[nodiscard]] QVariantList render_waveform() const { return render_waveform_; }
  [[nodiscard]] QString playing() const { return playing_; }

  Q_INVOKABLE void load(const QUrl& file);
  Q_INVOKABLE void convertAndLoad();
  Q_INVOKABLE void render();
  Q_INVOKABLE void playSource();
  Q_INVOKABLE void playRender();
  Q_INVOKABLE void stop();
  Q_INVOKABLE void saveRender(const QUrl& file);

signals:
  void statusChanged();
  void sourceChanged();
  void renderChanged();
  void playingChanged();
  void settingsChanged();

private:
  struct Loaded;
  struct Rendered;

  void set_status(QString text);
  void set_busy(bool value);
  void load_path(const QString& path, const QString& note);
  void finish_load(std::shared_ptr<Loaded> loaded);
  void finish_render(std::shared_ptr<Rendered> rendered, std::uint64_t generation);
  void play(const QString& path, const QString& which);
  void join_worker();

  QString status_ = QStringLiteral("Drop a WAV file to begin");
  bool busy_ = false;
  QString source_path_;
  QString source_name_;
  QString source_info_;
  QVariantList source_waveform_;
  QVariantList transient_view_;
  QString conversion_input_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::vector<std::uint64_t> onsets_;
  std::vector<warp_dsp::analysis::PitchMark> marks_;
  QString render_path_;
  QString render_info_;
  QVariantList render_waveform_;
  QString playing_;
  QString cache_directory_;
  std::uint64_t generation_ = 0;
  bool render_queued_ = false;
  std::jthread worker_;
  QProcess* player_ = nullptr;
  QProcess* converter_ = nullptr;
  QTimer debounce_;

  int mode_ = 4;
  double speed_ = 1.0;
  int semitones_ = 0;
  int cents_ = 0;
  int tier_ = 0;
  bool tempo_change_ = false;
  int beats_loop_ = 1;
  double beats_envelope_ = 100.0;
  double formants_ = 100.0;
  double envelope_ = 128.0;
  double tones_grain_ = 40.0;
  double texture_grain_ = 100.0;
  double flux_ = 0.0;
  bool auto_render_ = true;
};

} // namespace oscillate::warp_lab
