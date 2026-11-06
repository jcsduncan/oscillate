#include "browser_preview_controller.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <utility>

namespace oscillate::quick {
namespace {

constexpr auto auto_preview_key = "browser/autoPreview";
constexpr auto sample_folder_key = "browser/sampleFolder";

// The first of the usual sample folders that exists, else ~/Music.
[[nodiscard]] QString default_folder() {
  const auto music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  const auto home = QDir::homePath();
  for (const auto& candidate :
       {music + QStringLiteral("/Samples"), home + QStringLiteral("/Samples"), music})
    if (!candidate.isEmpty() && QFileInfo(candidate).isDir())
      return candidate;
  return music;
}

} // namespace

BrowserPreviewController::BrowserPreviewController(SongNow song, Output output,
                                                   BufferFrames buffer_frames, Report report,
                                                   bool encoded, QObject* parent)
    : QObject(parent), song_(std::move(song)), output_(std::move(output)),
      buffer_frames_(std::move(buffer_frames)), report_(std::move(report)), encoded_(encoded),
      library_timer_(this) {
  const QSettings settings;
  auto_preview_ = settings.value(QString::fromLatin1(auto_preview_key), true).toBool();
  folder_ = settings.value(QString::fromLatin1(sample_folder_key), default_folder()).toString();
  library_timer_.setInterval(100);
  connect(&library_timer_, &QTimer::timeout, this, &BrowserPreviewController::poll_library);
  // Errors arrive on the preview worker; show them on this thread.
  preview_ = std::make_unique<SamplePreview>([this](std::string message) {
    QMetaObject::invokeMethod(
        this,
        [this, text = QString::fromStdString(message)] {
          if (!playing_key_.isEmpty()) {
            playing_key_.clear();
            emit playingChanged();
          }
          if (report_)
            report_(text);
        },
        Qt::QueuedConnection);
  });
  rescan();
}

BrowserPreviewController::~BrowserPreviewController() = default;

void BrowserPreviewController::set_auto_preview(bool on) {
  if (on == auto_preview_)
    return;
  auto_preview_ = on;
  QSettings{}.setValue(QString::fromLatin1(auto_preview_key), on);
  if (!on)
    stop();
  emit autoPreviewChanged();
}

void BrowserPreviewController::preview_sample(const QString& key) {
  if (!key.startsWith(QStringLiteral("file:")))
    return;
  const auto path = key.mid(5);
  const auto song = song_ ? song_() : std::nullopt;
  SamplePreview::Request request;
  request.file = path.toStdString();
  request.project_rate = song ? song->sample_rate : 48'000;
  if (song) {
    request.project_bpm = song->bpm;
    request.clock = song->clock;
  }
  request.buffer_frames = buffer_frames_ ? buffer_frames_() : 256;
  if (output_ && !output_(request.output_factory, request.target_serial))
    return;
  if (!preview_->play(std::move(request))) {
    if (report_)
      report_(QStringLiteral("The sample preview is busy; try again"));
    return;
  }
  playing_key_ = key;
  emit playingChanged();
}

void BrowserPreviewController::stop() {
  preview_->stop();
  if (playing_key_.isEmpty())
    return;
  playing_key_.clear();
  emit playingChanged();
}

void BrowserPreviewController::choose_folder() {
  const auto chosen =
      QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Choose your sample folder"),
                                        folder_.isEmpty() ? QDir::homePath() : folder_);
  if (!chosen.isEmpty())
    set_folder(chosen);
}

void BrowserPreviewController::set_folder(const QString& path) {
  if (path.isEmpty() || !QFileInfo(path).isDir()) {
    if (report_)
      report_(QStringLiteral("Choose a folder that exists"));
    return;
  }
  folder_ = QDir(path).absolutePath();
  QSettings{}.setValue(QString::fromLatin1(sample_folder_key), folder_);
  rescan();
}

void BrowserPreviewController::rescan() {
  samples_.clear();
  library_.scan(folder_.toStdString(), encoded_);
  library_timer_.start();
  emit samplesChanged();
}

void BrowserPreviewController::poll_library() {
  auto found = library_.take();
  if (!found)
    return;
  library_timer_.stop();
  publish(*found);
}

void BrowserPreviewController::publish(const std::vector<SampleEntry>& entries) {
  QVariantList cards;
  cards.reserve(static_cast<qsizetype>(entries.size()));
  for (const auto& entry : entries) {
    const auto path = QString::fromStdString(entry.path.string());
    const auto folder = QString::fromStdString(entry.folder);
    // The tag is the top folder: "Drums", "Loops", "Vocals"...
    const auto tag = folder.isEmpty() ? QStringLiteral("Loose") : folder.section(u'/', 0, 0);
    const auto kind = entry.bpm ? QStringLiteral("Loop · %1 BPM").arg(*entry.bpm, 0, 'g', 4)
                                : QStringLiteral("Sample");
    cards.push_back(QVariantMap{
        {QStringLiteral("key"), QStringLiteral("file:") + path},
        {QStringLiteral("engineId"), QStringLiteral("file")},
        {QStringLiteral("engineName"), QStringLiteral("Sample")},
        {QStringLiteral("presetIndex"), -1},
        {QStringLiteral("presetName"), QString::fromStdString(entry.name)},
        {QStringLiteral("tag"), tag},
        {QStringLiteral("category"), QStringLiteral("Samples")},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("color"), QStringLiteral("#d6c38f")},
        {QStringLiteral("blurb"), folder.isEmpty() ? kind : kind + QStringLiteral(" · ") + folder},
        {QStringLiteral("about"), path},
        {QStringLiteral("instrument"), false},
        {QStringLiteral("source"), QStringLiteral("file")},
        {QStringLiteral("path"), path},
        {QStringLiteral("url"), QUrl::fromLocalFile(path).toString()}});
  }
  samples_ = std::move(cards);
  emit samplesChanged();
}

} // namespace oscillate::quick
