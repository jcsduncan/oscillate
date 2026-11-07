#pragma once

#include "keyed_list_model.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/project/model.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <vector>

namespace oscillate::quick {

class NativeSynthDeviceController;

// The QML face of a synth's structured document (ADR-0057): modulation
// routes as a keyed list model, curves as point lists, effect chains, media
// slots, labels and sequences. Each part signals alone, so a curve drag
// repaints the curve editor and nothing else (ADR-0054). Edits are plain
// document transforms submitted whole through the panel adapter; a drag is
// begin_edit, edits, then commit_edit or cancel_edit, one undo step. Route
// amounts are the engine's flat parameters `<engine>.mod.<slot>.amount`,
// written through the device controller like any knob.
class NativeSynthDocumentModel final : public QObject {
  Q_OBJECT
  // Rows keyed by slot: {slot, source, aux, destination, bipolar, tension,
  // scale, amount_id}, in slot order.
  Q_PROPERTY(QObject* routes READ routes CONSTANT)
  Q_PROPERTY(int route_count READ route_count NOTIFY routes_changed)
  // Each increments when its part of the document changes; bindings read
  // them to refresh only what depends on that part.
  Q_PROPERTY(int routes_revision READ routes_revision NOTIFY routes_changed)
  Q_PROPERTY(int curves_revision READ curves_revision NOTIFY curves_changed)
  Q_PROPERTY(int chains_revision READ chains_revision NOTIFY chains_changed)
  Q_PROPERTY(int media_revision READ media_revision NOTIFY media_changed)
  Q_PROPERTY(int labels_revision READ labels_revision NOTIFY labels_changed)
  Q_PROPERTY(int sequences_revision READ sequences_revision NOTIFY sequences_changed)
  // Nova Analogue's painted Voice Lanes (ADR-0073), including undo and redo.
  Q_PROPERTY(int analogue_revision READ analogue_revision NOTIFY analogue_changed)
  Q_PROPERTY(bool editing READ editing NOTIFY editing_changed)

public:
  NativeSynthDocumentModel(NativeSynthPanelAdapter& adapter,
                           NativeSynthDeviceController& controller, QObject* parent = nullptr);

  [[nodiscard]] QObject* routes() noexcept { return &routes_; }
  [[nodiscard]] int route_count() const noexcept {
    return static_cast<int>(document_.routes.size());
  }
  [[nodiscard]] int routes_revision() const noexcept { return routes_revision_; }
  [[nodiscard]] int curves_revision() const noexcept { return curves_revision_; }
  [[nodiscard]] int chains_revision() const noexcept { return chains_revision_; }
  [[nodiscard]] int media_revision() const noexcept { return media_revision_; }
  [[nodiscard]] int labels_revision() const noexcept { return labels_revision_; }
  [[nodiscard]] int sequences_revision() const noexcept { return sequences_revision_; }
  [[nodiscard]] int analogue_revision() const noexcept { return analogue_revision_; }
  [[nodiscard]] bool editing() const noexcept { return editing_; }
  [[nodiscard]] const project::NativeSynthDocument& document() const noexcept { return document_; }

  // ---- Routes ----
  // The flat parameter holding `slot`'s amount.
  Q_INVOKABLE QString amount_id(int slot) const;
  Q_INVOKABLE QVariantMap route(int slot) const;
  // Every route into `destination`, oldest slot first.
  Q_INVOKABLE QVariantList routes_to(const QString& destination) const;
  Q_INVOKABLE int routes_from(const QString& source) const;
  // The slot a new route takes: the lowest above every slot in use, else the
  // lowest free one; -1 when all are taken.
  int next_free_slot() const;
  // Adds source -> destination with `amount` (-1..1) and returns its slot,
  // or the existing route's slot when the pair is already routed; -1 when
  // full or not editable. The amount is written first, then the document.
  Q_INVOKABLE int add_route(const QString& source, const QString& destination, double amount);
  // Removes the route and resets its amount, so the slot starts clean.
  Q_INVOKABLE bool remove_route(int slot);
  // Changes any of {source, aux, destination, bipolar, tension, scale}.
  Q_INVOKABLE bool set_route(int slot, const QVariantMap& changes);
  // Sets the polarity of every route from `source` (an LFO's BIPOLAR pill).
  Q_INVOKABLE void set_source_polarity(const QString& source, bool bipolar);

  // ---- Curves ----
  // Points [{x, y, tension, step}]; the registered default while the
  // document has none, else empty.
  Q_INVOKABLE QVariantList curve(const QString& id) const;
  Q_INVOKABLE bool has_curve(const QString& id) const;
  // What curve(id) shows, and edits start from, while the document has no
  // such curve, ahead of the engine's own default document. Panel-local:
  // nothing is submitted.
  Q_INVOKABLE void set_curve_default(const QString& id, const QVariantList& points);
  // Replaces the whole curve (a shape from the library). Points are sorted,
  // clamped and pinned to x = 0 and x = 1.
  Q_INVOKABLE bool set_curve(const QString& id, const QVariantList& points);
  // Drops the curve from the document, so what reads it falls back (Pump's
  // band and slot shapes follow their slot's again). False when absent.
  Q_INVOKABLE bool remove_curve(const QString& id);
  // Moves point `index`. The end points keep their x and, with `link_ends`,
  // share their y, so a cycle loops seamlessly (an LFO); without it each
  // end moves alone (Pump's duck starts low and ends high). Inner points
  // stay between their neighbours.
  Q_INVOKABLE bool move_curve_point(const QString& id, int index, double x, double y,
                                    bool link_ends = true);
  // Adds a point and returns its index, or -1.
  Q_INVOKABLE int insert_curve_point(const QString& id, double x, double y);
  Q_INVOKABLE bool remove_curve_point(const QString& id, int index);
  // The segment from point `index` to the next: its bend, -1..1, and kind.
  Q_INVOKABLE bool set_curve_tension(const QString& id, int index, double tension);
  Q_INVOKABLE bool set_curve_step(const QString& id, int index, bool step);

  // ---- Chains ----
  // Modules in order: [{uid, module}].
  Q_INVOKABLE QVariantList chain(const QString& id) const;
  // The lowest uid in 1..max_uid used by no chain, or -1.
  Q_INVOKABLE int free_chain_uid(int max_uid) const;
  Q_INVOKABLE bool move_chain_module(const QString& chain, int from, int to);
  // Moves the module at `from` in `from_chain` to position `to` (clamped to
  // the end) of `to_chain`, keeping its uid and so its values; makes the
  // target chain when it is missing; false when the target is full.
  Q_INVOKABLE bool move_chain_module_to(const QString& from_chain, int from,
                                        const QString& to_chain, int to);
  // Adds `module` at `index` (clamped to the end) of `chain` in slot `uid`.
  Q_INVOKABLE bool insert_chain_module(const QString& chain, int index, const QString& module,
                                       int uid);
  Q_INVOKABLE bool remove_chain_module(const QString& chain, int index);
  // Swaps the module type in place, keeping the uid.
  Q_INVOKABLE bool set_chain_module(const QString& chain, int index, const QString& module);

  // ---- Media, labels, sequences ----
  // {kind: "builtin" | "managed" | "", name, frame_length}; managed media
  // also carries {frames, sample_rate}.
  Q_INVOKABLE QVariantMap media(const QString& id) const;
  Q_INVOKABLE bool set_builtin_media(const QString& id, const QString& name, int frame_length);
  // The label's text, or `fallback` when the document has none.
  Q_INVOKABLE QString label(const QString& id, const QString& fallback) const;
  Q_INVOKABLE bool set_label(const QString& id, const QString& text);
  // {length (quarter notes), notes [{start, length (quarter notes), key,
  // velocity}]}; length 0 when the document has no such sequence.
  Q_INVOKABLE QVariantMap sequence(const QString& id) const;
  // Adds a note at (start, key), or removes the note that covers it.
  Q_INVOKABLE bool toggle_sequence_note(const QString& id, double start, int key, double length,
                                        int velocity, double sequence_length);
  // Sets the sequence's length in quarter notes, dropping notes that start
  // after it and shortening notes that run past it.
  Q_INVOKABLE bool set_sequence_length(const QString& id, double length);

  // ---- Nova Analogue: Voice Lanes (ADR-0073) ----
  // `kind` is "notes" or "unison"; `osc` 0..2 picks the unison stack (0 for
  // notes); `lane` is a lane id ("pitch", "cutoff", "env", "level", "pan",
  // "phase", "moda", "modb"; unison takes pitch, level, pan and phase).
  // Voices 0..count-1 as the engine plays them: [{value -1..1, edited}],
  // painted or from the seed (nova::analogue_lane_values). Empty for an
  // unknown kind or lane, or when the device is not Nova.
  Q_INVOKABLE QVariantList analogueLane(const QString& kind, int osc, const QString& lane,
                                        int count) const;
  // Paints one voice (clamped to -1..1). Inside begin_edit/commit_edit the
  // writes are one undo step.
  Q_INVOKABLE void setAnalogueLaneValue(const QString& kind, int osc, const QString& lane,
                                        int voice, double value);
  // Returns every voice of the lane to the seed.
  Q_INVOKABLE void clearAnalogueLane(const QString& kind, int osc, const QString& lane);

  // ---- Gestures ----
  Q_INVOKABLE void begin_edit();
  Q_INVOKABLE void commit_edit();
  Q_INVOKABLE void cancel_edit();

  // Adopts the adapter's document; signals only the parts that changed.
  void refresh();

signals:
  void routes_changed();
  void curves_changed();
  void chains_changed();
  void media_changed();
  void labels_changed();
  void sequences_changed();
  void analogue_changed();
  void editing_changed();

private:
  [[nodiscard]] bool editable() const;
  // Submits `next` as a single edit, or as an update inside a gesture.
  bool submit(project::NativeSynthDocument next);
  [[nodiscard]] project::NativeCurve working_curve(const QString& id) const;
  bool replace_curve(project::NativeCurve curve);
  [[nodiscard]] project::NativeChain* find_chain(project::NativeSynthDocument& document,
                                                 const QString& id) const;

  NativeSynthPanelAdapter& adapter_;
  NativeSynthDeviceController& controller_;
  project::NativeSynthDocument document_;
  KeyedListModel routes_;
  std::map<QString, std::vector<project::NativeCurvePoint>> default_curves_;
  // The engine's default document's curves, behind the panel's own defaults.
  std::map<QString, std::vector<project::NativeCurvePoint>> engine_curves_;
  int routes_revision_ = 0;
  int curves_revision_ = 0;
  int chains_revision_ = 0;
  int media_revision_ = 0;
  int labels_revision_ = 0;
  int sequences_revision_ = 0;
  int analogue_revision_ = 0;
  bool editing_ = false;
};

} // namespace oscillate::quick
