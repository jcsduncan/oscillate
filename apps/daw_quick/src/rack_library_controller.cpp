#include "rack_library_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/persistence/rack_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::quick {
namespace {

bool usable_name(const QString& name) {
  if (name.isEmpty() || name.size() > 64 || name.startsWith(QLatin1Char('.')))
    return false;
  for (const QChar c : name) {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\') || !c.isPrint())
      return false;
  }
  return true;
}

QString local_path(const QString& path) {
  const QUrl url(path);
  return url.isLocalFile() ? url.toLocalFile() : path;
}

std::optional<QByteArray> read_file(const QString& path) {
  QFile file(path);
  if (file.size() > static_cast<qint64>(persistence::max_rack_bytes) ||
      !file.open(QIODevice::ReadOnly))
    return std::nullopt;
  return file.readAll();
}

bool write_file(const QString& path, const std::string& text) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  if (file.write(text.data(), static_cast<qint64>(text.size())) !=
      static_cast<qint64>(text.size())) {
    file.cancelWriting();
    return false;
  }
  return file.commit();
}

std::optional<project::TrackId> parse_track(const QString& id) {
  const auto parsed = project::TrackId::parse(id.toStdString());
  if (!parsed)
    return std::nullopt;
  return parsed.value();
}

} // namespace

RackLibraryController::RackLibraryController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)) {}

QString RackLibraryController::directory() const {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty())
    data = QDir::homePath() + QStringLiteral("/.local/share");
  return data + QStringLiteral("/oscillate/racks");
}

QString RackLibraryController::file_of(const QString& name) const {
  return directory() + QLatin1Char('/') + name + QStringLiteral(".json");
}

void RackLibraryController::report(const QString& message) const {
  if (hooks_.report)
    hooks_.report(message);
}

QVariantList RackLibraryController::racks() const {
  QStringList names;
  for (const auto& file :
       QDir(directory()).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::Readable)) {
    if (usable_name(file.completeBaseName()))
      names.append(file.completeBaseName());
  }
  names.sort(Qt::CaseInsensitive);
  QVariantList list;
  for (const auto& name : names)
    list.append(QVariantMap{{QStringLiteral("name"), name}});
  return list;
}

bool RackLibraryController::save_rack(const QString& track_id, const QString& name) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto track = parse_track(track_id);
  const auto trimmed = name.trimmed();
  if (session == nullptr || !track) {
    report(QStringLiteral("Select a track to save its rack"));
    return false;
  }
  if (!usable_name(trimmed)) {
    report(QStringLiteral("A rack needs a name without slashes, up to 64 characters"));
    return false;
  }
  auto captured = session->track_devices().capture_track_rack(*track);
  if (!captured) {
    report(QStringLiteral("Rack: %1").arg(QString::fromStdString(captured.error().message)));
    return false;
  }
  if (!QDir().mkpath(directory()) ||
      !write_file(file_of(trimmed), persistence::encode_rack(captured.value().rack))) {
    report(QStringLiteral("Rack \"%1\" could not be written").arg(trimmed));
    return false;
  }
  const auto skipped = captured.value().skipped;
  report(skipped == 0
             ? QStringLiteral("Saved rack \"%1\"").arg(trimmed)
             : QStringLiteral("Saved rack \"%1\"; %2 VST3 or convolution device%3 left out")
                   .arg(trimmed)
                   .arg(skipped)
                   .arg(skipped == 1 ? QString() : QStringLiteral("s")));
  emit racks_changed();
  return true;
}

bool RackLibraryController::load_rack(const QString& track_id, const QString& name) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto track = parse_track(track_id);
  if (session == nullptr || !track) {
    report(QStringLiteral("Select a track to load a rack onto"));
    return false;
  }
  if (hooks_.can_edit && !hooks_.can_edit()) {
    report(QStringLiteral("Finish the current project operation before loading a rack"));
    return false;
  }
  if (!usable_name(name))
    return false;
  const auto text = read_file(file_of(name));
  if (!text) {
    report(QStringLiteral("Rack \"%1\" could not be read").arg(name));
    return false;
  }
  auto rack = persistence::decode_rack(
      std::string_view(text->constData(), static_cast<std::size_t>(text->size())));
  if (!rack) {
    report(
        QStringLiteral("Rack \"%1\": %2").arg(name, QString::fromStdString(rack.error().message)));
    return false;
  }
  const auto before = session->snapshot().revision;
  if (auto loaded = session->track_devices().load_track_rack(*track, rack.value()); !loaded) {
    report(QStringLiteral("Rack \"%1\": %2")
               .arg(name, QString::fromStdString(loaded.error().message)));
    return false;
  }
  if (session->snapshot().revision != before) {
    if (hooks_.after_edit)
      hooks_.after_edit();
    if (hooks_.save)
      hooks_.save();
  }
  report(QStringLiteral("Loaded rack \"%1\" · one undo step").arg(name));
  return true;
}

bool RackLibraryController::delete_rack(const QString& name) {
  if (!usable_name(name) || !QFile::remove(file_of(name)))
    return false;
  emit racks_changed();
  return true;
}

bool RackLibraryController::export_rack(const QString& name, const QString& path) {
  if (!usable_name(name))
    return false;
  const auto text = read_file(file_of(name));
  // Only a rack that still reads is written out.
  if (!text || !persistence::decode_rack(
                   std::string_view(text->constData(), static_cast<std::size_t>(text->size())))) {
    report(QStringLiteral("Rack \"%1\" could not be read").arg(name));
    return false;
  }
  if (!write_file(local_path(path),
                  std::string(text->constData(), static_cast<std::size_t>(text->size())))) {
    report(QStringLiteral("Rack \"%1\" could not be exported").arg(name));
    return false;
  }
  return true;
}

bool RackLibraryController::import_rack(const QString& path) {
  const auto file = local_path(path);
  const auto text = read_file(file);
  auto name = QFileInfo(file).completeBaseName().trimmed();
  if (!usable_name(name))
    name = QStringLiteral("Imported");
  if (!text) {
    report(QStringLiteral("The rack file could not be read"));
    return false;
  }
  const std::string body(text->constData(), static_cast<std::size_t>(text->size()));
  if (auto rack = persistence::decode_rack(body); !rack) {
    report(QStringLiteral("Not a rack: %1").arg(QString::fromStdString(rack.error().message)));
    return false;
  }
  if (!QDir().mkpath(directory()) || !write_file(file_of(name), body)) {
    report(QStringLiteral("Rack \"%1\" could not be kept").arg(name));
    return false;
  }
  emit racks_changed();
  report(QStringLiteral("Imported rack \"%1\"").arg(name));
  return true;
}

} // namespace oscillate::quick
