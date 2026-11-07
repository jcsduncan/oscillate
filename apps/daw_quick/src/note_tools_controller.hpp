#pragma once

#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/midi_velocity_edit.hpp"
#include "oscillate/application/project_session.hpp"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::quick {

// The piano roll's musical tools (ADR-0059): scale lock, chord stamps,
// strum, humanise, quantise with strength and swing, the groove pool, and
// the pedal strip (ADR-0035) and velocity lane beneath the roll.
// The maths is application::midi_note_tools; every edit is one undo step
// through NoteService::apply_note_edit. An empty note list means the
// whole clip. Scale lock is a view setting, not saved.
class NoteToolsController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool scaleLock READ scale_lock WRITE set_scale_lock NOTIFY scaleLockChanged)
  Q_PROPERTY(QVariantList grooves READ grooves NOTIFY groovesChanged)
  Q_PROPERTY(QStringList chordShapes READ chord_shapes CONSTANT)
  // The piano roll's grids as steps per bar (application::midi_grid_steps_per_bar).
  Q_PROPERTY(QList<int> gridDivisions READ grid_divisions CONSTANT)
  // A 4/4 bar in project ticks, which every grid divides.
  Q_PROPERTY(double barTicks READ bar_ticks CONSTANT)
  // The groove pool's choice and amounts, shared by the piano roll's tools
  // and the clip menu (view settings, not saved).
  Q_PROPERTY(QString currentGroove MEMBER current_groove_ NOTIFY grooveSettingsChanged)
  Q_PROPERTY(double grooveQuantize MEMBER groove_quantize_ NOTIFY grooveSettingsChanged)
  Q_PROPERTY(double grooveTiming MEMBER groove_timing_ NOTIFY grooveSettingsChanged)
  Q_PROPERTY(double grooveVelocity MEMBER groove_velocity_ NOTIFY grooveSettingsChanged)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  // Plays an edited note at its new velocity (the dry preview).
  using Audition =
      std::function<void(const QString& clip_id, const QString& note_id, std::uint8_t velocity)>;

  NoteToolsController(Session session, CanEdit can_edit, Report report, Commit commit,
                      QObject* parent = nullptr);

  [[nodiscard]] bool scale_lock() const noexcept { return scale_lock_; }
  void set_scale_lock(bool locked);
  [[nodiscard]] QVariantList grooves() const;
  [[nodiscard]] static QStringList chord_shapes();
  [[nodiscard]] static QList<int> grid_divisions();
  [[nodiscard]] static double bar_ticks();

  // The key a drawn or dragged note lands on: snapped into the song's key
  // while Scale lock is on (and a key is set), else unchanged.
  Q_INVOKABLE int lock_key(int key) const;
  // The chord a stamp would draw from `root` (for the ghost under the mouse).
  Q_INVOKABLE QVariantList chord_preview(int root, const QString& shape, int inversion) const;
  // Draws a chord from `root` at grid `step`, `length_steps` long. Returns
  // the new notes' ids (empty when nothing was drawn).
  Q_INVOKABLE QStringList stamp_chord(const QString& clip_id, int step, int root, int steps_per_bar,
                                      const QString& shape, int inversion, int length_steps);
  // Scale lock's move: notes move by whole grid steps and by the scale
  // degrees between the grabbed note's key and the key it was dropped on.
  Q_INVOKABLE void move_notes_in_scale(const QString& clip_id, const QStringList& note_ids,
                                       int step_delta, int from_key, int to_key, int steps_per_bar);
  // Where move_notes_in_scale would put a note on `key` (for the drag ghost).
  Q_INVOKABLE int moved_key_in_scale(int key, int from_key, int to_key) const;
  // Spreads each chord's tones `amount` 128th notes apart.
  Q_INVOKABLE void strum(const QString& clip_id, const QStringList& note_ids, int amount,
                         bool upward);
  // Timing (0..1: up to a 32nd note either way) and velocity (0..1)
  // variation, one undo step; another click gives another variation.
  Q_INVOKABLE void humanize(const QString& clip_id, const QStringList& note_ids, double timing,
                            double velocity);
  Q_INVOKABLE void quantize(const QString& clip_id, const QStringList& note_ids, int steps_per_bar,
                            double strength, double swing, bool ends);
  Q_INVOKABLE void apply_groove(const QString& clip_id, const QStringList& note_ids,
                                const QString& name, double quantize, double timing,
                                double velocity);
  // The current groove at the current amounts.
  Q_INVOKABLE void apply_current_groove(const QString& clip_id, const QStringList& note_ids);
  // Keeps `groove` with the user's grooves under its name (cleaned for a
  // file name) and makes it the current groove. Grooves from a record
  // (GrooveCaptureController, ADR-0079) are kept here.
  bool keep_groove(persistence::GrooveTemplate groove);
  // A groove in the pool by name (built-in or the user's).
  [[nodiscard]] std::optional<persistence::GrooveTemplate> find_groove(const QString& name) const;
  Q_INVOKABLE bool delete_groove(const QString& name);
  Q_INVOKABLE bool import_groove(const QString& path);
  Q_INVOKABLE bool export_groove(const QString& name, const QString& path);
  [[nodiscard]] QString groove_directory() const;
  void set_audition(Audition audition) { audition_ = std::move(audition); }

  // The pedal strip (ADR-0035). Pedals are "sustain", "sostenuto" or "soft".
  // Painting takes grid steps; span edits name the span by a tick inside it
  // and take tick positions the strip has already snapped to its grid.
  Q_INVOKABLE void paint_pedal_grid(const QString& clip_id, const QString& pedal, int from_step,
                                    int to_step, double depth, int steps_per_bar);
  Q_INVOKABLE void move_pedal_span(const QString& clip_id, const QString& pedal, double at_ticks,
                                   double start_ticks, double end_ticks);
  Q_INVOKABLE void set_pedal_depth(const QString& clip_id, const QString& pedal, double at_ticks,
                                   double depth);
  Q_INVOKABLE void remove_pedal_at(const QString& clip_id, const QString& pedal, double at_ticks);
  Q_INVOKABLE void pedal_per_chord(const QString& clip_id);
  Q_INVOKABLE void clear_pedals(const QString& clip_id);
  // The velocity lane. `note_ids` is the edit's scope (the selected note or
  // pad, or the whole clip, as the lane decides). Humanise and randomise
  // draw a fresh variation on every call; each call is one undo step.
  Q_INVOKABLE void set_note_velocity(const QString& clip_id, const QString& note_id, int velocity);
  Q_INVOKABLE void draw_velocity_line(const QString& clip_id, const QStringList& note_ids,
                                      double from_ticks, double from_velocity, double to_ticks,
                                      double to_velocity);
  Q_INVOKABLE void humanize_velocities(const QString& clip_id, const QStringList& note_ids,
                                       double strength);
  Q_INVOKABLE void randomize_velocities(const QString& clip_id, const QStringList& note_ids,
                                        int minimum, int maximum);
  // New notes take this velocity; the lane sets it to the last value used.
  Q_INVOKABLE int new_note_velocity() const;

signals:
  void scaleLockChanged();
  void groovesChanged();
  void grooveSettingsChanged();

private:
  struct Target {
    application::ProjectSession* session = nullptr;
    const project::MidiClip* clip = nullptr;
    std::vector<project::NoteId> scope;
  };
  [[nodiscard]] std::optional<Target> target(const QString& clip_id, const QStringList& note_ids,
                                             const QString& action);
  [[nodiscard]] std::optional<project::MusicalKey> song_key() const;
  [[nodiscard]] std::vector<persistence::GrooveTemplate> user_grooves() const;
  // One pedal-strip or velocity edit with the shared guards, status and refresh.
  using PedalEdit = std::function<foundation::Result<void>(project::ClipId, project::MidiPedal)>;
  bool apply_pedal_edit(const QString& clip_id, const QString& pedal, const PedalEdit& edit);
  using VelocityEdit = std::function<std::vector<application::VelocityChange>(
      const project::MidiClip&, const std::vector<project::NoteId>&)>;
  bool apply_velocity_edit(const QString& clip_id, const QStringList& note_ids,
                           const VelocityEdit& edit);
  bool commit(application::ProjectSession& session, project::ClipId clip,
              std::span<const project::MidiNote> changed, std::span<const project::MidiNote> added,
              const QString& action, const QString& done,
              std::vector<project::NoteId>* created = nullptr);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  bool scale_lock_ = false;
  QString current_groove_ = QStringLiteral("MPC 16ths 58%");
  double groove_quantize_ = 1.0;
  double groove_timing_ = 1.0;
  double groove_velocity_ = 0.5;
  std::uint64_t seed_ = 0x6E0F'7E5DULL; // Advanced by every humanise.
  // Advanced by every velocity humanise or randomise.
  std::uint64_t velocity_seed_ = 0x5EEDULL;
  Audition audition_;
};

} // namespace oscillate::quick
