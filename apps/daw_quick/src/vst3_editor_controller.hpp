#pragma once

#include "background_job.hpp"
#include "routed_playback.hpp"

#include "oscillate/application/project_session.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/project_binding.hpp"
#include "oscillate/plugins/vst3_bundle_loader.hpp"
#include "oscillate/plugins/vst3_catalogue.hpp"
#include "oscillate/plugins/vst3_effect_host.hpp"
#endif

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oscillate::plugin_ui {
class Vst3EditorAttachment;
}

namespace oscillate::quick {

class PluginAutomationController;

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
// Reads and hashes a bundle's binary on a worker (plugins::hash_vst3_binary).
using Vst3BinaryHasher =
    std::function<std::optional<plugins::Vst3HashedBinary>(const std::filesystem::path&)>;
#endif

// The VST3 parameter editor (QML: projectBridge.vst3Editor): one saved
// effect or instrument source at a time, as its native X11 view or a list of
// its parameters. Completed gestures save as undo steps and, while the
// routed graph plays, reach live audio when the plugin qualifies. Opening
// hashes the plugin binary on a worker while the editor shows as pending;
// the plugin then loads on the GUI thread.
class Vst3EditorController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap vst3InsertEditor READ editor NOTIFY editorChanged)
  Q_PROPERTY(QWindow* vst3NativeWindow READ native_window NOTIFY nativeWindowChanged)

public:
  struct Host {
    std::function<application::ProjectSession*()> session;
    // An audio edit or media verification holds the project.
    std::function<bool()> control_busy;
    // An export or import is running.
    std::function<bool()> transfers_busy;
    std::function<bool()> playback_busy;
    std::function<bool()> browser_busy;
    // The routed run (null without one), and whether no stop or transport
    // change is pending on it.
    std::function<RoutedPlayback*()> routed;
    std::function<bool()> routed_settled;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    std::function<const plugins::Vst3CatalogueSnapshot&()> catalogue;
    // Empty: plugins::hash_vst3_binary.
    Vst3BinaryHasher hash_binary;
#endif
    // A committed edit: re-project and update playback.
    std::function<void()> commit;
    std::function<void()> schedule_save;
    std::function<void(QString)> status;

    [[nodiscard]] bool jobs_busy() const { return transfers_busy() || control_busy(); }
  };

  Vst3EditorController(Host host, PluginAutomationController& plugin_automation,
                       QObject* parent = nullptr);
  ~Vst3EditorController() override;
  Vst3EditorController(const Vst3EditorController&) = delete;
  Vst3EditorController& operator=(const Vst3EditorController&) = delete;

  [[nodiscard]] QVariantMap editor() const { return editor_; }
  [[nodiscard]] QWindow* native_window() const noexcept { return native_window_; }

  Q_INVOKABLE void open_vst3_insert_editor(const QString& track_id, const QString& slot_id);
  // Saved references only: the plugins layer has no instrument-source binding yet.
  Q_INVOKABLE void open_vst3_instrument_editor(const QString& track_id);
  Q_INVOKABLE void apply_vst3_insert_editor();
  Q_INVOKABLE void set_vst3_insert_parameter(quint32 parameter_id, double normalized_value);
  Q_INVOKABLE void discard_vst3_insert_editor();

  // A native editor saves its state before the project changes or the
  // window closes. Returns why the state did not reach the project; when the
  // plug-in still holds that state, the editor keeps it with its view closed.
  [[nodiscard]] std::optional<QString> commit_native_editor();
  // Shows a kept editor's view again: false (and the editor discarded) when
  // none is kept or its view cannot reopen.
  [[nodiscard]] bool resume_native_editor();
  // Follows the latest live gesture's receipt in the open editor.
  void refresh_live_status();

signals:
  void editorChanged();
  void nativeWindowChanged();

private:
  [[nodiscard]] application::ProjectSession* session() const { return host_.session(); }
  void set_parameter(quint32 parameter_id, double normalized_value, bool from_native_editor,
                     std::uint64_t gesture_sequence = 0);
  void poll_native_gestures();
  void publish_instrument_editor(const project::Vst3PluginInstance& source,
                                 const project::TrackId& track, const QString& message,
                                 QVariantList parameters, bool editable, bool native);
  void show_native_window();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // An editor whose binary is being hashed on a worker.
  struct PendingVerification {
    project::Vst3PluginInstance saved;
    project::TrackId track;
    // None: the track's instrument source.
    std::optional<project::InsertSlotId> slot;
    QString track_id;
    QString slot_id;
    plugins::ScanResult scan;
    // The project the editor was opened in (ProjectSession::session_generation).
    std::uint64_t session = 0;
  };
  // Closes the native view and saves its state as one undo step. Returns
  // why the state did not reach the project; with `keep`, a plug-in that
  // still holds that state stays loaded for resume_native_editor.
  [[nodiscard]] std::optional<QString> close_native_editor(bool keep);
  [[nodiscard]] std::optional<QString>
  keep_uncaptured_editor(bool keep, QString failure,
                         std::vector<project::Vst3ParameterReference> references);
  [[nodiscard]] bool reattach_native_view();
  void begin_verification(PendingVerification pending);
  void poll_verification();
  void finish_insert_open(const PendingVerification& pending,
                          plugins::Vst3ProjectBindingResult binding);
  void finish_instrument_open(const PendingVerification& pending,
                              plugins::Vst3ProjectBindingResult binding);
  void publish_insert_editor(const project::Vst3PluginInstance& saved, const QString& track_id,
                             const QString& slot_id, const QString& message, bool native);
#endif

  Host host_;
  PluginAutomationController& plugin_automation_;
  QTimer timer_;
  QVariantMap editor_;
  QPointer<QWindow> native_window_;
  // Latest run gesture status shown in the editor, as (saved revision, phase).
  std::pair<std::uint64_t, int> live_status_shown_{0, -1};
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  struct PendingNativeGesture {
    plugins::Vst3ParameterEvent event;
    std::uint64_t sequence;
  };
  std::unique_ptr<plugins::LoadedVst3Effect> plugin_;
  std::shared_ptr<plugin_ui::Vst3EditorAttachment> attachment_;
  std::optional<project::Vst3PluginInstance> original_;
  std::optional<project::TrackId> track_;
  std::optional<project::InsertSlotId> slot_;
  // Parameter intent of a kept editor whose state was not captured; the
  // next capture starts from it instead of the saved parameters.
  std::optional<std::vector<project::Vst3ParameterReference>> uncaptured_parameters_;
  std::uint64_t revision_ = 0;
  std::vector<PendingNativeGesture> pending_gestures_;
  std::unordered_map<std::uint32_t, std::uint64_t> active_sequences_;
  std::uint64_t next_sequence_{0};
  std::uint32_t restart_flags_ = 0;
  std::optional<PendingVerification> verifying_;
  QTimer verify_timer_;
  // The hash runs here; its worker holds no controller state.
  BackgroundJob<std::optional<plugins::Vst3HashedBinary>> hash_;
#endif
};

} // namespace oscillate::quick
