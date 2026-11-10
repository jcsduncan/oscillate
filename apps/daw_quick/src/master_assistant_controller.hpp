#pragma once

#include "background_job.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/application/master_assistant.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
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
#include <vector>

namespace oscillate::application {
class ProjectSession;
} // namespace oscillate::application

// The master assistant (ADR-0124) for QML: the goals (a genre target or
// reference tracks, a loudness target, a character, the whole song or the
// loop), Analyse, which measures and plans on a worker, the measurements,
// the proposed devices and nudges each with its line and a switch to keep
// it, the level-matched A/B on the preview device, Accept (one undo step)
// and Reject, and the master measured again once accepted.
namespace oscillate::quick {

class MasterAssistantController final : public QObject {
  Q_OBJECT
  // "idle", "working", "proposed", "accepted" or "verified".
  Q_PROPERTY(QString phase READ phase NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
  Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  // The mix changed since it was measured: Analyse again.
  Q_PROPERTY(bool stale READ stale NOTIFY changed)
  // Goals.
  Q_PROPERTY(QVariantList loudnessTargets READ loudness_targets CONSTANT)
  Q_PROPERTY(QString loudness READ loudness WRITE set_loudness NOTIFY changed)
  Q_PROPERTY(double customLufs READ custom_lufs WRITE set_custom_lufs NOTIFY changed)
  Q_PROPERTY(double targetLufs READ target_lufs NOTIFY changed)
  Q_PROPERTY(double targetCeiling READ target_ceiling NOTIFY changed)
  Q_PROPERTY(QStringList characters READ characters CONSTANT)
  Q_PROPERTY(QString character READ character WRITE set_character NOTIFY changed)
  Q_PROPERTY(QVariantList genres READ genres CONSTANT)
  Q_PROPERTY(QString genre READ genre WRITE set_genre NOTIFY changed)
  // Reference tracks, when any: {name, path}. They replace the genre.
  Q_PROPERTY(QVariantList references READ references NOTIFY changed)
  Q_PROPERTY(QString referenceName READ reference_name NOTIFY changed)
  Q_PROPERTY(bool loopAvailable READ loop_available NOTIFY changed)
  Q_PROPERTY(bool useLoop READ use_loop WRITE set_use_loop NOTIFY changed)
  // What was measured: readouts, the regions and the tonal curves.
  Q_PROPERTY(QVariantMap measured READ measured NOTIFY changed)
  // The proposal: {index, engine, name, reason, settings, accepted}.
  Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
  // {index, role, gain, reason, accepted}.
  Q_PROPERTY(QVariantList nudges READ nudges NOTIFY changed)
  Q_PROPERTY(QStringList notes READ notes NOTIFY changed)
  // The proposal's outcome against the goals, and once accepted the
  // master's as rendered.
  Q_PROPERTY(QStringList prediction READ prediction NOTIFY changed)
  Q_PROPERTY(QStringList verification READ verification NOTIFY changed)
  Q_PROPERTY(bool verifiedOnTarget READ verified_on_target NOTIFY changed)
  // The A/B.
  Q_PROPERTY(bool auditioning READ auditioning NOTIFY changed)
  Q_PROPERTY(bool hearingProposal READ hearing_proposal NOTIFY changed)
  Q_PROPERTY(QVariantMap audition READ audition NOTIFY changed)

public:
  // The worker is polled this often.
  static constexpr int poll_ms = 50;
  // The tonal view's span below and above the target, in dB.
  static constexpr double curve_range_db = 12.0;

  // The preview device: its output factory (empty: PipeWire), target and
  // block, or nothing (the status says why).
  struct Preview {
    application::PlaybackOutputFactory factory;
    std::optional<std::uint64_t> target_serial;
    std::uint32_t buffer_frames = 0;
  };
  // What the controller needs from the shell. blocked() names why a run
  // cannot start now (an export, another operation), or is empty.
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    std::function<QString()> blocked;
    std::function<bool()> can_edit;
    std::function<void()> commit;
    std::function<std::optional<application::master_assistant::Range>()> loop;
    std::function<std::optional<Preview>()> preview;
  };

  explicit MasterAssistantController(Hooks hooks, QObject* parent = nullptr);
  MasterAssistantController(const MasterAssistantController&) = delete;
  MasterAssistantController& operator=(const MasterAssistantController&) = delete;
  ~MasterAssistantController() override;

  // Measures (when the mix changed or was never measured) and proposes.
  Q_INVOKABLE void analyse();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void setDeviceAccepted(int index, bool accepted);
  Q_INVOKABLE void setNudgeAccepted(int index, bool accepted);
  // The kept devices and nudges as one undo step, then the master measured.
  Q_INVOKABLE void accept();
  // Drops the proposal; the project never changed.
  Q_INVOKABLE void reject();
  Q_INVOKABLE void chooseReferences();
  Q_INVOKABLE void addReference(const QString& path);
  Q_INVOKABLE void removeReference(int index);
  Q_INVOKABLE void startAudition();
  Q_INVOKABLE void stopAudition();
  Q_INVOKABLE void hearProposal(bool proposal);

  [[nodiscard]] QString phase() const;
  [[nodiscard]] bool busy() const noexcept { return job_.running(); }
  [[nodiscard]] double progress() const noexcept;
  [[nodiscard]] QString stage() const;
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] bool stale() const noexcept { return stale_; }
  [[nodiscard]] QVariantList loudness_targets() const;
  [[nodiscard]] QString loudness() const { return loudness_; }
  void set_loudness(const QString& id);
  [[nodiscard]] double custom_lufs() const noexcept { return custom_lufs_; }
  void set_custom_lufs(double lufs);
  [[nodiscard]] double target_lufs() const noexcept { return goals().lufs; }
  [[nodiscard]] double target_ceiling() const noexcept { return goals().ceiling_dbtp; }
  [[nodiscard]] QStringList characters() const;
  [[nodiscard]] QString character() const;
  void set_character(const QString& name);
  [[nodiscard]] QVariantList genres() const;
  [[nodiscard]] QString genre() const { return genre_; }
  void set_genre(const QString& id);
  [[nodiscard]] QVariantList references() const;
  [[nodiscard]] QString reference_name() const;
  [[nodiscard]] bool loop_available() const;
  [[nodiscard]] bool use_loop() const noexcept { return use_loop_; }
  void set_use_loop(bool on);
  [[nodiscard]] QVariantMap measured() const;
  [[nodiscard]] QVariantList devices() const;
  [[nodiscard]] QVariantList nudges() const;
  [[nodiscard]] QStringList notes() const;
  [[nodiscard]] QStringList prediction() const;
  [[nodiscard]] QStringList verification() const { return verification_; }
  [[nodiscard]] bool verified_on_target() const noexcept { return verified_on_target_; }
  [[nodiscard]] bool auditioning() const noexcept { return output_ != nullptr; }
  [[nodiscard]] bool hearing_proposal() const;
  [[nodiscard]] QVariantMap audition() const;

  // The project's revision moved or another project opened.
  void project_changed();
  // Tests: waits for the running job and installs its result.
  bool wait();
  // Tests: the A/B player.
  [[nodiscard]] application::master_assistant::Audition* player() const noexcept {
    return player_.get();
  }

signals:
  void changed();
  void progressChanged();

private:
  struct Progress {
    std::atomic<double> fraction{0.0};
    std::atomic<bool> cancel{false};
    mutable std::mutex mutex;
    std::string stage;
  };
  struct Done {
    foundation::Result<void> result = foundation::Result<void>::success();
    std::optional<application::master_assistant::Measured> measured;
    std::optional<application::master_assistant::Proposed> proposed;
    std::optional<native_dsp::assistant::Analysis> verified;
    std::map<std::string, native_dsp::assistant::Analysis> references;
  };

  [[nodiscard]] native_dsp::assistant::Goals goals() const noexcept;
  [[nodiscard]] application::master_assistant::Selection selection() const;
  void start(bool verify);
  void poll();
  void finish();
  void set_status(QString text);
  void goals_changed();

  Hooks hooks_;
  QTimer timer_;
  BackgroundJob<Done> job_;
  std::shared_ptr<Progress> progress_;
  std::uint64_t generation_ = 0;
  StatusText status_;
  bool stale_ = false;
  bool accepted_ = false;
  QString loudness_;
  double custom_lufs_;
  native_dsp::assistant::Character character_ = native_dsp::assistant::Character::clean;
  QString genre_;
  std::vector<std::string> reference_paths_;
  std::map<std::string, native_dsp::assistant::Analysis> reference_cache_;
  bool use_loop_ = false;
  std::shared_ptr<const application::master_assistant::Measured> measured_;
  std::optional<application::master_assistant::Proposed> proposed_;
  std::optional<native_dsp::assistant::Reference> reference_;
  std::vector<bool> device_kept_;
  std::vector<bool> nudge_kept_;
  QStringList verification_;
  bool verified_on_target_ = false;
  // The A/B.
  std::unique_ptr<application::master_assistant::Audition> player_;
  std::unique_ptr<audio_io::TransportCursor> cursor_;
  std::unique_ptr<application::PlaybackOutput> output_;
};

} // namespace oscillate::quick
