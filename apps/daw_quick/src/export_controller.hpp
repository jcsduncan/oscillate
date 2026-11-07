#pragma once

#include "routed_playback.hpp"

#include "oscillate/application/project_session.hpp"
#if defined(OSCILLATE_HAVE_JACK_EXPORT) && OSCILLATE_HAVE_JACK_EXPORT
#include "realtime_export_run.hpp"
#endif

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace oscillate::quick {

// The export dialog (QML: projectBridge.exports): the target file, format and
// output rate, master, stem and selected-source exports on a worker, and the
// JACK real-time capture. One export runs at a time; the project's other jobs
// (playback, import, media repair) gate it through Host::idle.
class ExportController final : public QObject {
  Q_OBJECT
  // The target split for the dialog: folder, name without extension, and
  // the extension the chosen format writes.
  Q_PROPERTY(QString exportFolder READ export_folder NOTIFY changed)
  Q_PROPERTY(QString exportFileName READ export_file_name NOTIFY changed)
  Q_PROPERTY(QString exportExtension READ export_extension NOTIFY changed)
  // True when exporting would replace an existing file the user named.
  Q_PROPERTY(bool exportReplacesFile READ export_replaces_file NOTIFY changed)
  // The file (or stem folder) the last export published; empty after a failure.
  Q_PROPERTY(QString exportSavedPath READ export_saved_path NOTIFY changed)
  Q_PROPERTY(bool exportTrimTail READ export_trim_tail WRITE set_export_trim_tail NOTIFY changed)
  Q_PROPERTY(
      int exportFormatIndex READ export_format_index WRITE set_export_format_index NOTIFY changed)
  Q_PROPERTY(int exportRateIndex READ export_rate_index WRITE set_export_rate_index NOTIFY changed)
  Q_PROPERTY(QVariantList exportRateAvailability READ export_rate_availability NOTIFY changed)
  Q_PROPERTY(bool masterExportRateValid READ master_export_rate_valid NOTIFY changed)
  Q_PROPERTY(QString exportRateStatus READ export_rate_status NOTIFY changed)
  Q_PROPERTY(QString exportStatus READ export_status NOTIFY changed)
  Q_PROPERTY(bool exportBusy READ export_busy NOTIFY changed)
  Q_PROPERTY(bool exportEnabled READ export_enabled NOTIFY changed)
  // With two instrument sources (one VST3), stems export one chosen
  // source at a time rather than every track.
  Q_PROPERTY(bool exportsSelectedSourceStems READ exports_selected_source_stems NOTIFY changed)
  // The format and sample-rate menus, by index (exportFormatIndex,
  // exportRateIndex).
  Q_PROPERTY(QStringList exportFormatLabels READ export_format_labels CONSTANT)
  Q_PROPERTY(QStringList exportRateLabels READ export_rate_labels CONSTANT)
  Q_PROPERTY(bool twoInstrumentSourceStemAvailable READ two_instrument_source_stem_available NOTIFY
                 changed)
  Q_PROPERTY(
      QString twoInstrumentSourceStemReason READ two_instrument_source_stem_reason NOTIFY changed)
  Q_PROPERTY(int realtimeExportEndBar READ realtime_export_end_bar WRITE set_realtime_export_end_bar
                 NOTIFY changed)
  Q_PROPERTY(bool realtimeExportAvailable READ realtime_export_available CONSTANT)
  Q_PROPERTY(bool realtimeExportReady READ realtime_export_ready NOTIFY changed)
  Q_PROPERTY(bool realtimeExportBusy READ realtime_export_busy NOTIFY changed)
  Q_PROPERTY(bool encodedExportAvailable READ encoded_export_available CONSTANT)

public:
  struct Host {
    std::function<application::ProjectSession*()> session;
    // No playback, import, media repair or close is under way.
    std::function<bool()> idle;
    std::function<QString()> project_name;
    // The open project's folder: stems are written inside it.
    std::function<std::filesystem::path()> project_root;
    std::function<std::uint32_t()> buffer_frames;
    std::function<Vst3LiveQualification()> live_qualification;
    // An export started or finished: playback and edit gates changed.
    std::function<void()> busy_changed;
  };

  explicit ExportController(Host host, QObject* parent = nullptr);
  ~ExportController() override;
  ExportController(const ExportController&) = delete;
  ExportController& operator=(const ExportController&) = delete;

  [[nodiscard]] QString export_target() const { return export_target_; }
  [[nodiscard]] QString export_folder() const;
  [[nodiscard]] QString export_file_name() const;
  [[nodiscard]] QString export_extension() const;
  [[nodiscard]] bool export_replaces_file() const;
  [[nodiscard]] QString export_saved_path() const { return export_saved_path_; }
  [[nodiscard]] bool export_trim_tail() const noexcept { return export_trim_tail_; }
  void set_export_trim_tail(bool trim);
  [[nodiscard]] int export_format_index() const { return export_format_index_; }
  void set_export_format_index(int index);
  [[nodiscard]] int export_rate_index() const { return export_rate_index_; }
  void set_export_rate_index(int index);
  [[nodiscard]] static bool output_rate_conversion_available();
  [[nodiscard]] QVariantList export_rate_availability() const;
  Q_INVOKABLE bool export_rate_available(int index) const;
  [[nodiscard]] bool master_export_rate_valid() const;
  [[nodiscard]] QString export_rate_status() const;
  [[nodiscard]] QString export_status() const { return export_status_; }
  [[nodiscard]] bool export_busy() const noexcept { return completion_ != nullptr; }
  [[nodiscard]] bool export_enabled() const;
  [[nodiscard]] bool exports_selected_source_stems() const;
  [[nodiscard]] static QStringList export_format_labels();
  [[nodiscard]] static QStringList export_rate_labels();
  [[nodiscard]] bool two_instrument_source_stem_available() const;
  [[nodiscard]] QString two_instrument_source_stem_reason() const;
  [[nodiscard]] int realtime_export_end_bar() const noexcept { return realtime_export_end_bar_; }
  void set_realtime_export_end_bar(int bar);
  [[nodiscard]] static bool realtime_export_available() noexcept;
  [[nodiscard]] bool realtime_export_ready() const;
  [[nodiscard]] bool realtime_export_busy() const noexcept {
    return completion_ && completion_->realtime;
  }
  [[nodiscard]] static bool encoded_export_available() noexcept;
  // Waiting for an export to publish before the window closes.
  [[nodiscard]] bool closing() const noexcept { return close_after_export_; }

  Q_INVOKABLE void set_export_target(const QString& target);
  Q_INVOKABLE void choose_export_target();
  Q_INVOKABLE void set_export_file_name(const QString& name);
  Q_INVOKABLE void set_export_folder(const QString& folder);
  // Re-proposes the default name when it was taken since it was proposed
  // (the dialog calls this as it opens).
  Q_INVOKABLE void refresh_export_target();
  Q_INVOKABLE void show_export_in_folder();
  Q_INVOKABLE void export_master();
  Q_INVOKABLE void export_stems();
  Q_INVOKABLE void export_instrument_source_stem(const QString& track_id);
  Q_INVOKABLE void export_realtime_jack();
  Q_INVOKABLE void cancel_realtime_export();

  // Another project opened or closed: the dialog starts over.
  void reset();
  // The realtime end bar follows the timeline until the user picks one.
  void follow_timeline_bars(int bars);
  // Clears a finished export's message (playback or an import started).
  void clear_status();
  // The window wants to close: true when it must wait for readyToClose.
  [[nodiscard]] bool close_when_done();
  // Re-announces the gates after project, playback or job changes.
  void notify() { emit changed(); }

signals:
  void changed();
  void readyToClose();

private:
  struct Completion {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel_requested{false};
    bool realtime = false;
    std::uint64_t revision = 0;
    QString format_name;
    QString target;
    std::optional<foundation::Result<exporting::PublishOutcome>> result;
#if defined(OSCILLATE_HAVE_JACK_EXPORT) && OSCILLATE_HAVE_JACK_EXPORT
    std::unique_ptr<RealtimeExportRun> realtime_run;
#endif
  };
  using Job = std::function<foundation::Result<exporting::PublishOutcome>()>;

  [[nodiscard]] application::ProjectSession* session() const { return host_.session(); }
  // The proposed target: <Music>/oscillate/<project name>.<ext>, numbered
  // past any existing file so a default export never replaces one.
  [[nodiscard]] QString default_export_target() const;
  void reset_export_target();
  void fail(QString message);
  // Runs `job` on the export worker, reporting `failure_prefix` when the
  // thread cannot start and `running` while it works.
  void start(std::shared_ptr<Completion> completion, Job job, const QString& failure_prefix,
             const QString& running);
  void start_export_task(application::MasterExportTask task, const QString& format_name,
                         const std::filesystem::path& target);
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  void start_vst3_effect_export_task(application::StoppedSnapshotExportTask task,
                                     exporting::MasterOutputOptions options);
  void start_vst3_instrument_export_task(application::StoppedSnapshotExportTask task,
                                         std::filesystem::path bundle,
                                         exporting::MasterOutputOptions options,
                                         bool with_managed_audio);
  void start_vst3_instrument_stem_export_task(application::StoppedSnapshotExportTask task,
                                              project::TrackId source_track,
                                              exporting::MasterRenderOptions options,
                                              bool with_managed_audio,
                                              std::filesystem::path bundle);
#endif
  void poll();

  Host host_;
  QTimer timer_;
  std::shared_ptr<Completion> completion_;
  std::jthread worker_;
  QString export_target_;
  QString export_status_;
  QString export_saved_path_;
  // True while the target is the controller's proposal rather than the
  // user's choice; only a proposed name is renumbered to avoid replacing a
  // file.
  bool export_target_is_default_ = true;
  bool export_trim_tail_ = true;
  int export_format_index_ = 0;
  int export_rate_index_ = 0;
  int realtime_export_end_bar_ = 16;
  bool realtime_export_end_bar_custom_ = false;
  bool close_after_export_ = false;
};

} // namespace oscillate::quick
