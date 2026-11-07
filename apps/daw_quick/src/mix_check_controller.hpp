#pragma once

#include "oscillate/application/mix_check.hpp"
#include "status_text.hpp"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace oscillate::quick {

// Mix Check (ADR-0074, docs/ui/mix-check-mockup.html) for the QML screen.
// run() renders and measures on a worker (cancellable, never blocking the
// UI); apply() commits a finding's fix as one undo step and re-checks;
// listen() solos the finding's tracks and plays from where it is worst.
class MixCheckController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool running READ running NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QString stage READ stage NOTIFY changed)
  Q_PROPERTY(bool stale READ stale NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(bool hasResult READ has_result NOTIFY changed)
  // Re-read every few seconds ("checked 41 s ago"), on its own signal so
  // the findings are not rebuilt each time.
  Q_PROPERTY(QString checkedAt READ checked_at NOTIFY checkedAtChanged)
  Q_PROPERTY(QVariantMap summary READ summary NOTIFY changed)
  Q_PROPERTY(QVariantMap spectrum READ spectrum NOTIFY changed)
  Q_PROPERTY(QVariantList meters READ meters NOTIFY changed)
  Q_PROPERTY(QString target READ target WRITE set_target NOTIFY changed)
  Q_PROPERTY(QVariantList targets READ targets CONSTANT)
  Q_PROPERTY(QVariantMap loudness READ loudness NOTIFY changed)
  Q_PROPERTY(QVariantList energy READ energy NOTIFY changed)
  Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)
  Q_PROPERTY(QString energyNote READ energy_note NOTIFY changed)
  Q_PROPERTY(QVariantList findings READ findings NOTIFY changed)
  Q_PROPERTY(QVariantList fine READ fine NOTIFY changed)
  Q_PROPERTY(QVariantList sinceLast READ since_last NOTIFY changed)
  Q_PROPERTY(QString lastCheckedAt READ last_checked_at NOTIFY changed)
  Q_PROPERTY(QVariantList reference READ reference NOTIFY changed)
  Q_PROPERTY(QString referenceName READ reference_name NOTIFY changed)
  Q_PROPERTY(QString listening READ listening NOTIFY changed)

public:
  // What the controller needs from the shell. blocked() names why a check
  // cannot start now (playing, exporting, another operation), or is empty;
  // can_edit() says whether a fix may be committed (and reports why not).
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    std::function<QString()> blocked;
    std::function<bool()> can_edit;
    std::function<void()> commit;
    std::function<QString()> project_name;
    std::function<void(project::TrackId, bool)> set_solo;
    std::function<void(std::uint64_t)> play_from;
    std::function<void()> stop;
  };

  explicit MixCheckController(Hooks hooks, QObject* parent = nullptr);
  MixCheckController(const MixCheckController&) = delete;
  MixCheckController& operator=(const MixCheckController&) = delete;
  ~MixCheckController() override;

  Q_INVOKABLE void run();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void apply(const QString& finding_id);
  Q_INVOKABLE void listen(const QString& finding_id);
  Q_INVOKABLE void stopListening();
  Q_INVOKABLE void setReference(const QString& path);
  // Opens a file dialog for an audio file, then setReference.
  Q_INVOKABLE void chooseReference();
  Q_INVOKABLE void clearReference();

  [[nodiscard]] bool running() const noexcept { return job_ != nullptr; }
  [[nodiscard]] double progress() const noexcept;
  [[nodiscard]] QString stage() const;
  [[nodiscard]] bool stale() const noexcept { return stale_; }
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] bool has_result() const noexcept { return report_.has_value(); }
  [[nodiscard]] QString checked_at() const;
  [[nodiscard]] QVariantMap summary() const;
  [[nodiscard]] QVariantMap spectrum() const;
  [[nodiscard]] QVariantList meters() const;
  [[nodiscard]] QString target() const { return QString::fromStdString(target_); }
  void set_target(const QString& id);
  [[nodiscard]] QVariantList targets() const;
  [[nodiscard]] QVariantMap loudness() const;
  [[nodiscard]] QVariantList energy() const;
  [[nodiscard]] QVariantList sections() const;
  [[nodiscard]] QString energy_note() const;
  [[nodiscard]] QVariantList findings() const;
  [[nodiscard]] QVariantList fine() const;
  [[nodiscard]] QVariantList since_last() const;
  [[nodiscard]] QString last_checked_at() const;
  [[nodiscard]] QVariantList reference() const;
  [[nodiscard]] QString reference_name() const { return reference_name_; }
  [[nodiscard]] QString listening() const { return listening_; }

  // The project's revision moved: the result goes stale when the mix did.
  void project_changed();
  // A project closed or opened: everything measured is forgotten.
  void reset();
  // Tests: drive the current job to its end on this thread.
  bool wait(int timeout_ms);

signals:
  void changed();
  void checkedAtChanged();

private:
  struct Job {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<double> progress{0.0};
    mutable std::mutex mutex;
    std::string stage;
    std::string target;
    std::optional<foundation::Result<application::mix_check::Measurements>> result;
    std::optional<application::mix_check::Report> report;
    std::optional<application::mix_check::History> previous;
    std::int64_t checked_at = 0;
    // Set for the re-check after a fix: the finding applied.
    std::optional<application::mix_check::Finding> applied;
    // The session the run started in; a report for another is dropped.
    std::uint64_t session = 0;
  };
  struct ReferenceJob {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::optional<foundation::Result<application::mix_check::ReferenceTrack>> result;
  };

  void start(std::optional<application::mix_check::Finding> applied);
  void poll();
  void finish();
  void finish_reference();
  void rebuild();
  void set_status(QString status);
  [[nodiscard]] const application::mix_check::Finding* find(const QString& id) const;

  Hooks hooks_;
  QTimer timer_;
  QTimer clock_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
  std::shared_ptr<ReferenceJob> reference_job_;
  std::jthread reference_worker_;

  std::string target_{application::mix_check::default_target};
  StatusText status_;
  bool stale_ = false;
  std::shared_ptr<const application::mix_check::Measurements> measured_;
  std::optional<application::mix_check::Report> report_;
  // The first check's spectrum: "before" until the next Run.
  std::vector<double> baseline_spectrum_;
  bool after_ = false;
  std::map<std::string, application::mix_check::Finding> applied_;
  std::optional<application::mix_check::History> previous_history_;
  std::optional<application::mix_check::History> current_history_;
  QDateTime checked_time_;
  std::optional<application::mix_check::ReferenceTrack> reference_track_;
  QString reference_name_;
  QString listening_;
  std::map<project::TrackId, bool> solo_before_;
};

} // namespace oscillate::quick
