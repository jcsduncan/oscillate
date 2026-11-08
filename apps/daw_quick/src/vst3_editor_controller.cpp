#include "vst3_editor_controller.hpp"
#include "plugin_automation_controller.hpp"
#include "saved_processing.hpp"
#include "vst3_editor_commit.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/plugins/project_binding.hpp"
#include "vst3_editor_attachment.hpp"
#endif

#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {
// A native editor's gestures are collected about once a display frame.
constexpr int native_gesture_poll_ms = 16;
// The editor's offline host renders nothing; it binds at this block size.
constexpr std::uint32_t editor_block_frames = 128;
// How often a pending binary verification is checked for its result.
constexpr int verification_poll_ms = 20;
// Parameter values as the list shows them.
constexpr int parameter_decimals = 3;

// The editor and automation projections list every parameter. Their inputs
// are already bounded: saved references by project::max_vst3_parameter_references
// and host metadata by the VST3 host's load-time parameter limit. The panel
// searches and pages this list instead of truncating it.

[[nodiscard]] QVariantList saved_parameter_view(const project::Vst3PluginInstance& saved) {
  QVariantList result;
  result.reserve(static_cast<qsizetype>(saved.parameters.size()));
  for (const auto& item : saved.parameters) {
    result.push_back(
        QVariantMap{{"id", item.id},
                    {"name", QStringLiteral("Parameter %1").arg(item.id)},
                    {"editable", false},
                    {"value", QString::number(item.normalized_value, 'f', parameter_decimals)}});
  }
  return result;
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
[[nodiscard]] QString editor_failure(plugin_ui::EditorOpenStatus status) {
  switch (status) {
  case plugin_ui::EditorOpenStatus::native_wayland_unavailable:
    return QStringLiteral("Native VST3 UI is unavailable on Wayland; relaunch the DAW with "
                          "QT_QPA_PLATFORM=xcb to try its X11 window");
  case plugin_ui::EditorOpenStatus::no_native_window:
    return QStringLiteral("Native VST3 UI requires an X11 window; try QT_QPA_PLATFORM=xcb");
  case plugin_ui::EditorOpenStatus::no_view:
  case plugin_ui::EditorOpenStatus::no_controller:
    return QStringLiteral("Plugin has no native editor");
  case plugin_ui::EditorOpenStatus::unsupported_platform:
    return QStringLiteral("Plugin does not support X11 editor embedding");
  case plugin_ui::EditorOpenStatus::frame_rejected:
  case plugin_ui::EditorOpenStatus::attach_failed:
  case plugin_ui::EditorOpenStatus::plugin_exception:
  case plugin_ui::EditorOpenStatus::host_unavailable:
    return QStringLiteral("Plugin editor could not attach safely");
  case plugin_ui::EditorOpenStatus::attached:
    return QStringLiteral("Native editor open; completed parameter gestures save automatically");
  }
  return QStringLiteral("Native editor unavailable");
}

[[nodiscard]] QVariantList parameter_view(const project::Vst3PluginInstance& saved,
                                          const plugins::Vst3EffectHost* host) {
  QVariantList result;
  if (host) {
    result.reserve(static_cast<qsizetype>(host->parameters().size()));
    for (const auto& item : host->parameters()) {
      const auto found =
          std::ranges::find(saved.parameters, item.id, &project::Vst3ParameterReference::id);
      result.push_back(QVariantMap{
          {"id", item.id},
          {"name", QString::fromStdString(item.title)},
          {"editable", host->parameter_precision(item.id) ==
                           plugins::Vst3ParameterPrecision::sample_offsets_forwarded},
          {"value", found == saved.parameters.end()
                        ? QStringLiteral("—")
                        : QString::number(found->normalized_value, 'f', parameter_decimals)}});
    }
  } else {
    result = saved_parameter_view(saved);
  }
  return result;
}

// Saved-versus-live text for the latest gesture. Only an exact applied
// receipt is described as live.
[[nodiscard]] QString live_status_text(const application::Vst3ManualEditStatus& status,
                                       bool save_pending = false) {
  switch (status.phase) {
  case application::Vst3ManualEditPhase::pending:
    return QStringLiteral("Revision %1 %2; waiting for the audio callback to apply it")
        .arg(status.saved_revision)
        .arg(save_pending ? QStringLiteral("queued to save") : QStringLiteral("saved"));
  case application::Vst3ManualEditPhase::applied:
    return QStringLiteral("Revision %1 live from sample %2%3")
        .arg(status.live_revision)
        .arg(status.applied_sample)
        .arg(save_pending ? QStringLiteral("; disk save pending") : QString{});
  case application::Vst3ManualEditPhase::failed:
    return QStringLiteral("Revision %1 %2 but not live: the plugin did not apply it. Stop and "
                          "Play to hear it")
        .arg(status.saved_revision)
        .arg(save_pending ? QStringLiteral("queued to save") : QStringLiteral("saved"));
  case application::Vst3ManualEditPhase::saved_not_live:
    return QStringLiteral("Revision %1 %2 but not live: %3. Stop and Play to hear it")
        .arg(status.saved_revision)
        .arg(save_pending ? QStringLiteral("queued to save") : QStringLiteral("saved"))
        .arg(status.live_refusal ? QString::fromStdString(status.live_refusal->message)
                                 : QString::fromUtf8(vst3_manual_refusal(status.admission)));
  case application::Vst3ManualEditPhase::model_not_saved:
    return QStringLiteral("Revision %1 changed in memory but was not saved; it is not live")
        .arg(status.model_revision);
  case application::Vst3ManualEditPhase::idle:
    break;
  }
  return {};
}

[[nodiscard]] std::vector<VerifiedPluginParameter>
verified_parameters(const plugins::Vst3EffectHost& host) {
  std::vector<VerifiedPluginParameter> result;
  result.reserve(host.parameters().size());
  for (const auto& item : host.parameters()) {
    result.push_back(
        {item.id, QString::fromStdString(item.title),
         item.step_count == 0 && host.parameter_precision(item.id) ==
                                     plugins::Vst3ParameterPrecision::sample_offsets_forwarded});
  }
  return result;
}
#endif

} // namespace

Vst3EditorController::Vst3EditorController(Host host, PluginAutomationController& plugin_automation,
                                           QObject* parent)
    : QObject(parent), host_(std::move(host)), plugin_automation_(plugin_automation), timer_(this) {
  timer_.setInterval(native_gesture_poll_ms);
  connect(&timer_, &QTimer::timeout, this, &Vst3EditorController::poll_native_gestures);
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  verify_timer_.setInterval(verification_poll_ms);
  connect(&verify_timer_, &QTimer::timeout, this, &Vst3EditorController::poll_verification);
#endif
}

Vst3EditorController::~Vst3EditorController() = default;

bool Vst3EditorController::commit_native_editor() {
  if (!editor_.value(QStringLiteral("native")).toBool())
    return true;
  apply_vst3_insert_editor();
  return !editor_.value(QStringLiteral("native")).toBool();
}

// QML invokable arguments are positional and retain stable track/slot roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Vst3EditorController::open_vst3_insert_editor(const QString& track_id,
                                                   const QString& slot_id) {
  if (editor_.value(QStringLiteral("native")).toBool()) {
    apply_vst3_insert_editor();
    if (editor_.value(QStringLiteral("native")).toBool())
      return;
  } else {
    discard_vst3_insert_editor();
  }
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || host_.jobs_busy()) {
    host_.status(QStringLiteral("Finish active tasks before opening a VST3 editor"));
    return;
  }
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto slot = project::InsertSlotId::parse(slot_id.toStdString());
  if (!track || !slot) {
    host_.status(QStringLiteral("Select a saved VST3 effect slot"));
    return;
  }
  const auto& snapshot = session()->snapshot();
  const auto owner = std::ranges::find(snapshot.tracks, track.value(), &project::Track::id);
  if (owner == snapshot.tracks.end()) {
    host_.status(QStringLiteral("Selected track no longer exists"));
    return;
  }
  const auto found = std::ranges::find(owner->effects, slot.value(), &project::EffectInsert::id);
  if (found == owner->effects.end() || !detail::vst3_effect(*found)) {
    host_.status(QStringLiteral("Selected VST3 insert no longer exists"));
    return;
  }
  const auto saved = *detail::vst3_effect(*found);
  original_ = saved;
  track_ = track.value();
  slot_ = slot.value();
  revision_ = snapshot.revision;
  QString message = QStringLiteral("Native editor unavailable; saved parameters are read-only");
  const auto receipt = std::ranges::find_if(host_.catalogue().entries, [&saved](const auto& entry) {
    return entry.scan.ok() && entry.scan.binary.sha256 == saved.binary_sha256 &&
           entry.canonical_bundle == std::filesystem::path(saved.bundle_path);
  });
  if (!saved.missing && receipt != host_.catalogue().entries.end()) {
    const auto klass =
        std::ranges::find_if(receipt->scan.metadata.classes, [&saved](const auto& candidate) {
          return candidate.id == saved.class_id && candidate.name == saved.name &&
                 candidate.vendor == saved.vendor && candidate.version == saved.version;
        });
    if (klass != receipt->scan.metadata.classes.end()) {
      begin_verification({.saved = saved,
                          .track = track.value(),
                          .slot = slot.value(),
                          .track_id = track_id,
                          .slot_id = slot_id,
                          .scan = receipt->scan,
                          .session = session()->session_generation()});
      return;
    }
  } else if (saved.missing) {
    message = QStringLiteral("Plugin missing; saved parameters are read-only");
  } else {
    message = host_.browser_busy()
                  ? QStringLiteral("Scanning saved plugin; parameters are read-only until scan "
                                   "completes. Reopen this editor then")
                  : QStringLiteral("Scan this plugin in the browser to open its native editor");
  }
  publish_insert_editor(saved, track_id, slot_id, message, false);
#else
  (void)track_id;
  (void)slot_id;
  host_.status(QStringLiteral("VST3 editor unavailable in this build"));
#endif
}

void Vst3EditorController::open_vst3_instrument_editor(const QString& track_id) {
  if (editor_.value(QStringLiteral("native")).toBool()) {
    apply_vst3_insert_editor();
    if (editor_.value(QStringLiteral("native")).toBool())
      return;
  } else {
    discard_vst3_insert_editor();
  }
  if (!session() || host_.control_busy()) {
    host_.status(QStringLiteral("Finish the current project operation before opening the source"));
    return;
  }
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto& snapshot = session()->snapshot();
  const auto owner = track ? std::ranges::find(snapshot.tracks, track.value(), &project::Track::id)
                           : snapshot.tracks.end();
  const auto* source =
      owner == snapshot.tracks.end() ? nullptr : detail::vst3_instrument_source(*owner);
  if (source == nullptr || owner->kind != project::TrackKind::instrument) {
    host_.status(QStringLiteral("Select a track with a saved VST3 instrument source"));
    return;
  }
  QString message = QStringLiteral("Scan this instrument to inspect its parameters");
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (host_.transfers_busy()) {
    message = QStringLiteral("Finish active tasks to edit instrument values");
  } else {
    const auto receipt =
        std::ranges::find_if(host_.catalogue().entries, [source](const auto& entry) {
          return entry.scan.ok() && entry.scan.binary.sha256 == source->binary_sha256 &&
                 entry.canonical_bundle == std::filesystem::path(source->bundle_path);
        });
    if (receipt != host_.catalogue().entries.end()) {
      begin_verification({.saved = *source,
                          .track = track.value(),
                          .slot = std::nullopt,
                          .track_id = track_id,
                          .slot_id = {},
                          .scan = receipt->scan,
                          .session = session()->session_generation()});
      return;
    }
    if (source->missing) {
      message = QStringLiteral("Instrument missing; saved parameters are read-only");
    } else if (host_.browser_busy()) {
      message = QStringLiteral("Scanning saved instrument; parameters are read-only until scan "
                               "completes. Reopen this editor then");
    }
  }
#endif
  publish_instrument_editor(*source, track.value(), message, saved_parameter_view(*source), false,
                            false);
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void Vst3EditorController::publish_insert_editor(const project::Vst3PluginInstance& saved,
                                                 const QString& track_id, const QString& slot_id,
                                                 const QString& message, bool native) {
  const auto* host = plugin_ ? &plugin_->host() : nullptr;
  const int native_width = native ? attachment_->window_shell()->width() : 0;
  const int native_height = native ? attachment_->window_shell()->height() : 0;
  editor_ = QVariantMap{{"open", true},
                        {"native", native},
                        {"editable", !native && host != nullptr},
                        {"name", QString::fromStdString(saved.name)},
                        {"role", QStringLiteral("effect")},
                        {"trackId", track_id},
                        {"slotId", slot_id},
                        {"nativeWidth", native_width},
                        {"nativeHeight", native_height},
                        {"message", message},
                        {"parameters", parameter_view(saved, host)}};
  if (host)
    plugin_automation_.publish_verified_parameters(saved.id, verified_parameters(*host));
  if (track_)
    plugin_automation_.select_plugin(*track_, saved.id);
  emit editorChanged();
  show_native_window();
}
#endif

void Vst3EditorController::publish_instrument_editor(const project::Vst3PluginInstance& source,
                                                     const project::TrackId& track,
                                                     const QString& message,
                                                     QVariantList parameters, bool editable,
                                                     bool native) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  const int native_width = native ? attachment_->window_shell()->width() : 0;
  const int native_height = native ? attachment_->window_shell()->height() : 0;
#else
  const int native_width = 0;
  const int native_height = 0;
#endif
  editor_ = QVariantMap{{"open", true},
                        {"native", native},
                        {"editable", editable},
                        {"name", QString::fromStdString(source.name)},
                        {"role", QStringLiteral("source")},
                        {"nativeWidth", native_width},
                        {"nativeHeight", native_height},
                        {"message", message},
                        {"parameters", std::move(parameters)}};
  plugin_automation_.select_plugin(track, source.id);
  emit editorChanged();
  show_native_window();
}

void Vst3EditorController::show_native_window() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!native_window_)
    return;
  emit nativeWindowChanged();
  auto* shell = attachment_->window_shell();
  auto attachment = attachment_;
  QTimer::singleShot(0, shell, [attachment] { attachment->show(); });
  timer_.start();
#endif
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void Vst3EditorController::begin_verification(PendingVerification pending) {
  auto hash = host_.hash_binary ? host_.hash_binary : Vst3BinaryHasher{plugins::hash_vst3_binary};
  auto bundle = pending.scan.binary.canonical_bundle;
  const auto failed = hash_.start(
      pending.session,
      [hash = std::move(hash), bundle = std::move(bundle)] { return hash(bundle); },
      [](const std::string&) { return std::optional<plugins::Vst3HashedBinary>{}; });
  if (failed) {
    host_.status(QStringLiteral("VST3 editor: %1").arg(QString::fromStdString(*failed)));
    return;
  }
  const bool instrument = !pending.slot;
  editor_ = QVariantMap{{"open", true},
                        {"pending", true},
                        {"native", false},
                        {"editable", false},
                        {"name", QString::fromStdString(pending.saved.name)},
                        {"role", instrument ? QStringLiteral("source") : QStringLiteral("effect")},
                        {"nativeWidth", 0},
                        {"nativeHeight", 0},
                        {"message", QStringLiteral("Verifying the plugin binary; saved parameters "
                                                   "are read-only until it loads")},
                        {"parameters", saved_parameter_view(pending.saved)}};
  if (!instrument) {
    editor_[QStringLiteral("trackId")] = pending.track_id;
    editor_[QStringLiteral("slotId")] = pending.slot_id;
  }
  plugin_automation_.select_plugin(pending.track, pending.saved.id);
  verifying_ = std::move(pending);
  verify_timer_.start();
  emit editorChanged();
}

void Vst3EditorController::poll_verification() {
  if (!hash_.finished())
    return;
  verify_timer_.stop();
  auto finished = hash_.take();
  if (!verifying_)
    return;
  auto pending = std::move(*verifying_);
  verifying_.reset();
  auto* current = session();
  // A result for a replaced project never loads into the new one.
  if (current == nullptr || current->session_generation() != pending.session) {
    discard_vst3_insert_editor();
    return;
  }
  const bool instrument = !pending.slot;
  const auto& snapshot = current->snapshot();
  const bool unchanged =
      instrument
          ? can_commit_vst3_instrument_state(snapshot, pending.track, pending.saved)
          : can_commit_vst3_editor_state(snapshot, pending.track, *pending.slot, pending.saved);
  if (!unchanged) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("The VST3 plugin changed or was removed while it was verified; "
                                "open its editor again"));
    return;
  }
  if (instrument ? host_.control_busy() : host_.jobs_busy()) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("Finish active tasks before opening a VST3 editor"));
    return;
  }
  if (instrument && host_.transfers_busy()) {
    publish_instrument_editor(pending.saved, pending.track,
                              QStringLiteral("Finish active tasks to edit instrument values"),
                              saved_parameter_view(pending.saved), false, false);
    return;
  }
  plugins::Vst3ProjectBindingResult binding;
  if (!finished || !finished->result) {
    binding.status = plugins::Vst3BindingStatus::load_failed;
    binding.diagnostic = "VST3 bundle binary could not be read, or changed while it was verified";
  } else {
    // Load, plugin creation and state restore stay on the GUI thread, as
    // VST3 threading requires; only the hash ran on the worker.
    binding =
        instrument
            ? plugins::bind_hashed_vst3_project_instrument(pending.saved, pending.scan,
                                                           *finished->result, snapshot.sample_rate,
                                                           editor_block_frames)
            : plugins::bind_hashed_vst3_project_effect(pending.saved, pending.scan,
                                                       *finished->result, snapshot.sample_rate,
                                                       editor_block_frames);
  }
  if (instrument)
    finish_instrument_open(pending, std::move(binding));
  else
    finish_insert_open(pending, std::move(binding));
}

void Vst3EditorController::finish_insert_open(const PendingVerification& pending,
                                              plugins::Vst3ProjectBindingResult binding) {
  const auto& saved = pending.saved;
  revision_ = session()->snapshot().revision;
  QString message;
  bool native = false;
  if (!binding.ready() || !binding.unresolved_parameter_ids.empty()) {
    message = QStringLiteral("Plugin values unavailable; saved parameters are read-only: %1")
                  .arg(QString::fromStdString(binding.diagnostic));
  } else {
    plugin_ = std::move(binding.effect);
    plugin_->host().suspend();
    if (!plugin_->host().static_parameter_values_ready()) {
      plugin_.reset();
      message = QStringLiteral("Plugin could not suspend safely; parameters are read-only");
    } else {
      // Saved-state restoration may request a parameter refresh. Only
      // requests made after opening the editor belong to this gesture.
      (void)plugin_->host().take_restart_requests();
      attachment_ = std::make_shared<plugin_ui::Vst3EditorAttachment>();
      const auto status = attachment_->open(plugin_->host());
      native = status == plugin_ui::EditorOpenStatus::attached;
      message = editor_failure(status);
      if (!binding.diagnostic.empty())
        message += QStringLiteral("; %1").arg(QString::fromStdString(binding.diagnostic));
      if (native) {
        plugin_->host().begin_editor_parameter_capture();
        if (host_.playback_busy())
          message = QStringLiteral("Continuous gestures save automatically; qualified "
                                   "controls change live audio. Closing saves remaining "
                                   "editor state");
        attachment_->window_shell()->setWindowTitle(QString::fromStdString(saved.name));
        (void)attachment_->window_shell()->winId();
        native_window_ = attachment_->window_shell()->windowHandle();
      }
    }
  }
  publish_insert_editor(saved, pending.track_id, pending.slot_id, message, native);
}

void Vst3EditorController::finish_instrument_open(const PendingVerification& pending,
                                                  plugins::Vst3ProjectBindingResult binding) {
  const auto& source = pending.saved;
  QString message;
  QVariantList parameters = saved_parameter_view(source);
  bool editable = false;
  bool native = false;
  if (binding.ready() && binding.unresolved_parameter_ids.empty()) {
    plugin_ = std::move(binding.effect);
    plugin_->host().suspend();
    if (plugin_->host().static_parameter_values_ready()) {
      (void)plugin_->host().take_restart_requests();
      original_ = source;
      track_ = pending.track;
      revision_ = session()->snapshot().revision;
      attachment_ = std::make_shared<plugin_ui::Vst3EditorAttachment>();
      const auto status = attachment_->open(plugin_->host());
      native = status == plugin_ui::EditorOpenStatus::attached;
      message = native ? editor_failure(status)
                       : QStringLiteral("%1; verified continuous instrument parameters can be "
                                        "saved while playing")
                             .arg(editor_failure(status));
      if (!binding.diagnostic.empty())
        message += QStringLiteral("; %1").arg(QString::fromStdString(binding.diagnostic));
      if (native) {
        plugin_->host().begin_editor_parameter_capture();
        if (host_.playback_busy())
          message = QStringLiteral("Continuous gestures save automatically; qualified "
                                   "controls change live audio. Closing saves remaining "
                                   "editor state");
        attachment_->window_shell()->setWindowTitle(QString::fromStdString(source.name));
        (void)attachment_->window_shell()->winId();
        native_window_ = attachment_->window_shell()->windowHandle();
      }
      parameters = parameter_view(source, &plugin_->host());
      editable = !native;
      plugin_automation_.publish_verified_parameters(source.id,
                                                     verified_parameters(plugin_->host()));
    } else {
      plugin_.reset();
      message = QStringLiteral("Instrument could not suspend safely; values are read-only");
    }
  } else {
    message = QStringLiteral("Instrument values unavailable: %1")
                  .arg(QString::fromStdString(binding.diagnostic));
  }
  publish_instrument_editor(source, pending.track, message, std::move(parameters), editable,
                            native);
}
#endif

void Vst3EditorController::apply_vst3_insert_editor() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || !original_ || !plugin_ || !attachment_ || !attachment_->attached() || !track_) {
    host_.status(QStringLiteral("Native VST3 editor is unavailable; no changes applied"));
    return;
  }
  const auto original = *original_;
  const auto track = *track_;
  const auto slot = slot_;
  const bool instrument_source = !slot.has_value();
  const auto commit_revision = session()->snapshot().revision;
  const bool unchanged =
      instrument_source
          ? can_commit_vst3_instrument_state(session()->snapshot(), track, original)
          : can_commit_vst3_editor_state(session()->snapshot(), track, *slot, original);
  if (!unchanged) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("Project changed while VST3 editor was open; edits discarded"));
    return;
  }
  if (host_.jobs_busy()) {
    host_.status(QStringLiteral("Finish the active task before closing the VST3 editor"));
    return;
  }
  native_window_.clear();
  emit nativeWindowChanged();
  attachment_->close();
  auto edits = plugin_->host().take_editor_parameter_edits();
  if (!edits) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("VST3 editor exceeded the bounded parameter edit limit"));
    return;
  }
  auto references = original.parameters;
  if (!edits->empty()) {
    std::ranges::sort(*edits, {}, &plugins::Vst3ParameterEvent::id);
    std::string error;
    if (!plugin_->host().stage_static_parameter_values(*edits, error) ||
        !plugin_->host().activate(error) || !plugin_->host().flush_static_parameter_values(error)) {
      discard_vst3_insert_editor();
      host_.status(QStringLiteral("VST3 parameter edits were not applied: %1")
                       .arg(QString::fromStdString(error)));
      return;
    }
    plugin_->host().suspend();
    for (const auto& edit : *edits) {
      const auto found =
          std::ranges::find(references, edit.id, &project::Vst3ParameterReference::id);
      if (found == references.end())
        references.push_back({edit.id, edit.normalized_value});
      else
        found->normalized_value = edit.normalized_value;
    }
    std::ranges::sort(references, {}, &project::Vst3ParameterReference::id);
  }
  std::string error;
  auto state = plugin_->host().save_state_pair(error);
  if (!state) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("VST3 editor state could not be captured: %1")
                     .arg(QString::fromStdString(error)));
    return;
  }
  const bool controller_changed = state->controller_state != original.controller_state;
  if (state->component_state == original.component_state && !controller_changed &&
      references == original.parameters) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("VST3 editor closed; no state change captured"));
    return;
  }
  const auto prior_revision = session()->snapshot().revision;
  const auto committed =
      slot ? session()->effects().set_vst3_editor_state(
                 commit_revision, track, *slot, original, std::move(state->component_state),
                 std::move(state->controller_state), std::move(references),
                 application::EditorSavePolicy::background)
           : session()->instruments().set_vst3_instrument_state(
                 commit_revision, track, original, std::move(state->component_state),
                 std::move(state->controller_state), std::move(references),
                 application::EditorSavePolicy::background);
  discard_vst3_insert_editor();
  if (session()->snapshot().revision != prior_revision) {
    host_.schedule_save();
    host_.commit();
  }
  QString message = QStringLiteral("VST3 editor state accepted as one undo step; save queued");
  if (host_.playback_busy() && controller_changed)
    message =
        instrument_source
            ? QStringLiteral("VST3 instrument controller state queued to save, not yet audible. "
                             "Stop and Play to apply.")
            : QStringLiteral("VST3 effect controller state queued to save; awaiting an exact live "
                             "graph revision receipt.");
  if (!committed)
    message = QStringLiteral("VST3 editor state: %1")
                  .arg(QString::fromStdString(committed.error().message));
  host_.status(message);
#else
  host_.status(QStringLiteral("VST3 editor unavailable in this build"));
#endif
}

// QML invokable arguments are positional and keep stable ID/value roles.
void Vst3EditorController::set_vst3_insert_parameter(quint32 parameter_id,
                                                     double normalized_value) {
  set_parameter(parameter_id, normalized_value, false);
}

// The native view sends many performEdit values during a drag; a completed
// endEdit becomes one saved project transaction.
void Vst3EditorController::set_parameter(quint32 parameter_id, double normalized_value,
                                         bool from_native_editor, std::uint64_t gesture_sequence) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!session() || !original_ || !plugin_ ||
      (!from_native_editor && attachment_ && attachment_->attached()) || !track_ ||
      !std::isfinite(normalized_value) || normalized_value < 0.0 || normalized_value > 1.0 ||
      plugin_->host().parameter_precision(parameter_id) !=
          plugins::Vst3ParameterPrecision::sample_offsets_forwarded ||
      host_.jobs_busy() ||
      (host_.playback_busy() &&
       (!host_.routed() || !host_.routed_settled() || host_.routed()->paused()))) {
    host_.status(QStringLiteral("VST3 parameter cannot be edited in the current project state"));
    return;
  }
  const auto original = *original_;
  const bool instrument_source = !slot_.has_value();
  const auto commit_revision = session()->snapshot().revision;
  const bool unchanged =
      instrument_source
          ? can_commit_vst3_instrument_state(session()->snapshot(), *track_, original)
          : can_commit_vst3_editor_state(session()->snapshot(), *track_, *slot_, original);
  if (!unchanged) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("Project changed while VST3 parameters were open"));
    return;
  }
  auto references = original.parameters;
  const auto found =
      std::ranges::find(references, parameter_id, &project::Vst3ParameterReference::id);
  if (found != references.end() && found->normalized_value == normalized_value)
    return;
  const std::array<plugins::Vst3ParameterEvent, 1> value{{{parameter_id, 0, normalized_value}}};
  const bool live = host_.playback_busy();
  bool live_qualified = false;
  QString inactive_reason;
  if (live) {
    auto ready = host_.routed()->preflight_vst3_manual_edit(commit_revision, *track_, slot_,
                                                            original.id, value.front());
    live_qualified = ready.has_value();
    if (!ready) {
      const bool continuing_saved_only_edit = editor_.value(QStringLiteral("liveStatus"))
                                                  .toString()
                                                  .startsWith(QStringLiteral("Saved revision "));
      if (ready.error().code == foundation::ErrorCode::conflict && !continuing_saved_only_edit) {
        const auto message =
            QStringLiteral("VST3 parameter not saved or live: %1. Wait for the active edit.")
                .arg(QString::fromStdString(ready.error().message));
        editor_[QStringLiteral("liveStatus")] = message;
        emit editorChanged();
        host_.status(message);
        return;
      }
      inactive_reason = QString::fromStdString(ready.error().message);
    }
  }
  std::string error;
  auto& host = plugin_->host();
  if (!host.stage_static_parameter_values(value, error) || !host.activate(error) ||
      !host.flush_static_parameter_values(error)) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("VST3 parameter could not be applied: %1")
                     .arg(QString::fromStdString(error)));
    return;
  }
  host.suspend();
  restart_flags_ |= host.take_restart_requests();
  if (live && restart_flags_ != 0) {
    live_qualified = false;
    inactive_reason = QStringLiteral("plugin requested a component restart");
  }
  auto state = host.save_state_pair(error);
  if (!state) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("VST3 parameter state could not be saved: %1")
                     .arg(QString::fromStdString(error)));
    return;
  }
  restart_flags_ |= host.take_restart_requests();
  if (live && restart_flags_ != 0) {
    live_qualified = false;
    inactive_reason = QStringLiteral("plugin requested a component restart");
  }
  const bool controller_changed = state->controller_state != original.controller_state;
  if (live && controller_changed) {
    live_qualified = false;
    inactive_reason = instrument_source
                          ? QStringLiteral("the controller state changed; instrument replacement "
                                           "is not qualified")
                          : QStringLiteral("the controller state changed; it requires an exact "
                                           "qualified graph replacement receipt");
  }
  if (found == references.end())
    references.push_back({parameter_id, normalized_value});
  else
    found->normalized_value = normalized_value;
  std::ranges::sort(references, {}, &project::Vst3ParameterReference::id);
  const auto prior_revision = session()->snapshot().revision;
  std::optional<application::Vst3ManualEditStatus> live_status;
  foundation::Result<void> committed = foundation::Result<void>::success();
  if (live_qualified) {
    auto result = host_.routed()->commit_vst3_manual_edit(
        *session(),
        {commit_revision, *track_, slot_, original, std::move(state->component_state),
         std::move(references), value.front(), gesture_sequence},
        application::EditorSavePolicy::background);
    if (result)
      live_status = result.value();
    else
      committed = foundation::Result<void>::failure(result.error());
  } else {
    committed = instrument_source
                    ? session()->instruments().set_vst3_instrument_state(
                          commit_revision, *track_, original, std::move(state->component_state),
                          std::move(state->controller_state), std::move(references),
                          application::EditorSavePolicy::background)
                    : session()->effects().set_vst3_editor_state(
                          commit_revision, *track_, *slot_, original,
                          std::move(state->component_state), std::move(state->controller_state),
                          std::move(references), application::EditorSavePolicy::background);
  }
  if (!committed) {
    // The clone holds a value the project may not; close it rather than
    // presenting that value as saved or live.
    discard_vst3_insert_editor();
    if (session()->snapshot().revision != prior_revision) {
      host_.schedule_save();
      host_.commit();
    }
    host_.status(QStringLiteral("VST3 parameter edit failed%1: %2")
                     .arg(live ? QStringLiteral(" and is not live") : QString{})
                     .arg(QString::fromStdString(committed.error().message)));
    return;
  }
  const auto& snapshot = session()->snapshot();
  const auto owner = std::ranges::find(snapshot.tracks, *track_, &project::Track::id);
  if (owner == snapshot.tracks.end()) {
    discard_vst3_insert_editor();
    host_.status(QStringLiteral("Edited VST3 track disappeared after commit"));
    return;
  }
  if (instrument_source) {
    const auto* source = std::get_if<project::Vst3PluginInstance>(&owner->instrument_source);
    if (!source) {
      discard_vst3_insert_editor();
      host_.status(QStringLiteral("Edited VST3 source disappeared after commit"));
      return;
    }
    original_ = *source;
  } else {
    const auto slot = std::ranges::find(owner->effects, *slot_, &project::EffectInsert::id);
    if (slot == owner->effects.end() ||
        !std::holds_alternative<project::Vst3PluginInstance>(slot->processor)) {
      discard_vst3_insert_editor();
      host_.status(QStringLiteral("Edited VST3 slot disappeared after commit"));
      return;
    }
    original_ = std::get<project::Vst3PluginInstance>(slot->processor);
  }
  revision_ = snapshot.revision;
  host_.schedule_save();
  const auto live_text = live_status ? live_status_text(*live_status, true)
                         : live
                             ? QStringLiteral("Revision %1 queued to save; not yet audible: %2%3")
                                   .arg(snapshot.revision)
                                   .arg(inactive_reason)
                                   .arg(!instrument_source && controller_changed
                                            ? QStringLiteral(". Awaiting graph receipt.")
                                            : QStringLiteral(". Stop and Play to apply."))
                             : QString{};
  if (live_status)
    live_status_shown_ = {live_status->saved_revision, static_cast<int>(live_status->phase)};
  if (live_text.isEmpty())
    editor_.remove(QStringLiteral("liveStatus"));
  else
    editor_[QStringLiteral("liveStatus")] = live_text;
  editor_[QStringLiteral("parameters")] = parameter_view(*original_, &host);
  emit editorChanged();
  host_.commit();
  host_.status(live_text.isEmpty() ? QStringLiteral("VST3 parameter accepted; save queued")
                                   : live_text);
#else
  (void)parameter_id;
  (void)normalized_value;
  (void)from_native_editor;
  (void)gesture_sequence;
  host_.status(QStringLiteral("VST3 parameters unavailable in this build"));
#endif
}

void Vst3EditorController::poll_native_gestures() {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (!plugin_ || !attachment_ || !attachment_->attached())
    return;
  auto& host = plugin_->host();
  restart_flags_ |= host.take_restart_requests();
  auto completed = host.take_completed_editor_gestures();
  if (!completed) {
    pending_gestures_.clear();
    active_sequences_.clear();
    const auto warning =
        QStringLiteral("Native editor gesture capture exceeded its limit; close and reopen the "
                       "editor. These edits cannot be saved");
    if (editor_.value(QStringLiteral("liveStatus")).toString() != warning) {
      editor_[QStringLiteral("liveStatus")] = warning;
      emit editorChanged();
    }
    return;
  }
  // A completed endEdit precedes any still-active preview returned by this
  // poll. Keep one instance-wide serial per gesture even when callbacks for
  // several parameters interleave or a drag spans multiple UI polls.
  for (const auto& gesture : *completed) {
    const auto active = active_sequences_.find(gesture.id);
    if (active == active_sequences_.end() &&
        next_sequence_ == std::numeric_limits<std::uint64_t>::max())
      return; // Fail closed before serial reuse.
    const auto sequence = active == active_sequences_.end() ? ++next_sequence_ : active->second;
    if (active != active_sequences_.end())
      active_sequences_.erase(active);
    const auto found =
        std::ranges::find_if(pending_gestures_, [&](const PendingNativeGesture& pending) {
          return pending.event.id == gesture.id;
        });
    if (found == pending_gestures_.end())
      pending_gestures_.push_back({gesture, sequence});
    else
      *found = {gesture, sequence};
  }
  const auto previews = host.take_editor_parameter_previews();
  if (previews && host_.playback_busy() && restart_flags_ == 0 && session() && original_ &&
      track_ && host_.routed()) {
    std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
    plugins::Vst3ActiveGraphIdentity identity;
    if (slot_) {
      if (auto binding =
              host_.routed()->active_vst3_effect_manual_events(*track_, *slot_, original_->id)) {
        slot = std::move(binding->slot);
        identity = binding->identity;
      }
    } else if (auto binding =
                   host_.routed()->active_vst3_instrument_manual_events(*track_, original_->id)) {
      slot = std::move(binding->slot);
      identity = binding->identity;
    }
    if (!slot) {
      if (auto pending =
              host_.routed()->pending_vst3_manual_preview_events(*track_, slot_, original_->id)) {
        slot = std::move(pending->slot);
        identity = pending->identity;
      }
    }
    if (slot) {
      for (const auto& event : *previews) {
        auto [active, inserted] = active_sequences_.try_emplace(event.id, 0);
        if (inserted) {
          if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            active_sequences_.erase(active);
            continue;
          }
          active->second = ++next_sequence_;
        }
        (void)slot->preview(identity, event, active->second);
      }
    }
  }
  if (host_.playback_busy() && restart_flags_ != 0 && !pending_gestures_.empty()) {
    pending_gestures_.clear();
    editor_[QStringLiteral("liveStatus")] =
        QStringLiteral("Plugin requested a component restart. These changes are in the editor "
                       "only; closing saves them while playback continues");
    emit editorChanged();
    return;
  }
  if (pending_gestures_.empty() || !session() || !original_ || !track_ ||
      (host_.playback_busy() && !host_.routed()))
    return;
  const auto gesture = pending_gestures_.front();
  if (host_.playback_busy()) {
    const auto ready = host_.routed()->preflight_vst3_manual_edit(
        session()->snapshot().revision, *track_, slot_, original_->id, gesture.event);
    if (!ready) {
      const auto reason = QString::fromStdString(ready.error().message);
      if (reason == QString::fromUtf8(vst3_manual_refusal(plugins::Vst3ManualEventStatus::busy)))
        return; // Keep the newest gesture until the previous revision is heard.
    }
  }
  pending_gestures_.erase(pending_gestures_.begin());
  // The parameter transaction also handles a failed live preflight: it saves
  // the completed native gesture and reports why the active graph lacks it.
  // A pending live event or an unrelated graph conflict still fails closed.
  set_parameter(gesture.event.id, gesture.event.normalized_value, true, gesture.sequence);
#endif
}

void Vst3EditorController::refresh_live_status() {
  poll_native_gestures();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (restart_flags_ != 0 && host_.playback_busy())
    return;
#endif
  if (editor_.isEmpty() || !editor_.contains(QStringLiteral("liveStatus")))
    return;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  const auto saved_only = editor_.value(QStringLiteral("liveStatus")).toString();
  const auto queued_only = QStringLiteral("Revision %1 queued to save").arg(revision_);
  if ((saved_only.startsWith(QStringLiteral("Saved revision ")) ||
       saved_only.startsWith(queued_only)) &&
      host_.routed()) {
    const auto graph = host_.routed()->status();
    if (graph.model_revision != revision_)
      return;
    const bool persisted = session() && session()->saving().persisted_revision() >= revision_;
    const QString prefix = persisted ? QStringLiteral("Saved revision %1")
                                     : QStringLiteral("Revision %1 queued to save");
    QString receipt;
    if (graph.application == application::PlaybackApplication::active &&
        graph.active_graph_revision == revision_) {
      receipt = QStringLiteral("%1 is active in live audio").arg(prefix.arg(revision_));
    } else if (graph.application == application::PlaybackApplication::failed) {
      receipt = QStringLiteral("%1 is not audible: %2. Stop and Play to retry.")
                    .arg(prefix.arg(revision_))
                    .arg(graph.failure ? QString::fromStdString(graph.failure->message)
                                       : QStringLiteral("graph replacement failed"));
    } else {
      return;
    }
    if (saved_only != receipt) {
      editor_[QStringLiteral("liveStatus")] = receipt;
      emit editorChanged();
    }
    return;
  }
#endif
  QString text;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (host_.routed()) {
    // A refusal stays until the previous gesture's receipt changes state.
    const auto latest = host_.routed()->vst3_manual_edit_status();
    if (!latest || !original_ || latest->plugin != original_->id ||
        latest->status.saved_revision != revision_)
      return;
    const std::pair<std::uint64_t, int> shown{latest->status.saved_revision,
                                              static_cast<int>(latest->status.phase)};
    live_status_shown_ = shown;
    text = live_status_text(latest->status, session() && session()->saving().persisted_revision() <
                                                             latest->status.model_revision);
  }
#endif
  if (!host_.routed())
    live_status_shown_ = {0, -1};
  // Without a run the editor is back on the stopped path; drop the run's text.
  if (editor_.value(QStringLiteral("liveStatus")).toString() == text)
    return;
  if (text.isEmpty())
    editor_.remove(QStringLiteral("liveStatus"));
  else
    editor_[QStringLiteral("liveStatus")] = text;
  emit editorChanged();
}

void Vst3EditorController::discard_vst3_insert_editor() {
  timer_.stop();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // A pending hash is given up, not waited for: its result is dropped.
  verify_timer_.stop();
  hash_.abandon();
  verifying_.reset();
  if (native_window_) {
    native_window_.clear();
    emit nativeWindowChanged();
  }
  if (attachment_)
    attachment_->close();
  attachment_.reset();
  plugin_.reset();
  original_.reset();
  track_.reset();
  slot_.reset();
  revision_ = 0;
  pending_gestures_.clear();
  active_sequences_.clear();
  restart_flags_ = 0;
#endif
  plugin_automation_.clear_selection();
  if (!editor_.isEmpty()) {
    editor_.clear();
    emit editorChanged();
  }
}

} // namespace oscillate::quick
