#include "export_controller.hpp"
#include "saved_processing.hpp"
#include "vst3_helper_path.hpp"

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/application/vst3/offline_export.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#endif

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {
constexpr std::array<std::uint32_t, 5> export_rates{0, 44'100, 48'000, 88'200, 96'000};
// What each export format index writes, as the format menu names it: WAV
// first (always available), then the encoded formats the codec adds.
constexpr std::array<std::string_view, 6> export_format_names{
    "WAV 32-bit float", "WAV 24-bit", "WAV 16-bit", "FLAC 24-bit", "FLAC 16-bit", "MP3"};
constexpr std::size_t wav_format_count = 3;
constexpr int wav_formats_end = static_cast<int>(wav_format_count);
constexpr int mp3_format_index = 5;
// MP3 masters stop at this output rate.
constexpr std::uint32_t max_mp3_rate = 48'000;
// Stem folders numbered past an existing one stop here.
constexpr unsigned max_stem_folder_candidates = 20;
constexpr int max_realtime_end_bar = 1'000'000;
constexpr double hz_per_khz = 1'000.0;
// Default exports go to this folder inside the user's Music folder.
constexpr auto export_folder_name = "oscillate";
// Proposed names stop counting here; past it the last candidate is offered.
constexpr int max_default_export_number = 9'999;
// How often the UI thread looks for a finished export.
constexpr int poll_interval_ms = 50;

// Format indices 0–2 write WAV, 3–4 FLAC and 5 MP3.
[[nodiscard]] const char* export_extension_for(int format_index) {
  return format_index < wav_formats_end     ? ".wav"
         : format_index == mp3_format_index ? ".mp3"
                                            : ".flac";
}

[[nodiscard]] bool is_export_extension(const std::filesystem::path& path) {
  return path.extension() == ".wav" || path.extension() == ".flac" || path.extension() == ".mp3";
}

// A typed name or folder may start with ~ for the home folder.
[[nodiscard]] QString expand_home(const QString& text) {
  if (text == QStringLiteral("~"))
    return QDir::homePath();
  if (text.startsWith(QStringLiteral("~/")))
    return QDir::homePath() + text.mid(1);
  return text;
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
[[nodiscard]] QString output_format_name(const exporting::MasterOutputOptions& options) {
  if (std::holds_alternative<exporting::MasterRenderOptions>(options)) {
    return QStringLiteral("WAV");
  }
  return std::get<exporting::EncodedMasterOptions>(options).format ==
                 exporting::EncodedMasterFormat::mp3
             ? QStringLiteral("MP3")
             : QStringLiteral("FLAC");
}

[[nodiscard]] const std::filesystem::path&
output_target(const exporting::MasterOutputOptions& options) {
  return std::visit([](const auto& value) -> const std::filesystem::path& { return value.target; },
                    options);
}
#endif

[[nodiscard]] std::size_t vst3_instrument_source_count(const project::Snapshot& snapshot) {
  return static_cast<std::size_t>(std::ranges::count_if(snapshot.tracks, [](const auto& track) {
    return track.kind == project::TrackKind::instrument &&
           detail::vst3_instrument_source(track) != nullptr;
  }));
}

[[nodiscard]] std::size_t native_instrument_source_count(const project::Snapshot& snapshot) {
  return static_cast<std::size_t>(std::ranges::count_if(snapshot.tracks, [](const auto& track) {
    return track.kind == project::TrackKind::instrument &&
           std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source);
  }));
}

// With exactly two instrument sources, one of them VST3, stems export one
// chosen source at a time instead of every track.
[[nodiscard]] bool selected_source_stems(const project::Snapshot& snapshot) {
  const auto vst3_count = vst3_instrument_source_count(snapshot);
  return vst3_count + native_instrument_source_count(snapshot) == 2 && vst3_count > 0;
}

// Why the selected-source stem cannot export, or empty when it can. Any
// layout renders (ADR-0050); only the build can lack a VST3 host.
[[nodiscard]] QString two_instrument_source_stem_layout_reason(const project::Snapshot& snapshot) {
  if (!selected_source_stems(snapshot))
    return QStringLiteral("Select two instrument sources including a VST3 instrument");
#if !defined(OSCILLATE_HAVE_VST3) || !OSCILLATE_HAVE_VST3
  return QStringLiteral("Instrument source stems require a scanner-enabled VST3 build");
#else
  return {};
#endif
}

// True when a saved VST3 instrument or effect makes the render need a VST3
// host. A bypassed slot still names one, so it takes the VST3 path too.
[[nodiscard]] bool has_vst3_stage(const project::Snapshot& snapshot) {
  const auto vst3_effect = [](const project::EffectInsert& effect) {
    return std::holds_alternative<project::Vst3PluginInstance>(effect.processor);
  };
  return std::ranges::any_of(snapshot.master_effects, vst3_effect) ||
         std::ranges::any_of(snapshot.tracks, [&](const project::Track& track) {
           return detail::vst3_instrument_source(track) != nullptr ||
                  std::ranges::any_of(track.effects, vst3_effect);
         });
}
} // namespace

ExportController::ExportController(Host host, QObject* parent)
    : QObject(parent), host_(std::move(host)), timer_(this) {
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &ExportController::poll);
}

ExportController::~ExportController() = default;

bool ExportController::exports_selected_source_stems() const {
  return session() && selected_source_stems(session()->snapshot());
}

QStringList ExportController::export_format_labels() {
  QStringList labels;
  const auto count = encoded_export_available() ? export_format_names.size() : wav_format_count;
  for (std::size_t index = 0; index < count; ++index)
    labels.push_back(
        QString::fromLatin1(export_format_names[index].data(),
                            static_cast<qsizetype>(export_format_names[index].size())));
  return labels;
}

QStringList ExportController::export_rate_labels() {
  QStringList labels;
  for (const auto rate : export_rates)
    labels.push_back(rate == 0 ? QStringLiteral("Project rate")
                               : QStringLiteral("%1 kHz").arg(rate / hz_per_khz));
  return labels;
}

bool ExportController::two_instrument_source_stem_available() const {
  return session() && two_instrument_source_stem_layout_reason(session()->snapshot()).isEmpty();
}

QString ExportController::two_instrument_source_stem_reason() const {
  return session() ? two_instrument_source_stem_layout_reason(session()->snapshot())
                   : QStringLiteral("Open a project first");
}

void ExportController::set_export_target(const QString& target) {
  if (target != export_target_)
    export_target_is_default_ = false;
  export_target_ = target;
  emit changed();
}

QString ExportController::default_export_target() const {
  auto music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  if (music.isEmpty())
    music = QDir::homePath();
  const auto folder = std::filesystem::path(music.toStdString()) / export_folder_name;
  auto name = host_.project_name().trimmed();
  name.replace(QLatin1Char('/'), QLatin1Char('-'));
  if (name.isEmpty())
    name = QStringLiteral("Untitled");
  const auto extension = std::string(export_extension_for(export_format_index_));
  std::filesystem::path candidate;
  for (int number = 1; number <= max_default_export_number; ++number) {
    const auto numbered = number == 1 ? name : QStringLiteral("%1 %2").arg(name).arg(number);
    candidate = folder / (numbered.toStdString() + extension);
    std::error_code error;
    if (!std::filesystem::exists(candidate, error) && !error)
      break;
  }
  return QString::fromStdString(candidate.string());
}

void ExportController::reset_export_target() {
  export_target_ = default_export_target();
  export_target_is_default_ = true;
  export_saved_path_.clear();
  emit changed();
}

void ExportController::refresh_export_target() {
  if (!export_target_is_default_ || export_busy())
    return;
  const auto proposed = default_export_target();
  if (proposed != export_target_) {
    export_target_ = proposed;
    emit changed();
  }
}

QString ExportController::export_folder() const {
  return QString::fromStdString(
      std::filesystem::path(export_target_.toStdString()).parent_path().string());
}

QString ExportController::export_file_name() const {
  const auto path = std::filesystem::path(export_target_.toStdString());
  return QString::fromStdString(is_export_extension(path) ? path.stem().string()
                                                          : path.filename().string());
}

QString ExportController::export_extension() const {
  return QString::fromLatin1(export_extension_for(export_format_index_));
}

bool ExportController::export_replaces_file() const {
  // A proposed name is renumbered instead (refresh_export_target).
  std::error_code error;
  return !export_target_is_default_ && !export_target_.isEmpty() &&
         std::filesystem::exists(std::filesystem::path(export_target_.toStdString()), error);
}

void ExportController::set_export_file_name(const QString& name) {
  auto cleaned = name.trimmed();
  cleaned.replace(QLatin1Char('/'), QLatin1Char('-'));
  if (export_busy() || cleaned.isEmpty() || cleaned == export_file_name())
    return;
  const auto path = std::filesystem::path(export_target_.toStdString()).parent_path() /
                    (cleaned.toStdString() + export_extension_for(export_format_index_));
  set_export_target(QString::fromStdString(path.string()));
}

void ExportController::set_export_folder(const QString& folder) {
  const auto cleaned = expand_home(folder.trimmed());
  if (export_busy() || cleaned.isEmpty() || cleaned == export_folder())
    return;
  auto file = std::filesystem::path(export_target_.toStdString()).filename();
  if (file.empty())
    file = std::filesystem::path(default_export_target().toStdString()).filename();
  const auto path = std::filesystem::path(cleaned.toStdString()) / file;
  set_export_target(QString::fromStdString(path.string()));
}

void ExportController::set_export_trim_tail(bool trim) {
  if (trim == export_trim_tail_)
    return;
  export_trim_tail_ = trim;
  emit changed();
}

void ExportController::show_export_in_folder() {
  const auto shown = export_saved_path_.isEmpty() ? export_target_ : export_saved_path_;
  if (shown.isEmpty())
    return;
  const auto path = std::filesystem::path(shown.toStdString());
  std::error_code error;
  // A stem export publishes a folder; open it rather than its parent.
  const auto folder = std::filesystem::is_directory(path, error) ? path : path.parent_path();
  if (folder.empty())
    return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder.string())));
}

void ExportController::set_export_format_index(int index) {
  if (export_busy() || index < 0 ||
      index > (encoded_export_available() ? mp3_format_index : wav_formats_end - 1)) {
    return;
  }
  if (index == export_format_index_) {
    return;
  }
  export_format_index_ = index;
  auto path = std::filesystem::path(export_target_.toStdString());
  if (export_target_is_default_) {
    // The proposal is renumbered for the new extension's own files.
    export_target_ = default_export_target();
  } else if (is_export_extension(path)) {
    path.replace_extension(export_extension_for(index));
    export_target_ = QString::fromStdString(path.string());
  }
  emit changed();
}

bool ExportController::output_rate_conversion_available() {
  return OSCILLATE_HAVE_EXPORT_RESAMPLER != 0;
}

QVariantList ExportController::export_rate_availability() const {
  QVariantList available;
  for (int index = 0; index < static_cast<int>(export_rates.size()); ++index) {
    available.push_back(export_rate_available(index));
  }
  return available;
}

bool ExportController::export_rate_available(int index) const {
  if (index < 0 || index >= static_cast<int>(export_rates.size()) || !session()) {
    return false;
  }
  const auto project_rate = session()->snapshot().sample_rate;
  const auto rate = export_rates[static_cast<std::size_t>(index)] == 0
                        ? project_rate
                        : export_rates[static_cast<std::size_t>(index)];
  return output_rate_conversion_available() || rate == project_rate;
}

bool ExportController::master_export_rate_valid() const {
  if (!export_rate_available(export_rate_index_)) {
    return false;
  }
  const auto selected = export_rates[static_cast<std::size_t>(export_rate_index_)];
  const auto rate = selected == 0 ? session()->snapshot().sample_rate : selected;
  return export_format_index_ != mp3_format_index || rate <= max_mp3_rate;
}

QString ExportController::export_rate_status() const {
  if (!session() || master_export_rate_valid()) {
    return output_rate_conversion_available()
               ? QStringLiteral("Output rate applies to the master and WAV stems")
               : QStringLiteral("Other rates require the libsamplerate export build");
  }
  if (export_format_index_ == mp3_format_index &&
      (export_rate_index_ == 0
           ? session()->snapshot().sample_rate
           : export_rates[static_cast<std::size_t>(export_rate_index_)]) > max_mp3_rate) {
    return QStringLiteral("MP3 master supports up to 48 kHz; WAV stems can use this rate");
  }
  return QStringLiteral("This rate requires the libsamplerate export build");
}

void ExportController::set_export_rate_index(int index) {
  if (export_busy() || !export_rate_available(index) || index == export_rate_index_) {
    return;
  }
  export_rate_index_ = index;
  emit changed();
}

void ExportController::choose_export_target() {
  if (!export_enabled()) {
    return;
  }
  const auto filter = export_format_index_ < wav_formats_end ? QStringLiteral("WAV audio (*.wav)")
                      : export_format_index_ == mp3_format_index
                          ? QStringLiteral("MP3 audio (*.mp3)")
                          : QStringLiteral("FLAC audio (*.flac)");
  const auto chosen = QFileDialog::getSaveFileName(nullptr, QStringLiteral("Export master"),
                                                   export_target_, filter);
  if (!chosen.isEmpty()) {
    set_export_target(chosen);
  }
}

void ExportController::export_master() {
  const auto format_name = export_format_index_ < wav_formats_end     ? QStringLiteral("WAV")
                           : export_format_index_ == mp3_format_index ? QStringLiteral("MP3")
                                                                      : QStringLiteral("FLAC");
  if (!session()) {
    export_status_ = QStringLiteral("Open a project first");
    emit changed();
    return;
  }
  if (!export_enabled()) {
    export_status_ = QStringLiteral("Stop playback before exporting a master");
    emit changed();
    return;
  }
  // A proposed name taken since it was shown moves on to the next free one.
  refresh_export_target();
  const auto target = export_target_.trimmed();
  if (target.isEmpty()) {
    export_status_ = QStringLiteral("Choose an output file");
    emit changed();
    return;
  }
  if (!master_export_rate_valid()) {
    export_status_ = export_rate_status();
    emit changed();
    return;
  }
  if (const auto folder = std::filesystem::path(target.toStdString()).parent_path();
      !folder.empty()) {
    std::error_code folder_error;
    std::filesystem::create_directories(folder, folder_error);
    if (folder_error) {
      export_status_ = QStringLiteral("Cannot create the folder %1: %2")
                           .arg(QString::fromStdString(folder.string()))
                           .arg(QString::fromStdString(folder_error.message()));
      emit changed();
      return;
    }
  }
  constexpr std::array wav_formats{exporting::WavFormat::float32, exporting::WavFormat::pcm24,
                                   exporting::WavFormat::pcm16};
  constexpr std::array encoded_formats{exporting::EncodedMasterFormat::flac24,
                                       exporting::EncodedMasterFormat::flac16,
                                       exporting::EncodedMasterFormat::mp3};
  const auto path = std::filesystem::path(target.toStdString());
  const auto output_rate = export_rates[static_cast<std::size_t>(export_rate_index_)];
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // VST3 masters render their plugins at the project rate and share the
  // native master's format and output-rate stage.
  const auto vst3_output =
      export_format_index_ < wav_formats_end
          ? exporting::MasterOutputOptions{exporting::MasterRenderOptions{
                path, wav_formats[static_cast<std::size_t>(export_format_index_)], 0, output_rate,
                nullptr, export_trim_tail_}}
          : exporting::MasterOutputOptions{exporting::EncodedMasterOptions{
                path,
                encoded_formats[static_cast<std::size_t>(export_format_index_ - wav_formats_end)],
                0, 0.5, output_rate, export_trim_tail_}};
#endif
  if (has_vst3_stage(session()->snapshot())) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    auto prepared = session()->exports().prepare_stopped_snapshot_export();
    if (!prepared) {
      export_status_ = QStringLiteral("VST3 export failed: %1")
                           .arg(QString::fromStdString(prepared.error().message));
      emit changed();
      return;
    }
    start_vst3_master_export_task(std::move(prepared.value()), vst3_output);
#else
    export_status_ = QStringLiteral("VST3 export requires a scanner-enabled build");
    emit changed();
#endif
    return;
  }
  auto prepared =
      export_format_index_ < wav_formats_end
          ? session()->exports().prepare_wav_master_export(
                {path, wav_formats[static_cast<std::size_t>(export_format_index_)], 0, output_rate,
                 nullptr, export_trim_tail_})
          : session()->exports().prepare_encoded_master_export(
                {path,
                 encoded_formats[static_cast<std::size_t>(export_format_index_ - wav_formats_end)],
                 0, 0.5, output_rate, export_trim_tail_});
  if (!prepared) {
    export_status_ = QStringLiteral("%1 export failed: %2")
                         .arg(format_name)
                         .arg(QString::fromStdString(prepared.error().message));
    emit changed();
    return;
  }
  start_export_task(std::move(prepared.value()), format_name, path);
}

void ExportController::export_stems() {
  if (!session()) {
    export_status_ = QStringLiteral("Open a project first");
    emit changed();
    return;
  }
  if (!export_enabled()) {
    export_status_ = QStringLiteral("Stop playback before exporting stems");
    emit changed();
    return;
  }
  if (selected_source_stems(session()->snapshot())) {
    const auto reason = two_instrument_source_stem_layout_reason(session()->snapshot());
    export_status_ =
        reason.isEmpty()
            ? QStringLiteral("Choose an instrument source track to export one WAV stem at a time")
            : reason;
    emit changed();
    return;
  }
  const auto stem_base =
      host_.project_root() / ("stems-rev-" + std::to_string(session()->snapshot().revision));
  std::filesystem::path target;
  std::error_code error;
  for (unsigned index = 0; index < max_stem_folder_candidates; ++index) {
    auto candidate =
        index == 0 ? stem_base
                   : std::filesystem::path(stem_base.string() + "-" + std::to_string(index + 1));
    if (!std::filesystem::exists(candidate, error) && !error) {
      target = std::move(candidate);
      break;
    }
    if (error) {
      break;
    }
  }
  if (target.empty()) {
    export_status_ = QStringLiteral("Cannot choose a new stem folder in this project");
    emit changed();
    return;
  }
  if (has_vst3_stage(session()->snapshot())) {
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
    auto stopped = session()->exports().prepare_stopped_snapshot_export();
    if (!stopped) {
      export_status_ = QStringLiteral("VST3 stems export failed: %1")
                           .arg(QString::fromStdString(stopped.error().message));
      emit changed();
      return;
    }
    start_vst3_stems_export_task(
        std::move(stopped.value()),
        {.target_directory = target,
         .format = exporting::WavFormat::float32,
         .dither_seed = 0,
         .sample_rate = export_rates[static_cast<std::size_t>(export_rate_index_)]});
#else
    export_status_ = QStringLiteral("VST3 stems export requires a scanner-enabled build");
    emit changed();
#endif
    return;
  }
  auto prepared = session()->exports().prepare_wav_stem_export(
      {.target_directory = target,
       .format = exporting::WavFormat::float32,
       .dither_seed = 0,
       .sample_rate = export_rates[static_cast<std::size_t>(export_rate_index_)]});
  if (!prepared) {
    export_status_ = QStringLiteral("WAV stems export failed: %1")
                         .arg(QString::fromStdString(prepared.error().message));
    emit changed();
    return;
  }
  start_export_task(std::move(prepared.value()), QStringLiteral("WAV stems"), target);
}

void ExportController::export_instrument_source_stem(const QString& track_id) {
  if (!session()) {
    export_status_ = QStringLiteral("Open a project first");
    emit changed();
    return;
  }
  if (!export_enabled()) {
    export_status_ = QStringLiteral("Stop playback before exporting a stem");
    emit changed();
    return;
  }
  const auto& snapshot = session()->snapshot();
  const auto layout_reason = two_instrument_source_stem_layout_reason(snapshot);
  if (!layout_reason.isEmpty()) {
    export_status_ = layout_reason;
    emit changed();
    return;
  }
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  const auto track = parsed
                         ? std::ranges::find(snapshot.tracks, parsed.value(), &project::Track::id)
                         : snapshot.tracks.end();
  if (track == snapshot.tracks.end() ||
      (!detail::vst3_instrument_source(*track) &&
       !std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source) &&
       track->kind != project::TrackKind::audio)) {
    export_status_ = QStringLiteral("Choose a saved source track");
    emit changed();
    return;
  }
  const auto target = std::filesystem::path(export_target_.trimmed().toStdString());
  if (target.empty() || target.extension() != ".wav") {
    export_status_ = QStringLiteral("Choose a .wav output file for the selected source stem");
    emit changed();
    return;
  }
  if (!export_rate_available(export_rate_index_)) {
    export_status_ = export_rate_status();
    emit changed();
    return;
  }
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  auto prepared = session()->exports().prepare_stopped_snapshot_export();
  if (!prepared) {
    export_status_ = QStringLiteral("Source stem export failed: %1")
                         .arg(QString::fromStdString(prepared.error().message));
    emit changed();
    return;
  }
  start_vst3_stem_export_task(std::move(prepared.value()), track->id,
                              {target, exporting::WavFormat::float32, 0,
                               export_rates[static_cast<std::size_t>(export_rate_index_)]});
#else
  export_status_ =
      QStringLiteral("Instrument source stem export requires a scanner-enabled VST3 build");
  emit changed();
#endif
}

bool ExportController::realtime_export_ready() const {
  return realtime_export_available() && export_enabled() && export_format_index_ == 0 &&
         export_rate_index_ == 0;
}

void ExportController::set_realtime_export_end_bar(int bar) {
  if (export_busy() || bar < 1 || bar > max_realtime_end_bar || bar == realtime_export_end_bar_)
    return;
  realtime_export_end_bar_ = bar;
  realtime_export_end_bar_custom_ = true;
  emit changed();
}

void ExportController::cancel_realtime_export() {
  if (completion_ && completion_->realtime) {
    completion_->cancel_requested.store(true, std::memory_order_release);
    export_status_ =
        QStringLiteral("Cancelling JACK real-time export; the existing file will remain intact");
    emit changed();
  }
}

void ExportController::export_realtime_jack() {
#if defined(OSCILLATE_HAVE_JACK_EXPORT) && OSCILLATE_HAVE_JACK_EXPORT
  if (!realtime_export_ready()) {
    export_status_ = realtime_export_available()
                         ? QStringLiteral("JACK real-time export needs a stopped project, "
                                          "project-rate WAV 32-bit float and no other export")
                         : QStringLiteral("JACK real-time export is unavailable in this build");
    emit changed();
    return;
  }
  const auto target = export_target_.trimmed();
  if (target.isEmpty() || std::filesystem::path(target.toStdString()).extension() != ".wav") {
    export_status_ = QStringLiteral("Choose a .wav target for JACK real-time export");
    emit changed();
    return;
  }
  auto snapshot = session()->snapshot();
  const auto tick =
      snapshot.meter.tick_at({static_cast<std::int64_t>(realtime_export_end_bar_) + 1, 1, 0});
  const auto end = tick ? snapshot.tempo.tick_to_sample(tick.value(), snapshot.sample_rate)
                        : foundation::Result<timeline::SamplePosition>::failure(tick.error());
  if (!end || end.value().value <= 0 ||
      static_cast<std::uint64_t>(end.value().value) >= std::numeric_limits<std::uint64_t>::max()) {
    export_status_ = QStringLiteral("Cannot map the selected end bar to a positive project sample");
    emit changed();
    return;
  }
  auto reservation = session()->reserve_playback_start();
  if (!reservation) {
    export_status_ = QStringLiteral("JACK export cannot reserve the stopped project: %1")
                         .arg(QString::fromStdString(reservation.error().message));
    emit changed();
    return;
  }
  auto held_token =
      std::make_shared<std::optional<application::ControlTransportGate::PlaybackToken>>();
  held_token->emplace(std::move(reservation).value());
  const auto revision = snapshot.revision;
  const auto rate = snapshot.sample_rate;
  const auto expected = static_cast<std::uint64_t>(end.value().value);
  const auto path = std::filesystem::path(target.toStdString());
  const auto root = host_.project_root();
  const auto frames = host_.buffer_frames();
  auto completion = std::make_shared<Completion>();
  completion->revision = revision;
  completion->format_name =
      QStringLiteral("JACK real-time WAV 32F (bars 1–%1)").arg(realtime_export_end_bar_);
  completion->target = target;
  completion->realtime = true;
  try {
    worker_ = std::jthread([root, snapshot = std::move(snapshot), held_token, path, expected,
                            frames, completion, qualification = host_.live_qualification()](
                               std::stop_token stop) mutable {
      using Result = foundation::Result<exporting::PublishOutcome>;
      try {
        auto opened = RealtimeExportRun::begin(root, std::move(snapshot), std::move(**held_token),
                                               path, expected, frames, std::move(qualification));
        if (!opened) {
          completion->result.emplace(Result::failure(opened.error()));
        } else {
          auto run = std::move(opened).value();
          auto ready = run->wait_until_ready(completion->cancel_requested, stop);
          if (ready)
            completion->realtime_run = std::move(run);
          else
            completion->result.emplace(Result::failure(ready.error()));
        }
      } catch (const std::exception& error) {
        if (held_token->has_value())
          (*held_token)->finish_after_join();
        completion->result.emplace(
            Result::failure({foundation::ErrorCode::internal,
                             std::string("JACK real-time export failed: ") + error.what()}));
      } catch (...) {
        if (held_token->has_value())
          (*held_token)->finish_after_join();
        completion->result.emplace(Result::failure(
            {foundation::ErrorCode::internal, "JACK real-time export failed unexpectedly"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    (*held_token)->finish_after_join();
    export_status_ = QStringLiteral("Cannot start JACK export worker: %1").arg(error.what());
    emit changed();
    return;
  }
  completion_ = std::move(completion);
  export_status_ =
      QStringLiteral(
          "Capturing %1 at %2 Hz; sources stop after bar %3 and supported effect tails ring out")
          .arg(completion_->format_name)
          .arg(rate)
          .arg(realtime_export_end_bar_);
  emit changed();
  host_.busy_changed();
  timer_.start();
#else
  export_status_ = QStringLiteral("JACK real-time export is unavailable in this build");
  emit changed();
#endif
}

// The failure prefix, then the running status.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void ExportController::start(std::shared_ptr<Completion> completion, Job job,
                             const QString& failure_prefix, const QString& running) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  try {
    worker_ = std::jthread([job = std::move(job), completion]() mutable {
      using Result = foundation::Result<exporting::PublishOutcome>;
      try {
        completion->result.emplace(job());
      } catch (const std::exception& error) {
        completion->result.emplace(Result::failure(
            {foundation::ErrorCode::internal, std::string("Export failed: ") + error.what()}));
      } catch (...) {
        completion->result.emplace(
            Result::failure({foundation::ErrorCode::internal, "Export failed unexpectedly"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    fail(QStringLiteral("%1: %2").arg(failure_prefix).arg(error.what()));
    return;
  }
  completion_ = std::move(completion);
  export_status_ = running.isEmpty() ? QStringLiteral("Exporting %1 from project revision %2")
                                           .arg(completion_->format_name)
                                           .arg(completion_->revision)
                                     : running.arg(completion_->revision);
  emit changed();
  host_.busy_changed();
  timer_.start();
}

void ExportController::start_export_task(application::MasterExportTask task,
                                         const QString& format_name,
                                         const std::filesystem::path& target) {
  auto completion = std::make_shared<Completion>();
  completion->revision = task.revision();
  completion->format_name = format_name;
  completion->target = QString::fromStdString(target.string());
  start(std::move(completion),
        [task = std::make_shared<application::MasterExportTask>(std::move(task))] {
          return std::move(*task).run();
        },
        QStringLiteral("Cannot start export"), {});
}

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
void ExportController::start_vst3_master_export_task(application::StoppedSnapshotExportTask task,
                                                     exporting::MasterOutputOptions options) {
  auto completion = std::make_shared<Completion>();
  completion->revision = task.snapshot().revision;
  completion->format_name = QStringLiteral("VST3 %1 master").arg(output_format_name(options));
  completion->target = QString::fromStdString(output_target(options).string());
  start(std::move(completion),
        [task = std::make_shared<application::StoppedSnapshotExportTask>(std::move(task)),
         options = std::move(options), helper = detail::vst3_helper_path()] {
          plugins::Vst3Scanner scanner(helper);
          const auto& snapshot = task->snapshot();
          const auto& reader = task->reader();
          auto authority = std::move(*task).take_authority();
          return exporting::render_vst3_master(snapshot, reader, std::move(authority), scanner,
                                               options);
        },
        QStringLiteral("Cannot start VST3 export"), {});
}

void ExportController::start_vst3_stems_export_task(application::StoppedSnapshotExportTask task,
                                                    exporting::StemRenderOptions options) {
  auto completion = std::make_shared<Completion>();
  completion->revision = task.snapshot().revision;
  completion->format_name = QStringLiteral("VST3 WAV stems");
  completion->target = QString::fromStdString(options.target_directory.string());
  start(std::move(completion),
        [task = std::make_shared<application::StoppedSnapshotExportTask>(std::move(task)),
         options = std::move(options), helper = detail::vst3_helper_path()] {
          plugins::Vst3Scanner scanner(helper);
          const auto& snapshot = task->snapshot();
          const auto& reader = task->reader();
          auto authority = std::move(*task).take_authority();
          return exporting::render_vst3_stems(snapshot, reader, std::move(authority), scanner,
                                              options);
        },
        QStringLiteral("Cannot start VST3 stem export"), {});
}

void ExportController::start_vst3_stem_export_task(application::StoppedSnapshotExportTask task,
                                                   project::TrackId source_track,
                                                   exporting::MasterRenderOptions options) {
  auto completion = std::make_shared<Completion>();
  completion->revision = task.snapshot().revision;
  completion->format_name = QStringLiteral("source WAV stem");
  completion->target = QString::fromStdString(options.target.string());
  start(
      std::move(completion),
      [task = std::make_shared<application::StoppedSnapshotExportTask>(std::move(task)),
       source_track, options = std::move(options), helper = detail::vst3_helper_path()] {
        plugins::Vst3Scanner scanner(helper);
        const auto& snapshot = task->snapshot();
        const auto& reader = task->reader();
        auto authority = std::move(*task).take_authority();
        return exporting::render_vst3_stem(snapshot, reader, std::move(authority), scanner,
                                           source_track, options);
      },
      QStringLiteral("Cannot start source stem export"),
      QStringLiteral("Exporting selected source WAV stem from project revision %1"));
}
#endif

void ExportController::poll() {
  if (!completion_ || !completion_->done.load(std::memory_order_acquire)) {
    return;
  }
  timer_.stop();
  if (worker_.joinable()) {
    worker_.join();
  }
#if defined(OSCILLATE_HAVE_JACK_EXPORT) && OSCILLATE_HAVE_JACK_EXPORT
  if (completion_->realtime_run) {
    const auto current = session() ? session()->snapshot().revision : 0;
    completion_->result.emplace(completion_->realtime_run->finish_if_current(
        completion_->revision, current,
        completion_->cancel_requested.load(std::memory_order_acquire)));
    completion_->realtime_run.reset();
  }
#endif
  const auto revision = completion_->revision;
  const auto format_name = completion_->format_name;
  const auto target = completion_->target;
  if (!completion_->result) {
    export_status_ = QStringLiteral("%1 export failed unexpectedly").arg(format_name);
  } else if (!*completion_->result) {
    export_status_ = QStringLiteral("%1 export failed: %2")
                         .arg(format_name)
                         .arg(QString::fromStdString(completion_->result->error().message));
  } else if (completion_->result->value() == exporting::PublishOutcome::durability_uncertain) {
    export_status_ = QStringLiteral("%1 published from revision %2; disk durability uncertain")
                         .arg(format_name)
                         .arg(revision);
  } else {
    export_status_ = QStringLiteral("%1 exported from revision %2 to %3")
                         .arg(format_name)
                         .arg(revision)
                         .arg(target);
  }
  const bool published = completion_->result && static_cast<bool>(*completion_->result);
  export_saved_path_ = published ? target : QString{};
  completion_.reset();
  // The next proposal moves past the file just written.
  refresh_export_target();
  emit changed();
  host_.busy_changed();
  if (close_after_export_) {
    emit readyToClose();
  }
}

bool ExportController::export_enabled() const {
  return session() && !export_busy() && !close_after_export_ && host_.idle();
}

bool ExportController::realtime_export_available() noexcept {
#if defined(OSCILLATE_HAVE_JACK_EXPORT) && OSCILLATE_HAVE_JACK_EXPORT
  return true;
#else
  return false;
#endif
}

bool ExportController::encoded_export_available() noexcept { return OSCILLATE_HAVE_CODEC != 0; }

void ExportController::fail(QString message) {
  export_status_ = std::move(message);
  emit changed();
}

void ExportController::reset() {
  export_rate_index_ = 0;
  realtime_export_end_bar_custom_ = false;
  export_status_.clear();
  emit changed();
  reset_export_target();
}

void ExportController::follow_timeline_bars(int bars) {
  if (realtime_export_end_bar_custom_)
    return;
  realtime_export_end_bar_ = bars;
  emit changed();
}

void ExportController::clear_status() {
  if (export_status_.isEmpty())
    return;
  export_status_.clear();
  emit changed();
}

bool ExportController::close_when_done() {
  if (!export_busy())
    return false;
  close_after_export_ = true;
  if (completion_->realtime)
    cancel_realtime_export();
  else
    export_status_ = QStringLiteral("Master export is finishing before closing");
  emit changed();
  return true;
}

} // namespace oscillate::quick
