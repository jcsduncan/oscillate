#pragma once

#include "audio_import_controller.hpp"

#include "oscillate/application/midi_file_import.hpp"
#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace oscillate::quick {

// Audio and MIDI import (QML: projectBridge.imports). Audio decodes on the
// AudioImportController's worker and lands as one undo step; a MIDI file
// becomes new instrument tracks, or notes on the track it was dropped on.
class ImportController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString importStatus READ import_status NOTIFY changed)
  Q_PROPERTY(bool importBusy READ import_busy NOTIFY changed)
  Q_PROPERTY(bool importEnabled READ import_enabled NOTIFY changed)
  Q_PROPERTY(bool encodedImportAvailable READ encoded_import_available CONSTANT)

public:
  struct Host {
    std::function<application::ProjectSession*()> session;
    // No playback, export, media repair or close is under way.
    std::function<bool()> idle;
    // An audio edit or media verification holds the project.
    std::function<bool()> control_busy;
    // An audio import started: other jobs' gates and messages change.
    std::function<void()> started;
    std::function<void()> refresh;
    std::function<void()> submit_playback_edit;
    std::function<void(int)> track_created;
    std::function<std::uint64_t()> revision;
    std::function<void(QString)> status;
  };

  explicit ImportController(Host host, QObject* parent = nullptr);

  [[nodiscard]] AudioImportController& audio() noexcept { return audio_; }
  [[nodiscard]] const AudioImportController& audio() const noexcept { return audio_; }
  [[nodiscard]] QString import_status() const { return audio_.status(); }
  [[nodiscard]] bool import_busy() const noexcept { return audio_.busy(); }
  [[nodiscard]] bool import_enabled() const;
  [[nodiscard]] static bool encoded_import_available() noexcept;

  Q_INVOKABLE void choose_import_audio();
  Q_INVOKABLE void choose_import_audio_above_placeholder();
  Q_INVOKABLE void import_audio(const QUrl& source);
  Q_INVOKABLE void choose_import_midi();
  Q_INVOKABLE void import_midi(const QUrl& source);
  // A MIDI file dropped on an instrument track: its notes on that track from
  // the zero-based bar.
  Q_INVOKABLE void import_midi_to_track(const QUrl& source, const QString& track_id,
                                        int zero_based_bar);
  void notify() { emit changed(); }

signals:
  void changed();

private:
  void choose_import_audio_at(std::optional<std::size_t> insert_index);
  void import_audio_at(const QUrl& source, std::optional<std::size_t> insert_index);
  void import_midi_with(const QUrl& source, application::MidiImportOptions options);

  Host host_;
  AudioImportController audio_;
};

} // namespace oscillate::quick
