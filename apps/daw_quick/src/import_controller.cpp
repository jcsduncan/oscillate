#include "import_controller.hpp"

#include "oscillate/midi_file/smf.hpp"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace oscillate::quick {
namespace {
constexpr std::uint64_t bytes_per_mebibyte = std::uint64_t{1024} * 1024U;
// How often a MIDI file being read is checked for its bytes.
constexpr int midi_poll_ms = 20;

[[nodiscard]] foundation::Result<QByteArray> too_large() {
  return foundation::Result<QByteArray>::failure(
      {foundation::ErrorCode::resource_exhausted,
       QStringLiteral("the file is larger than %1 MiB")
           .arg(midi_file::max_file_bytes / bytes_per_mebibyte)
           .toStdString()});
}

// Worker: the file's bytes, refused past the size limit.
[[nodiscard]] foundation::Result<QByteArray> read_midi_file(const QString& path) {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly))
    return foundation::Result<QByteArray>::failure(
        {foundation::ErrorCode::io_failure,
         QStringLiteral("cannot open %1").arg(input.fileName()).toStdString()});
  if (input.size() > static_cast<qint64>(midi_file::max_file_bytes))
    return too_large();
  // Read at most one byte past the limit, so a file that grew since the size
  // check is still refused rather than read whole.
  auto bytes = input.read(static_cast<qint64>(midi_file::max_file_bytes) + 1);
  if (bytes.size() > static_cast<qsizetype>(midi_file::max_file_bytes))
    return too_large();
  return foundation::Result<QByteArray>::success(std::move(bytes));
}
} // namespace

ImportController::ImportController(Host host, QObject* parent)
    : QObject(parent), host_(std::move(host)), audio_(this), midi_timer_(this) {
  connect(&audio_, &AudioImportController::changed, this, &ImportController::changed);
  midi_timer_.setInterval(midi_poll_ms);
  connect(&midi_timer_, &QTimer::timeout, this, &ImportController::poll_midi);
}

bool ImportController::import_enabled() const {
  return host_.session() && !import_busy() && !audio_.closing() && host_.idle();
}

bool ImportController::encoded_import_available() noexcept { return OSCILLATE_HAVE_CODEC != 0; }

void ImportController::choose_import_audio() { choose_import_audio_at(std::nullopt); }

void ImportController::choose_import_audio_above_placeholder() {
  choose_import_audio_at(std::nullopt);
}

void ImportController::choose_import_audio_at(std::optional<std::size_t> insert_index) {
  if (!import_enabled())
    return;
  const auto filter =
      encoded_import_available()
          ? QStringLiteral("Audio (*.wav *.wave *.aiff *.aif *.flac *.mp3);;All files (*)")
          : QStringLiteral("WAV audio (*.wav *.wave);;All files (*)");
  const auto chosen = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Import audio"),
                                                   QDir::homePath(), filter);
  if (!chosen.isEmpty())
    import_audio_at(QUrl::fromLocalFile(chosen), insert_index);
}

void ImportController::import_audio(const QUrl& source) { import_audio_at(source, std::nullopt); }

void ImportController::import_audio_at(const QUrl& source,
                                       std::optional<std::size_t> insert_index) {
  if (!host_.session()) {
    audio_.reject(QStringLiteral("Open a project first"));
  } else if (!import_enabled()) {
    audio_.reject(QStringLiteral("Stop playback or finish export/import before importing"));
  } else if (audio_.start(source, insert_index)) {
    host_.started();
  }
  host_.status(audio_.status());
}

void ImportController::choose_import_midi() {
  if (!import_enabled())
    return;
  const auto chosen = QFileDialog::getOpenFileName(
      nullptr, QStringLiteral("Import MIDI"), QDir::homePath(),
      QStringLiteral("MIDI files (*.mid *.midi *.smf);;All files (*)"));
  if (!chosen.isEmpty())
    import_midi(QUrl::fromLocalFile(chosen));
}

// A Standard MIDI File becomes new instrument tracks at bar 1, one undo step.
void ImportController::import_midi(const QUrl& source) { import_midi_with(source, {}); }

void ImportController::import_midi_to_track(const QUrl& source, const QString& track_id,
                                            int zero_based_bar) {
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed || zero_based_bar < 0) {
    host_.status(QStringLiteral("MIDI import: drop on an instrument track"));
    return;
  }
  import_midi_with(source, {.target = application::MidiImportOptions::Target::track,
                            .track = parsed.value(),
                            .zero_based_bar = zero_based_bar});
}

// The file's tempo and meter are taken only while the project has no clips,
// so a file dropped into a song never retimes it.
void ImportController::import_midi_with(const QUrl& source,
                                        application::MidiImportOptions options) {
  if (!host_.session()) {
    host_.status(QStringLiteral("Open a project first"));
    return;
  }
  if (!import_enabled() || host_.control_busy()) {
    host_.status(QStringLiteral("Stop playback or finish export/import before importing"));
    return;
  }
  const auto path = source.toLocalFile();
  const auto failed = midi_read_.start(
      0, [path] { return read_midi_file(path); },
      [](std::string what) {
        return foundation::Result<QByteArray>::failure(
            {foundation::ErrorCode::internal, std::move(what)});
      });
  if (failed) {
    host_.status(QStringLiteral("MIDI import: %1").arg(QString::fromStdString(*failed)));
    return;
  }
  midi_ = MidiRead{path, options, host_.session()->session_generation()};
  midi_timer_.start();
  host_.started();
  host_.status(QStringLiteral("Reading %1").arg(QFileInfo(path).fileName()));
  emit changed();
}

void ImportController::poll_midi() {
  if (!midi_read_.finished())
    return;
  midi_timer_.stop();
  auto finished = midi_read_.take();
  if (!midi_)
    return;
  const auto read = std::move(*midi_);
  midi_.reset();
  emit changed();
  auto options = read.options;
  if (!finished) {
    host_.status(QStringLiteral("MIDI import: the file could not be read"));
    return;
  }
  const auto& bytes = finished->result;
  if (!bytes) {
    host_.status(
        QStringLiteral("MIDI import: %1").arg(QString::fromStdString(bytes.error().message)));
    return;
  }
  // The notes go to the session they were dropped into, and only when no
  // other job holds the project.
  if (!host_.session() || host_.session()->session_generation() != read.session) {
    host_.status(QStringLiteral("MIDI import dropped: the project changed"));
    return;
  }
  if (!import_enabled() || host_.control_busy()) {
    host_.status(QStringLiteral("MIDI import: the project became busy; import the file again"));
    return;
  }
  const auto& tracks = host_.session()->snapshot().tracks;
  options.import_tempo_and_meter = std::ranges::all_of(
      tracks, [](const auto& track) { return track.clips.empty() && track.midi_clips.empty(); });
  const bool new_tracks = options.target == application::MidiImportOptions::Target::new_tracks;
  const auto result = host_.session()->imports().import_midi_bytes(
      std::span(reinterpret_cast<const std::uint8_t*>(bytes.value().constData()),
                static_cast<std::size_t>(bytes.value().size())),
      options);
  if (!result) {
    host_.status(
        QStringLiteral("MIDI import: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  const auto& imported = result.value();
  host_.refresh();
  if (new_tracks && !imported.tracks.empty())
    host_.track_created(static_cast<int>(
        host_.session()->tracks().track_index(imported.tracks.front()).value_or(0)));
  host_.submit_playback_edit();
  const auto name = QFileInfo(read.path).completeBaseName();
  host_.status(new_tracks ? QStringLiteral("%1 imported: %2 notes on %3 tracks · revision %4")
                                .arg(name)
                                .arg(imported.notes)
                                .arg(imported.tracks.size())
                                .arg(host_.revision())
                          : QStringLiteral("%1 imported: %2 notes · revision %3")
                                .arg(name)
                                .arg(imported.notes)
                                .arg(host_.revision()));
}

} // namespace oscillate::quick
