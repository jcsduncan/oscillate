#include "note_tools_controller.hpp"

#include "oscillate/application/midi_pedal_edit.hpp"
#include "oscillate/application/midi_velocity_edit.hpp"
#include "oscillate/persistence/groove_file.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::quick {
namespace {
// MIDI velocities; a project without a stored choice draws at the default.
constexpr int min_velocity = 1;
constexpr int max_velocity = 127;
constexpr int default_new_note_velocity = 100;
// A groove's name, which is also its file's name.
constexpr int max_groove_name_length = 64;
// How often groove file work is checked for its result.
constexpr int groove_poll_ms = 20;

[[nodiscard]] std::optional<project::MidiPedal> parse_pedal(const QString& name) noexcept {
  if (name == QStringLiteral("sustain"))
    return project::MidiPedal::sustain;
  if (name == QStringLiteral("sostenuto"))
    return project::MidiPedal::sostenuto;
  if (name == QStringLiteral("soft"))
    return project::MidiPedal::soft;
  return std::nullopt;
}

[[nodiscard]] bool finite_tick(double ticks) noexcept {
  return std::isfinite(ticks) && ticks >= 0.0 &&
         ticks < static_cast<double>(std::numeric_limits<std::int64_t>::max());
}

struct ShapeName {
  const char* name;
  application::ChordShape shape;
};
constexpr std::array<ShapeName, 5> shape_names{{{"Triad", application::ChordShape::triad},
                                                {"7th", application::ChordShape::seventh},
                                                {"Sus2", application::ChordShape::sus2},
                                                {"Sus4", application::ChordShape::sus4},
                                                {"Add9", application::ChordShape::add9}}};

[[nodiscard]] std::optional<application::ChordShape> parse_shape(const QString& text) {
  for (const auto& entry : shape_names) {
    if (text.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0)
      return entry.shape;
  }
  return std::nullopt;
}

[[nodiscard]] bool usable_name(const QString& name) {
  if (name.isEmpty() || name.size() > max_groove_name_length || name.startsWith(QLatin1Char('.')))
    return false;
  return std::ranges::none_of(name, [](QChar c) {
    return c == QLatin1Char('/') || c == QLatin1Char('\\') || !c.isPrint();
  });
}

[[nodiscard]] QString local_path(const QString& path) {
  const QUrl url(path);
  return url.isLocalFile() ? url.toLocalFile() : path;
}

[[nodiscard]] std::optional<persistence::GrooveTemplate> read_groove(const QString& path) {
  QFile file(path);
  if (file.size() > static_cast<qint64>(persistence::max_groove_file_bytes) ||
      !file.open(QIODevice::ReadOnly))
    return std::nullopt;
  // At most one byte past the limit: a file that grew since the size check
  // is refused rather than read whole.
  const auto bytes = file.read(static_cast<qint64>(persistence::max_groove_file_bytes) + 1);
  if (bytes.size() > static_cast<qsizetype>(persistence::max_groove_file_bytes))
    return std::nullopt;
  auto groove = persistence::decode_groove(
      std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
  if (!groove)
    return std::nullopt;
  return std::move(groove).value();
}

// Worker: the user's grooves in `directory`, each named by its file.
[[nodiscard]] std::vector<persistence::GrooveTemplate> scan_grooves(const QString& directory) {
  std::vector<persistence::GrooveTemplate> grooves;
  const auto files = QDir(directory).entryInfoList(
      {QStringLiteral("*.json")}, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
  for (const auto& file : files) {
    if (!usable_name(file.completeBaseName()))
      continue;
    if (auto groove = read_groove(file.absoluteFilePath())) {
      groove->name = file.completeBaseName().toStdString();
      grooves.push_back(std::move(*groove));
    }
  }
  return grooves;
}

[[nodiscard]] bool write_groove(const QString& path, const persistence::GrooveTemplate& groove) {
  const auto text = persistence::encode_groove(groove);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(text.data(), static_cast<qint64>(text.size()));
  return file.commit();
}

[[nodiscard]] QVariantMap groove_entry(const persistence::GrooveTemplate& groove, bool user) {
  QVariantList timings;
  QVariantList velocities;
  for (const auto& step : groove.steps) {
    timings.append(step.timing);
    velocities.append(step.velocity);
  }
  return {{QStringLiteral("name"), QString::fromStdString(groove.name)},
          {QStringLiteral("user"), user},
          {QStringLiteral("stepsPerQuarter"), static_cast<int>(groove.steps_per_quarter)},
          {QStringLiteral("timings"), timings},
          {QStringLiteral("velocities"), velocities}};
}

} // namespace

NoteToolsController::NoteToolsController(Session session, CanEdit can_edit, Report report,
                                         Commit commit, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)), files_timer_(this) {
  files_timer_.setInterval(groove_poll_ms);
  connect(&files_timer_, &QTimer::timeout, this, &NoteToolsController::poll_files);
  rescan_grooves();
}

void NoteToolsController::set_scale_lock(bool locked) {
  if (scale_lock_ == locked)
    return;
  scale_lock_ = locked;
  emit scaleLockChanged();
}

QStringList NoteToolsController::chord_shapes() {
  QStringList names;
  for (const auto& entry : shape_names)
    names.append(QLatin1String(entry.name));
  return names;
}

double NoteToolsController::bar_ticks() {
  return static_cast<double>(application::midi_grid_bar_ticks);
}

QList<int> NoteToolsController::grid_divisions() {
  return {application::midi_grid_steps_per_bar.begin(), application::midi_grid_steps_per_bar.end()};
}

std::optional<project::MusicalKey> NoteToolsController::song_key() const {
  auto* session = session_();
  return session ? session->snapshot().key : std::nullopt;
}

int NoteToolsController::lock_key(int key) const {
  const auto scale = song_key();
  if (!scale_lock_ || !scale)
    return std::clamp(key, 0, 127);
  return application::snap_to_scale(key, *scale);
}

int NoteToolsController::moved_key_in_scale(int key, int from_key, int to_key) const {
  const auto scale = song_key();
  if (!scale)
    return std::clamp(key + to_key - from_key, 0, 127);
  return application::transpose_in_scale(
      key, application::scale_degrees_between(from_key, to_key, *scale), *scale);
}

QVariantList NoteToolsController::chord_preview(int root, const QString& shape,
                                                int inversion) const {
  QVariantList keys;
  const auto parsed = parse_shape(shape);
  if (!parsed)
    return keys;
  for (const auto key : application::chord_keys(root, *parsed, inversion, song_key()))
    keys.append(static_cast<int>(key));
  return keys;
}

std::optional<NoteToolsController::Target> NoteToolsController::target(const QString& clip_id,
                                                                       const QStringList& note_ids,
                                                                       const QString& action) {
  auto* session = session_();
  if (session == nullptr || !can_edit_()) {
    report_(QStringLiteral("Finish the current project operation before %1").arg(action));
    return std::nullopt;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const project::MidiClip* found = nullptr;
  if (clip) {
    for (const auto& track : session->snapshot().tracks) {
      const auto match = std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id);
      if (match != track.midi_clips.end())
        found = &*match;
    }
  }
  if (found == nullptr) {
    report_(QStringLiteral("Select a MIDI clip"));
    return std::nullopt;
  }
  Target result{session, found, {}};
  for (const auto& text : note_ids) {
    const auto id = project::NoteId::parse(text.toStdString());
    if (!id) {
      report_(QStringLiteral("Select MIDI notes"));
      return std::nullopt;
    }
    result.scope.push_back(id.value());
  }
  // No selection: the whole clip.
  if (result.scope.empty()) {
    for (const auto& note : found->notes)
      result.scope.push_back(note.id);
  }
  return result;
}

bool NoteToolsController::commit(application::ProjectSession& session, project::ClipId clip,
                                 std::span<const project::MidiNote> changed,
                                 std::span<const project::MidiNote> added, const QString& action,
                                 const QString& done, std::vector<project::NoteId>* created) {
  if (changed.empty() && added.empty()) {
    report_(QStringLiteral("%1: nothing to change").arg(action));
    return false;
  }
  const auto before = session.snapshot().revision;
  auto result = session.notes().apply_note_edit(clip, changed, added);
  if (!result) {
    report_(QStringLiteral("%1: %2").arg(action, QString::fromStdString(result.error().message)));
    return false;
  }
  if (created != nullptr)
    *created = std::move(result).value();
  if (session.snapshot().revision != before)
    commit_();
  report_(done);
  return true;
}

QStringList NoteToolsController::stamp_chord(const QString& clip_id, int step, int root,
                                             int steps_per_bar, const QString& shape, int inversion,
                                             int length_steps) {
  const auto parsed = parse_shape(shape);
  auto found = target(clip_id, {}, QStringLiteral("drawing a chord"));
  if (!found || !parsed || !application::supported_midi_grid(steps_per_bar) || step < 0 ||
      length_steps < 1)
    return {};
  const auto grid = application::midi_grid_ticks(steps_per_bar);
  const auto start = static_cast<std::int64_t>(step) * grid;
  const auto pattern = found->clip->pattern_length.value;
  if (start >= pattern)
    return {};
  const auto end = std::min(start + static_cast<std::int64_t>(length_steps) * grid, pattern);
  std::vector<project::MidiNote> added;
  // The session gives each new note its own id; this one only fills the slot.
  const auto placeholder = project::NoteId::from_bits({0, 1}).value();
  for (const auto key : application::chord_keys(root, *parsed, inversion, song_key())) {
    // A tone already drawn there stays as it is.
    if (std::ranges::any_of(found->clip->notes, [&](const project::MidiNote& note) {
          return note.start.value == start && note.key == key;
        }))
      continue;
    added.push_back(
        {placeholder, {start}, {end}, key, found->session->notes().new_note_velocity()});
  }
  std::vector<project::NoteId> created;
  if (!commit(*found->session, found->clip->id, {}, added, QStringLiteral("Chord"),
              QStringLiteral("%1 chord drawn · one undo step").arg(shape), &created))
    return {};
  QStringList ids;
  for (const auto& id : created)
    ids.append(QString::fromStdString(id.to_string()));
  return ids;
}

void NoteToolsController::move_notes_in_scale(const QString& clip_id, const QStringList& note_ids,
                                              int step_delta, int from_key, int to_key,
                                              int steps_per_bar) {
  auto found = target(clip_id, note_ids, QStringLiteral("moving notes"));
  const auto scale = song_key();
  if (!found || !application::supported_midi_grid(steps_per_bar) || note_ids.isEmpty())
    return;
  const auto delta =
      static_cast<std::int64_t>(step_delta) * application::midi_grid_ticks(steps_per_bar);
  const auto degrees = scale ? application::scale_degrees_between(from_key, to_key, *scale) : 0;
  const auto pattern = found->clip->pattern_length.value;
  std::vector<project::MidiNote> changed;
  for (const auto& note : found->clip->notes) {
    if (std::ranges::find(found->scope, note.id) == found->scope.end())
      continue;
    auto moved = note;
    const auto duration = note.end.value - note.start.value;
    const auto latest = note.end.value <= pattern ? pattern - duration : pattern - 1;
    moved.start = {note.start.value + delta};
    moved.end = {moved.start.value + duration};
    if (moved.start.value < 0 || moved.start.value > latest) {
      report_(QStringLiteral("MIDI notes not moved: they would leave the clip"));
      return;
    }
    moved.key = scale ? application::transpose_in_scale(note.key, degrees, *scale)
                      : static_cast<std::uint8_t>(std::clamp(note.key + to_key - from_key, 0, 127));
    changed.push_back(moved);
  }
  (void)commit(*found->session, found->clip->id, changed, {}, QStringLiteral("Move"),
               QStringLiteral("Moved in key"));
}

void NoteToolsController::strum(const QString& clip_id, const QStringList& note_ids, int amount,
                                bool upward) {
  auto found = target(clip_id, note_ids, QStringLiteral("strumming"));
  if (!found)
    return;
  const auto step =
      static_cast<std::int64_t>(std::clamp(amount, 1, 16)) * (timeline::ticks_per_quarter / 32);
  const auto changed = application::strum_notes(found->clip->notes, found->scope, step, upward,
                                                found->clip->pattern_length.value);
  if (changed.empty()) {
    report_(QStringLiteral("Strum needs notes that start together"));
    return;
  }
  (void)commit(*found->session, found->clip->id, changed, {}, QStringLiteral("Strum"),
               upward ? QStringLiteral("Strummed up · one undo step")
                      : QStringLiteral("Strummed down · one undo step"));
}

void NoteToolsController::humanize(const QString& clip_id, const QStringList& note_ids,
                                   double timing, double velocity) {
  auto found = target(clip_id, note_ids, QStringLiteral("humanising"));
  if (!found)
    return;
  const auto seed = ++seed_;
  const auto max_ticks = static_cast<std::int64_t>(
      std::clamp(timing, 0.0, 1.0) * static_cast<double>(timeline::ticks_per_quarter / 8));
  auto changed = application::humanize_timing(found->clip->notes, found->scope, max_ticks, seed,
                                              found->clip->pattern_length.value);
  if (velocity > 0.0) {
    for (const auto& [id, value] : application::humanize_velocities(
             found->clip->notes, found->scope, std::clamp(velocity, 0.0, 1.0), seed ^ 0xA5A5ULL)) {
      const auto moved = std::ranges::find(changed, id, &project::MidiNote::id);
      if (moved != changed.end()) {
        moved->velocity = value;
        continue;
      }
      const auto note = std::ranges::find(found->clip->notes, id, &project::MidiNote::id);
      auto edited = *note;
      edited.velocity = value;
      changed.push_back(edited);
    }
  }
  (void)commit(*found->session, found->clip->id, changed, {}, QStringLiteral("Humanise"),
               QStringLiteral("Humanised · click again for another variation"));
}

void NoteToolsController::quantize(const QString& clip_id, const QStringList& note_ids,
                                   int steps_per_bar, double strength, double swing, bool ends) {
  auto found = target(clip_id, note_ids, QStringLiteral("quantising"));
  if (!found || !application::supported_midi_grid(steps_per_bar))
    return;
  const application::QuantizeSettings settings{application::midi_grid_ticks(steps_per_bar),
                                               strength, swing, ends};
  auto changed = application::quantize_notes(found->clip->notes, found->scope, settings,
                                             application::midi_clip_start_tick(*found->clip),
                                             found->clip->pattern_length.value);
  if (!changed) {
    report_(QStringLiteral("Quantise: %1").arg(QString::fromStdString(changed.error().message)));
    return;
  }
  if (changed.value().empty()) {
    report_(QStringLiteral("Already on the grid"));
    return;
  }
  (void)commit(*found->session, found->clip->id, changed.value(), {}, QStringLiteral("Quantise"),
               QStringLiteral("Quantised to 1/%1 at %2% · one undo step")
                   .arg(steps_per_bar)
                   .arg(std::lround(std::clamp(strength, 0.0, 1.0) * 100.0)));
}

QString NoteToolsController::groove_directory() const {
  QString data = qEnvironmentVariable("XDG_DATA_HOME");
  if (data.isEmpty())
    data = QDir::homePath() + QStringLiteral("/.local/share");
  return data + QStringLiteral("/oscillate/grooves");
}

QVariantList NoteToolsController::grooves() const {
  QVariantList list;
  for (const auto& groove : application::builtin_grooves())
    list.append(groove_entry(groove, false));
  for (const auto& groove : user_grooves_)
    list.append(groove_entry(groove, true));
  return list;
}

std::optional<persistence::GrooveTemplate>
NoteToolsController::find_groove(const QString& name) const {
  const auto wanted = name.toStdString();
  for (const auto& groove : user_grooves_) {
    if (groove.name == wanted)
      return groove;
  }
  for (auto& groove : application::builtin_grooves()) {
    if (groove.name == wanted)
      return std::move(groove);
  }
  return std::nullopt;
}

void NoteToolsController::apply_groove(const QString& clip_id, const QStringList& note_ids,
                                       const QString& name, double quantize, double timing,
                                       double velocity) {
  auto found = target(clip_id, note_ids, QStringLiteral("applying a groove"));
  if (!found)
    return;
  const auto groove = find_groove(name);
  if (!groove) {
    report_(QStringLiteral("Groove not found: %1").arg(name));
    return;
  }
  auto changed = application::apply_groove(
      found->clip->notes, found->scope, *groove, {quantize, timing, velocity},
      application::midi_clip_start_tick(*found->clip), found->clip->pattern_length.value);
  if (!changed) {
    report_(QStringLiteral("Groove: %1").arg(QString::fromStdString(changed.error().message)));
    return;
  }
  if (changed.value().empty()) {
    report_(QStringLiteral("The notes already sit in this groove"));
    return;
  }
  (void)commit(*found->session, found->clip->id, changed.value(), {}, QStringLiteral("Groove"),
               QStringLiteral("%1 applied · one undo step").arg(name));
}

void NoteToolsController::apply_current_groove(const QString& clip_id,
                                               const QStringList& note_ids) {
  apply_groove(clip_id, note_ids, current_groove_, groove_quantize_, groove_timing_,
               groove_velocity_);
}

bool NoteToolsController::keep_groove(persistence::GrooveTemplate groove) {
  // A record's or a clip's name may carry characters a file name cannot.
  auto cleaned = QString::fromStdString(groove.name);
  cleaned.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('\\'), QLatin1Char('-'));
  const auto trimmed = cleaned.trimmed().left(max_groove_name_length);
  if (!usable_name(trimmed)) {
    report_(QStringLiteral("Give the groove a name"));
    return false;
  }
  groove.name = trimmed.toStdString();
  queue([directory = groove_directory(), trimmed, groove = std::move(groove)] {
    FileOutcome outcome{.kind = FileOutcome::Kind::saved, .name = trimmed};
    outcome.ok =
        QDir().mkpath(directory) &&
        write_groove(directory + QLatin1Char('/') + trimmed + QStringLiteral(".json"), groove);
    if (outcome.ok)
      outcome.grooves.push_back(groove);
    return outcome;
  });
  current_groove_ = trimmed;
  emit grooveSettingsChanged();
  return true;
}

bool NoteToolsController::delete_groove(const QString& name) {
  const auto wanted = name.toStdString();
  if (!usable_name(name) || std::ranges::none_of(user_grooves_, [&](const auto& groove) {
        return groove.name == wanted;
      }))
    return false;
  queue([directory = groove_directory(), name] {
    return FileOutcome{
        .kind = FileOutcome::Kind::removed,
        .ok = QFile::remove(directory + QLatin1Char('/') + name + QStringLiteral(".json")),
        .name = name};
  });
  return true;
}

bool NoteToolsController::import_groove(const QString& path) {
  const auto file = local_path(path);
  const auto name = QFileInfo(file).completeBaseName().trimmed();
  if (!usable_name(name)) {
    report_(QStringLiteral("Not a groove file"));
    return false;
  }
  queue([directory = groove_directory(), file, name] {
    FileOutcome outcome{.kind = FileOutcome::Kind::imported, .name = name};
    auto groove = read_groove(file);
    if (!groove)
      return outcome;
    groove->name = name.toStdString();
    outcome.ok =
        QDir().mkpath(directory) &&
        write_groove(directory + QLatin1Char('/') + name + QStringLiteral(".json"), *groove);
    if (outcome.ok)
      outcome.grooves.push_back(std::move(*groove));
    return outcome;
  });
  return true;
}

bool NoteToolsController::export_groove(const QString& name, const QString& path) {
  auto groove = find_groove(name);
  if (!groove)
    return false;
  queue([path = local_path(path), name, groove = std::move(*groove)] {
    return FileOutcome{
        .kind = FileOutcome::Kind::exported, .ok = write_groove(path, groove), .name = name};
  });
  return true;
}

void NoteToolsController::rescan_grooves() {
  queue([directory = groove_directory()] {
    return FileOutcome{
        .kind = FileOutcome::Kind::scan, .ok = true, .grooves = scan_grooves(directory)};
  });
}

void NoteToolsController::queue(FileWork work) {
  waiting_.push_back(std::move(work));
  start_file_work();
}

void NoteToolsController::start_file_work() {
  if (files_.running() || waiting_.empty())
    return;
  auto work = std::move(waiting_.front());
  waiting_.pop_front();
  const auto failed =
      files_.start(0, std::move(work), [](const std::string& error) {
        return FileOutcome{.kind = FileOutcome::Kind::failed,
                           .ok = false,
                           .name = QString::fromStdString(error)};
      });
  if (failed) {
    report_(QStringLiteral("Groove files: %1").arg(QString::fromStdString(*failed)));
    start_file_work();
    return;
  }
  files_timer_.start();
}

void NoteToolsController::poll_files() {
  if (!files_.finished())
    return;
  auto finished = files_.take();
  if (finished)
    finish(std::move(finished->result));
  if (!files_.running() && waiting_.empty())
    files_timer_.stop();
  start_file_work();
}

void NoteToolsController::finish(FileOutcome outcome) {
  using Kind = FileOutcome::Kind;
  const auto replace = [&](persistence::GrooveTemplate groove) {
    std::erase_if(user_grooves_, [&](const auto& kept) { return kept.name == groove.name; });
    user_grooves_.push_back(std::move(groove));
    std::ranges::sort(user_grooves_, [](const auto& a, const auto& b) {
      return QString::fromStdString(a.name).compare(QString::fromStdString(b.name),
                                                    Qt::CaseInsensitive) < 0;
    });
  };
  switch (outcome.kind) {
  case Kind::failed:
    // A worker that threw never changes the cached grooves.
    report_(QStringLiteral("Groove files: %1").arg(outcome.name));
    return;
  case Kind::scan:
    if (!outcome.ok)
      return;
    user_grooves_ = std::move(outcome.grooves);
    break;
  case Kind::saved:
  case Kind::imported:
    if (!outcome.ok || outcome.grooves.empty()) {
      report_(outcome.kind == Kind::saved
                  ? QStringLiteral("Groove: could not save %1").arg(outcome.name)
                  : QStringLiteral("Not a groove file"));
      return;
    }
    replace(std::move(outcome.grooves.front()));
    break;
  case Kind::removed:
    if (!outcome.ok) {
      report_(QStringLiteral("Groove: could not delete %1").arg(outcome.name));
      return;
    }
    std::erase_if(user_grooves_, [&](const auto& groove) {
      return QString::fromStdString(groove.name) == outcome.name;
    });
    break;
  case Kind::exported:
    report_(outcome.ok ? QStringLiteral("Groove %1 exported").arg(outcome.name)
                       : QStringLiteral("Groove: could not export %1").arg(outcome.name));
    return;
  }
  emit groovesChanged();
}

bool NoteToolsController::wait_for_groove_files(int timeout_ms) {
  QElapsedTimer waited;
  waited.start();
  while (groove_files_busy() && waited.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, groove_poll_ms);
    poll_files();
  }
  return !groove_files_busy();
}

bool NoteToolsController::apply_pedal_edit(const QString& clip_id, const QString& pedal_name,
                                           const PedalEdit& edit) {
  if (session_() == nullptr || !can_edit_()) {
    report_(QStringLiteral("Finish the current project operation before editing pedals"));
    return false;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  const auto pedal = parse_pedal(pedal_name);
  if (!clip || !pedal) {
    report_(QStringLiteral("Select a MIDI clip and a pedal"));
    return false;
  }
  const auto before = session_()->snapshot().revision;
  const auto result = edit(clip.value(), *pedal);
  if (!result) {
    report_(QStringLiteral("Pedal: %1").arg(QString::fromStdString(result.error().message)));
    return false;
  }
  if (session_()->snapshot().revision != before) {
    commit_();
  }
  return true;
}

void NoteToolsController::paint_pedal_grid(const QString& clip_id, const QString& pedal,
                                           int from_step, int to_step, double depth,
                                           int steps_per_bar) {
  // Steps count from the clip's start; the session keeps edits in the pattern.
  if (!application::supported_midi_grid(steps_per_bar) || from_step < 0 || to_step <= from_step) {
    report_(QStringLiteral("Drag across the pedal strip to press the pedal"));
    return;
  }
  const auto step = application::midi_grid_ticks(steps_per_bar);
  if (apply_pedal_edit(clip_id, pedal, [&](project::ClipId clip, project::MidiPedal which) {
        return session_()->pedals().paint_pedal_span(clip, which, {from_step * step},
                                                     {to_step * step}, depth);
      }))
    report_(QStringLiteral("Pedal pressed · Undo to remove"));
}

void NoteToolsController::move_pedal_span(const QString& clip_id, const QString& pedal,
                                          double at_ticks, double start_ticks, double end_ticks) {
  if (!finite_tick(at_ticks) || !finite_tick(start_ticks) || !finite_tick(end_ticks)) {
    report_(QStringLiteral("Select a pedal span"));
    return;
  }
  (void)apply_pedal_edit(clip_id, pedal, [&](project::ClipId clip, project::MidiPedal which) {
    return session_()->pedals().move_pedal_span(clip, which, {static_cast<std::int64_t>(at_ticks)},
                                                {static_cast<std::int64_t>(start_ticks)},
                                                {static_cast<std::int64_t>(end_ticks)});
  });
}

void NoteToolsController::set_pedal_depth(const QString& clip_id, const QString& pedal,
                                          double at_ticks, double depth) {
  if (!finite_tick(at_ticks) || !std::isfinite(depth) || depth <= 0.0 || depth > 1.0) {
    report_(QStringLiteral("Select a pedal span and a depth"));
    return;
  }
  (void)apply_pedal_edit(clip_id, pedal, [&](project::ClipId clip, project::MidiPedal which) {
    const auto at = static_cast<std::int64_t>(at_ticks);
    for (const auto& track : session_()->snapshot().tracks) {
      const auto found = std::ranges::find(track.midi_clips, clip, &project::MidiClip::id);
      if (found == track.midi_clips.end())
        continue;
      const auto lane = std::ranges::find(found->pedals, which, &project::MidiPedalLane::pedal);
      if (lane == found->pedals.end())
        break;
      for (const auto& span : application::pedal_spans(lane->points, found->pattern_length.value)) {
        if (at >= span.start && at < span.end)
          return session_()->pedals().paint_pedal_span(clip, which, {span.start}, {span.end},
                                                       depth);
      }
      break;
    }
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::not_found, "No pedal span there"});
  });
}

void NoteToolsController::remove_pedal_at(const QString& clip_id, const QString& pedal,
                                          double at_ticks) {
  if (!finite_tick(at_ticks)) {
    report_(QStringLiteral("Select a pedal span"));
    return;
  }
  if (apply_pedal_edit(clip_id, pedal, [&](project::ClipId clip, project::MidiPedal which) {
        return session_()->pedals().remove_pedal_span(clip, which,
                                                      {static_cast<std::int64_t>(at_ticks)});
      }))
    report_(QStringLiteral("Pedal lifted · Undo to restore"));
}

void NoteToolsController::pedal_per_chord(const QString& clip_id) {
  if (apply_pedal_edit(clip_id, QStringLiteral("sustain"),
                       [&](project::ClipId clip, project::MidiPedal) {
                         return session_()->pedals().pedal_per_chord(clip);
                       }))
    report_(QStringLiteral("Pedalled per chord · Undo to restore"));
}

void NoteToolsController::clear_pedals(const QString& clip_id) {
  if (apply_pedal_edit(clip_id, QStringLiteral("sustain"),
                       [&](project::ClipId clip, project::MidiPedal) {
                         return session_()->pedals().clear_midi_pedals(clip);
                       }))
    report_(QStringLiteral("Pedals cleared · Undo to restore"));
}

bool NoteToolsController::apply_velocity_edit(const QString& clip_id, const QStringList& note_ids,
                                              const VelocityEdit& edit) {
  if (session_() == nullptr || !can_edit_()) {
    report_(QStringLiteral("Finish the current project operation before editing velocity"));
    return false;
  }
  const auto clip = project::ClipId::parse(clip_id.toStdString());
  if (!clip) {
    report_(QStringLiteral("Select a MIDI clip"));
    return false;
  }
  std::vector<project::NoteId> scope;
  for (const auto& text : note_ids) {
    const auto id = project::NoteId::parse(text.toStdString());
    if (!id) {
      report_(QStringLiteral("Select MIDI notes to edit their velocity"));
      return false;
    }
    scope.push_back(id.value());
  }
  const project::MidiClip* found = nullptr;
  for (const auto& track : session_()->snapshot().tracks) {
    const auto match = std::ranges::find(track.midi_clips, clip.value(), &project::MidiClip::id);
    if (match != track.midi_clips.end()) {
      found = &*match;
      break;
    }
  }
  if (!found) {
    report_(QStringLiteral("Select a MIDI clip"));
    return false;
  }
  const auto changes = edit(*found, scope);
  const auto before = session_()->snapshot().revision;
  const auto result = session_()->notes().set_note_velocities(clip.value(), changes);
  if (!result) {
    report_(QStringLiteral("Velocity: %1").arg(QString::fromStdString(result.error().message)));
    return false;
  }
  if (session_()->snapshot().revision != before) {
    commit_();
  }
  return true;
}

void NoteToolsController::set_note_velocity(const QString& clip_id, const QString& note_id,
                                            int velocity) {
  if (velocity < min_velocity || velocity > max_velocity) {
    report_(QStringLiteral("Velocity is 1 to 127"));
    return;
  }
  if (apply_velocity_edit(clip_id, {note_id},
                          [&](const project::MidiClip&, const std::vector<project::NoteId>& scope) {
                            return std::vector<application::VelocityChange>{
                                {scope.front(), static_cast<std::uint8_t>(velocity)}};
                          })) {
    session_()->notes().set_new_note_velocity(static_cast<std::uint8_t>(velocity));
    report_(QStringLiteral("Velocity %1 · new notes use it").arg(velocity));
    if (audition_)
      audition_(clip_id, note_id, static_cast<std::uint8_t>(velocity));
  }
}

void NoteToolsController::draw_velocity_line(const QString& clip_id, const QStringList& note_ids,
                                             double from_ticks, double from_velocity,
                                             double to_ticks, double to_velocity) {
  if (!std::isfinite(from_ticks) || !std::isfinite(to_ticks) || !std::isfinite(from_velocity) ||
      !std::isfinite(to_velocity)) {
    report_(QStringLiteral("Draw across the velocity stems"));
    return;
  }
  if (apply_velocity_edit(
          clip_id, note_ids,
          [&](const project::MidiClip& clip, const std::vector<project::NoteId>& scope) {
            return application::velocity_line(clip.notes, scope,
                                              static_cast<std::int64_t>(from_ticks), from_velocity,
                                              static_cast<std::int64_t>(to_ticks), to_velocity);
          })) {
    session_()->notes().set_new_note_velocity(application::clamp_velocity(to_velocity));
    report_(QStringLiteral("Velocity line drawn · Undo to restore"));
  }
}

void NoteToolsController::humanize_velocities(const QString& clip_id, const QStringList& note_ids,
                                              double strength) {
  const auto seed = ++velocity_seed_;
  if (apply_velocity_edit(
          clip_id, note_ids,
          [&](const project::MidiClip& clip, const std::vector<project::NoteId>& scope) {
            return application::humanize_velocities(clip.notes, scope, strength, seed);
          }))
    report_(QStringLiteral("Humanised · click again for another variation"));
}

void NoteToolsController::randomize_velocities(const QString& clip_id, const QStringList& note_ids,
                                               int minimum, int maximum) {
  const auto seed = ++velocity_seed_;
  if (apply_velocity_edit(
          clip_id, note_ids,
          [&](const project::MidiClip& clip, const std::vector<project::NoteId>& scope) {
            return application::randomize_velocities(clip.notes, scope, minimum, maximum, seed);
          }))
    report_(QStringLiteral("Randomised %1-%2 · click again to reroll").arg(minimum).arg(maximum));
}

int NoteToolsController::new_note_velocity() const {
  return session_() != nullptr ? static_cast<int>(session_()->notes().new_note_velocity())
                               : default_new_note_velocity;
}

} // namespace oscillate::quick
