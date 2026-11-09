#include "project_bridge.hpp"

#include "plugin_automation_controller.hpp"
#include "waveform_controller.hpp"

#include <QSettings>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

// The open project: changing, closing and releasing it, the plugin state a
// change waits on, its saved view state and the project lifecycle commands.
namespace oscillate::quick {
namespace {

[[nodiscard]] QString track_heights_key(const project::ProjectId& id) {
  return QStringLiteral("trackRowHeights/%1").arg(QString::fromStdString(id.to_string()));
}

[[nodiscard]] QString collapsed_buses_key(const project::ProjectId& id) {
  return QStringLiteral("collapsedBuses/%1").arg(QString::fromStdString(id.to_string()));
}

[[nodiscard]] bool is_group_bus(const project::Snapshot& snapshot, const QString& id) {
  return std::ranges::any_of(snapshot.tracks, [&id](const project::Track& track) {
    return track.kind == project::TrackKind::group_bus &&
           QString::fromStdString(track.id.to_string()) == id;
  });
}

} // namespace

bool ProjectBridge::plugin_state_saved(std::function<void()> change) {
  if (plugin_state_waived_) {
    vst3_editor_->discard_vst3_insert_editor();
    return true;
  }
  const auto failure = plugin_editor_commit_();
  if (!failure) {
    held_project_change_ = {};
    set_plugin_state_problem({});
    return true;
  }
  held_project_change_ = std::move(change);
  set_plugin_state_problem(
      QStringLiteral("Nothing changed: the open plug-in's latest state could not be saved. %1")
          .arg(*failure));
  return false;
}

void ProjectBridge::set_plugin_state_problem(QString problem) {
  if (!problem.isEmpty())
    set_status(problem);
  if (problem == plugin_state_problem_)
    return;
  plugin_state_problem_ = std::move(problem);
  emit pluginStateProblemChanged();
}

void ProjectBridge::continue_without_plugin_state() {
  auto change = std::exchange(held_project_change_, {});
  set_plugin_state_problem({});
  if (!change)
    return;
  plugin_state_waived_ = true;
  change();
  plugin_state_waived_ = false;
}

void ProjectBridge::cancel_project_change() {
  held_project_change_ = {};
  set_plugin_state_problem({});
  // A window close stopped the save timers before the capture refused it.
  if (session_ && autosave_interval_seconds_ > 0 && !autosave_timer_.isActive())
    autosave_timer_.start();
  if (background_save_pending_ && !background_save_timer_.isActive())
    background_save_timer_.start();
  set_status(vst3_editor_->resume_native_editor()
                 ? QStringLiteral("Project change cancelled; the plug-in editor is open again")
                 : QStringLiteral("Project change cancelled"));
}

bool ProjectBridge::ready_to_change_project(std::function<void()> change) {
  if (background_save_job_.running()) {
    set_status(QStringLiteral("Wait for project save to finish before changing projects"));
    return false;
  }
  if (autosave_job_.running()) {
    set_status(QStringLiteral("Wait for autosave to finish before changing projects"));
    return false;
  }
  if (playback_busy() || export_busy() || import_busy()) {
    set_status(managed_media_busy()
                   ? QStringLiteral("Finish media verification before changing projects")
               : import_busy() ? QStringLiteral("Finish import before changing projects")
               : export_busy() ? QStringLiteral("Finish export before changing projects")
                               : QStringLiteral("Stop playback before changing projects"));
    return false;
  }
  if (!plugin_state_saved(std::move(change)))
    return false;
  return save_current_project_if_dirty();
}

// Everything that belongs to the open project, stopped and let go.
void ProjectBridge::release_project() {
  background_save_timer_.stop();
  background_save_pending_ = false;
  vst3_editor_->discard_vst3_insert_editor();
  close_native_synth_editor();
  close_native_effect_editor();
  clear_device_strips();
  plugin_automation_->reset_project();
  key_review_->set_project(nullptr, {});
  playback_.reset();
  routed_playback_.reset();
  play_after_monitor_ = false;
  if (monitor_completion_) {
    monitor_completion_->stop_requested.store(true, std::memory_order_release);
    if (monitor_worker_.joinable())
      monitor_worker_.join();
    if (monitor_completion_->result && *monitor_completion_->result)
      (void)monitor_completion_->result->value()->stop_and_retire();
    monitor_completion_.reset();
  }
  stop_monitor_now();
  meter_->invalidate_after_quiesce();
  true_peak_->invalidate_after_quiesce();
  playhead_sample_ = 0;
}

void ProjectBridge::close_project() {
  if (!session_ || !ready_to_change_project([this] { close_project(); }))
    return;
  release_project();
  autosave_timer_.stop();
  managed_media_->set_project(nullptr);
  imports_->audio().set_project(nullptr);
  session_.reset();
  ++project_generation_;
  project_name_.clear();
  set_save_problem({});
  track_row_heights_.clear();
  emit trackRowHeightsChanged();
  collapsed_buses_.clear();
  emit collapsedBusesChanged();
  // The next project publishes everything afresh.
  published_ = {};
  emit projectChanged();
  refresh_available_projects();
  set_status(QStringLiteral("Project closed"));
}

void ProjectBridge::set_project(std::unique_ptr<application::ProjectSession> session,
                                const std::filesystem::path& opened_root) {
  // A change the plug-in editor stops keeps the opened project until the
  // user answers.
  auto held = std::make_shared<std::unique_ptr<application::ProjectSession>>(std::move(session));
  if (!ready_to_change_project(
          [this, held, opened_root] { set_project(std::move(*held), opened_root); }))
    return;
  session = std::move(*held);
  // Every consumer of the root (warp sources, caches, recents) gets one
  // absolute, normalized path: a project opened as "build/song" or "song/"
  // otherwise plays until a clip needs warping.
  std::error_code absolute_error;
  auto root = std::filesystem::absolute(opened_root, absolute_error).lexically_normal();
  if (absolute_error)
    root = opened_root.lexically_normal();
  if (!root.has_filename())
    root = root.parent_path();
  release_project();
  session_ = std::move(session);
  set_save_problem({});
  refresh_meters();
  last_autosave_revision_ = session_->snapshot().revision;
  autosave_status_.clear();
  emit autosaveStatusChanged();
  if (autosave_interval_seconds_ > 0)
    autosave_timer_.start();
  track_row_heights_.clear();
  const auto saved_heights = QSettings{}.value(track_heights_key(session_->snapshot().id)).toMap();
  for (const auto& track : session_->snapshot().tracks) {
    const auto id = QString::fromStdString(track.id.to_string());
    const int height = saved_heights.value(id).toInt();
    if (height >= 64 && height <= 480)
      track_row_heights_.insert(id, height);
  }
  emit trackRowHeightsChanged();
  collapsed_buses_.clear();
  for (const auto& id :
       QSettings{}.value(collapsed_buses_key(session_->snapshot().id)).toStringList()) {
    if (is_group_bus(session_->snapshot(), id) && !collapsed_buses_.contains(id))
      collapsed_buses_.push_back(id);
  }
  emit collapsedBusesChanged();
  managed_media_->set_project(session_.get());
  imports_->audio().set_project(session_.get());
  key_review_->set_project(session_.get(), root);
  ++project_generation_;
  shown_lead_bars_ = static_cast<int>(session_->snapshot().lead_bars);
  playhead_sample_ = bar_one_sample();
  waveform_root_ = root;
  waveform_->set_project(root);
  playback_ = std::make_unique<application::InstrumentPlayback>(*session_, audio_output_->factory(),
                                                                audio_output_->buffer_frames());
  audio_edit_failure_.clear();
  set_playback_status({});
  const auto name = root.filename().empty() ? root.string() : root.filename().string();
  project_name_ = QString::fromStdString(name);
  exports_->reset();
  refresh();
  remember_project(QString::fromStdString(root.string()));
  set_status(QStringLiteral("Project ready · model revision %1").arg(revision_));
  vst3_browser_->scan_for_project(session_->snapshot());
  cache_ui_->register_project(root);
  refresh_named_revisions();
  managed_media_->refresh();
}

void ProjectBridge::set_track_row_height(const QString& track_id, int height) {
  if (!session_ || height < 64 || height > 480)
    return;
  const auto& tracks = session_->snapshot().tracks;
  const bool exists = std::ranges::any_of(tracks, [&track_id](const project::Track& track) {
    return QString::fromStdString(track.id.to_string()) == track_id;
  });
  if (!exists)
    return;
  if (height == 88)
    track_row_heights_.remove(track_id);
  else
    track_row_heights_.insert(track_id, height);
  QSettings{}.setValue(track_heights_key(session_->snapshot().id), track_row_heights_);
  emit trackRowHeightsChanged();
}

void ProjectBridge::set_bus_collapsed(const QString& bus_id, bool collapsed) {
  if (!session_ || !is_group_bus(session_->snapshot(), bus_id) ||
      collapsed_buses_.contains(bus_id) == collapsed)
    return;
  if (collapsed)
    collapsed_buses_.push_back(bus_id);
  else
    collapsed_buses_.removeAll(bus_id);
  QSettings{}.setValue(collapsed_buses_key(session_->snapshot().id), collapsed_buses_);
  emit collapsedBusesChanged();
}

void ProjectBridge::set_arrangement_viewport(double first_bar, double visible_bars,
                                             int pixel_width) {
  // Only the waveform columns in view change.
  if (waveform_->set_viewport(first_bar, visible_bars, pixel_width))
    refresh_clip_waveforms();
}

bool ProjectBridge::managed_media_busy() const { return managed_media_->busy(); }

bool ProjectBridge::managed_media_repair_enabled() const {
  return session_ && !managed_media_busy() && !playback_busy() && !export_busy() &&
         !import_busy() && !vst3_browser_busy() && !close_after_playback_ &&
         !closing_after_export() && !imports_->audio().closing() && !managed_media_->closing();
}

void ProjectBridge::collect_project_copy() {
  if (!session_ || waveform_root_.empty() || collection_->busy() || playback_busy() ||
      export_busy() || import_busy() || managed_media_busy() || vst3_browser_busy()) {
    set_status(QStringLiteral("Finish active project work before collecting a copy"));
    return;
  }
  if (const auto failure = collection_->collect(*session_, waveform_root_, projects_root_))
    set_status(*failure);
}

void ProjectBridge::cancel_project_collection() { collection_->cancel(); }

void ProjectBridge::save_copy(const QString& copy_name) {
  if (!session_ || waveform_root_.empty() || collection_->busy() || !jobs_idle() ||
      vst3_browser_busy()) {
    set_status(QStringLiteral("Finish active project work before saving a copy"));
    return;
  }
  const auto destination = lifecycle_->copy_destination(copy_name);
  if (!destination)
    return;
  // An open plug-in editor's state belongs in the copy, not only the original.
  if (!plugin_state_saved([this, copy_name] { save_copy(copy_name); }))
    return;
  finish_in_flight_saves();
  if (const auto failure = collection_->save_copy(*session_, waveform_root_, *destination))
    set_status(*failure);
}

void ProjectBridge::refresh_available_projects() { lifecycle_->refresh_available_projects(); }
void ProjectBridge::remember_project(const QString& path) { lifecycle_->remember_project(path); }
bool ProjectBridge::save_current_project_if_dirty() {
  return lifecycle_->save_current_project_if_dirty();
}
void ProjectBridge::create_default_project(const QString& name) {
  lifecycle_->create_default_project(name);
}
QString ProjectBridge::suggested_copy_name() const { return lifecycle_->suggested_copy_name(); }
void ProjectBridge::open_project(const QUrl& folder) { lifecycle_->open_project(folder); }
void ProjectBridge::choose_open_project() { lifecycle_->choose_open_project(); }
void ProjectBridge::open_listed(const QString& path) { lifecycle_->open_listed(path); }
void ProjectBridge::forget_project(const QString& path) { lifecycle_->forget_project(path); }
void ProjectBridge::trash_project(const QString& path) { lifecycle_->trash_project(path); }
void ProjectBridge::rename_project(const QString& path, const QString& name) {
  lifecycle_->rename_project(path, name);
}
void ProjectBridge::choose_recovery(const QString& generation, const QString& kind) {
  lifecycle_->choose_recovery(generation, kind);
}
void ProjectBridge::cancel_recovery() { lifecycle_->cancel_recovery(); }
void ProjectBridge::save() { lifecycle_->save(); }
bool ProjectBridge::named_revision_enabled() const { return lifecycle_->named_revision_enabled(); }
void ProjectBridge::refresh_named_revisions() { lifecycle_->refresh_named_revisions(); }
void ProjectBridge::create_named_revision(const QString& name) {
  lifecycle_->create_named_revision(name);
}
void ProjectBridge::restore_named_revision(const QString& name) {
  lifecycle_->restore_named_revision(name);
}

} // namespace oscillate::quick
