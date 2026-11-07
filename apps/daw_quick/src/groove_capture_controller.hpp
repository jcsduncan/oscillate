#pragma once

#include "oscillate/application/groove_capture.hpp"
#include "oscillate/application/project_session.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace oscillate::quick {

// Groove from a record (ADR-0079, docs/ui/creative-tools-mockup.html#groove):
// the panel's state. A reference is an audio clip in the project or a
// library file; "Take groove" measures it on a worker (application::
// groove_capture), and the result is a fingerprint for the panel, a groove
// to keep in the pool (through NoteToolsController::keep_groove) and an
// A/B audition of straight hats against the grooved ones, written as two
// short WAV files the browser's sample preview plays.
class GrooveCaptureController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool open READ is_open WRITE set_open NOTIFY openChanged)
  // The project's audio clips: {id, name, track}.
  Q_PROPERTY(QVariantList clips READ clips NOTIFY clipsChanged)
  // "clip:<id>", "file:<path>" or "" before one is chosen.
  Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
  Q_PROPERTY(QString sourceName READ source_name NOTIFY sourceChanged)
  Q_PROPERTY(bool sourceIsFile READ source_is_file NOTIFY sourceChanged)
  Q_PROPERTY(QStringList bandNames READ band_names CONSTANT)
  // An index into bandNames.
  Q_PROPERTY(int band MEMBER band_ NOTIFY settingsChanged)
  Q_PROPERTY(int bars MEMBER bars_ NOTIFY settingsChanged)
  // 16 or 32 steps.
  Q_PROPERTY(int steps MEMBER steps_ NOTIFY settingsChanged)
  // 2 (8ths), 4 (16ths) or 8 (32nds).
  Q_PROPERTY(int stepsPerQuarter MEMBER steps_per_quarter_ NOTIFY settingsChanged)
  // Library files: the record's tempo, 0 to find it (its name, then its kick).
  Q_PROPERTY(double bpm MEMBER bpm_ NOTIFY settingsChanged)
  // The name the groove is kept under.
  Q_PROPERTY(QString name MEMBER name_ NOTIFY settingsChanged)
  Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  Q_PROPERTY(QString stage READ stage NOTIFY runningChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(bool hasResult READ has_result NOTIFY resultChanged)
  // The fingerprint: {bpm, gridFit, confidence, swing, offbeatMs, accents,
  // hits, stepMs, stepsPerQuarter, summary, steps: [{timing, timingMs,
  // spread, spreadMs, velocity, hits}], folded: [{position, strength}],
  // barPeaks: [..], barOnsets: [..], timings: [..], velocities: [..]}.
  Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
  // The A/B audition files ("" until a groove is taken).
  Q_PROPERTY(QString auditionStraight READ audition_straight NOTIFY resultChanged)
  Q_PROPERTY(QString auditionGrooved READ audition_grooved NOTIFY resultChanged)

public:
  using Session = std::function<application::ProjectSession*()>;
  using Keep = std::function<bool(persistence::GrooveTemplate)>;
  using Report = std::function<void(QString)>;

  GrooveCaptureController(Session session, Keep keep, Report report, QObject* parent = nullptr);
  ~GrooveCaptureController() override;

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  void set_open(bool open);
  [[nodiscard]] QVariantList clips() const { return clips_; }
  [[nodiscard]] QString source() const { return source_; }
  [[nodiscard]] QString source_name() const { return source_name_; }
  [[nodiscard]] bool source_is_file() const { return source_.startsWith(QStringLiteral("file:")); }
  [[nodiscard]] static QStringList band_names();
  [[nodiscard]] static int min_bars() noexcept;
  [[nodiscard]] static int max_bars() noexcept;
  [[nodiscard]] bool running() const noexcept { return job_ != nullptr; }
  [[nodiscard]] QString stage() const;
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] bool has_result() const noexcept { return capture_.has_value(); }
  [[nodiscard]] QVariantMap result() const { return result_; }
  [[nodiscard]] QString audition_straight() const { return audition_straight_; }
  [[nodiscard]] QString audition_grooved() const { return audition_grooved_; }

  // Opens the panel on an audio clip (its menu's "Take groove…").
  Q_INVOKABLE void open_for_clip(const QString& clip_id);
  Q_INVOKABLE void use_clip(const QString& clip_id);
  // A path or a file:// URL.
  Q_INVOKABLE void use_file(const QString& path);
  Q_INVOKABLE void choose_file();
  // Measures the reference on a worker; the result arrives with resultChanged.
  Q_INVOKABLE void take();
  Q_INVOKABLE void cancel();
  // Keeps the measured groove in the pool under `name` and makes it the
  // current groove; false (with a status) when there is none or no name.
  Q_INVOKABLE bool save();

  // Test hook: waits for a running take to finish.
  bool wait_for_take(int timeout_ms);

public slots:
  // The project changed: the clip list follows, a clip that went away is
  // no longer the reference.
  void project_changed();

signals:
  void openChanged();
  void clipsChanged();
  void sourceChanged();
  void settingsChanged();
  void runningChanged();
  void statusChanged();
  void resultChanged();

private:
  struct Job {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    mutable std::mutex mutex;
    std::string stage;
    std::optional<foundation::Result<application::groove_capture::Capture>> result;
  };

  [[nodiscard]] std::optional<application::groove_capture::Request> request() const;
  void poll();
  void finish();
  void publish(const application::groove_capture::Capture& capture);
  void write_audition();
  void set_status(QString status);
  void set_source(QString source, QString name);

  Session session_;
  Keep keep_;
  Report report_;
  QTimer timer_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;

  bool open_ = false;
  QVariantList clips_;
  QString source_;
  QString source_name_;
  int band_ = static_cast<int>(application::groove_capture::Band::hats);
  int bars_ = static_cast<int>(application::groove_capture::default_bars);
  int steps_ = 16;
  int steps_per_quarter_ = 4;
  double bpm_ = 0.0;
  QString name_;
  StatusText status_;
  std::optional<application::groove_capture::Capture> capture_;
  QVariantMap result_;
  QString audition_straight_;
  QString audition_grooved_;
};

} // namespace oscillate::quick
