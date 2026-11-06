#pragma once

#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"

#include "oscillate/application/browser_preview.hpp"
#include "oscillate/application/knock_patterns.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::quick {

// Knock's Patterns area (ADR-0068, docs/ui/knock-mockup.html): the loop
// library, the generator (Roll, Mutate, lane locks, Evolve) and a working
// pattern the panel shows and edits. All of it is the panel's own state:
// nothing touches the project until write_to_clip(), which writes the
// pattern (evolved when Evolve is on) into the track's selected MIDI clip,
// or a new one, as one undo step. The patterns themselves are the
// application's pure knock_patterns functions; pads' roles follow the
// controller's values.
class KnockPatternsModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int lane_count READ lane_count CONSTANT)
  Q_PROPERTY(int steps_per_bar READ steps_per_bar CONSTANT)
  Q_PROPERTY(int steps_per_quarter READ steps_per_quarter CONSTANT)
  // The generator's styles, by name; its bar choices; Evolve's lengths in
  // bars (0 is off).
  Q_PROPERTY(QStringList style_names READ style_names CONSTANT)
  Q_PROPERTY(QVariantList bar_choices READ bar_choices CONSTANT)
  Q_PROPERTY(QVariantList evolve_choices READ evolve_choices CONSTANT)
  // A cell's kind by number, as cells() and the thumbnails give it:
  // "off", "hit", "accent", "ghost", "chance", "ratchet".
  Q_PROPERTY(QStringList cell_names READ cell_names CONSTANT)
  // The library: [{name, kit (the preset's name, empty for the thirty),
  // style (index into style_names), bars, thumb (the first bar's cells,
  // lane by lane, lane_count × steps_per_bar)}], kits' loops first. The
  // generated ones follow the pads' roles.
  Q_PROPERTY(QVariantList loops READ loops NOTIFY loops_changed)
  // Each pad's part from its sound: "low", "mid" or "high".
  Q_PROPERTY(QStringList roles READ roles NOTIFY loops_changed)
  // The loop last loaded (empty after a roll).
  Q_PROPERTY(QString loop_name READ loop_name NOTIFY changed)
  // Generator settings. Changing one reshapes the same roll (the seed stays),
  // keeping the locked lanes.
  Q_PROPERTY(int style READ style WRITE set_style NOTIFY changed)
  Q_PROPERTY(double density READ density WRITE set_density NOTIFY changed)
  Q_PROPERTY(double syncopation READ syncopation WRITE set_syncopation NOTIFY changed)
  Q_PROPERTY(double ghosts READ ghosts WRITE set_ghosts NOTIFY changed)
  Q_PROPERTY(double ratchets READ ratchets WRITE set_ratchets NOTIFY changed)
  Q_PROPERTY(double chance READ chance WRITE set_chance NOTIFY changed)
  Q_PROPERTY(double variation READ variation WRITE set_variation NOTIFY changed)
  Q_PROPERTY(int bars READ bars WRITE set_bars NOTIFY changed)
  Q_PROPERTY(int seed READ seed NOTIFY changed)
  // Bars Write to clip writes with Evolve (0: the loop as it is).
  Q_PROPERTY(int evolve_bars READ evolve_bars WRITE set_evolve_bars NOTIFY changed)
  // The working pattern: its bars, the bar the grid shows, that bar's cells
  // (lane by lane), each lane's lock, and how many bars a write makes.
  Q_PROPERTY(int pattern_bars READ pattern_bars NOTIFY changed)
  Q_PROPERTY(int view_bar READ view_bar WRITE set_view_bar NOTIFY changed)
  Q_PROPERTY(QVariantList cells READ cells NOTIFY changed)
  Q_PROPERTY(QVariantList locked READ locked NOTIFY changed)
  Q_PROPERTY(int written_bars READ written_bars NOTIFY changed)
  // Increments with every change to the working pattern or settings.
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  // What the last write or refusal said.
  Q_PROPERTY(QString message READ message NOTIFY changed)

public:
  // Rolled seeds stay in 1..max_seed: four digits on the seed chip.
  static constexpr std::uint32_t max_seed = 9'999;

  // `adapter` writes clips; null leaves the patterns unwritable (a preview).
  KnockPatternsModel(NativeSynthDeviceController& controller, NativeSynthPanelAdapter* adapter,
                     QObject* parent = nullptr);

  [[nodiscard]] static int lane_count() noexcept;
  [[nodiscard]] static int steps_per_bar() noexcept;
  [[nodiscard]] static int steps_per_quarter() noexcept;
  [[nodiscard]] static QStringList style_names();
  [[nodiscard]] static QVariantList bar_choices();
  [[nodiscard]] static QVariantList evolve_choices();
  [[nodiscard]] static QStringList cell_names();
  [[nodiscard]] QVariantList loops() const { return loops_; }
  [[nodiscard]] QStringList roles() const;
  [[nodiscard]] QString loop_name() const { return loop_name_; }
  [[nodiscard]] int style() const noexcept { return static_cast<int>(settings_.style); }
  void set_style(int style);
  [[nodiscard]] double density() const noexcept { return settings_.density; }
  void set_density(double value);
  [[nodiscard]] double syncopation() const noexcept { return settings_.syncopation; }
  void set_syncopation(double value);
  [[nodiscard]] double ghosts() const noexcept { return settings_.ghosts; }
  void set_ghosts(double value);
  [[nodiscard]] double ratchets() const noexcept { return settings_.ratchets; }
  void set_ratchets(double value);
  [[nodiscard]] double chance() const noexcept { return settings_.chance; }
  void set_chance(double value);
  [[nodiscard]] double variation() const noexcept { return settings_.variation; }
  void set_variation(double value);
  [[nodiscard]] int bars() const noexcept { return static_cast<int>(settings_.bars); }
  void set_bars(int bars);
  [[nodiscard]] int seed() const noexcept { return static_cast<int>(settings_.seed); }
  [[nodiscard]] int evolve_bars() const noexcept { return static_cast<int>(evolve_bars_); }
  void set_evolve_bars(int bars);
  [[nodiscard]] int pattern_bars() const noexcept { return static_cast<int>(pattern_.bars.size()); }
  [[nodiscard]] int view_bar() const noexcept { return static_cast<int>(view_bar_); }
  void set_view_bar(int bar);
  [[nodiscard]] QVariantList cells() const;
  [[nodiscard]] QVariantList locked() const;
  [[nodiscard]] int written_bars() const noexcept;
  [[nodiscard]] int revision() const noexcept { return revision_; }
  [[nodiscard]] QString message() const { return message_; }

  // Loads a library loop by name as the working pattern; its settings
  // become the generator's (a hand-written one keeps them). False for an
  // unknown name.
  Q_INVOKABLE bool load_loop(const QString& name);
  // A fresh seed's loop with the current settings, keeping locked lanes.
  Q_INVOKABLE void roll();
  // Changes a little of every unlocked lane.
  Q_INVOKABLE void mutate();
  Q_INVOKABLE void set_locked(int lane, bool locked);
  // The cell's kind (cell_names index) in bar `bar`; 0 out of range.
  Q_INVOKABLE int cell(int bar, int lane, int step) const;
  // A click on the grid: the shown bar's cell steps off, hit, accent,
  // ghost, chance, ratchet and round again.
  Q_INVOKABLE void cycle_cell(int lane, int step);
  // How many notes a write makes now (Evolve and chance included).
  Q_INVOKABLE int note_count() const;
  // Writes the pattern (evolved to evolve_bars when on): into the first of
  // `selected_clip_ids` that is a MIDI clip on this track, replacing its
  // notes and pattern length, else into a new clip at zero-based bar
  // `start_bar`. One undo step either way; false when refused (message
  // says why).
  Q_INVOKABLE bool write_to_clip(int start_bar, const QStringList& selected_clip_ids);

  // The working pattern's opening notes for the dry audition, at most
  // `limit` of them, at `bpm` and `sample_rate`.
  [[nodiscard]] std::vector<application::PreviewNote>
  audition_notes(double bpm, std::uint32_t sample_rate, std::size_t limit) const;
  [[nodiscard]] const application::knock_patterns::Pattern& pattern() const noexcept {
    return pattern_;
  }

signals:
  void changed();
  void loops_changed();

private:
  void follow_values();
  void rebuild_loops();
  void reshape(); // Re-rolls the current seed after a settings change.
  void touched();
  [[nodiscard]] application::knock_patterns::Pattern written() const;
  void refuse(QString text);

  NativeSynthDeviceController& controller_;
  NativeSynthPanelAdapter* adapter_;
  application::knock_patterns::Roles roles_{};
  application::knock_patterns::GeneratorSettings settings_ =
      application::knock_patterns::default_settings;
  application::knock_patterns::Pattern pattern_;
  application::knock_patterns::Locks locks_{};
  std::size_t evolve_bars_ = 0;
  std::size_t view_bar_ = 0;
  // Mutations so far, so each draws afresh.
  std::uint32_t mutations_ = 0;
  QVariantList loops_;
  QString loop_name_;
  QString message_;
  int revision_ = 0;
};

} // namespace oscillate::quick
