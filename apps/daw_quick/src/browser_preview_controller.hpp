#pragma once

#include "sample_library.hpp"
#include "sample_preview.hpp"

#include "oscillate/application/instrument_playback.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace oscillate::quick {

// The browser's previews (ADR-0059): the auto-preview switch, the user's
// sample folder and its cards, and the sample preview itself. Instrument
// presets preview through the bridge's audition; this owns everything a
// sample needs.
class BrowserPreviewController final : public QObject {
  Q_OBJECT
  // Selecting or hovering a card previews it (saved per user).
  Q_PROPERTY(bool autoPreview READ auto_preview WRITE set_auto_preview NOTIFY autoPreviewChanged)
  Q_PROPERTY(QString sampleFolder READ sample_folder NOTIFY samplesChanged)
  // One card per audio file, in BrowserCatalogue's card shape with
  // source "file", plus `path` and `url`.
  Q_PROPERTY(QVariantList samples READ samples NOTIFY samplesChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY samplesChanged)
  // The card key of the sample previewing now, or "".
  Q_PROPERTY(QString playingKey READ playing_key NOTIFY playingChanged)

public:
  // The song's rate and tempo now; absent without a project.
  struct Song {
    std::uint32_t sample_rate = 48'000;
    double bpm = 120.0;
    // Present while the transport plays: loops then start on its beat.
    std::optional<SamplePreview::BeatClock> clock;
  };
  using SongNow = std::function<std::optional<Song>()>;
  // The bridge's preview device choice (ProjectBridge::preview_output).
  using Output =
      std::function<bool(application::PlaybackOutputFactory&, std::optional<std::uint64_t>&)>;
  using BufferFrames = std::function<std::uint32_t()>;
  using Report = std::function<void(QString)>;

  BrowserPreviewController(SongNow song, Output output, BufferFrames buffer_frames, Report report,
                           bool encoded, QObject* parent = nullptr);
  ~BrowserPreviewController() override;

  [[nodiscard]] bool auto_preview() const { return auto_preview_; }
  void set_auto_preview(bool on);
  [[nodiscard]] QString sample_folder() const { return folder_; }
  [[nodiscard]] QVariantList samples() const { return samples_; }
  [[nodiscard]] bool scanning() const { return library_.scanning(); }
  [[nodiscard]] QString playing_key() const { return playing_key_; }

  Q_INVOKABLE void preview_sample(const QString& key);
  Q_INVOKABLE void stop();
  Q_INVOKABLE void choose_folder();
  void set_folder(const QString& path);
  void rescan();

signals:
  void autoPreviewChanged();
  void samplesChanged();
  void playingChanged();

private:
  void poll_library();
  void publish(const std::vector<SampleEntry>& entries);

  SongNow song_;
  Output output_;
  BufferFrames buffer_frames_;
  Report report_;
  bool encoded_ = false;
  bool auto_preview_ = true;
  QString folder_;
  QVariantList samples_;
  QString playing_key_;
  QTimer library_timer_;
  SampleLibrary library_;
  std::unique_ptr<SamplePreview> preview_;
};

} // namespace oscillate::quick
