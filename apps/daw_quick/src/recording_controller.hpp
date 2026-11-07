#pragma once

#include "oscillate/application/midi_recording.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/project/model.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace oscillate::quick {

// MIDI recording (ADR-0086): Record arms a take; notes played on the
// computer keyboard or the piano roll's keys while the song plays are
// placed where the song was heard and written into the track they play
// (NoteService::record_notes), each as it is let go, the whole take one
// undo step. Held notes are drawn growing until they end. The click and
// count-in toggles live here too; the bridge applies them to the device.
class RecordingController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool armed READ armed NOTIFY changed)
  // Armed and the song playing past any count-in: notes are being taken.
  Q_PROPERTY(bool recording READ recording NOTIFY changed)
  Q_PROPERTY(bool countingIn READ counting_in NOTIFY changed)
  Q_PROPERTY(bool click READ click WRITE set_click NOTIFY changed)
  Q_PROPERTY(bool countIn READ count_in WRITE set_count_in NOTIFY changed)
  // Held notes, growing: {trackId, startBar, endBar, key} in zero-based bars.
  Q_PROPERTY(QVariantList heldNotes READ held_notes NOTIFY heldChanged)

public:
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    // The song is playing (not stopped, paused or counting in).
    std::function<bool()> playing;
    // The project sample being heard now.
    std::function<std::optional<std::uint64_t>()> heard_sample;
    // The transport's loop while it is on.
    std::function<std::optional<application::LoopSamples>()> loop;
    // Starts playback from the playhead, after a count-in when asked.
    std::function<void(bool count_in)> play;
    // Whether the device is still counting in.
    std::function<bool()> device_counting_in;
    // Whether the click sounds now: the Click toggle, while recording.
    std::function<void(bool on)> click_changed;
    // Zero-based bar of a project sample, for drawing.
    std::function<double(std::uint64_t sample)> bar_at;
    // After the project changed here: refresh the views and playback.
    std::function<void()> commit;
    std::function<void(QString)> status;
  };

  explicit RecordingController(Hooks hooks, QObject* parent = nullptr);

  [[nodiscard]] bool armed() const noexcept { return armed_; }
  [[nodiscard]] bool recording() const;
  [[nodiscard]] bool counting_in() const;
  [[nodiscard]] bool click() const noexcept { return click_; }
  void set_click(bool on);
  [[nodiscard]] bool count_in() const noexcept { return count_in_; }
  void set_count_in(bool on);
  [[nodiscard]] QVariantList held_notes() const;

  // Record: arms a take (and starts the song when stopped, after a
  // count-in if on), or ends it.
  Q_INVOKABLE void toggle_record();
  // A key played and let go on `track_id`'s instrument.
  Q_INVOKABLE void note_on(const QString& track_id, int key, int velocity);
  Q_INVOKABLE void note_off(int key);
  // Held notes inside the MIDI clip `clip_id`, in its ticks (a looping
  // clip folds them into its pattern): {startTicks, endTicks, key}.
  Q_INVOKABLE QVariantList clip_held_notes(const QString& clip_id) const;

  // The bridge: playback started, moved or paused.
  void transport_changed();
  // The bridge: Stop. The take ends, held keys where the song stopped, and
  // Record disarms.
  void stopped();

signals:
  void changed();
  void heldChanged();

private:
  void apply_click();
  struct Held {
    project::TrackId track;
    std::uint64_t start_sample;
    std::uint8_t velocity;
  };
  // Ends every held note now and forgets the take.
  void end_take();
  void finish(int key, const Held& held, std::uint64_t end_sample);
  [[nodiscard]] std::optional<std::int64_t> tick_at(std::uint64_t sample) const;
  [[nodiscard]] std::uint64_t now_sample(std::uint64_t start) const;

  Hooks hooks_;
  bool armed_ = false;
  bool click_ = true;
  bool count_in_ = true;
  bool was_recording_ = false;
  std::map<int, Held> held_;
  // The take's own clip on each track, and its undo gesture.
  std::map<project::TrackId, project::ClipId> take_clips_;
  std::uint64_t gesture_ = 0;
  std::uint64_t next_gesture_ = 1;
};

} // namespace oscillate::quick
