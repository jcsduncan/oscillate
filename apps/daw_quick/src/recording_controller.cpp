#include "recording_controller.hpp"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <utility>

namespace oscillate::quick {

namespace {
// Undo gestures for takes, apart from other controllers' ("REC" high bytes).
constexpr std::uint64_t take_gesture_base = 0x5245'4300'0000'0000ULL;
constexpr int max_midi_value = 127;
const QString click_setting = QStringLiteral("recording/click");
const QString count_in_setting = QStringLiteral("recording/countIn");
} // namespace

RecordingController::RecordingController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)) {
  QSettings settings;
  click_ = settings.value(click_setting, true).toBool();
  count_in_ = settings.value(count_in_setting, true).toBool();
  apply_click();
}

// The click sounds while recording (armed, counting in included) with the
// Click toggle on; plain playback never clicks.
void RecordingController::apply_click() {
  if (hooks_.click_changed)
    hooks_.click_changed(click_ && armed_);
}

bool RecordingController::counting_in() const {
  return armed_ && hooks_.device_counting_in && hooks_.device_counting_in();
}

bool RecordingController::recording() const {
  return armed_ && !counting_in() && hooks_.playing && hooks_.playing();
}

void RecordingController::set_click(bool on) {
  if (on == click_)
    return;
  click_ = on;
  QSettings{}.setValue(click_setting, on);
  apply_click();
  emit changed();
}

void RecordingController::set_count_in(bool on) {
  if (on == count_in_)
    return;
  count_in_ = on;
  QSettings{}.setValue(count_in_setting, on);
  emit changed();
}

void RecordingController::toggle_record() {
  if (armed_) {
    end_take();
    armed_ = false;
    apply_click();
    emit changed();
    return;
  }
  if (!hooks_.session || hooks_.session() == nullptr)
    return;
  armed_ = true;
  apply_click();
  gesture_ = take_gesture_base + next_gesture_++;
  take_clips_.clear();
  const bool playing = hooks_.playing && hooks_.playing();
  if (!playing && hooks_.play)
    hooks_.play(count_in_);
  emit changed();
}

std::optional<std::int64_t> RecordingController::tick_at(std::uint64_t sample) const {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr)
    return std::nullopt;
  const auto& state = session->snapshot();
  const auto tick =
      state.tempo.sample_to_tick({static_cast<std::int64_t>(sample)}, state.sample_rate);
  return tick ? std::optional{tick.value().value} : std::nullopt;
}

std::uint64_t RecordingController::now_sample(std::uint64_t start) const {
  const auto heard = hooks_.heard_sample ? hooks_.heard_sample() : std::nullopt;
  if (!heard)
    return start;
  // The loop wrapped while the key was down: the note ends at the loop end.
  if (*heard < start) {
    const auto loop = hooks_.loop ? hooks_.loop() : std::nullopt;
    return loop && loop->end > start ? loop->end : start;
  }
  return *heard;
}

void RecordingController::note_on(const QString& track_id, int key, int velocity) {
  if (key < 0 || key > max_midi_value || !recording())
    return;
  const auto track = project::TrackId::parse(track_id.toStdString());
  const auto heard = hooks_.heard_sample ? hooks_.heard_sample() : std::nullopt;
  if (!track || !heard)
    return;
  // A key struck again while held ends the first note there.
  if (const auto found = held_.find(key); found != held_.end()) {
    const auto held = found->second;
    held_.erase(found);
    finish(key, held, now_sample(held.start_sample));
  }
  held_.insert_or_assign(key,
                         Held{track.value(), *heard,
                              static_cast<std::uint8_t>(std::clamp(velocity, 1, max_midi_value))});
  emit heldChanged();
}

void RecordingController::note_off(int key) {
  const auto found = held_.find(key);
  if (found == held_.end())
    return;
  const auto held = found->second;
  held_.erase(found);
  finish(key, held, now_sample(held.start_sample));
  emit heldChanged();
}

void RecordingController::finish(int key, const Held& held, std::uint64_t end_sample) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr)
    return;
  const auto start = tick_at(held.start_sample);
  const auto end = tick_at(end_sample);
  if (!start || !end)
    return;
  // A tap shorter than a tick still sounds: a 32nd at least.
  constexpr std::int64_t shortest = timeline::ticks_per_quarter / 8;
  const std::array notes{application::RecordedNote{{*start},
                                                   {std::max(*end, *start + shortest)},
                                                   static_cast<std::uint8_t>(key),
                                                   held.velocity}};
  const auto take = take_clips_.find(held.track);
  auto written = session->notes().record_notes(
      held.track, notes, take != take_clips_.end() ? std::optional{take->second} : std::nullopt,
      gesture_);
  if (!written) {
    if (hooks_.status)
      hooks_.status(
          QStringLiteral("Recording: %1").arg(QString::fromStdString(written.error().message)));
    return;
  }
  if (written.value())
    take_clips_.insert_or_assign(held.track, *written.value());
  if (hooks_.commit)
    hooks_.commit();
}

void RecordingController::end_take() {
  for (const auto& [key, held] : std::exchange(held_, {}))
    finish(key, held, now_sample(held.start_sample));
  take_clips_.clear();
  emit heldChanged();
}

void RecordingController::transport_changed() {
  const bool playing = hooks_.playing && hooks_.playing();
  const bool now = recording();
  // Stop (or pause) ends the take and disarms; held keys end where it stopped.
  if (armed_ && !playing && !counting_in() && was_recording_) {
    end_take();
    armed_ = false;
    apply_click();
  }
  if (now != was_recording_ || !armed_)
    emit changed();
  was_recording_ = now;
  if (!held_.empty())
    emit heldChanged();
}

void RecordingController::stopped() {
  if (!armed_)
    return;
  end_take();
  armed_ = false;
  apply_click();
  was_recording_ = false;
  emit changed();
}

QVariantList RecordingController::held_notes() const {
  QVariantList out;
  if (!hooks_.bar_at)
    return out;
  for (const auto& [key, held] : held_) {
    out.push_back(
        QVariantMap{{QStringLiteral("trackId"), QString::fromStdString(held.track.to_string())},
                    {QStringLiteral("startBar"), hooks_.bar_at(held.start_sample)},
                    {QStringLiteral("endBar"), hooks_.bar_at(now_sample(held.start_sample))},
                    {QStringLiteral("key"), key}});
  }
  return out;
}

QVariantList RecordingController::clip_held_notes(const QString& clip_id) const {
  QVariantList out;
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto id = project::ClipId::parse(clip_id.toStdString());
  if (session == nullptr || !id || held_.empty())
    return out;
  const auto& state = session->snapshot();
  for (const auto& track : state.tracks) {
    const auto clip = std::ranges::find(track.midi_clips, id.value(), &project::MidiClip::id);
    if (clip == track.midi_clips.end())
      continue;
    const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement);
    if (musical == nullptr)
      return out;
    const auto pattern = clip->pattern_length.value;
    for (const auto& [key, held] : held_) {
      if (held.track != track.id)
        continue;
      const auto start = tick_at(held.start_sample);
      const auto end = tick_at(now_sample(held.start_sample));
      if (!start || !end || *start < musical->start.value || *start >= musical->end.value ||
          pattern <= 0)
        continue;
      auto local = *start - musical->start.value;
      if (clip->loop)
        local %= pattern;
      out.push_back(
          QVariantMap{{QStringLiteral("startTicks"), static_cast<qlonglong>(local)},
                      {QStringLiteral("endTicks"),
                       static_cast<qlonglong>(std::min(local + (*end - *start), pattern))},
                      {QStringLiteral("key"), key}});
    }
    return out;
  }
  return out;
}

} // namespace oscillate::quick
