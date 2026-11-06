#pragma once

#include "oscillate/application/nova_morph.hpp"
#include "oscillate/application/sound_map.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace oscillate::quick {

class NativeSynthDeviceController;

// Nova's Constellation (ADR-0077, docs/ui/nova-redesign-mockup.html): every
// factory and user preset rendered and measured off the GUI thread, placed
// as a star by brightness (x) and length (y); dragging between stars morphs
// the patch live as one undo step; MUTATE scatters six measured variations;
// KEEP THIS saves the blend as a user preset, which becomes a star.
// Measurements are cached on disk by sound, so re-opening is instant and an
// edited user preset is the only one listened to again.
class NovaConstellation final : public QObject {
  Q_OBJECT
  // Placed sounds: [{key, name, category, user, variation, x, y, movement,
  // width}], each of x, y, movement and width 0..1 (y 1 is long).
  Q_PROPERTY(QVariantList stars READ stars NOTIFY stars_changed)
  // MUTATE's measured variations, as stars (variation true).
  Q_PROPERTY(QVariantList variations READ variations NOTIFY variations_changed)
  Q_PROPERTY(bool started READ started NOTIFY progress_changed)
  // Every sound listened to; analysed of total so far.
  Q_PROPERTY(bool ready READ ready NOTIFY progress_changed)
  Q_PROPERTY(int analysed READ analysed NOTIFY progress_changed)
  Q_PROPERTY(int total READ total NOTIFY progress_changed)
  Q_PROPERTY(bool mutating READ mutating NOTIFY variations_changed)
  Q_PROPERTY(bool morphing READ morphing NOTIFY morphing_changed)
  // The last morph's parts, heaviest first: [{key, name, category, weight}].
  Q_PROPERTY(QVariantList blend READ blend NOTIFY blend_changed)
  // The star applied exactly last ("" after a morph).
  Q_PROPERTY(QString selected READ selected NOTIFY selected_changed)
  // KEEP MINE: any of lock_fx, lock_envelopes, lock_routes.
  Q_PROPERTY(QStringList locks READ locks NOTIFY locks_changed)
  // Why the last request did nothing, or "".
  Q_PROPERTY(QString message READ message NOTIFY message_changed)

public:
  static constexpr std::string_view factory_prefix = "factory/";
  static constexpr std::string_view user_prefix = "user/";
  static constexpr std::string_view variation_prefix = "variation/";
  static constexpr std::string_view variation_name = "Variation ";
  static constexpr std::string_view blend_name = "Blend ";
  static constexpr std::string_view user_category = "User";
  static constexpr std::string_view lock_fx = "fx";
  static constexpr std::string_view lock_envelopes = "env";
  static constexpr std::string_view lock_routes = "mod";

  // `cache_file` holds the measurements (default_cache_file in the app).
  NovaConstellation(NativeSynthDeviceController& controller, QString cache_file,
                    QObject* parent = nullptr);
  NovaConstellation(const NovaConstellation&) = delete;
  NovaConstellation& operator=(const NovaConstellation&) = delete;
  ~NovaConstellation() override;

  // $XDG_CACHE_HOME (else ~/.cache)/oscillate/constellation/nova.json.
  [[nodiscard]] static QString default_cache_file();

  [[nodiscard]] QVariantList stars() const { return stars_; }
  [[nodiscard]] QVariantList variations() const { return variation_stars_; }
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] bool ready() const noexcept { return started_ && analysed_ == total_; }
  [[nodiscard]] int analysed() const noexcept { return analysed_; }
  [[nodiscard]] int total() const noexcept { return total_; }
  [[nodiscard]] bool mutating() const noexcept { return mutating_; }
  [[nodiscard]] bool morphing() const noexcept { return morphing_; }
  [[nodiscard]] QVariantList blend() const { return blend_; }
  [[nodiscard]] QString selected() const { return selected_; }
  [[nodiscard]] QStringList locks() const;
  [[nodiscard]] QString message() const { return message_; }

  // Starts listening the first time (the view calls it when it opens).
  Q_INVOKABLE void start();
  // Looks at the presets again: new or edited ones are listened to.
  void rescan();
  // A drag over the sky: begin, then morph with the cursor's nearest stars
  // [{key, weight}] (up to three, weights summing to 1) as often as it moves,
  // then end; one undo step. A morph outside a drag is one edit.
  Q_INVOKABLE void begin_morph();
  Q_INVOKABLE bool morph(const QVariantList& points);
  Q_INVOKABLE void end_morph();
  Q_INVOKABLE void set_lock(const QString& name, bool locked);
  Q_INVOKABLE bool is_locked(const QString& name) const;
  // Six seeded variations around the sound now, measured; they appear in
  // `variations` as they land.
  Q_INVOKABLE void mutate();
  // Loads a star exactly: a factory or user preset, or a variation.
  Q_INVOKABLE bool apply_star(const QString& key);
  // Saves the sound now as a user preset (`name`, or the next free "Blend
  // N" when empty); returns the name, or "" when it could not.
  Q_INVOKABLE QString keep(const QString& name);
  Q_INVOKABLE QString next_blend_name() const;

signals:
  void stars_changed();
  void variations_changed();
  void progress_changed();
  void morphing_changed();
  void blend_changed();
  void selected_changed();
  void locks_changed();
  void message_changed();

private:
  struct Entry {
    QString key;
    QString name;
    QString category;
    bool user = false;
    int preset = -1; // Factory preset index, else -1.
    application::sound_map::Sound sound;
    application::sound_map::Register phrase = application::sound_map::Register::other;
    std::uint64_t hash = 0;
    bool done = false; // Listened to (features stays empty when it failed).
    std::optional<application::sound_map::Features> features;
  };

  [[nodiscard]] const Entry* find(const QString& key) const;
  [[nodiscard]] application::sound_map::Sound current_sound() const;
  [[nodiscard]] QVariantMap star(const Entry& entry) const;
  void set_message(const QString& message);
  void landed(std::uint64_t generation, std::size_t index,
              std::optional<application::sound_map::Features> features);
  void landed_variation(std::uint64_t generation, std::size_t index,
                        std::optional<application::sound_map::Features> features);
  void place();
  void listen(std::vector<std::size_t> pending);

  NativeSynthDeviceController& controller_;
  const native_dsp::synth::SynthEngine& engine_;
  QString cache_file_;
  std::vector<Entry> entries_;
  std::vector<Entry> variations_;
  application::sound_map::Sky sky_;
  QVariantList stars_;
  QVariantList variation_stars_;
  QVariantList blend_;
  QString selected_;
  QString message_;
  // The category the sound now came from: its phrase for variations.
  QString category_;
  application::nova_morph::Locks locks_;
  // The sound when the drag began (what locks keep) and the document last
  // sent during it.
  application::sound_map::Sound base_;
  project::NativeSynthDocument sent_document_;
  bool started_ = false;
  bool morphing_ = false;
  bool mutating_ = false;
  int analysed_ = 0;
  int total_ = 0;
  int variations_landed_ = 0;
  std::uint64_t mutations_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t variation_generation_ = 0;
  // Last, so they stop and join before anything they report into goes.
  std::jthread library_worker_;
  std::jthread variation_worker_;
};

} // namespace oscillate::quick
