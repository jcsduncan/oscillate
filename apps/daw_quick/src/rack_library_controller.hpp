#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

namespace oscillate::application {
class ProjectSession;
}

namespace oscillate::quick {

// The user's racks (ADR-0059): a track's built-in device set and macros saved
// as `$XDG_DATA_HOME/oscillate/racks/<name>.json` (~/.local/share when unset),
// listed for the browser and the command palette, loaded onto a track as
// one undo step, deleted, and exported to or imported from any file.
class RackLibraryController final : public QObject {
  Q_OBJECT
  // [{name}] sorted by name.
  Q_PROPERTY(QVariantList racks READ racks NOTIFY racks_changed)

public:
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    std::function<bool()> can_edit;
    std::function<void(QString)> report;
    // After an accepted edit: offer it to playback and refresh the views.
    std::function<void()> after_edit;
    std::function<void()> save;
  };

  explicit RackLibraryController(Hooks hooks, QObject* parent = nullptr);

  [[nodiscard]] QVariantList racks() const;
  [[nodiscard]] QString directory() const;

  // Each returns false, with a status message, when the name, file, rack
  // or track is unusable. Saving replaces a rack of the same name.
  Q_INVOKABLE bool save_rack(const QString& track_id, const QString& name);
  Q_INVOKABLE bool load_rack(const QString& track_id, const QString& name);
  Q_INVOKABLE bool delete_rack(const QString& name);
  Q_INVOKABLE bool export_rack(const QString& name, const QString& path);
  // Reads a rack from any file and keeps it under its file's name.
  Q_INVOKABLE bool import_rack(const QString& path);

signals:
  void racks_changed();

private:
  [[nodiscard]] QString file_of(const QString& name) const;
  void report(const QString& message) const;

  Hooks hooks_;
};

} // namespace oscillate::quick
