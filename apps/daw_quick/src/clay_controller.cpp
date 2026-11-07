#include "clay_controller.hpp"

#include "oscillate/clay_dsp/limits.hpp"
#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/export/wav_writer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <thread>
#include <utility>

namespace oscillate::quick {
namespace {

namespace dsp = oscillate::clay_dsp;
namespace source_clay = oscillate::processing::clay;
namespace song_clay = oscillate::application::clay;

constexpr int poll_interval_ms = 50;
// A detection's progress once the audio is read, before the analysis.
constexpr double detect_read_progress = 0.3;
constexpr int waveform_column_count = 1'024;
// Points drawn per note curve at most.
constexpr std::size_t curve_points = 64;
// Cents of margin above and below the notes in the pitch view, and the
// smallest span it shows.
constexpr double view_margin_cents = 300.0;
constexpr double min_view_cents = 1'200.0;
constexpr double default_low_cents = 4'800.0;
constexpr double default_high_cents = 8'400.0;
constexpr double seconds_per_minute = 60.0;
constexpr double decibels_per_bel = 20.0;
// Audition files carry a little audio either side of the notes.
constexpr double audition_pad_seconds = 0.05;
constexpr std::array<const char*, 3> mode_names{"melodic", "polyphonic", "percussive"};
constexpr std::array<const char*, 12> tonics{"C",  "C♯", "D",  "E♭", "E",  "F",
                                             "F♯", "G",  "A♭", "A",  "B♭", "B"};
constexpr std::array<const char*, 4> division_names{"1/4", "1/8", "1/16", "1/32"};

QString text(const std::string& value) { return QString::fromStdString(value); }

QString key_text(std::uint32_t note, std::uint32_t voice) {
  return QStringLiteral("%1:%2").arg(note).arg(voice);
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> parse_key(const QString& key) {
  const auto parts = key.split(QLatin1Char(':'));
  if (parts.size() != 2)
    return std::nullopt;
  bool note_ok = false;
  bool voice_ok = false;
  const auto note = parts[0].toUInt(&note_ok);
  const auto voice = parts[1].toUInt(&voice_ok);
  if (!note_ok || !voice_ok || note == 0)
    return std::nullopt;
  return std::pair{note, voice};
}

std::optional<project::ClayMode> mode_of(const QString& name) {
  for (std::size_t i = 0; i < mode_names.size(); ++i)
    if (name == QLatin1String(mode_names[i]))
      return static_cast<project::ClayMode>(i + 1);
  return std::nullopt;
}

QString name_of(project::ClayMode mode) {
  return QLatin1String(mode_names[static_cast<std::size_t>(mode) - 1]);
}

double db_to_gain(double db) { return std::pow(10.0, db / decibels_per_bel); }

// Every `stride`-th of `count` indices, always including the last.
template <typename F> void decimated(std::size_t count, F&& visit) {
  if (count == 0)
    return;
  const auto stride = std::max<std::size_t>(1, (count + curve_points - 1) / curve_points);
  for (std::size_t k = 0; k < count; k += stride)
    visit(k);
  if ((count - 1) % stride != 0)
    visit(count - 1);
}

} // namespace

ClayController::ClayController(Session session, CanEdit can_edit, Report report, Commit commit,
                               FindGroove find_groove, Step undo, Step redo, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)), find_groove_(std::move(find_groove)),
      undo_(std::move(undo)), redo_(std::move(redo)) {
  timer_.setInterval(poll_interval_ms);
  connect(&timer_, &QTimer::timeout, this, &ClayController::poll);
}

ClayController::~ClayController() {
  if (job_)
    job_->cancel.request_stop();
  if (warm_stop_)
    warm_stop_->request_stop();
}

QString ClayController::clip_id() const { return clip_ ? text(clip_->to_string()) : QString{}; }

QString ClayController::mode() const {
  if (preview_)
    return name_of(preview_->mode);
  return take_ ? name_of(take_->mode) : QString{};
}

QStringList ClayController::modes() {
  QStringList names;
  for (const auto* name : mode_names)
    names.push_back(QLatin1String(name));
  return names;
}

double ClayController::progress() const {
  return job_ ? job_->progress.load(std::memory_order_relaxed) : 0.0;
}

QString ClayController::stage() const {
  if (!job_)
    return {};
  const std::scoped_lock lock(job_->mutex);
  return text(job_->stage);
}

QStringList ClayController::selection() const {
  QStringList keys;
  for (const auto& [note, voice] : selected_)
    keys.push_back(key_text(note, voice));
  return keys;
}

int ClayController::waveform_columns() noexcept { return waveform_column_count; }

int ClayController::key_tonic() const {
  const auto& take = shown_take();
  return take.key ? take.key->tonic : -1;
}

bool ClayController::key_minor() const {
  const auto& take = shown_take();
  return take.key && take.key->mode == project::KeyMode::minor;
}

QString ClayController::key_name() const {
  const auto& take = shown_take();
  if (!take.key)
    return QStringLiteral("Chromatic");
  return QStringLiteral("%1 %2").arg(QString::fromUtf8(tonics[take.key->tonic]),
                                     take.key->mode == project::KeyMode::minor
                                         ? QStringLiteral("minor")
                                         : QStringLiteral("major"));
}

QStringList ClayController::tonic_names() {
  QStringList names;
  for (const auto* name : tonics)
    names.push_back(QString::fromUtf8(name));
  return names;
}

int ClayController::song_key_tonic() const { return song_key_ ? song_key_->tonic : -1; }

bool ClayController::song_key_minor() const {
  return song_key_ && song_key_->mode == project::KeyMode::minor;
}

QVariantList ClayController::instrument_tracks() const { return instrument_tracks_; }

QStringList ClayController::divisions() {
  QStringList names;
  for (const auto* name : division_names)
    names.push_back(QLatin1String(name));
  return names;
}

const project::ClayTake& ClayController::shown_take() const {
  static const project::ClayTake none{};
  if (preview_)
    return *preview_;
  return take_ ? *take_ : none;
}

application::ProjectSession* ClayController::editable(const QString& action) {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || (can_edit_ && !can_edit_())) {
    set_status(QStringLiteral("Cannot %1 now").arg(action));
    return nullptr;
  }
  if (!clip_ || !take_ || !edited_ || !time_) {
    set_status(QStringLiteral("Detect the clip's notes first"));
    return nullptr;
  }
  return session;
}

void ClayController::set_status(QString status) {
  if (status_.set(std::move(status)))
    emit statusChanged();
}

void ClayController::set_render_state(QString state) {
  if (render_state_ == state)
    return;
  render_state_ = std::move(state);
  emit renderStateChanged();
}

// ---- Opening and detection ----

void ClayController::open_clip(const QString& clip_id) {
  const auto parsed = project::ClipId::parse(clip_id.toStdString());
  auto* session = session_ ? session_() : nullptr;
  if (!parsed || session == nullptr) {
    set_status(QStringLiteral("That audio clip is not in the project"));
    return;
  }
  auto found = song_clay::find_clip(session->snapshot(), parsed.value());
  if (!found) {
    set_status(text(found.error().message));
    return;
  }
  cancel_detect();
  clip_ = parsed.value();
  loaded_.reset();
  edited_.reset();
  preview_.reset();
  base_.reset();
  selected_.clear();
  history_.clear();
  waveform_.clear();
  emit historyChanged();
  reload_from_snapshot();
  if (take_)
    start_detect(take_->mode, false);
  else
    set_status(QStringLiteral("Choose how to detect the notes"));
  emit waveformChanged();
}

void ClayController::close() {
  cancel_detect();
  clip_.reset();
  take_.reset();
  preview_.reset();
  base_.reset();
  loaded_.reset();
  edited_.reset();
  time_.reset();
  selected_.clear();
  notes_.clear();
  waveform_.clear();
  set_render_state(QStringLiteral("none"));
  emit clipChanged();
  emit takeChanged();
  emit notesChanged();
  emit waveformChanged();
}

void ClayController::detect(const QString& mode) {
  const auto parsed = mode_of(mode);
  if (!parsed) {
    set_status(QStringLiteral("Detect as melodic, polyphonic or percussive"));
    return;
  }
  if (!clip_) {
    set_status(QStringLiteral("Open an audio clip first"));
    return;
  }
  start_detect(*parsed, !take_ || take_->mode != *parsed);
}

void ClayController::start_detect(project::ClayMode mode, bool save) {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !clip_)
    return;
  if (job_)
    cancel_detect();
  auto found = song_clay::find_clip(session->snapshot(), *clip_);
  if (!found) {
    set_status(text(found.error().message));
    return;
  }
  auto pinned = session->exports().pin_snapshot();
  if (!pinned) {
    set_status(QStringLiteral("Cannot read the clip: %1").arg(text(pinned.error().message)));
    return;
  }
  auto job = std::make_shared<Job>();
  job->stage = "Reading the audio";
  job->session = session->session_generation();
  const auto source = *found.value().source;
  auto decode = decode_ ? decode_ : Decode(source_clay::decode_source);
  const auto failed = detect_.start(
      0,
      [job, source, mode, decode = std::move(decode), pinned = std::move(pinned).value()] {
        // Cancelling stops the reading and the analysis between hops.
        const auto stop = job->cancel.get_token();
        auto audio = decode(pinned.reader, source, stop);
        if (!audio)
          return Detected::failure(audio.error());
        job->progress.store(detect_read_progress, std::memory_order_relaxed);
        {
          const std::scoped_lock lock(job->mutex);
          job->stage = "Finding notes";
        }
        std::optional<media::CacheStore> cache;
        if (auto directory = pinned.reader.open_cache_directory())
          if (auto adopted = media::CacheStore::adopt_directory_fd(directory.value()))
            cache.emplace(std::move(adopted).value());
        auto analysed = source_clay::analysed_take(cache ? &*cache : nullptr, source, mode,
                                                   audio.value(), stop);
        job->progress.store(1.0, std::memory_order_relaxed);
        if (!analysed)
          return Detected::failure(analysed.error());
        auto loaded = std::make_shared<Loaded>(source);
        loaded->mode = mode;
        loaded->audio = std::move(audio).value();
        loaded->analysed = std::move(analysed).value();
        return Detected::success(std::shared_ptr<const Loaded>(std::move(loaded)));
      },
      [](std::string what) {
        return Detected::failure({foundation::ErrorCode::internal, std::move(what)});
      });
  if (failed) {
    set_status(QStringLiteral("Cannot start detecting: %1").arg(QString::fromStdString(*failed)));
    return;
  }
  job_ = std::move(job);
  save_on_detect_ = save;
  emit detectingChanged();
  timer_.start();
}

void ClayController::cancel_detect() {
  if (!job_)
    return;
  // Never waits: the worker stops at its next check and is joined once done.
  job_->cancel.request_stop();
  detect_.abandon();
  job_.reset();
  emit detectingChanged();
}

void ClayController::poll() {
  if (job_ && detect_.finished()) {
    finish_detect();
  } else if (job_) {
    emit detectingChanged();
  }
  if (warm_.finished()) {
    auto finished = warm_.take();
    warm_stop_.reset();
    if (finished && finished->result)
      set_status(
          QStringLiteral("The edit could not render: %1").arg(text(finished->result->message)));
    set_render_state(take_ ? QStringLiteral("ready") : QStringLiteral("none"));
  }
  if (audition_.finished()) {
    if (auto finished = audition_.take(); finished && !finished->result) {
      set_status(QStringLiteral("Cannot audition: %1").arg(text(finished->result.error().message)));
    } else if (finished) {
      audition_path_ = finished->result.value();
      emit auditionReady(audition_path_);
    }
  }
  if (!job_ && !warm_.running() && !audition_.running())
    timer_.stop();
}

void ClayController::finish_detect() {
  auto job = std::move(job_);
  job_.reset();
  auto finished = detect_.take();
  emit detectingChanged();
  if (!finished || job->cancel.stop_requested())
    return;
  if (auto* session = session_ ? session_() : nullptr;
      session == nullptr || session->session_generation() != job->session) {
    set_status(QStringLiteral("Detection dropped: the project changed"));
    return;
  }
  auto& result = finished->result;
  if (!result) {
    set_status(QStringLiteral("No notes: %1").arg(text(result.error().message)));
    return;
  }
  loaded_ = std::move(result).value();
  rebuild_waveform();
  if (save_on_detect_) {
    auto* session = session_ ? session_() : nullptr;
    if (session == nullptr || !clip_)
      return;
    auto found = song_clay::find_clip(session->snapshot(), *clip_);
    if (!found)
      return;
    auto fresh = song_clay::new_take(session->snapshot(), *found.value().clip, loaded_->mode);
    key_from_analysis_ = true;
    if (!commit(std::move(fresh), QStringLiteral("Detect ") + name_of(loaded_->mode)))
      return;
  }
  reload_from_snapshot();
  set_status(QStringLiteral("%1 notes").arg(loaded_->analysed.notes.size()));
}

void ClayController::reload_from_snapshot() {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !clip_)
    return;
  const auto& snapshot = session->snapshot();
  auto found = song_clay::find_clip(snapshot, *clip_);
  if (!found) {
    close();
    return;
  }
  const auto& clip = *found.value().clip;
  const auto& track = *found.value().track;
  clip_name_ = clip.name.empty() ? (track.name.empty() ? QStringLiteral("Audio") : text(track.name))
                                 : text(clip.name);
  track_name_ = text(track.name);
  song_key_ = snapshot.key;
  auto time = song_clay::ClipTime::of(snapshot, clip);
  if (time) {
    time_.emplace(std::move(time).value());
    clip_start_ = time_->first_song_seconds();
    clip_end_ = time_->end_song_seconds();
  } else {
    time_.reset();
  }
  double bpm = 0.0;
  for (const auto& event : snapshot.tempo.events()) {
    auto at = snapshot.tempo.tick_to_sample(event.tick, snapshot.sample_rate);
    if (at && static_cast<double>(at.value().value) <= clip_start_ * snapshot.sample_rate)
      bpm = event.bpm;
  }
  if (bpm > 0.0)
    beat_seconds_ = seconds_per_minute / bpm;
  if (!snapshot.meter.events().empty())
    beats_per_bar_ = snapshot.meter.events().front().numerator;
  instrument_ids_.clear();
  instrument_tracks_.clear();
  for (const auto& item : snapshot.tracks) {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&item.instrument_source);
    if (item.kind != project::TrackKind::instrument || patch == nullptr)
      continue;
    instrument_ids_.push_back(item.id);
    instrument_tracks_.push_back(QVariantMap{
        {QStringLiteral("id"), text(item.id.to_string())},
        {QStringLiteral("name"), item.name.empty() ? text(patch->engine) : text(item.name)},
        {QStringLiteral("engine"), text(patch->engine)},
        {QStringLiteral("hasNotes"),
         std::ranges::any_of(item.midi_clips, [](const auto& c) { return !c.notes.empty(); })}});
  }
  const auto was = take_;
  take_ = clip.clay;
  if (take_ != was && take_ && was && take_->key != was->key)
    key_from_analysis_ = false;
  if (!take_)
    key_from_analysis_ = false;
  emit clipChanged();
  emit takeChanged();
  // A take in another mode than the loaded analysis needs detecting again.
  if (take_ && loaded_ && loaded_->mode != take_->mode && !job_)
    start_detect(take_->mode, false);
  rebuild_edited();
  if (!take_) {
    set_render_state(QStringLiteral("none"));
  } else if (!warm_.running()) {
    auto pinned = session->exports().pin_snapshot();
    if (pinned && source_clay::render_cached(pinned.value().reader, *found.value().source, *take_))
      set_render_state(QStringLiteral("ready"));
    else
      warm_render();
  }
}

void ClayController::project_changed() {
  if (!clip_ || preview_)
    return;
  reload_from_snapshot();
}

void ClayController::rebuild_edited() {
  edited_.reset();
  if (loaded_ && take_ && loaded_->mode == take_->mode) {
    auto copy = loaded_->analysed;
    if (auto replayed = source_clay::apply_ops(copy, take_->ops); replayed)
      edited_ = std::move(copy);
    else
      set_status(QStringLiteral("The take no longer matches its notes: %1")
                     .arg(text(replayed.error().message)));
  }
  // Selected notes that went away are no longer selected.
  std::erase_if(selected_, [&](const Key& key) {
    if (!edited_ || edited_->find(key.first) == nullptr)
      return true;
    return key.second != 0 &&
           std::ranges::none_of(shown_take().edits, [&](const project::ClayNoteEdit& edit) {
             return edit.note == key.first && edit.voice == key.second;
           });
  });
  rebuild_notes();
}

const clay_dsp::Note* ClayController::note_of(std::uint32_t id) const {
  return edited_ ? edited_->find(id) : nullptr;
}

void ClayController::rebuild_notes() {
  notes_.clear();
  low_cents_ = default_low_cents;
  high_cents_ = default_high_cents;
  if (!edited_ || !time_) {
    emit notesChanged();
    return;
  }
  const auto& take = shown_take();
  const auto& analysed = *edited_;
  double max_amplitude = 0.0;
  for (const auto& note : analysed.notes)
    for (const auto value : note.amplitude)
      max_amplitude = std::max(max_amplitude, static_cast<double>(value));
  if (!(max_amplitude > 0.0))
    max_amplitude = 1.0;
  double low = std::numeric_limits<double>::max();
  double high = std::numeric_limits<double>::lowest();
  std::vector<std::pair<double, QVariantMap>> rows;
  const auto row = [&](const dsp::Note& note, const project::ClayNoteEdit& edit) {
    const auto start = static_cast<double>(edit.start.value_or(note.start));
    const auto end = static_cast<double>(edit.end.value_or(note.end));
    const auto span = static_cast<double>(std::max<std::int64_t>(1, note.end - note.start));
    const auto stretch = (end - start) / span;
    const auto placed = [&](double frame) {
      return start + (frame - static_cast<double>(note.start)) * stretch;
    };
    const double centre = note.centre + edit.pitch_cents;
    QVariantList curve;
    QVariantList original;
    QVariantList levels;
    decimated(note.hops(), [&](std::size_t k) {
      const auto frame =
          static_cast<double>((note.first_hop + static_cast<std::int64_t>(k)) * analysed.hop);
      const auto t = time_->song_seconds(placed(frame));
      const auto t_original = time_->song_seconds(frame);
      if (note.pitch[k] > 0.0F) {
        curve.push_back(t);
        curve.push_back(centre + note.drift[k] * edit.drift + note.modulation[k] * edit.modulation);
        original.push_back(t_original);
        original.push_back(dsp::hz_to_cents(note.pitch[k]));
      }
      levels.push_back(t);
      levels.push_back(note.amplitude[k] / max_amplitude * db_to_gain(edit.gain_db));
    });
    if (note.kind == dsp::NoteKind::segment && curve.isEmpty()) {
      curve = {time_->song_seconds(start), centre, time_->song_seconds(end), centre};
      original = {time_->song_seconds(static_cast<double>(note.start)), note.centre,
                  time_->song_seconds(static_cast<double>(note.end)), note.centre};
    }
    low = std::min(low, centre);
    high = std::max(high, centre);
    const bool selected = selected_.contains({note.id, edit.voice});
    QVariantMap map{
        {QStringLiteral("key"), key_text(note.id, edit.voice)},
        {QStringLiteral("id"), static_cast<int>(note.id)},
        {QStringLiteral("voice"), static_cast<int>(edit.voice)},
        {QStringLiteral("harmony"), edit.voice != 0},
        {QStringLiteral("muted"), edit.muted},
        {QStringLiteral("selected"), selected},
        {QStringLiteral("start"), time_->song_seconds(start)},
        {QStringLiteral("end"), time_->song_seconds(end)},
        {QStringLiteral("sourceStart"), start},
        {QStringLiteral("sourceEnd"), end},
        {QStringLiteral("centre"), centre},
        {QStringLiteral("originalCentre"), note.centre},
        {QStringLiteral("pitchCents"), edit.pitch_cents},
        {QStringLiteral("formant"), edit.formant_cents},
        {QStringLiteral("gainDb"), edit.gain_db},
        {QStringLiteral("level"), note.mean_amplitude() / max_amplitude * db_to_gain(edit.gain_db)},
        {QStringLiteral("modulation"), edit.modulation},
        {QStringLiteral("drift"), edit.drift},
        {QStringLiteral("noise"), static_cast<double>(note.noise_share)},
        {QStringLiteral("lyric"), QString{}},
        {QStringLiteral("curve"), curve},
        {QStringLiteral("originalCurve"), original},
        {QStringLiteral("levels"), levels}};
    rows.emplace_back(time_->song_seconds(start), std::move(map));
  };
  for (const auto& note : analysed.notes)
    row(note, edit_for(take, {note.id, 0}));
  for (const auto& edit : take.edits) {
    if (edit.voice == 0)
      continue;
    if (const auto* note = analysed.find(edit.note))
      row(*note, edit);
  }
  std::ranges::stable_sort(rows, {}, &std::pair<double, QVariantMap>::first);
  for (auto& [start, map] : rows)
    notes_.push_back(std::move(map));
  if (low <= high) {
    low_cents_ = low - view_margin_cents;
    high_cents_ = std::max(high + view_margin_cents, low_cents_ + min_view_cents);
  }
  emit notesChanged();
}

void ClayController::rebuild_waveform() {
  waveform_.clear();
  if (!loaded_ || !time_) {
    emit waveformChanged();
    return;
  }
  const auto& audio = loaded_->audio;
  const auto first = static_cast<double>(time_->first_source_frame());
  const auto end = static_cast<double>(time_->end_source_frame());
  const auto total = static_cast<double>(audio.left.size());
  for (int column = 0; column < waveform_column_count; ++column) {
    const auto from =
        std::clamp(first + (end - first) * column / waveform_column_count, 0.0, total);
    const auto to =
        std::clamp(first + (end - first) * (column + 1) / waveform_column_count, from, total);
    float low = 0.0F;
    float high = 0.0F;
    for (auto i = static_cast<std::size_t>(from); i < static_cast<std::size_t>(to); ++i) {
      const float value =
          audio.right.empty() ? audio.left[i] : 0.5F * (audio.left[i] + audio.right[i]);
      low = std::min(low, value);
      high = std::max(high, value);
    }
    waveform_.push_back(static_cast<double>(low));
    waveform_.push_back(static_cast<double>(high));
  }
  emit waveformChanged();
}

// ---- Edits ----

project::ClayNoteEdit ClayController::edit_for(const project::ClayTake& take, Key key) const {
  const auto found = std::ranges::find_if(take.edits, [&](const project::ClayNoteEdit& edit) {
    return edit.note == key.first && edit.voice == key.second;
  });
  if (found != take.edits.end())
    return *found;
  project::ClayNoteEdit fresh;
  fresh.note = key.first;
  fresh.voice = key.second;
  return fresh;
}

void ClayController::put_edit(project::ClayTake& take, const project::ClayNoteEdit& edit) const {
  const auto found = std::ranges::find_if(take.edits, [&](const project::ClayNoteEdit& item) {
    return item.note == edit.note && item.voice == edit.voice;
  });
  const bool identity = edit.voice == 0 && !edit.muted && edit.pitch_cents == 0.0 &&
                        edit.modulation == 1.0 && edit.drift == 1.0 && edit.formant_cents == 0.0 &&
                        edit.gain_db == 0.0 && !edit.start && !edit.end;
  if (found != take.edits.end()) {
    if (identity)
      take.edits.erase(found);
    else
      *found = edit;
  } else if (!identity) {
    take.edits.push_back(edit);
  }
}

std::vector<ClayController::Key> ClayController::scope() const {
  return {selected_.begin(), selected_.end()};
}

std::vector<std::uint32_t> ClayController::selected_notes() const {
  std::vector<std::uint32_t> notes;
  for (const auto& [note, voice] : selected_)
    if (voice == 0)
      notes.push_back(note);
  return notes;
}

double ClayController::source_frames_per_second() const {
  if (!time_)
    return 0.0;
  return time_->source_frame_at_seconds(clip_start_ + 1.0) -
         time_->source_frame_at_seconds(clip_start_);
}

bool ClayController::commit(project::ClayTake take, const QString& label) {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !clip_)
    return false;
  if (can_edit_ && !can_edit_()) {
    set_status(QStringLiteral("Cannot edit the clip now"));
    return false;
  }
  if (take_ && *take_ == take)
    return true;
  auto applied = session->audio_clips().set_clip_clay(*clip_, std::move(take));
  if (!applied) {
    set_status(QStringLiteral("%1 failed: %2").arg(label, text(applied.error().message)));
    return false;
  }
  history_.push_back(QVariantMap{{QStringLiteral("label"), label}});
  emit historyChanged();
  if (commit_)
    commit_();
  reload_from_snapshot();
  warm_render();
  set_status(label);
  return true;
}

void ClayController::warm_render() {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !clip_ || !take_)
    return;
  // A render of an older take is stopped and left to finish on its own.
  if (warm_stop_)
    warm_stop_->request_stop();
  warm_.abandon();
  warm_stop_.reset();
  auto found = song_clay::find_clip(session->snapshot(), *clip_);
  auto pinned = session->exports().pin_snapshot();
  if (!found || !pinned)
    return;
  std::stop_source stop;
  const auto source = *found.value().source;
  const auto take = *take_;
  const auto failed = warm_.start(
      0,
      [stop = stop.get_token(), source, take, pinned = std::move(pinned).value()] {
        auto rendered = source_clay::open_rendered(pinned.reader, source, take, stop);
        return rendered || stop.stop_requested() ? std::optional<foundation::Error>{}
                                                 : std::optional(rendered.error());
      },
      [](std::string what) {
        return std::optional<foundation::Error>(
            foundation::Error{foundation::ErrorCode::internal, std::move(what)});
      });
  if (failed)
    return;
  warm_stop_ = std::move(stop);
  set_render_state(QStringLiteral("rendering"));
  timer_.start();
}

void ClayController::remove_take() {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !clip_ || !take_ || (can_edit_ && !can_edit_()))
    return;
  auto applied = session->audio_clips().set_clip_clay(*clip_, std::nullopt);
  if (!applied) {
    set_status(QStringLiteral("Cannot remove the take: %1").arg(text(applied.error().message)));
    return;
  }
  if (commit_)
    commit_();
  history_.push_back(QVariantMap{{QStringLiteral("label"), QStringLiteral("Remove take")}});
  emit historyChanged();
  reload_from_snapshot();
  set_status(QStringLiteral("The clip plays as recorded"));
}

void ClayController::select(const QString& key, bool additive) {
  const auto parsed = parse_key(key);
  if (!additive)
    selected_.clear();
  if (parsed) {
    if (additive && selected_.contains(*parsed))
      selected_.erase(*parsed);
    else
      selected_.insert(*parsed);
  }
  rebuild_notes();
}

void ClayController::select_all() {
  selected_.clear();
  for (const auto& entry : notes_) {
    if (const auto parsed = parse_key(entry.toMap().value(QStringLiteral("key")).toString()))
      selected_.insert(*parsed);
  }
  rebuild_notes();
}

void ClayController::clear_selection() {
  selected_.clear();
  rebuild_notes();
}

void ClayController::select_box(double from_seconds, double to_seconds, double low_cents,
                                double high_cents, bool additive) {
  if (!additive)
    selected_.clear();
  const auto t0 = std::min(from_seconds, to_seconds);
  const auto t1 = std::max(from_seconds, to_seconds);
  const auto c0 = std::min(low_cents, high_cents);
  const auto c1 = std::max(low_cents, high_cents);
  for (const auto& entry : notes_) {
    const auto map = entry.toMap();
    const auto start = map.value(QStringLiteral("start")).toDouble();
    const auto end = map.value(QStringLiteral("end")).toDouble();
    const auto centre = map.value(QStringLiteral("centre")).toDouble();
    if (end >= t0 && start <= t1 && centre >= c0 && centre <= c1)
      if (const auto parsed = parse_key(map.value(QStringLiteral("key")).toString()))
        selected_.insert(*parsed);
  }
  rebuild_notes();
}

bool ClayController::begin_edit(const QString& tool) {
  static const QStringList tools{
      QStringLiteral("move"),       QStringLiteral("pitch"),    QStringLiteral("modulation"),
      QStringLiteral("drift"),      QStringLiteral("formant"),  QStringLiteral("amplitude"),
      QStringLiteral("time_start"), QStringLiteral("time_end"), QStringLiteral("time_move")};
  if (!tools.contains(tool) || !take_ || !edited_ || selected_.empty())
    return false;
  tool_ = tool;
  base_ = *take_;
  preview_ = *take_;
  return true;
}

void ClayController::update_edit(double seconds, double cents, double amount) {
  if (!base_ || !preview_ || !edited_ || !time_)
    return;
  auto take = *base_;
  const auto frames = seconds * source_frames_per_second();
  const auto scale = source_clay::dsp_scale(take.key);
  for (const auto& key : scope()) {
    const auto* note = note_of(key.first);
    if (note == nullptr)
      continue;
    auto edit = edit_for(*base_, key);
    const auto span = static_cast<double>(std::max<std::int64_t>(1, note->end - note->start));
    const auto start = static_cast<double>(edit.start.value_or(note->start));
    const auto end = static_cast<double>(edit.end.value_or(note->end));
    const auto place = [&](double new_start, double new_end) {
      new_start = std::max(0.0, new_start);
      const auto shortest = span / dsp::max_time_scale;
      const auto longest = span * dsp::max_time_scale;
      new_end = std::clamp(new_end, new_start + shortest, new_start + longest);
      edit.start = static_cast<std::int64_t>(std::llround(new_start));
      edit.end = static_cast<std::int64_t>(std::llround(new_end));
      if (*edit.start == note->start && *edit.end == note->end) {
        edit.start.reset();
        edit.end.reset();
      }
    };
    if (tool_ == QStringLiteral("move") || tool_ == QStringLiteral("pitch")) {
      double target = note->centre + edit.pitch_cents + cents;
      if (snap_ == QStringLiteral("scale"))
        target = dsp::nearest_in_scale(target, scale);
      else if (snap_ == QStringLiteral("chromatic"))
        target = dsp::nearest_in_scale(target, dsp::Scale::chromatic());
      edit.pitch_cents =
          std::clamp(target - note->centre, -dsp::max_edit_cents, dsp::max_edit_cents);
      if (tool_ == QStringLiteral("move") && frames != 0.0)
        place(start + frames, end + frames);
    } else if (tool_ == QStringLiteral("modulation")) {
      edit.modulation = std::clamp(edit.modulation * amount, 0.0, dsp::max_modulation_scale);
    } else if (tool_ == QStringLiteral("drift")) {
      edit.drift = std::clamp(edit.drift * amount, 0.0, dsp::max_drift_scale);
    } else if (tool_ == QStringLiteral("formant")) {
      edit.formant_cents =
          std::clamp(edit.formant_cents + amount, -dsp::max_edit_cents, dsp::max_edit_cents);
    } else if (tool_ == QStringLiteral("amplitude")) {
      edit.gain_db =
          std::clamp(edit.gain_db + amount, dsp::min_edit_gain_db, dsp::max_edit_gain_db);
    } else if (tool_ == QStringLiteral("time_start")) {
      place(std::min(start + frames, end - span / dsp::max_time_scale), end);
    } else if (tool_ == QStringLiteral("time_end")) {
      place(start, end + frames);
    } else if (tool_ == QStringLiteral("time_move")) {
      place(start + frames, end + frames);
    }
    put_edit(take, edit);
  }
  preview_ = std::move(take);
  rebuild_notes();
}

void ClayController::commit_edit() {
  if (!preview_)
    return;
  auto take = std::move(*preview_);
  preview_.reset();
  base_.reset();
  static const std::map<QString, QString> labels{
      {QStringLiteral("move"), QStringLiteral("Move notes")},
      {QStringLiteral("pitch"), QStringLiteral("Pitch")},
      {QStringLiteral("modulation"), QStringLiteral("Pitch modulation")},
      {QStringLiteral("drift"), QStringLiteral("Pitch drift")},
      {QStringLiteral("formant"), QStringLiteral("Formant")},
      {QStringLiteral("amplitude"), QStringLiteral("Amplitude")},
      {QStringLiteral("time_start"), QStringLiteral("Note start")},
      {QStringLiteral("time_end"), QStringLiteral("Note end")},
      {QStringLiteral("time_move"), QStringLiteral("Move in time")}};
  const auto label = labels.contains(tool_) ? labels.at(tool_) : QStringLiteral("Edit notes");
  if (!commit(std::move(take), label))
    rebuild_notes();
}

void ClayController::cancel_edit() {
  preview_.reset();
  base_.reset();
  rebuild_notes();
}

void ClayController::snap_selection() {
  if (!take_ || !edited_)
    return;
  auto take = *take_;
  const auto scale = source_clay::dsp_scale(take.key);
  for (const auto& key : scope()) {
    const auto* note = note_of(key.first);
    if (note == nullptr)
      continue;
    auto edit = edit_for(take, key);
    edit.pitch_cents = dsp::nearest_in_scale(note->centre + edit.pitch_cents, scale) - note->centre;
    put_edit(take, edit);
  }
  commit(std::move(take), QStringLiteral("Snap to scale"));
}

void ClayController::split(const QString& key, double seconds) {
  const auto parsed = parse_key(key);
  if (!parsed || parsed->second != 0 || !take_ || !edited_ || !time_) {
    set_status(QStringLiteral("Split a note, not a harmony"));
    return;
  }
  const auto* note = note_of(parsed->first);
  if (note == nullptr)
    return;
  auto take = *take_;
  auto edit = edit_for(take, *parsed);
  // A placed note splits where its original sound is under the cut.
  auto frame = time_->source_frame_at_seconds(seconds);
  if (edit.start && edit.end) {
    const auto span = static_cast<double>(*edit.end - *edit.start);
    frame = static_cast<double>(note->start) + (frame - static_cast<double>(*edit.start)) *
                                                   static_cast<double>(note->end - note->start) /
                                                   std::max(1.0, span);
  }
  const auto cut = static_cast<std::int64_t>(std::llround(frame));
  auto copy = *edited_;
  auto second = dsp::split_note(copy, note->id, cut);
  if (!second) {
    set_status(QStringLiteral("Cannot split there: %1").arg(text(second.error().message)));
    return;
  }
  take.ops.push_back({project::ClayOpKind::split, note->id, 0, cut});
  // Both halves keep the note's pitch, level and formant; a placement goes.
  edit.start.reset();
  edit.end.reset();
  put_edit(take, edit);
  auto other = edit;
  other.note = second.value();
  put_edit(take, other);
  commit(std::move(take), QStringLiteral("Split note"));
}

void ClayController::join(const QString& first, const QString& second) {
  const auto a = parse_key(first);
  const auto b = parse_key(second);
  if (!a || !b || a->second != 0 || b->second != 0 || !take_ || !edited_) {
    set_status(QStringLiteral("Join two notes, not harmonies"));
    return;
  }
  auto copy = *edited_;
  if (auto joined = dsp::join_notes(copy, a->first, b->first); !joined) {
    set_status(QStringLiteral("Cannot join: %1").arg(text(joined.error().message)));
    return;
  }
  auto take = *take_;
  take.ops.push_back({project::ClayOpKind::join, a->first, b->first, 0});
  std::erase_if(take.edits,
                [&](const project::ClayNoteEdit& edit) { return edit.note == b->first; });
  selected_.erase(*b);
  commit(std::move(take), QStringLiteral("Join notes"));
}

void ClayController::delete_selection() {
  if (!take_ || selected_.empty())
    return;
  auto take = *take_;
  for (const auto& key : scope()) {
    if (key.second != 0) {
      std::erase_if(take.edits, [&](const project::ClayNoteEdit& edit) {
        return edit.note == key.first && edit.voice == key.second;
      });
      continue;
    }
    auto edit = edit_for(take, key);
    edit.muted = true;
    put_edit(take, edit);
  }
  selected_.clear();
  commit(std::move(take), QStringLiteral("Delete notes"));
}

// ---- Macros ----

std::optional<project::ClayTake> ClayController::macro_result(
    const std::function<std::vector<dsp::NoteEdit>(std::span<const dsp::NoteEdit>,
                                                   std::span<const std::uint32_t>)>& run) {
  const auto* base = base_ ? &*base_ : (take_ ? &*take_ : nullptr);
  if (base == nullptr || !edited_)
    return std::nullopt;
  const auto edits = source_clay::dsp_edits(*base, false);
  const auto notes = selected_notes();
  const auto result = run(edits, notes);
  return source_clay::with_dsp_edits(*base, result);
}

void ClayController::begin_macro() {
  if (!take_ || !edited_)
    return;
  base_ = *take_;
  preview_ = *take_;
}

void ClayController::preview_correct_pitch(double centre, double drift) {
  if (!base_)
    begin_macro();
  const auto scale = source_clay::dsp_scale(base_ ? base_->key : std::nullopt);
  if (auto result = macro_result([&](auto edits, auto notes) {
        return dsp::correct_pitch(*edited_, edits, std::clamp(centre, 0.0, 1.0),
                                  std::clamp(drift, 0.0, 1.0), scale, notes);
      })) {
    preview_ = std::move(result);
    rebuild_notes();
  }
}

void ClayController::preview_quantize(const QString& grid, double amount) {
  if (!base_)
    begin_macro();
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !time_ || !edited_)
    return;
  std::optional<persistence::GrooveTemplate> groove;
  auto steps = song_clay::steps_per_quarter(grid.toStdString());
  if (!steps) {
    groove = find_groove_ ? find_groove_(grid) : std::nullopt;
    if (!groove) {
      set_status(QStringLiteral("No grid or groove called %1").arg(grid));
      return;
    }
  }
  auto made = song_clay::quantize_grid(session->snapshot(), *time_, steps.value_or(0), groove);
  if (!made) {
    set_status(text(made.error().message));
    return;
  }
  if (auto result = macro_result([&](auto edits, auto notes) {
        return dsp::quantize_time(*edited_, edits, made.value(), std::clamp(amount, 0.0, 1.0),
                                  notes);
      })) {
    preview_ = std::move(result);
    rebuild_notes();
  }
}

void ClayController::preview_level(double amount) {
  if (!base_)
    begin_macro();
  if (auto result = macro_result([&](auto edits, auto notes) {
        return dsp::level_amplitude(*edited_, edits, std::clamp(amount, 0.0, 1.0), notes);
      })) {
    preview_ = std::move(result);
    rebuild_notes();
  }
}

void ClayController::commit_macro(const QString& label) {
  if (!preview_)
    return;
  auto take = std::move(*preview_);
  preview_.reset();
  base_.reset();
  if (!commit(std::move(take), label.isEmpty() ? QStringLiteral("Macro") : label))
    rebuild_notes();
}

void ClayController::set_key(int tonic, bool minor) {
  if (!take_ || tonic < 0 || tonic >= static_cast<int>(tonics.size()))
    return;
  auto take = *take_;
  take.key = project::MusicalKey{static_cast<std::uint8_t>(tonic),
                                 minor ? project::KeyMode::minor : project::KeyMode::major};
  key_from_analysis_ = false;
  commit(std::move(take), QStringLiteral("Key"));
}

void ClayController::set_chromatic() {
  if (!take_)
    return;
  auto take = *take_;
  take.key.reset();
  key_from_analysis_ = false;
  commit(std::move(take), QStringLiteral("Chromatic"));
}

// ---- Creative tools ----

void ClayController::to_midi(const QString& track_id) {
  auto* session = editable(QStringLiteral("send the notes to MIDI"));
  if (session == nullptr)
    return;
  std::optional<project::TrackId> track;
  if (!track_id.isEmpty()) {
    const auto parsed = project::TrackId::parse(track_id.toStdString());
    if (!parsed) {
      set_status(QStringLiteral("That track is not in the project"));
      return;
    }
    track = parsed.value();
  }
  // Muted notes and voices stay out of the MIDI.
  auto take = *take_;
  auto analysed = *edited_;
  std::erase_if(analysed.notes,
                [&](const dsp::Note& note) { return edit_for(take, {note.id, 0}).muted; });
  std::erase_if(take.edits, [&](const project::ClayNoteEdit& edit) {
    return edit.muted || analysed.find(edit.note) == nullptr;
  });
  const auto midi = dsp::to_midi(analysed, source_clay::dsp_edits(take, false));
  auto out = song_clay::midi_clip_out(*time_, midi);
  if (!out) {
    set_status(QStringLiteral("To MIDI failed: %1").arg(text(out.error().message)));
    return;
  }
  auto made = session->audio_clips().add_clay_midi_clip(track, out.value());
  if (!made) {
    set_status(QStringLiteral("To MIDI failed: %1").arg(text(made.error().message)));
    return;
  }
  if (commit_)
    commit_();
  history_.push_back(QVariantMap{{QStringLiteral("label"), QStringLiteral("To MIDI")}});
  emit historyChanged();
  reload_from_snapshot();
  set_status(QStringLiteral("%1 notes sent to MIDI").arg(out.value().notes.size()));
}

void ClayController::harmonise(int degrees, double formant_cents, double gain_db) {
  if (editable(QStringLiteral("harmonise")) == nullptr || degrees == 0)
    return;
  std::map<std::uint32_t, std::uint32_t> voices_before;
  for (const auto& edit : take_->edits)
    voices_before[edit.note] = std::max(voices_before[edit.note], edit.voice);
  const auto scale = source_clay::dsp_scale(take_->key);
  auto result = macro_result([&](auto edits, auto notes) {
    return dsp::harmonise(*edited_, edits, degrees, scale, notes);
  });
  if (!result)
    return;
  for (auto& edit : result->edits) {
    if (edit.voice > voices_before[edit.note]) {
      edit.formant_cents = std::clamp(formant_cents, -dsp::max_edit_cents, dsp::max_edit_cents);
      edit.gain_db =
          std::clamp(edit.gain_db + gain_db, dsp::min_edit_gain_db, dsp::max_edit_gain_db);
    }
  }
  if (std::ranges::any_of(result->edits, [](const project::ClayNoteEdit& edit) {
        return edit.voice > project::max_clay_voices;
      })) {
    set_status(QStringLiteral("A note has as many harmonies as it can take"));
    return;
  }
  commit(std::move(*result), QStringLiteral("Harmonise"));
}

void ClayController::fit_to_key(int tonic, bool minor, bool follow_quality) {
  if (editable(QStringLiteral("fit to the key")) == nullptr || tonic < 0 ||
      tonic >= static_cast<int>(tonics.size()))
    return;
  const project::MusicalKey to{static_cast<std::uint8_t>(tonic),
                               minor ? project::KeyMode::minor : project::KeyMode::major};
  const auto from_scale = source_clay::dsp_scale(take_->key);
  const auto to_scale = source_clay::dsp_scale(to);
  auto result = macro_result([&](auto edits, auto notes) {
    return dsp::fit_to_key(*edited_, edits, from_scale, to_scale, follow_quality, notes);
  });
  if (!result)
    return;
  result->key = to;
  commit(std::move(*result), QStringLiteral("Fit to key"));
}

void ClayController::follow_notes(const QString& track_id, double time_amount, bool stay_in_key) {
  auto* session = editable(QStringLiteral("follow the notes"));
  if (session == nullptr)
    return;
  const auto parsed = project::TrackId::parse(track_id.toStdString());
  if (!parsed) {
    set_status(QStringLiteral("Choose a track to follow"));
    return;
  }
  const auto targets = song_clay::follow_targets(session->snapshot(), *time_, parsed.value());
  if (targets.empty()) {
    set_status(QStringLiteral("That track has no notes under the clip"));
    return;
  }
  const auto scale = source_clay::dsp_scale(take_->key);
  auto result = macro_result([&](auto edits, auto notes) {
    auto followed =
        dsp::follow_notes(*edited_, edits, targets, std::clamp(time_amount, 0.0, 1.0), notes);
    if (stay_in_key)
      followed = dsp::correct_pitch(*edited_, followed, 1.0, 0.0, scale, notes);
    return followed;
  });
  if (result)
    commit(std::move(*result), QStringLiteral("Follow the bass"));
}

void ClayController::variation(int seed, double amount) {
  if (editable(QStringLiteral("vary the notes")) == nullptr)
    return;
  auto result = macro_result([&](auto edits, auto notes) {
    return dsp::variation(*edited_, edits, static_cast<std::uint64_t>(seed),
                          std::clamp(amount, 0.0, 1.0), notes);
  });
  if (result)
    commit(std::move(*result), QStringLiteral("Variation"));
}

// ---- Auditions ----

void ClayController::audition_note(const QString& key) {
  const auto parsed = parse_key(key);
  if (!parsed || !edited_ || !take_)
    return;
  const auto* note = note_of(parsed->first);
  if (note == nullptr)
    return;
  const auto edit = edit_for(*take_, *parsed);
  audition_span(static_cast<double>(ab_original_ ? note->start : edit.start.value_or(note->start)),
                static_cast<double>(ab_original_ ? note->end : edit.end.value_or(note->end)));
}

void ClayController::audition_selection() {
  if (!edited_ || !take_)
    return;
  double from = std::numeric_limits<double>::max();
  double to = std::numeric_limits<double>::lowest();
  for (const auto& entry : notes_) {
    const auto map = entry.toMap();
    if (!selected_.empty() && !map.value(QStringLiteral("selected")).toBool())
      continue;
    from = std::min(from, map.value(QStringLiteral("sourceStart")).toDouble());
    to = std::max(to, map.value(QStringLiteral("sourceEnd")).toDouble());
  }
  if (from < to)
    audition_span(from, to);
}

void ClayController::audition_span(double from_frame, double to_frame) {
  auto* session = session_ ? session_() : nullptr;
  if (session == nullptr || !loaded_ || !take_ || audition_.running())
    return;
  auto pinned = session->exports().pin_snapshot();
  if (!pinned)
    return;
  const auto folder =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/clay");
  if (!QDir().mkpath(folder))
    return;
  static std::uint64_t serial = 0;
  ++serial;
  const auto path = QStringLiteral("%1/audition-%2.wav").arg(folder).arg(serial);
  const auto pad = audition_pad_seconds * loaded_->audio.sample_rate;
  const auto total = static_cast<double>(loaded_->audio.left.size());
  const auto first = static_cast<std::size_t>(std::clamp(from_frame - pad, 0.0, total));
  const auto last = static_cast<std::size_t>(std::clamp(to_frame + pad, 0.0, total));
  if (last <= first)
    return;
  const auto failed = audition_.start(
      0,
      [loaded = loaded_, take = *take_, original = ab_original_, first, last, path,
       pinned = std::move(pinned).value()] {
        return [&]() -> foundation::Result<QString> {
          const auto& audio = loaded->audio;
          std::vector<float> left;
          std::vector<float> right;
          if (original) {
            left = audio.left;
            right = audio.right;
          } else {
            left.resize(audio.left.size());
            right.resize(audio.right.size());
            std::optional<media::CacheStore> cache;
            if (auto directory = pinned.reader.open_cache_directory())
              if (auto adopted = media::CacheStore::adopt_directory_fd(directory.value()))
                cache.emplace(std::move(adopted).value());
            auto rendered = source_clay::render_take(cache ? &*cache : nullptr, loaded->source,
                                                     take, audio, left, right);
            if (!rendered)
              return foundation::Result<QString>::failure(rendered.error());
          }
          std::vector<float> interleaved;
          interleaved.reserve((last - first) * 2);
          for (auto i = first; i < last; ++i) {
            interleaved.push_back(left[i]);
            interleaved.push_back(right.empty() ? left[i] : right[i]);
          }
          auto writer = exporting::WavWriter::begin(
              path.toStdString(),
              {audio.sample_rate, exporting::WavFormat::float32, last - first, 0});
          if (!writer)
            return foundation::Result<QString>::failure(writer.error());
          if (auto written = writer.value().write_interleaved_stereo(interleaved); !written)
            return foundation::Result<QString>::failure(written.error());
          if (auto finished = writer.value().finish(); !finished)
            return foundation::Result<QString>::failure(finished.error());
          return foundation::Result<QString>::success(path);
        }();
      },
      [](std::string what) {
        return foundation::Result<QString>::failure(
            {foundation::ErrorCode::internal, std::move(what)});
      });
  if (failed)
    return;
  timer_.start();
}

void ClayController::undo() {
  if (undo_)
    undo_();
}

void ClayController::redo() {
  if (redo_)
    redo_();
}

// ---- Test hooks ----

bool ClayController::wait_for_detect(int timeout_ms) {
  QElapsedTimer clock;
  clock.start();
  while (job_ && clock.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, poll_interval_ms);
    if (job_ && detect_.finished())
      poll();
  }
  return !job_;
}

bool ClayController::wait_for_render(int timeout_ms) {
  QElapsedTimer clock;
  clock.start();
  while ((warm_.running() || audition_.running()) && clock.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, poll_interval_ms);
    poll();
  }
  return !warm_.running() && !audition_.running();
}

} // namespace oscillate::quick
