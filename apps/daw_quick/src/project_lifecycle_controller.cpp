#include "project_lifecycle_controller.hpp"

#include "oscillate/persistence/project_creation.hpp"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <optional>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace oscillate::quick {
namespace {

constexpr qsizetype max_project_name_length = 80;
// Numbered names tried after the plain one before giving up on a suggestion.
constexpr int max_name_suggestions = 9999;
constexpr int first_numbered_suggestion = 2;
QString default_project_name() { return QStringLiteral("Untitled Project"); }

// Why a project (folder) name can't be used, or empty when it can.
QString name_problem(const QString& name) {
  if (name.isEmpty() || name.size() > max_project_name_length || name == "." || name == ".." ||
      name.contains('/') || name.contains('\\'))
    return QStringLiteral("Choose a folder and a valid project name");
  for (const auto character : name) {
    if (character.isNull() || character.unicode() < 32)
      return QStringLiteral("Project name contains unsupported characters");
  }
  return {};
}

// `base`, or `base N` from 2 up, whichever is first absent from `parent`.
// Empty when every candidate is taken or unusable.
QString first_free_name(const std::filesystem::path& parent, const QString& base) {
  for (int number = first_numbered_suggestion - 1; number <= max_name_suggestions; ++number) {
    auto name =
        number < first_numbered_suggestion ? base : QStringLiteral("%1 %2").arg(base).arg(number);
    if (!name_problem(name).isEmpty())
      return {};
    std::error_code error;
    if (!std::filesystem::exists(parent / name.toStdString(), error) && !error)
      return name;
  }
  return {};
}

} // namespace

ProjectLifecycleController::ProjectLifecycleController(std::filesystem::path projects_root,
                                                       Host host)
    : projects_root_(std::move(projects_root)), host_(std::move(host)) {
  recent_paths_ = QSettings{}.value(QStringLiteral("recentProjects")).toStringList();
  while (recent_paths_.size() > 6)
    recent_paths_.removeLast();
  // Stages left by a creation killed in an earlier process. A failure only
  // leaves hidden folders behind; the next creation in the library retries.
  (void)persistence::ProjectCreation::clean_abandoned(projects_root_);
  refresh_available_projects();
}

void ProjectLifecycleController::refresh_available_projects() {
  QVariantList entries;
  QStringList included;
  const auto open_root = host_.session() ? host_.current_root() : std::filesystem::path{};
  const auto add_project = [&](const std::filesystem::path& root) {
    // A creation stage is not a project until it is renamed into place.
    if (persistence::ProjectCreation::is_stage_name(root.filename().string()))
      return;
    std::error_code error;
    if (!std::filesystem::is_regular_file(root / "state" / "CURRENT", error) &&
        !std::filesystem::is_directory(root / "state" / "generations", error)) {
      return;
    }
    const auto path = QString::fromStdString(root.string());
    if (included.contains(path)) {
      return;
    }
    included.push_back(path);
    // Only projects in the library folder may be deleted from the list;
    // others (opened from elsewhere) can only be forgotten.
    entries.push_back(QVariantMap{{"name", QString::fromStdString(root.filename().string())},
                                  {"path", path},
                                  {"inLibrary", root.parent_path() == projects_root_},
                                  {"recent", recent_paths_.contains(path)},
                                  {"open", !open_root.empty() && root == open_root}});
  };
  for (const auto& path : recent_paths_) {
    add_project(std::filesystem::path(path.toStdString()));
  }
  std::error_code error;
  std::vector<std::filesystem::path> library_paths;
  for (std::filesystem::directory_iterator it(projects_root_, error), end; !error && it != end;
       it.increment(error)) {
    if (it->is_directory(error) && !error) {
      library_paths.push_back(it->path());
    }
  }
  std::ranges::sort(library_paths, [](const auto& left, const auto& right) {
    return left.filename().string() < right.filename().string();
  });
  for (const auto& path : library_paths) {
    add_project(path);
  }
  available_projects_ = std::move(entries);
  host_.available_projects_changed();
}

void ProjectLifecycleController::remember_project(const QString& path) {
  recent_paths_.removeAll(path);
  recent_paths_.prepend(path);
  while (recent_paths_.size() > 6) {
    recent_paths_.removeLast();
  }
  QSettings settings;
  settings.setValue(QStringLiteral("recentProjects"), recent_paths_);
  refresh_available_projects();
}

bool ProjectLifecycleController::save_current_project_if_dirty() {
  auto* session = host_.session();
  if (!session || !session->saving().has_unsaved_changes())
    return true;
  host_.finish_in_flight_saves();
  auto saved = session->saving().save();
  if (!saved) {
    host_.status(QStringLiteral("Project could not be saved: %1")
                     .arg(QString::fromStdString(saved.error().message)));
    return false;
  }
  return true;
}

void ProjectLifecycleController::create_project(const QUrl& parent_folder,
                                                const QString& project_name) {
  if (const auto reason = host_.project_change_block_reason(); !reason.isEmpty()) {
    host_.status(reason);
    return;
  }
  const QString name = project_name.trimmed();
  if (!parent_folder.isLocalFile()) {
    host_.status(QStringLiteral("Choose a folder and a valid project name"));
    return;
  }
  if (const auto problem = name_problem(name); !problem.isEmpty()) {
    host_.status(problem);
    return;
  }
  const auto parent = std::filesystem::path(parent_folder.toLocalFile().toStdString());
  std::error_code error;
  if (!std::filesystem::is_directory(parent, error)) {
    host_.status(QStringLiteral("Choose an existing parent folder"));
    return;
  }
  const auto root = parent / name.toStdString();
  if (std::filesystem::exists(root, error) || error) {
    host_.status(QStringLiteral("A project folder with that name already exists"));
    return;
  }
  // New projects start with the reverb and delay returns.
  auto created = application::ProjectSession::create(root, {.default_returns = true});
  if (!created) {
    host_.status(QStringLiteral("Could not create project: %1")
                     .arg(QString::fromStdString(created.error().message)));
    return;
  }
  const auto warning = created.value()->creation_warning();
  host_.install_project(std::move(created.value()), root);
  if (warning)
    host_.status(
        QStringLiteral("Project created, but %1").arg(QString::fromStdString(warning->message)));
}

QString ProjectLifecycleController::suggested_project_name() const {
  return first_free_name(projects_root_, default_project_name());
}

QString ProjectLifecycleController::suggested_copy_name() const {
  if (!host_.session())
    return {};
  const auto root = host_.current_root();
  return first_free_name(root.parent_path(), QStringLiteral("%1 copy").arg(
                                                 QString::fromStdString(root.filename().string())));
}

std::optional<std::filesystem::path>
ProjectLifecycleController::copy_destination(const QString& copy_name) const {
  if (!host_.session()) {
    host_.status(QStringLiteral("Open a project first"));
    return std::nullopt;
  }
  const QString name = copy_name.trimmed();
  if (const auto problem = name_problem(name); !problem.isEmpty()) {
    host_.status(problem);
    return std::nullopt;
  }
  auto root = host_.current_root().parent_path() / name.toStdString();
  std::error_code error;
  if (std::filesystem::exists(root, error) || error) {
    host_.status(QStringLiteral("A project folder with that name already exists"));
    return std::nullopt;
  }
  return root;
}

void ProjectLifecycleController::create_default_project(const QString& project_name) {
  std::error_code error;
  std::filesystem::create_directories(projects_root_, error);
  if (error) {
    host_.status(QStringLiteral("Could not create project library: %1")
                     .arg(QString::fromStdString(error.message())));
    return;
  }
  create_project(QUrl::fromLocalFile(QString::fromStdString(projects_root_.string())),
                 project_name);
}

void ProjectLifecycleController::open_path(const std::filesystem::path& root) {
  if (const auto reason = host_.project_change_block_reason(); !reason.isEmpty()) {
    host_.status(reason);
    return;
  }
  // Persist before loading: the destination may be the project already open.
  if (!save_current_project_if_dirty())
    return;
  if (host_.session() && root == host_.current_root()) {
    host_.status(QStringLiteral("Project is already open and saved"));
    return;
  }
  auto inspection = application::ProjectSession::inspect_recovery(root);
  if (!inspection) {
    host_.status(QStringLiteral("Could not inspect project recovery: %1")
                     .arg(QString::fromStdString(inspection.error().message)));
    return;
  }
  const auto saved = std::ranges::find_if(inspection.value().choices, [](const auto& choice) {
    return choice.pointed && choice.kind == persistence::SaveKind::explicit_save;
  });
  const bool newer_autosave =
      saved != inspection.value().choices.end() &&
      std::ranges::any_of(inspection.value().choices, [&](const auto& choice) {
        return choice.pointed && choice.kind == persistence::SaveKind::autosave &&
               choice.revision > saved->revision;
      });
  if (!inspection.value().has_current || newer_autosave || inspection.value().autosave_issue) {
    if (inspection.value().choices.empty()) {
      host_.status(QStringLiteral("Could not open project: no validated recovery generation"));
      return;
    }
    present_recovery(root, inspection.value());
    return;
  }
  clear_recovery();
  auto opened = application::ProjectSession::open(root);
  if (!opened) {
    host_.status(QStringLiteral("Could not open project: %1")
                     .arg(QString::fromStdString(opened.error().message)));
    return;
  }
  host_.install_project(std::move(opened.value()), root);
}

void ProjectLifecycleController::clear_recovery() {
  if (recovery_choice_.isEmpty())
    return;
  recovery_choice_.clear();
  pending_recovery_root_.clear();
  pending_recovery_choices_.clear();
  host_.recovery_choice_changed();
}

void ProjectLifecycleController::present_recovery(
    const std::filesystem::path& root,
    const application::ProjectSession::RecoveryInspection& inspection) {
  QVariantList options;
  for (const auto& choice : inspection.choices) {
    const auto kind = choice.kind == persistence::SaveKind::explicit_save
                          ? QStringLiteral("saved")
                          : QStringLiteral("autosave");
    const auto label = choice.pointed ? choice.kind == persistence::SaveKind::explicit_save
                                            ? QStringLiteral("Last saved project")
                                            : QStringLiteral("Recovered autosave")
                       : choice.kind == persistence::SaveKind::explicit_save
                           ? QStringLiteral("Repair from saved generation")
                           : QStringLiteral("Repair autosave pointer");
    const auto generation_path = root / "state" / "generations" / choice.generation;
    const auto modified =
        QFileInfo(QString::fromStdString(generation_path.string())).lastModified();
    options.push_back(QVariantMap{
        {"generation", QString::fromStdString(choice.generation)},
        {"kind", kind},
        {"label", label},
        {"revision", QVariant::fromValue<qulonglong>(choice.revision)},
        {"tracks", QVariant::fromValue<qulonglong>(choice.tracks)},
        {"clips", QVariant::fromValue<qulonglong>(choice.clips)},
        {"modified", modified.isValid() ? modified.toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                        : QStringLiteral("time unavailable")}});
  }
  pending_recovery_root_ = root;
  pending_recovery_choices_ = inspection.choices;
  recovery_choice_ = QVariantMap{
      {"active", true},
      {"projectName", QString::fromStdString(root.filename().string())},
      {"path", QString::fromStdString(root.string())},
      {"hasCurrent", inspection.has_current},
      {"currentIssue",
       inspection.current_issue ? QString::fromStdString(*inspection.current_issue) : QString{}},
      {"autosaveIssue",
       inspection.autosave_issue ? QString::fromStdString(*inspection.autosave_issue) : QString{}},
      {"options", options}};
  host_.recovery_choice_changed();
  host_.status(QStringLiteral("Choose a validated project state to open"));
}

// ProjectBridge forwards QML's positional call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectLifecycleController::choose_recovery(const QString& generation, const QString& kind) {
  if (recovery_choice_.isEmpty())
    return;
  if (const auto reason = host_.project_change_block_reason(); !reason.isEmpty()) {
    host_.status(reason);
    return;
  }
  const auto selected_kind = kind == QStringLiteral("saved") ? persistence::SaveKind::explicit_save
                                                             : persistence::SaveKind::autosave;
  const auto selected = std::ranges::find_if(pending_recovery_choices_, [&](const auto& choice) {
    return choice.generation == generation.toStdString() && choice.kind == selected_kind;
  });
  if ((kind != QStringLiteral("saved") && kind != QStringLiteral("autosave")) ||
      selected == pending_recovery_choices_.end()) {
    host_.status(QStringLiteral("Recovery choice is no longer available"));
    return;
  }
  auto opened = application::ProjectSession::open_recovered(
      pending_recovery_root_, selected->generation, selected->kind, selected->pointed);
  if (!opened) {
    host_.status(QStringLiteral("Could not open recovered project: %1")
                     .arg(QString::fromStdString(opened.error().message)));
    return;
  }
  const auto root = pending_recovery_root_;
  clear_recovery();
  host_.install_project(std::move(opened.value()), root);
  if (selected_kind == persistence::SaveKind::autosave)
    host_.status(
        QStringLiteral("Recovered autosave opened as unsaved work. Save to make it current."));
}

void ProjectLifecycleController::cancel_recovery() { clear_recovery(); }

void ProjectLifecycleController::open_project(const QUrl& folder) {
  if (!folder.isLocalFile()) {
    host_.status(QStringLiteral("Choose a local project folder"));
    return;
  }
  open_path(std::filesystem::path(folder.toLocalFile().toStdString()));
}

void ProjectLifecycleController::choose_open_project() {
  const auto folder =
      QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Open project"), QDir::homePath());
  if (!folder.isEmpty()) {
    open_project(QUrl::fromLocalFile(folder));
  }
}

void ProjectLifecycleController::open_listed(const QString& path) {
  const auto listed = std::ranges::any_of(available_projects_, [&path](const QVariant& entry) {
    return entry.toMap().value(QStringLiteral("path")).toString() == path;
  });
  if (!listed) {
    host_.status(QStringLiteral("That project is not in the project list"));
    return;
  }
  open_path(std::filesystem::path(path.toStdString()));
}

std::optional<QVariantMap> ProjectLifecycleController::listed(const QString& path) const {
  for (const auto& entry : available_projects_) {
    auto map = entry.toMap();
    if (map.value(QStringLiteral("path")).toString() == path)
      return map;
  }
  return std::nullopt;
}

void ProjectLifecycleController::forget_project(const QString& path) {
  if (recent_paths_.removeAll(path) == 0)
    return;
  QSettings{}.setValue(QStringLiteral("recentProjects"), recent_paths_);
  refresh_available_projects();
}

// ProjectBridge forwards QML's positional call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ProjectLifecycleController::rename_project(const QString& path, const QString& new_name) {
  const auto entry = listed(path);
  if (!entry || !entry->value(QStringLiteral("inLibrary")).toBool()) {
    host_.status(QStringLiteral("Only projects in the project library can be renamed here"));
    return;
  }
  if (entry->value(QStringLiteral("open")).toBool()) {
    host_.status(QStringLiteral("Close the project before renaming it"));
    return;
  }
  const QString name = new_name.trimmed();
  if (name == entry->value(QStringLiteral("name")).toString())
    return;
  if (const auto problem = name_problem(name); !problem.isEmpty()) {
    host_.status(problem);
    return;
  }
  const auto from = std::filesystem::path(path.toStdString());
  const auto to = from.parent_path() / name.toStdString();
  std::error_code error;
  if (std::filesystem::exists(to, error) || error) {
    host_.status(QStringLiteral("A project folder with that name already exists"));
    return;
  }
  std::filesystem::rename(from, to, error);
  if (error) {
    host_.status(QStringLiteral("Could not rename the project: %1")
                     .arg(QString::fromStdString(error.message())));
    return;
  }
  const auto renamed = QString::fromStdString(to.string());
  for (auto& recent : recent_paths_) {
    if (recent == path)
      recent = renamed;
  }
  QSettings{}.setValue(QStringLiteral("recentProjects"), recent_paths_);
  refresh_available_projects();
  host_.status(QStringLiteral("Renamed to %1").arg(name));
}

void ProjectLifecycleController::trash_project(const QString& path) {
  const auto entry = listed(path);
  if (!entry || !entry->value(QStringLiteral("inLibrary")).toBool()) {
    host_.status(QStringLiteral("Only projects in the project library can be deleted here"));
    return;
  }
  if (entry->value(QStringLiteral("open")).toBool()) {
    host_.status(QStringLiteral("Close the project before deleting it"));
    return;
  }
  const auto name = entry->value(QStringLiteral("name")).toString();
  // The Trash, not a hard delete: a mistaken click is recoverable.
  if (!QFile::moveToTrash(path)) {
    host_.status(QStringLiteral("Could not move %1 to the Trash").arg(name));
    return;
  }
  recent_paths_.removeAll(path);
  QSettings{}.setValue(QStringLiteral("recentProjects"), recent_paths_);
  refresh_available_projects();
  host_.status(QStringLiteral("Moved %1 to the Trash").arg(name));
}

void ProjectLifecycleController::save() {
  if (const auto reason = host_.save_block_reason(); !reason.isEmpty()) {
    host_.status(reason);
    return;
  }
  auto* session = host_.session();
  if (!session) {
    host_.status(QStringLiteral("Open a project first"));
    return;
  }
  host_.finish_in_flight_saves();
  const auto result = session->saving().save();
  if (result)
    host_.saved();
  if (!result) {
    host_.status(
        QStringLiteral("Save failed: %1").arg(QString::fromStdString(result.error().message)));
  } else if (const auto& warning = session->saving().save_maintenance_warning()) {
    host_.status(QStringLiteral("Saved · revision %1 · Storage cleanup needs attention: %2")
                     .arg(host_.revision())
                     .arg(QString::fromStdString(warning->message)));
  } else {
    host_.status(QStringLiteral("Saved · revision %1").arg(host_.revision()));
  }
}

void ProjectLifecycleController::refresh_named_revisions() {
  auto* session = host_.session();
  if (!session) {
    named_revisions_.clear();
    named_revision_issues_.clear();
    host_.named_revisions_changed();
    return;
  }
  auto listed = session->saving().list_named_revisions();
  if (!listed) {
    named_revisions_.clear();
    named_revision_issues_.clear();
    host_.named_revisions_changed();
    host_.status(QStringLiteral("Could not list named revisions: %1")
                     .arg(QString::fromStdString(listed.error().message)));
    return;
  }
  QVariantList revisions;
  QVariantList issues;
  for (const auto& item : listed.value().revisions) {
    revisions.push_back(QVariantMap{{"name", QString::fromStdString(item.name)},
                                    {"generation", QString::fromStdString(item.generation)}});
  }
  for (const auto& issue : listed.value().issues) {
    const QString reason = issue.kind == persistence::NamedRevisionIssue::Kind::incomplete
                               ? QStringLiteral("Incomplete")
                           : issue.kind == persistence::NamedRevisionIssue::Kind::stale
                               ? QStringLiteral("Missing saved generation")
                           : issue.kind == persistence::NamedRevisionIssue::Kind::duplicate
                               ? QStringLiteral("Duplicate name")
                               : QStringLiteral("Damaged entry");
    issues.push_back(QVariantMap{
        {"name", issue.name ? QString::fromStdString(*issue.name)
                            : QStringLiteral("Entry %1").arg(QString::fromStdString(issue.entry))},
        {"reason", reason}});
  }
  named_revisions_ = std::move(revisions);
  named_revision_issues_ = std::move(issues);
  host_.named_revisions_changed();
}

void ProjectLifecycleController::create_named_revision(const QString& name) {
  if (!host_.named_revision_enabled()) {
    host_.status(QStringLiteral("Stop playback and finish project jobs before naming a revision"));
    return;
  }
  auto* session = host_.session();
  auto created = session->saving().create_named_revision(name.toStdString());
  refresh_named_revisions();
  if (!created) {
    host_.status(QStringLiteral("Could not name revision: %1")
                     .arg(QString::fromStdString(created.error().message)));
    return;
  }
  host_.status(created.value().status == persistence::SaveStatus::committed
                   ? QStringLiteral("Named revision '%1' saved at model revision %2")
                         .arg(name)
                         .arg(host_.revision())
                   : QStringLiteral("Named revision '%1' may have been published; inspect the list "
                                    "before retrying")
                         .arg(name));
}

void ProjectLifecycleController::restore_named_revision(const QString& name) {
  if (!host_.named_revision_enabled()) {
    host_.status(QStringLiteral("Stop playback and finish project jobs before restoring"));
    return;
  }
  auto* session = host_.session();
  const auto before = session->snapshot().revision;
  auto restored = session->saving().restore_named_revision(name.toStdString());
  if (session->snapshot().revision != before)
    host_.restored();
  if (!restored) {
    host_.status(QStringLiteral("Restore '%1': %2")
                     .arg(name)
                     .arg(QString::fromStdString(restored.error().message)));
    return;
  }
  refresh_named_revisions();
  host_.status(
      QStringLiteral("Restored '%1' · saved as revision %2 · Undo is available this session")
          .arg(name)
          .arg(host_.revision()));
}

} // namespace oscillate::quick
