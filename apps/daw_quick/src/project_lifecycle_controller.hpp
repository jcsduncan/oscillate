#pragma once

#include "oscillate/application/project_session.hpp"

#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace oscillate::quick {

// UI-thread owner of project-library discovery and persisted project commands.
// Session installation stays with the bridge because it quiesces playback and
// media services; these callbacks make that boundary explicit.
class ProjectLifecycleController final {
public:
  struct Host {
    std::function<application::ProjectSession*()> session;
    std::function<std::filesystem::path()> current_root;
    std::function<QString()> project_change_block_reason;
    std::function<QString()> save_block_reason;
    // Waits for a background save or autosave that is writing the project,
    // so a save requested now is not refused as one already in progress.
    std::function<void()> finish_in_flight_saves;
    std::function<bool()> named_revision_enabled;
    std::function<void(std::unique_ptr<application::ProjectSession>, const std::filesystem::path&)>
        install_project;
    std::function<void(QString)> status;
    std::function<void()> available_projects_changed;
    std::function<void()> recovery_choice_changed;
    std::function<void()> named_revisions_changed;
    std::function<void()> saved;
    std::function<void()> restored;
    std::function<qulonglong()> revision;
  };

  ProjectLifecycleController(std::filesystem::path projects_root, Host host);

  [[nodiscard]] const QVariantList& available_projects() const noexcept {
    return available_projects_;
  }
  [[nodiscard]] const QVariantList& named_revisions() const noexcept { return named_revisions_; }
  [[nodiscard]] const QVariantList& named_revision_issues() const noexcept {
    return named_revision_issues_;
  }
  [[nodiscard]] bool named_revision_enabled() const { return host_.named_revision_enabled(); }
  [[nodiscard]] const QVariantMap& recovery_choice() const noexcept { return recovery_choice_; }

  void refresh_available_projects();
  void remember_project(const QString& path);
  [[nodiscard]] bool save_current_project_if_dirty();
  void create_project(const QUrl& parent_folder, const QString& project_name);
  void create_default_project(const QString& project_name);
  void open_project(const QUrl& folder);
  void choose_open_project();
  void open_listed(const QString& path);
  // Removes a project from the recent list; its files are untouched.
  void forget_project(const QString& path);
  // Renames a library project's folder (not the open project's).
  void rename_project(const QString& path, const QString& new_name);
  // Moves a library project (not the open one) to the system Trash.
  void trash_project(const QString& path);
  void choose_recovery(const QString& generation, const QString& kind);
  void cancel_recovery();
  // Saves the project's latest state now; a background save or autosave
  // writing it at the time finishes first.
  void save();
  void refresh_named_revisions();
  void create_named_revision(const QString& name);
  void restore_named_revision(const QString& name);

private:
  [[nodiscard]] std::optional<QVariantMap> listed(const QString& path) const;
  void open_path(const std::filesystem::path& root);
  void clear_recovery();
  void present_recovery(const std::filesystem::path& root,
                        const application::ProjectSession::RecoveryInspection& inspection);

  std::filesystem::path projects_root_;
  Host host_;
  QStringList recent_paths_;
  QVariantList available_projects_;
  QVariantList named_revisions_;
  QVariantList named_revision_issues_;
  QVariantMap recovery_choice_;
  std::filesystem::path pending_recovery_root_;
  std::vector<application::ProjectSession::RecoveryChoice> pending_recovery_choices_;
};

} // namespace oscillate::quick
