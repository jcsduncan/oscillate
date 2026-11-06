#pragma once

#include "oscillate/application/arrange_from_loop.hpp"
#include "status_text.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace oscillate::application {
class ProjectSession;
}

namespace oscillate::quick {

// Arrange from a loop (ADR-0080, docs/ui/creative-tools-mockup.html#arrange)
// for the QML view. open() takes the loop brace's bars; every option change
// plans again at once (the rule engine is pure and fast). The proposal never
// touches the project until it is heard or kept: listen(true) applies it as
// one undo step so the transport plays it, listen(false) and bin() undo
// that step, and keep() leaves it applied (applying it first if needed).
class ArrangeController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool open READ is_open NOTIFY changed)
  Q_PROPERTY(bool ready READ ready NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(bool listening READ listening NOTIFY changed)
  Q_PROPERTY(int loopStartBar READ loop_start_bar NOTIFY changed)
  Q_PROPERTY(int loopBars READ loop_bars NOTIFY changed)
  Q_PROPERTY(int style READ style NOTIFY changed)
  Q_PROPERTY(int lengthBars READ length_bars NOTIFY changed)
  Q_PROPERTY(int minLengthBars READ min_length_bars NOTIFY changed)
  Q_PROPERTY(int maxLengthBars READ max_length_bars CONSTANT)
  Q_PROPERTY(QString lengthText READ length_text NOTIFY changed)
  Q_PROPERTY(double bpm READ bpm NOTIFY changed)
  Q_PROPERTY(int edgeBars READ edge_bars NOTIFY changed)
  Q_PROPERTY(double breakdown READ breakdown NOTIFY changed)
  Q_PROPERTY(double variation READ variation NOTIFY changed)
  Q_PROPERTY(int seed READ seed NOTIFY changed)
  Q_PROPERTY(bool transitions READ transitions NOTIFY changed)
  Q_PROPERTY(QVariantList templates READ templates CONSTANT)
  Q_PROPERTY(QVariantList loopChoices READ loop_choices CONSTANT)
  Q_PROPERTY(QVariantList edgeChoices READ edge_choices CONSTANT)
  Q_PROPERTY(QStringList roleNames READ role_names CONSTANT)
  Q_PROPERTY(int phraseBars READ phrase_bars CONSTANT)
  Q_PROPERTY(int changeBars READ change_bars CONSTANT)
  // [{id, name, role, roleIndex, detected, evidence, locked, unsupported,
  //   hasContent, arranged, runs: [{start, end}], fills: [bar]}]
  Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
  // [{index, kind, name, start, bars}]
  Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)
  // [{kind, label, items: [{start, end, label, detail}]}]
  Q_PROPERTY(QVariantList lanes READ lanes NOTIFY changed)
  // [{bar, status: "held" | "added" | "failed", reason}]
  Q_PROPERTY(QVariantList checks READ checks NOTIFY changed)
  // [{text, holds}]
  Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
  // [{bar, text}]
  Q_PROPERTY(QVariantList decisions READ decisions NOTIFY changed)

public:
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    // Whether the project may change now (reports why not).
    std::function<bool()> can_edit;
    // After the project changed here: refresh the views and playback.
    std::function<void()> commit;
    // The loop brace as one-based [start, end) bars, when it is on.
    std::function<std::optional<std::pair<int, int>>()> loop;
  };

  explicit ArrangeController(Hooks hooks, QObject* parent = nullptr);

  // Takes the loop brace (or the first 16 bars) and plans.
  Q_INVOKABLE void open();
  // Leaves the view: a proposal being heard is binned.
  Q_INVOKABLE void close();
  Q_INVOKABLE void set_loop_bars(int bars);
  Q_INVOKABLE void set_style(int style);
  Q_INVOKABLE void set_length_bars(int bars);
  Q_INVOKABLE void set_edge_bars(int bars);
  Q_INVOKABLE void set_breakdown(double position);
  Q_INVOKABLE void set_variation(double amount);
  Q_INVOKABLE void set_transitions(bool on);
  Q_INVOKABLE void toggle_lock(const QString& track_id);
  // -1 goes back to the role found.
  Q_INVOKABLE void set_role(const QString& track_id, int role);
  // Moves the start of section `index` to `bar` (snapped to 8 bars).
  Q_INVOKABLE void move_section_edge(int index, int bar);
  Q_INVOKABLE void regenerate();
  Q_INVOKABLE void listen(bool on);
  Q_INVOKABLE void keep();
  Q_INVOKABLE void bin();

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] bool ready() const noexcept { return plan_.has_value(); }
  [[nodiscard]] QString error() const { return error_; }
  [[nodiscard]] QString status() const { return status_.text(); }
  [[nodiscard]] bool listening() const noexcept { return applied_revision_.has_value(); }
  [[nodiscard]] int loop_start_bar() const noexcept;
  [[nodiscard]] int loop_bars() const noexcept;
  [[nodiscard]] int style() const noexcept;
  [[nodiscard]] int length_bars() const noexcept;
  [[nodiscard]] int min_length_bars() const noexcept;
  [[nodiscard]] int max_length_bars() const noexcept;
  [[nodiscard]] QString length_text() const;
  [[nodiscard]] double bpm() const;
  [[nodiscard]] int edge_bars() const noexcept;
  [[nodiscard]] double breakdown() const noexcept { return options_.breakdown; }
  [[nodiscard]] double variation() const noexcept { return options_.variation; }
  [[nodiscard]] int seed() const noexcept { return static_cast<int>(options_.seed); }
  [[nodiscard]] bool transitions() const noexcept { return options_.transitions; }
  [[nodiscard]] QVariantList templates() const;
  [[nodiscard]] QVariantList loop_choices() const;
  [[nodiscard]] QVariantList edge_choices() const;
  [[nodiscard]] QStringList role_names() const;
  [[nodiscard]] int phrase_bars() const noexcept;
  [[nodiscard]] int change_bars() const noexcept;
  [[nodiscard]] QVariantList tracks() const;
  [[nodiscard]] QVariantList sections() const;
  [[nodiscard]] QVariantList lanes() const;
  [[nodiscard]] QVariantList checks() const;
  [[nodiscard]] QVariantList rules() const;
  [[nodiscard]] QVariantList decisions() const;

  // The project's revision moved (or another project opened).
  void project_changed();
  [[nodiscard]] const std::optional<application::arrange::Plan>& plan() const noexcept {
    return plan_;
  }

signals:
  void changed();
  // A menu or the palette asked for the view.
  void openRequested();
  // Kept or binned: the view can go.
  void closeRequested();

private:
  // Plans again from the project as it is without the proposal.
  void replan();
  // Withdraws a proposal being heard, plans, and plays the new one.
  void update();
  [[nodiscard]] bool apply();
  [[nodiscard]] bool withdraw();
  void set_status(QString status);
  [[nodiscard]] application::ProjectSession* session() const;

  Hooks hooks_;
  application::arrange::Options options_;
  std::optional<application::arrange::Plan> plan_;
  // The revision the heard proposal made, while it is the project's.
  std::optional<std::uint64_t> applied_revision_;
  bool open_ = false;
  bool busy_ = false;
  QString error_;
  StatusText status_;
};

} // namespace oscillate::quick
