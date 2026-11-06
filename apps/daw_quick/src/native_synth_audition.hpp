#pragma once

#include "oscillate/application/browser_preview.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace oscillate::quick {

// A separate, dry device preview for built-in instruments: auditioned notes
// of a fixed length (trigger) and live playing (note_on / note_off) from the
// piano roll's keys or the computer keyboard. The UI only queues commands;
// device creation, stopping, media loading and kernel preparation run on the
// control worker. The device callback sees a bounded SPSC note queue and the
// same NativeSynthRuntime used by project playback and export.
class NativeSynthAudition final {
public:
  using ErrorSink = std::function<void(std::string)>;
  // Decodes the document's managed media; runs on the worker.
  using MediaLoader = std::function<foundation::Result<native_dsp::synth::SynthMedia>()>;
  // What a live note plays: the engine, its values and its document.
  struct Sound {
    const native_dsp::synth::SynthEngine* engine = nullptr;
    std::vector<double> values;
    project::NativeSynthDocument document;
    MediaLoader load_media;
  };

  // The most notes a phrase() plays.
  static constexpr std::uint32_t max_phrase_notes = 64;

  explicit NativeSynthAudition(ErrorSink error_sink = {});
  NativeSynthAudition(const NativeSynthAudition&) = delete;
  NativeSynthAudition& operator=(const NativeSynthAudition&) = delete;
  ~NativeSynthAudition();

  [[nodiscard]] bool trigger(const native_dsp::synth::SynthEngine& engine,
                             std::vector<double> values, std::uint32_t sample_rate,
                             std::uint32_t buffer_frames, std::uint8_t key, std::uint8_t velocity,
                             std::optional<std::uint64_t> target_serial,
                             application::PlaybackOutputFactory output_factory = {},
                             bool replace_previous = false);
  // A short phrase (a preset preview): the notes play at their own frames
  // from now, replacing whatever sounded. At most max_phrase_notes notes.
  [[nodiscard]] bool phrase(const native_dsp::synth::SynthEngine& engine,
                            std::vector<double> values, std::uint32_t sample_rate,
                            std::uint32_t buffer_frames,
                            std::vector<application::PreviewNote> notes,
                            std::optional<std::uint64_t> target_serial,
                            application::PlaybackOutputFactory output_factory = {});
  // Live playing: the note sounds until note_off for its key (or
  // all_notes_off). Chords hold together.
  [[nodiscard]] bool note_on(Sound sound, std::uint32_t sample_rate, std::uint32_t buffer_frames,
                             std::uint8_t key, std::uint8_t velocity,
                             std::optional<std::uint64_t> target_serial,
                             application::PlaybackOutputFactory output_factory = {});
  bool note_off(std::uint8_t key);
  void all_notes_off();
  void stop() noexcept;

private:
  struct Stream;
  enum class Kind : std::uint8_t { trigger, phrase, on, off, all_off, stop };
  struct Request {
    Kind kind = Kind::trigger;
    const native_dsp::synth::SynthEngine* engine = nullptr;
    std::vector<double> values;
    project::NativeSynthDocument document;
    MediaLoader load_media;
    std::uint32_t sample_rate = 0;
    std::uint32_t buffer_frames = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 100;
    std::optional<std::uint64_t> target_serial;
    application::PlaybackOutputFactory output_factory;
    bool replace_previous = false;
    std::vector<application::PreviewNote> phrase;
  };

  bool enqueue(Request request);
  void run(std::stop_token stop_token) noexcept;
  void report(std::string message) const;

  ErrorSink error_sink_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<Request> requests_;
  std::jthread worker_;
};

} // namespace oscillate::quick
