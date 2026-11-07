#include "native_synth_audition.hpp"

#include "preview_pipewire_output.hpp"

#include "oscillate/engine/midi_events.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <span>
#include <tuple>
#include <utility>

namespace oscillate::quick {

struct NativeSynthAudition::Stream {
  static constexpr std::uint32_t capacity = max_phrase_notes;
  // A held note (`on`) sounds until an `off` for its key; a `trigger`
  // releases itself after half a second, or after `length_frames` when set.
  // A trigger `delay_frames` ahead is scheduled: a phrase's notes land on
  // their own frames, whatever the block size.
  enum class Command : std::uint8_t { trigger, on, off, all_off };
  struct NoteCommand {
    Command command = Command::trigger;
    std::uint8_t key = 0;
    std::uint8_t velocity = 100;
    bool replace_previous = false;
    std::uint32_t delay_frames = 0;
    std::uint32_t length_frames = 0;
  };
  static constexpr std::uint64_t held_until_released = ~std::uint64_t{0};
  struct HeldNote {
    std::uint8_t key = 0;
    std::uint64_t serial = 0;
    std::uint64_t release_frame = 0;
  };
  // A phrase note waiting for its frame; kept in start order.
  struct ScheduledNote {
    std::uint64_t on_frame = 0;
    std::uint64_t release_frame = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 100;
  };

  const native_dsp::synth::SynthEngine* engine;
  std::uint32_t sample_rate;
  std::optional<std::uint64_t> target_serial;
  audio_io::TransportCursor cursor;
  std::unique_ptr<native_dsp::synth::NativeSynthRuntime> runtime;
  std::unique_ptr<application::PlaybackOutput> output;
  std::array<NoteCommand, capacity> commands{};
  std::array<HeldNote, capacity> held{};           // Callback only.
  std::size_t held_count = 0;                      // Callback only.
  std::array<ScheduledNote, capacity> scheduled{}; // Callback only.
  std::size_t scheduled_count = 0;                 // Callback only.
  // Where the current phrase began: the block that took its first note
  // (the one replacing what sounded). Delays count from here, so a phrase
  // whose commands straddle blocks keeps its timing.
  std::uint64_t phrase_anchor = 0; // Callback only.
  std::atomic<std::uint32_t> write{0};
  std::atomic<std::uint32_t> read{0};
  std::uint64_t note_serial = 0;    // Callback only.
  std::uint64_t value_revision = 0; // Worker only.

  Stream(const native_dsp::synth::SynthEngine& selected, std::uint32_t rate,
         std::optional<std::uint64_t> serial,
         std::unique_ptr<native_dsp::synth::NativeSynthRuntime> prepared)
      : engine(&selected), sample_rate(rate), target_serial(serial), runtime(std::move(prepared)) {}

  [[nodiscard]] bool push(NoteCommand command) noexcept {
    const auto next = write.load(std::memory_order_relaxed);
    if (next - read.load(std::memory_order_acquire) >= capacity)
      return false;
    commands[next % capacity] = command;
    write.store(next + 1, std::memory_order_release);
    return true;
  }

  // A block's note events, sorted as the runtime takes them: offs (all at
  // frame 0) before ons, each by note id. An off only ever ends a note held
  // before the block, so there are at most `capacity`; ons come from at most
  // a queue of commands and a list of phrase notes.
  static constexpr std::size_t max_block_ons = capacity * 2;
  static constexpr std::size_t max_block_offs = capacity;
  static_assert(max_block_offs + max_block_ons <= engine::HeldNotes::instrument_capacity);
  std::array<engine::BlockMidiEvent, max_block_offs> offs{}; // Callback only.
  std::size_t off_count = 0;                                 // Callback only.
  std::array<engine::BlockMidiEvent, max_block_ons> ons{};   // Callback only.
  std::size_t on_count = 0;                                  // Callback only.
  std::array<engine::BlockMidiEvent, max_block_offs + max_block_ons>
      block_notes{}; // Callback only.
  // Notes with this serial or later started in the current block.
  std::uint64_t block_first_serial = 1; // Callback only.

  [[nodiscard]] static engine::BlockMidiEvent note_event(engine::MidiEventKind kind,
                                                         std::uint8_t key, std::uint64_t serial,
                                                         std::uint32_t offset,
                                                         std::uint8_t velocity) noexcept {
    engine::BlockMidiEvent event;
    event.offset = offset;
    event.clip_id_bits = {1, 1};
    event.note_id_bits = {0, serial};
    event.kind = kind;
    event.key = key;
    event.velocity = velocity;
    return event;
  }

  // Ends held note `index`. A note that started in this block never reaches
  // the instrument: its note-on is withdrawn, because the runtime takes a
  // frame's offs before its ons and the off would land first.
  void release(std::size_t index) noexcept {
    const auto note = held[index];
    held[index] = held[--held_count];
    if (note.serial < block_first_serial) {
      offs[off_count++] = note_event(engine::MidiEventKind::note_off, note.key, note.serial, 0, 0);
      return;
    }
    const auto first = ons.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(on_count);
    const auto started = std::find_if(first, last, [&](const engine::BlockMidiEvent& event) {
      return event.note_id_bits.low == note.serial;
    });
    if (started != last) {
      std::copy(started + 1, last, started);
      --on_count;
    }
  }

  // Starts a note. With every held slot taken, the oldest held note is
  // released first, as a synth steals its oldest voice: the newest key always
  // sounds and every note-on keeps a matching note-off.
  void start(std::uint8_t key, std::uint8_t velocity, std::uint32_t offset,
             std::uint64_t release_frame) noexcept {
    if (held_count == capacity) {
      const auto oldest = std::ranges::min_element(
          held.begin(), held.begin() + static_cast<std::ptrdiff_t>(held_count), {},
          &HeldNote::serial);
      release(static_cast<std::size_t>(oldest - held.begin()));
    }
    const auto serial = ++note_serial;
    ons[on_count++] = note_event(engine::MidiEventKind::note_on, key, serial, offset, velocity);
    held[held_count++] = {key, serial, release_frame};
  }

  static bool render(void* opaque, std::span<engine::StereoFrame> frames,
                     engine::BlockContext block) noexcept {
    auto& stream = *static_cast<Stream*>(opaque);
    std::fill(frames.begin(), frames.end(), engine::StereoFrame{0.0F, 0.0F});
    stream.off_count = 0;
    stream.on_count = 0;
    stream.block_first_serial = stream.note_serial + 1;
    // Release preview voices on the audio callback, even if no new UI
    // command arrives. A release is rounded to the next block boundary.
    for (std::size_t index = 0; index < stream.held_count;) {
      if (stream.held[index].release_frame > block.project_sample_position)
        ++index;
      else
        stream.release(index);
    }
    auto next = stream.read.load(std::memory_order_relaxed);
    const auto end = stream.write.load(std::memory_order_acquire);
    for (; next != end; ++next) {
      const auto command = stream.commands[next % capacity];
      if (command.command == Command::off || command.command == Command::all_off) {
        for (std::size_t index = 0; index < stream.held_count;) {
          if (command.command == Command::all_off || stream.held[index].key == command.key)
            stream.release(index);
          else
            ++index;
        }
        continue;
      }
      if (command.replace_previous) {
        while (stream.held_count > 0)
          stream.release(stream.held_count - 1);
        stream.scheduled_count = 0;
        stream.phrase_anchor = block.project_sample_position;
      }
      const auto release_after = [&](std::uint64_t start) {
        return command.command == Command::on ? held_until_released
               : command.length_frames > 0    ? start + command.length_frames
                                              : start + stream.sample_rate / 2;
      };
      if (command.delay_frames > 0) {
        if (stream.scheduled_count < capacity) {
          const auto start = stream.phrase_anchor + command.delay_frames;
          stream.scheduled[stream.scheduled_count++] = {start, release_after(start), command.key,
                                                        command.velocity};
        }
        continue;
      }
      stream.start(command.key, command.velocity, 0, release_after(block.project_sample_position));
    }
    stream.read.store(next, std::memory_order_release);
    // Scheduled phrase notes due in this block start at their own frame.
    const auto block_end = block.project_sample_position + frames.size();
    std::size_t due = 0;
    while (due < stream.scheduled_count && stream.scheduled[due].on_frame < block_end) {
      const auto& waiting = stream.scheduled[due++];
      stream.start(waiting.key, waiting.velocity,
                   static_cast<std::uint32_t>(waiting.on_frame > block.project_sample_position
                                                  ? waiting.on_frame - block.project_sample_position
                                                  : 0),
                   waiting.release_frame);
    }
    if (due > 0) {
      std::copy(stream.scheduled.begin() + static_cast<std::ptrdiff_t>(due),
                stream.scheduled.begin() + static_cast<std::ptrdiff_t>(stream.scheduled_count),
                stream.scheduled.begin());
      stream.scheduled_count -= due;
    }
    const auto by_frame_then_id = [](const engine::BlockMidiEvent& left,
                                     const engine::BlockMidiEvent& right) {
      return std::tie(left.offset, left.note_id_bits) < std::tie(right.offset, right.note_id_bits);
    };
    const auto offs = std::span(stream.offs).first(stream.off_count);
    const auto ons = std::span(stream.ons).first(stream.on_count);
    std::ranges::sort(offs, by_frame_then_id);
    std::ranges::sort(ons, by_frame_then_id);
    const auto notes_end =
        std::ranges::copy(ons, std::ranges::copy(offs, stream.block_notes.begin()).out).out;
    const auto notes = std::span(stream.block_notes.begin(), notes_end);
    const auto beat = static_cast<double>(block.project_sample_position) *
                      (120.0 / 60.0 / static_cast<double>(stream.sample_rate));
    const native_dsp::synth::SynthTransport transport{
        beat, 120.0 / 60.0 / static_cast<double>(stream.sample_rate), beat / 4.0, 4.0};
    const auto result = stream.runtime->process(frames, notes, block.project_sample_position,
                                                block.transport_generation, transport, {});
    if (result.status != native_dsp::synth::SynthProcessStatus::rendered)
      return false;
    // Audition is a dry device preview; leave mix headroom for simultaneous
    // transport playback and for kits with several overlapping one-shots.
    for (auto& frame : frames) {
      frame.left *= 0.5F;
      frame.right *= 0.5F;
    }
    return true;
  }
};

NativeSynthAudition::NativeSynthAudition(ErrorSink error_sink)
    : error_sink_(std::move(error_sink)), worker_([this](std::stop_token token) { run(token); }) {}

NativeSynthAudition::~NativeSynthAudition() {
  worker_.request_stop();
  wake_.notify_all();
}

bool NativeSynthAudition::trigger(const native_dsp::synth::SynthEngine& engine,
                                  std::vector<double> values, std::uint32_t sample_rate,
                                  std::uint32_t buffer_frames, std::uint8_t key,
                                  std::uint8_t velocity, std::optional<std::uint64_t> target_serial,
                                  application::PlaybackOutputFactory output_factory,
                                  bool replace_previous) {
  if (engine.descriptor == nullptr || values.size() != engine.descriptor->parameters.size() ||
      sample_rate == 0 || buffer_frames == 0 || key > 127 || velocity == 0 || velocity > 127)
    return false;
  Request request;
  request.kind = Kind::trigger;
  request.engine = &engine;
  request.values = std::move(values);
  request.sample_rate = sample_rate;
  request.buffer_frames = buffer_frames;
  request.key = key;
  request.velocity = velocity;
  request.target_serial = target_serial;
  request.output_factory = std::move(output_factory);
  request.replace_previous = replace_previous;
  return enqueue(std::move(request));
}

bool NativeSynthAudition::phrase(const native_dsp::synth::SynthEngine& engine,
                                 std::vector<double> values, std::uint32_t sample_rate,
                                 std::uint32_t buffer_frames,
                                 std::vector<application::PreviewNote> notes,
                                 std::optional<std::uint64_t> target_serial,
                                 application::PlaybackOutputFactory output_factory) {
  if (engine.descriptor == nullptr || values.size() != engine.descriptor->parameters.size() ||
      sample_rate == 0 || buffer_frames == 0 || notes.empty() || notes.size() > Stream::capacity ||
      std::ranges::any_of(notes, [](const application::PreviewNote& note) {
        return note.key > 127 || note.velocity == 0 || note.velocity > 127 ||
               note.start_frame > std::numeric_limits<std::uint32_t>::max() ||
               note.length_frames > std::numeric_limits<std::uint32_t>::max();
      }))
    return false;
  Request request;
  request.kind = Kind::phrase;
  request.engine = &engine;
  request.values = std::move(values);
  request.sample_rate = sample_rate;
  request.buffer_frames = buffer_frames;
  request.target_serial = target_serial;
  request.output_factory = std::move(output_factory);
  request.phrase = std::move(notes);
  return enqueue(std::move(request));
}

bool NativeSynthAudition::note_on(Sound sound, std::uint32_t sample_rate,
                                  std::uint32_t buffer_frames, std::uint8_t key,
                                  std::uint8_t velocity, std::optional<std::uint64_t> target_serial,
                                  application::PlaybackOutputFactory output_factory) {
  if (sound.engine == nullptr || sound.engine->descriptor == nullptr ||
      sound.values.size() != sound.engine->descriptor->parameters.size() || sample_rate == 0 ||
      buffer_frames == 0 || key > 127 || velocity == 0 || velocity > 127)
    return false;
  Request request;
  request.kind = Kind::on;
  request.engine = sound.engine;
  request.values = std::move(sound.values);
  request.document = std::move(sound.document);
  request.load_media = std::move(sound.load_media);
  request.sample_rate = sample_rate;
  request.buffer_frames = buffer_frames;
  request.key = key;
  request.velocity = velocity;
  request.target_serial = target_serial;
  request.output_factory = std::move(output_factory);
  return enqueue(std::move(request));
}

bool NativeSynthAudition::note_off(std::uint8_t key) {
  if (key > 127)
    return false;
  Request request;
  request.kind = Kind::off;
  request.key = key;
  return enqueue(std::move(request));
}

void NativeSynthAudition::all_notes_off() {
  Request request;
  request.kind = Kind::all_off;
  (void)enqueue(std::move(request));
}

bool NativeSynthAudition::enqueue(Request request) {
  try {
    std::lock_guard lock(mutex_);
    if (requests_.size() >= Stream::capacity)
      return false;
    requests_.push_back(std::move(request));
    wake_.notify_one();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void NativeSynthAudition::stop() noexcept {
  try {
    std::lock_guard lock(mutex_);
    requests_.clear();
    Request request;
    request.kind = Kind::stop;
    requests_.push_back(std::move(request));
    wake_.notify_one();
  } catch (const std::exception&) {
    worker_.request_stop();
    wake_.notify_all();
  }
}

void NativeSynthAudition::report(std::string message) const {
  if (!error_sink_)
    return;
  try {
    error_sink_(std::move(message));
  } catch (const std::exception&) {
  }
}

void NativeSynthAudition::run(std::stop_token stop_token) noexcept {
  std::unique_ptr<Stream> stream;
  while (!stop_token.stop_requested()) {
    Request request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, stop_token, [this] { return !requests_.empty(); });
      if (stop_token.stop_requested())
        break;
      request = std::move(requests_.front());
      requests_.pop_front();
    }
    if (request.kind == Kind::stop) {
      if (stream && stream->output)
        stream->output->stop();
      stream.reset();
      continue;
    }
    if (request.kind == Kind::off || request.kind == Kind::all_off) {
      // Nothing sounds without a stream; releases need no sound.
      if (stream && !stream->push({request.kind == Kind::off ? Stream::Command::off
                                                             : Stream::Command::all_off,
                                   request.key, 0, false, 0, 0}))
        report("Instrument preview queue is full");
      continue;
    }
    try {
      const auto state = stream ? stream->output->status().state : audio_io::OutputState::stopped;
      if (stream &&
          (stream->engine != request.engine || stream->sample_rate != request.sample_rate ||
           stream->target_serial != request.target_serial ||
           state == audio_io::OutputState::stopped ||
           state == audio_io::OutputState::disconnected || state == audio_io::OutputState::error)) {
        stream->output->stop();
        stream.reset();
      }
      // A document whose managed media changed needs a fresh runtime.
      if (stream && stream->runtime->document() != request.document) {
        auto offered = stream->runtime->offer_document(request.document, ++stream->value_revision);
        if (!offered || !offered.value()) {
          stream->output->stop();
          stream.reset();
        }
      }
      if (!stream) {
        native_dsp::synth::SynthMedia media;
        if (request.load_media) {
          auto loaded = request.load_media();
          if (!loaded) {
            report("Cannot load instrument media: " + loaded.error().message);
            continue;
          }
          media = std::move(loaded).value();
        }
        auto prepared = native_dsp::synth::NativeSynthRuntime::create(
            *request.engine, request.values, request.document, std::move(media),
            request.sample_rate);
        if (!prepared) {
          report("Cannot prepare instrument preview: " + prepared.error().message);
          continue;
        }
        stream = std::make_unique<Stream>(*request.engine, request.sample_rate,
                                          request.target_serial, std::move(prepared).value());
        const audio_io::StereoRenderSource source{stream.get(), &Stream::render, nullptr};
        stream->output = request.output_factory
                             ? request.output_factory(source, stream->cursor)
                             : std::make_unique<PreviewPipeWireOutput>(source, stream->cursor);
        if (!stream->output) {
          report("Instrument preview has no audio output");
          stream.reset();
          continue;
        }
        const auto started = stream->output->start(request.sample_rate, request.buffer_frames,
                                                   request.target_serial);
        if (!started) {
          report("Cannot start instrument preview: " + started.error().message);
          stream->output->stop();
          stream.reset();
          continue;
        }
      } else if (!stream->runtime->offer_values(request.values, ++stream->value_revision)) {
        report("Instrument preview is still applying a previous sound edit");
      }
      if (request.kind == Kind::phrase) {
        // The first note replaces what sounded and anchors the rest.
        bool first = true;
        for (const auto& note : request.phrase) {
          if (!stream->push({Stream::Command::trigger, note.key, note.velocity, first,
                             static_cast<std::uint32_t>(note.start_frame),
                             static_cast<std::uint32_t>(note.length_frames)})) {
            report("Instrument preview queue is full; try the preview again");
            break;
          }
          first = false;
        }
        continue;
      }
      const auto command =
          request.kind == Kind::on ? Stream::Command::on : Stream::Command::trigger;
      if (!stream->push({command, request.key, request.velocity, request.replace_previous, 0, 0}))
        report("Instrument preview queue is full; try the note again");
    } catch (const std::exception& error) {
      report(std::string("Instrument preview failed: ") + error.what());
      if (stream && stream->output)
        stream->output->stop();
      stream.reset();
    }
  }
  if (stream && stream->output)
    stream->output->stop();
}

} // namespace oscillate::quick
