#pragma once

#include "oscillate/application/project_session.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/application/vst3/insert_service.hpp"
#include "oscillate/plugins/vst3_catalogue.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace oscillate::quick {

// The VST3 browser (QML: projectBridge.vst3Browser): scans bundles in the
// helper process, lists their classes, and verifies a chosen effect or
// instrument (or an exact relink) on a worker before the project takes it.
class Vst3BrowserController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool vst3BrowserAvailable READ vst3_browser_available CONSTANT)
  Q_PROPERTY(bool vst3BrowserBusy READ busy NOTIFY changed)
  Q_PROPERTY(QString vst3BrowserStatus READ status NOTIFY changed)
  Q_PROPERTY(QVariantList vst3BrowserEntries READ entries NOTIFY changed)

public:
  struct Host {
    std::function<application::ProjectSession*()> session;
    // Bumped whenever another project opens: verifications for an old one stop.
    std::function<std::uint64_t()> project_generation;
    std::function<std::uint64_t()> revision;
    // An audio edit, import or export is running.
    std::function<bool()> jobs_busy;
    std::function<bool()> playback_busy;
    // An insert changed the project: re-project it and update playback.
    std::function<void()> commit;
    // A relink changed the project: re-project it only.
    std::function<void()> refresh;
    std::function<void(QString)> playback_status;
    std::function<void(int)> track_created;
    std::function<void(QString)> status;
  };

  explicit Vst3BrowserController(Host host, QObject* parent = nullptr);
  ~Vst3BrowserController() override;
  Vst3BrowserController(const Vst3BrowserController&) = delete;
  Vst3BrowserController& operator=(const Vst3BrowserController&) = delete;

  [[nodiscard]] static bool vst3_browser_available();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] QString status() const { return status_; }
  [[nodiscard]] QVariantList entries() const { return entries_; }
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  [[nodiscard]] const plugins::Vst3CatalogueSnapshot& catalogue() const { return catalogue_; }
#endif

  Q_INVOKABLE void refresh_vst3_catalogue();
  Q_INVOKABLE void add_vst3_search_path(const QString& path);
  Q_INVOKABLE void retry_vst3_bundle(const QString& bundle);
  Q_INVOKABLE void insert_vst3_effect(int entry_index, const QString& class_id,
                                      const QString& track_id);
  Q_INVOKABLE void insert_vst3_instrument(int entry_index, const QString& class_id,
                                          const QString& track_id);
  Q_INVOKABLE void create_vst3_instrument_track(int entry_index, const QString& class_id);
  Q_INVOKABLE void relink_vst3_plugin(const QString& track_id, const QString& slot_id,
                                      int entry_index);

  // A project opened: scans the standard folders when it saved a plugin
  // from one, so its editors and relinks can find their receipts.
  void scan_for_project(const project::Snapshot& snapshot);
  // The window wants to close: true when a scan must stop first
  // (scanStoppedForClose follows).
  [[nodiscard]] bool close_when_done();

signals:
  void changed();
  void scanStoppedForClose();

private:
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  struct Completion {
    enum class Action : std::uint8_t {
      refresh,
      verify_effect,
      verify_instrument,
      verify_new_instrument,
      relink_effect,
      relink_instrument
    };
    std::atomic<bool> done{false};
    Action action = Action::refresh;
    std::uint64_t project_generation = 0;
    std::uint64_t revision = 0;
    std::optional<project::TrackId> track;
    std::optional<project::InsertSlotId> slot;
    std::optional<project::Vst3PluginInstance> original_plugin;
    std::optional<plugins::Vst3CatalogueSnapshot> catalogue;
    std::optional<foundation::Result<plugins::VerifiedVst3EffectSelection>> verified;
    std::optional<foundation::Result<plugins::VerifiedVst3InstrumentSelection>> verified_instrument;
    QString failure;
  };
  void start_refresh(std::optional<std::filesystem::path> retry);
#endif
  [[nodiscard]] application::ProjectSession* session() const { return host_.session(); }
  void publish();
  void poll();

  Host host_;
  QTimer timer_;
  QString status_;
  QVariantList entries_;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  std::unique_ptr<plugins::Vst3Scanner> scanner_;
  plugins::Vst3CatalogueSnapshot catalogue_;
  std::vector<std::filesystem::path> explicit_paths_;
  std::shared_ptr<Completion> completion_;
  std::jthread worker_;
  bool close_after_scan_ = false;
#endif
};

} // namespace oscillate::quick
