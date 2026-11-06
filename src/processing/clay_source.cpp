#include "oscillate/processing/clay_source.hpp"

#include "oscillate/clay_dsp/analysis.hpp"
#include "oscillate/clay_dsp/limits.hpp"
#include "oscillate/clay_dsp/render.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/media/audio_reader.hpp"
#include "oscillate/media/cache_identity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oscillate::processing::clay {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

static_assert(std::endian::native == std::endian::little,
              "Clay's cached analysis is written in host order, which must be little-endian");

constexpr std::string_view analysis_algorithm = "clay-analysis";
constexpr std::string_view render_algorithm = "clay-render";
// Bump when the cached analysis bytes change for the same analysis.
constexpr std::uint32_t analysis_format_version = 1;
constexpr std::size_t decode_chunk_frames = 4'096;
constexpr std::size_t render_chunk_frames = 65'536;
// RIFF/WAVE with a 16-byte fmt chunk: the 44-byte canonical header.
constexpr std::uint32_t wav_header_bytes = 44;
constexpr std::uint16_t wav_float_format = 3;
constexpr std::uint16_t wav_float_bits = 32;
constexpr std::uint32_t wav_fmt_bytes = 16;
constexpr std::uint32_t riff_overhead_bytes = 36;

[[nodiscard]] foundation::Error invalid(std::string message) {
  return {ErrorCode::invalid_argument, std::move(message)};
}

using Fd = foundation::UniqueFd;
using foundation::io_error;

// Canonical little-endian bytes for cache keys and the cached analysis.
class Bytes {
public:
  void u8(std::uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }
  void u32(std::uint32_t value) { raw(&value, sizeof value); }
  void u64(std::uint64_t value) { raw(&value, sizeof value); }
  void i64(std::int64_t value) { raw(&value, sizeof value); }
  void f32(float value) { raw(&value, sizeof value); }
  void f64(double value) { raw(&value, sizeof value); }
  template <typename T> void floats(const std::vector<T>& values) {
    u64(values.size());
    raw(values.data(), values.size() * sizeof(T));
  }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(out_); }

private:
  void raw(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    out_.insert(out_.end(), bytes, bytes + size);
  }
  std::vector<std::byte> out_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
  template <typename T> [[nodiscard]] bool read(T& value) {
    if (bytes_.size() - at_ < sizeof(T))
      return false;
    std::memcpy(&value, bytes_.data() + at_, sizeof(T));
    at_ += sizeof(T);
    return true;
  }
  template <typename T> [[nodiscard]] bool vector(std::vector<T>& values) {
    std::uint64_t size = 0;
    if (!read(size) || size > (bytes_.size() - at_) / sizeof(T))
      return false;
    values.resize(size);
    std::memcpy(values.data(), bytes_.data() + at_, size * sizeof(T));
    at_ += size * sizeof(T);
    return true;
  }
  [[nodiscard]] bool done() const noexcept { return at_ == bytes_.size(); }

private:
  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
};

[[nodiscard]] std::vector<std::byte> encode_take(const clay_dsp::Take& take) {
  Bytes out;
  out.u32(analysis_format_version);
  out.u8(static_cast<std::uint8_t>(take.mode));
  out.u32(take.sample_rate);
  out.i64(take.frames);
  out.i64(take.hop);
  out.u32(take.next_id);
  out.u64(take.notes.size());
  for (const auto& note : take.notes) {
    out.u32(note.id);
    out.u8(static_cast<std::uint8_t>(note.kind));
    out.i64(note.start);
    out.i64(note.end);
    out.i64(note.first_hop);
    out.floats(note.pitch);
    out.floats(note.amplitude);
    out.f64(note.centre);
    out.floats(note.drift);
    out.floats(note.modulation);
    out.u64(note.partial_count);
    out.floats(note.partials);
    out.floats(note.envelope);
    out.f32(note.noise_share);
    out.f32(note.pan[0]);
    out.f32(note.pan[1]);
  }
  return std::move(out).take();
}

[[nodiscard]] std::optional<clay_dsp::Take> decode_take(std::span<const std::byte> bytes) {
  Reader in(bytes);
  clay_dsp::Take take;
  std::uint32_t version = 0;
  std::uint8_t mode = 0;
  std::uint64_t notes = 0;
  if (!in.read(version) || version != analysis_format_version || !in.read(mode) ||
      mode > static_cast<std::uint8_t>(clay_dsp::Mode::percussive) || !in.read(take.sample_rate) ||
      !in.read(take.frames) || !in.read(take.hop) || !in.read(take.next_id) || !in.read(notes))
    return std::nullopt;
  take.mode = static_cast<clay_dsp::Mode>(mode);
  if (notes > bytes.size())
    return std::nullopt;
  take.notes.resize(notes);
  for (auto& note : take.notes) {
    std::uint8_t kind = 0;
    std::uint64_t partials = 0;
    if (!in.read(note.id) || !in.read(kind) ||
        kind > static_cast<std::uint8_t>(clay_dsp::NoteKind::segment) || !in.read(note.start) ||
        !in.read(note.end) || !in.read(note.first_hop) || !in.vector(note.pitch) ||
        !in.vector(note.amplitude) || !in.read(note.centre) || !in.vector(note.drift) ||
        !in.vector(note.modulation) || !in.read(partials) || !in.vector(note.partials) ||
        !in.vector(note.envelope) || !in.read(note.noise_share) || !in.read(note.pan[0]) ||
        !in.read(note.pan[1]))
      return std::nullopt;
    note.kind = static_cast<clay_dsp::NoteKind>(kind);
    note.partial_count = static_cast<std::size_t>(partials);
    const auto hops = note.pitch.size();
    if (note.amplitude.size() != hops || note.drift.size() != hops ||
        note.modulation.size() != hops || note.partials.size() != hops * note.partial_count)
      return std::nullopt;
  }
  if (!in.done())
    return std::nullopt;
  return take;
}

// What a take renders from, in canonical bytes: everything that changes the
// render and nothing else (the key is presentation-free).
[[nodiscard]] std::vector<std::byte> canonical_take(const project::ClayTake& take) {
  Bytes out;
  out.u32(render_version);
  out.u32(clay_dsp::analysis_version);
  out.u8(static_cast<std::uint8_t>(take.mode));
  out.u64(take.ops.size());
  for (const auto& op : take.ops) {
    out.u8(static_cast<std::uint8_t>(op.kind));
    out.u32(op.note);
    out.u32(op.other);
    out.i64(op.frame);
  }
  const auto edits = dsp_edits(take, true);
  out.u64(edits.size());
  for (const auto& edit : edits) {
    out.u32(edit.note);
    out.u8(edit.duplicate ? 1U : 0U);
    out.f64(edit.pitch_cents);
    out.f64(edit.modulation);
    out.f64(edit.drift);
    out.f64(edit.formant_cents);
    out.f64(edit.gain_db);
    out.u8(edit.start ? 1U : 0U);
    out.i64(edit.start.value_or(0));
    out.i64(edit.end.value_or(0));
  }
  return std::move(out).take();
}

[[nodiscard]] Result<media::CacheKey> analysis_key(const project::SourceRecord& source,
                                                   project::ClayMode mode) {
  Bytes settings;
  settings.u32(analysis_format_version);
  return media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                .source = source,
                                .clip = std::nullopt,
                                .project_rate = std::nullopt,
                                .algorithm_id = std::string(analysis_algorithm),
                                .algorithm_version = clay_dsp::analysis_version,
                                .quality = static_cast<std::uint32_t>(mode),
                                .canonical_settings = std::move(settings).take(),
                                .tempo_sha256 = std::nullopt});
}

[[nodiscard]] Result<media::CacheKey> render_key(const project::SourceRecord& source,
                                                 const project::ClayTake& take) {
  // A render depends on the source and the take alone, never on a clip or
  // the project rate, so it is keyed like source-derived data: copies of a
  // clip with the same take share one render.
  return media::make_cache_key({.artifact = media::CacheArtifact::source_analysis,
                                .source = source,
                                .clip = std::nullopt,
                                .project_rate = std::nullopt,
                                .algorithm_id = std::string(render_algorithm),
                                .algorithm_version = render_version,
                                .quality = 1,
                                .canonical_settings = canonical_take(take),
                                .tempo_sha256 = std::nullopt});
}

[[nodiscard]] Result<std::uint64_t> wav_bytes(const project::SourceRecord& source) {
  const auto block = static_cast<std::uint64_t>(source.channels) * sizeof(float);
  if (source.frames > (std::numeric_limits<std::uint32_t>::max() - riff_overhead_bytes) / block)
    return Result<std::uint64_t>::failure(
        {ErrorCode::resource_exhausted, "A Clay render is limited to a 4 GiB WAV"});
  return Result<std::uint64_t>::success(wav_header_bytes + source.frames * block);
}

// Bridges a stop token to the DSP's polled flag.
class Cancel {
public:
  explicit Cancel(const std::stop_token& stop)
      : callback_(stop, [this] { flag_.store(true, std::memory_order_relaxed); }) {
    if (stop.stop_requested())
      flag_.store(true, std::memory_order_relaxed);
  }
  [[nodiscard]] const std::atomic<bool>* flag() const noexcept { return &flag_; }

private:
  std::atomic<bool> flag_{false};
  std::stop_callback<std::function<void()>> callback_;
};

[[nodiscard]] Result<Fd> render_directory_fd(const persistence::ProjectStore& store) {
  auto cache = store.open_cache_directory();
  if (!cache)
    return Result<Fd>::failure(cache.error());
  Fd parent(cache.value());
  const std::string name(render_directory);
  if (::mkdirat(parent.get(), name.c_str(), 0700) != 0 && errno != EEXIST)
    return Result<Fd>::failure(io_error("create the Clay render directory"));
  Fd directory(
      ::openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (directory.get() < 0)
    return Result<Fd>::failure(io_error("open the Clay render directory"));
  return Result<Fd>::success(std::move(directory));
}

[[nodiscard]] std::optional<media::CacheStore> cache_of(const persistence::ProjectStore& store) {
  auto directory = store.open_cache_directory();
  if (!directory)
    return std::nullopt;
  auto adopted = media::CacheStore::adopt_directory_fd(directory.value());
  if (!adopted)
    return std::nullopt;
  return std::move(adopted).value();
}

[[nodiscard]] std::string render_name(const project::SourceRecord& rendered) {
  return rendered.sha256 + ".wav";
}

void put_le16(std::array<unsigned char, wav_header_bytes>& header, std::size_t at,
              std::uint16_t value) {
  header[at] = static_cast<unsigned char>(value & 0xFFU);
  header[at + 1] = static_cast<unsigned char>(value >> 8U);
}

void put_le32(std::array<unsigned char, wav_header_bytes>& header, std::size_t at,
              std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i)
    header[at + i] = static_cast<unsigned char>((value >> (8U * i)) & 0xFFU);
}

[[nodiscard]] Result<void> write_all(int fd, std::span<const std::byte> bytes) {
  return foundation::write_all(fd, bytes, "write a Clay render");
}

// Writes the render to a private temporary name and publishes it atomically,
// so a reader only ever sees a complete file.
[[nodiscard]] Result<void> write_render(int directory, const project::SourceRecord& rendered,
                                        std::span<const float> left, std::span<const float> right) {
  static std::atomic<std::uint64_t> serial{0};
  const auto temporary = ".tmp-" + std::to_string(::getpid()) + "-" +
                         std::to_string(serial.fetch_add(1, std::memory_order_relaxed)) + ".wav";
  Fd file(::openat(directory, temporary.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (file.get() < 0)
    return Result<void>::failure(io_error("create a Clay render"));
  const auto discard = [&] { (void)::unlinkat(directory, temporary.c_str(), 0); };
  const auto channels = static_cast<std::uint16_t>(rendered.channels);
  const auto block = static_cast<std::uint16_t>(channels * sizeof(float));
  const auto data_bytes = static_cast<std::uint32_t>(rendered.byte_length - wav_header_bytes);
  std::array<unsigned char, wav_header_bytes> header{};
  std::memcpy(header.data(), "RIFF", 4);
  put_le32(header, 4, data_bytes + riff_overhead_bytes);
  std::memcpy(header.data() + 8, "WAVEfmt ", 8);
  put_le32(header, 16, wav_fmt_bytes);
  put_le16(header, 20, wav_float_format);
  put_le16(header, 22, channels);
  put_le32(header, 24, rendered.sample_rate);
  put_le32(header, 28, rendered.sample_rate * block);
  put_le16(header, 32, block);
  put_le16(header, 34, wav_float_bits);
  std::memcpy(header.data() + 36, "data", 4);
  put_le32(header, 40, data_bytes);
  if (auto written = write_all(file.get(), std::as_bytes(std::span(header))); !written) {
    discard();
    return written;
  }
  std::vector<float> chunk;
  chunk.reserve(render_chunk_frames * channels);
  for (std::size_t first = 0; first < left.size(); first += render_chunk_frames) {
    const auto count = std::min(render_chunk_frames, left.size() - first);
    chunk.clear();
    for (std::size_t i = 0; i < count; ++i) {
      chunk.push_back(left[first + i]);
      if (channels == 2)
        chunk.push_back(right[first + i]);
    }
    if (auto written = write_all(file.get(), std::as_bytes(std::span(chunk))); !written) {
      discard();
      return written;
    }
  }
  if (auto closed = file.close("close a Clay render"); !closed) {
    discard();
    return closed;
  }
  if (auto renamed =
          foundation::rename_in(directory, temporary, render_name(rendered),
                                foundation::PublishMode::replace, "publish a Clay render");
      !renamed) {
    discard();
    return renamed;
  }
  return Result<void>::success();
}

// Opens a published render and checks it is the complete file its record
// describes (a damaged one is a miss).
[[nodiscard]] std::optional<Fd> open_published(int directory,
                                               const project::SourceRecord& rendered) {
  const auto name = render_name(rendered);
  Fd file(::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0)
    return std::nullopt;
  struct stat status{};
  if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      static_cast<std::uint64_t>(status.st_size) != rendered.byte_length)
    return std::nullopt;
  auto reader = media::AudioReader::open_fd(file.get(), rendered);
  if (!reader)
    return std::nullopt;
  return file;
}

} // namespace

clay_dsp::Mode dsp_mode(project::ClayMode mode) noexcept {
  switch (mode) {
  case project::ClayMode::polyphonic:
    return clay_dsp::Mode::polyphonic;
  case project::ClayMode::percussive:
    return clay_dsp::Mode::percussive;
  case project::ClayMode::melodic:
    break;
  }
  return clay_dsp::Mode::melodic;
}

clay_dsp::Scale dsp_scale(const std::optional<project::MusicalKey>& key) {
  if (!key)
    return clay_dsp::Scale::chromatic();
  return key->mode == project::KeyMode::minor ? clay_dsp::Scale::minor(key->tonic)
                                              : clay_dsp::Scale::major(key->tonic);
}

std::vector<clay_dsp::NoteEdit> dsp_edits(const project::ClayTake& take, bool for_render) {
  std::vector<project::ClayNoteEdit> ordered(take.edits.begin(), take.edits.end());
  // Own edits first, then each note's voices in order, as the macros expect.
  std::ranges::stable_sort(ordered, [](const auto& a, const auto& b) {
    if ((a.voice == 0) != (b.voice == 0))
      return a.voice == 0;
    if (a.voice == 0)
      return a.note < b.note;
    return std::pair{a.note, a.voice} < std::pair{b.note, b.voice};
  });
  std::vector<clay_dsp::NoteEdit> edits;
  edits.reserve(ordered.size());
  for (const auto& edit : ordered) {
    clay_dsp::NoteEdit out;
    out.note = edit.note;
    out.duplicate = edit.voice != 0;
    out.pitch_cents = edit.pitch_cents;
    out.modulation = edit.modulation;
    out.drift = edit.drift;
    out.formant_cents = edit.formant_cents;
    out.gain_db = for_render && edit.muted ? clay_dsp::min_edit_gain_db : edit.gain_db;
    out.start = edit.start;
    out.end = edit.end;
    edits.push_back(out);
  }
  return edits;
}

project::ClayTake with_dsp_edits(const project::ClayTake& take,
                                 std::span<const clay_dsp::NoteEdit> edits) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, bool> muted;
  for (const auto& edit : take.edits)
    muted[{edit.note, edit.voice}] = edit.muted;
  std::map<std::uint32_t, std::uint32_t> voices;
  project::ClayTake out = take;
  out.edits.clear();
  for (const auto& edit : edits) {
    const auto voice = edit.duplicate ? ++voices[edit.note] : 0U;
    const auto was_muted = muted.find({edit.note, voice});
    const bool mute = was_muted != muted.end() && was_muted->second;
    if (!edit.duplicate && edit.identity() && !mute)
      continue;
    out.edits.push_back({edit.note, voice, edit.pitch_cents, edit.modulation, edit.drift,
                         edit.formant_cents, edit.gain_db, mute, edit.start, edit.end});
  }
  return out;
}

Result<void> apply_ops(clay_dsp::Take& take, std::span<const project::ClayStructuralOp> ops) {
  for (const auto& op : ops) {
    if (op.kind == project::ClayOpKind::split) {
      auto split = clay_dsp::split_note(take, op.note, op.frame);
      if (!split)
        return Result<void>::failure(split.error());
    } else {
      auto joined = clay_dsp::join_notes(take, op.note, op.other);
      if (!joined)
        return joined;
    }
  }
  return Result<void>::success();
}

namespace {
// Decodes a source already verified and pinned at `verified`.
Result<DecodedSource> decode_verified(int verified, const project::SourceRecord& source,
                                      const std::stop_token& stop) {
  auto reader = media::AudioReader::open_fd(verified, source);
  if (!reader)
    return Result<DecodedSource>::failure(reader.error());
  if (reader.value().frame_count() != source.frames ||
      reader.value().sample_rate() != source.sample_rate ||
      reader.value().channels() != source.channels)
    return Result<DecodedSource>::failure(
        {ErrorCode::corrupt_data, "Verified audio metadata changed"});
  DecodedSource decoded;
  decoded.sample_rate = source.sample_rate;
  try {
    decoded.left.resize(source.frames);
    if (source.channels == 2)
      decoded.right.resize(source.frames);
  } catch (const std::bad_alloc&) {
    return Result<DecodedSource>::failure(
        {ErrorCode::resource_exhausted, "Not enough memory to decode the clip for Clay"});
  }
  std::array<media::StereoFrame, decode_chunk_frames> chunk{};
  std::uint64_t at = 0;
  while (at < source.frames) {
    if (stop.stop_requested())
      return Result<DecodedSource>::failure({ErrorCode::cancelled, "Clay decoding cancelled"});
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(decode_chunk_frames, source.frames - at));
    auto read = reader.value().read_at(at, std::span(chunk).first(wanted));
    if (!read)
      return Result<DecodedSource>::failure(read.error());
    if (read.value() == 0)
      return Result<DecodedSource>::failure(
          {ErrorCode::corrupt_data, "The source ended early while decoding it for Clay"});
    for (std::size_t i = 0; i < read.value(); ++i) {
      decoded.left[at + i] = chunk[i].left;
      if (source.channels == 2)
        decoded.right[at + i] = chunk[i].right;
    }
    at += read.value();
  }
  return Result<DecodedSource>::success(std::move(decoded));
}

// The shape Clay accepts, checked before any source is opened.
Result<void> clay_shape(const project::SourceRecord& source) {
  if (source.channels != 1 && source.channels != 2)
    return Result<void>::failure(invalid("Clay edits mono or stereo audio"));
  if (source.sample_rate < clay_dsp::min_sample_rate ||
      source.sample_rate > clay_dsp::max_sample_rate ||
      static_cast<double>(source.frames) >
          clay_dsp::max_take_seconds * static_cast<double>(source.sample_rate))
    return Result<void>::failure(
        {ErrorCode::unsupported, "Clay cannot edit audio at this rate or length"});
  return Result<void>::success();
}
} // namespace

Result<DecodedSource> decode_source(const persistence::ProjectStore& store,
                                    const project::SourceRecord& source,
                                    const std::stop_token& stop) {
  if (auto shape = clay_shape(source); !shape)
    return Result<DecodedSource>::failure(shape.error());
  auto descriptor = store.open_verified_source(source, stop);
  if (!descriptor)
    return Result<DecodedSource>::failure(descriptor.error());
  const Fd pinned(descriptor.value());
  return decode_verified(pinned.get(), source, stop);
}

Result<clay_dsp::Take> analysed_take(media::CacheStore* cache, const project::SourceRecord& source,
                                     project::ClayMode mode, const DecodedSource& audio,
                                     const std::stop_token& stop) {
  auto key = analysis_key(source, mode);
  if (!key)
    return Result<clay_dsp::Take>::failure(key.error());
  if (cache != nullptr) {
    // A cache error only means analysing again.
    if (auto hit = cache->get(key.value()); hit && hit.value()) {
      if (auto take = decode_take(*hit.value()))
        return Result<clay_dsp::Take>::success(std::move(*take));
    }
  }
  const Cancel cancel(stop);
  clay_dsp::AnalysisOptions options;
  options.mode = dsp_mode(mode);
  auto analysed =
      clay_dsp::analyse({audio.left, audio.right, audio.sample_rate}, options, cancel.flag());
  if (!analysed)
    return analysed;
  if (cache != nullptr)
    (void)cache->put(key.value(), encode_take(analysed.value()));
  return analysed;
}

Result<clay_dsp::Take> edited_take(media::CacheStore* cache, const project::SourceRecord& source,
                                   const project::ClayTake& take, const DecodedSource& audio,
                                   const std::stop_token& stop) {
  if (take.analysis_version != clay_dsp::analysis_version)
    return Result<clay_dsp::Take>::failure(
        invalid("The Clay take was made by another analysis version"));
  auto analysed = analysed_take(cache, source, take.mode, audio, stop);
  if (!analysed)
    return analysed;
  if (auto replayed = apply_ops(analysed.value(), take.ops); !replayed)
    return Result<clay_dsp::Take>::failure(replayed.error());
  return analysed;
}

Result<project::SourceRecord> rendered_record(const project::SourceRecord& source,
                                              const project::ClayTake& take) {
  if (source.channels != 1 && source.channels != 2)
    return Result<project::SourceRecord>::failure(invalid("Clay edits mono or stereo audio"));
  auto key = render_key(source, take);
  if (!key)
    return Result<project::SourceRecord>::failure(key.error());
  auto bytes = wav_bytes(source);
  if (!bytes)
    return Result<project::SourceRecord>::failure(bytes.error());
  return Result<project::SourceRecord>::success(
      {source.id, project::SourceEncoding::wav, key.value().hex(), bytes.value(),
       source.sample_rate, source.channels, source.frames, std::nullopt});
}

Result<void> render_take(media::CacheStore* cache, const project::SourceRecord& source,
                         const project::ClayTake& take, const DecodedSource& audio,
                         std::span<float> left, std::span<float> right,
                         const std::stop_token& stop) {
  if (left.size() != audio.left.size() || right.size() != audio.right.size())
    return Result<void>::failure(invalid("A Clay render must cover the whole source"));
  auto edited = edited_take(cache, source, take, audio, stop);
  if (!edited)
    return Result<void>::failure(edited.error());
  const auto edits = dsp_edits(take, true);
  auto renderer = clay_dsp::Renderer::create(edited.value(), audio.left, audio.right, edits);
  if (!renderer)
    return Result<void>::failure(renderer.error());
  for (std::size_t first = 0; first < left.size(); first += render_chunk_frames) {
    if (stop.stop_requested())
      return Result<void>::failure({ErrorCode::cancelled, "Clay rendering cancelled"});
    const auto count = std::min(render_chunk_frames, left.size() - first);
    renderer.value().render(static_cast<std::int64_t>(first), left.subspan(first, count),
                            right.empty() ? right : right.subspan(first, count));
  }
  return Result<void>::success();
}

RenderedAudio::RenderedAudio(RenderedAudio&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), record_(std::move(other.record_)) {}

RenderedAudio& RenderedAudio::operator=(RenderedAudio&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0)
      (void)::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    record_ = std::move(other.record_);
  }
  return *this;
}

RenderedAudio::~RenderedAudio() {
  if (fd_ >= 0)
    (void)::close(fd_);
}

Result<RenderedAudio> open_rendered(const persistence::ProjectStore& store,
                                    const project::SourceRecord& source,
                                    const project::ClayTake& take, const std::stop_token& stop) {
  auto record = rendered_record(source, take);
  if (!record)
    return Result<RenderedAudio>::failure(record.error());
  auto directory = render_directory_fd(store);
  if (!directory)
    return Result<RenderedAudio>::failure(directory.error());
  // The source is verified even on a hit, so a missing or changed source is
  // never masked by derived data; a miss decodes that same verified file
  // rather than hashing it again.
  if (auto shape = clay_shape(source); !shape)
    return Result<RenderedAudio>::failure(shape.error());
  auto verified = store.open_verified_source(source, stop);
  if (!verified)
    return Result<RenderedAudio>::failure(verified.error());
  const Fd pinned(verified.value());
  if (auto hit = open_published(directory.value().get(), record.value()))
    return Result<RenderedAudio>::success(RenderedAudio{hit->release(), record.value()});
  auto audio = decode_verified(pinned.get(), source, stop);
  if (!audio)
    return Result<RenderedAudio>::failure(audio.error());
  auto cache = cache_of(store);
  std::vector<float> left;
  std::vector<float> right;
  try {
    left.resize(audio.value().left.size());
    right.resize(audio.value().right.size());
  } catch (const std::bad_alloc&) {
    return Result<RenderedAudio>::failure(
        {ErrorCode::resource_exhausted, "Not enough memory to render the Clay take"});
  }
  if (auto rendered =
          render_take(cache ? &*cache : nullptr, source, take, audio.value(), left, right, stop);
      !rendered)
    return Result<RenderedAudio>::failure(rendered.error());
  if (auto written = write_render(directory.value().get(), record.value(), left, right); !written)
    return Result<RenderedAudio>::failure(written.error());
  auto published = open_published(directory.value().get(), record.value());
  if (!published)
    return Result<RenderedAudio>::failure(
        {ErrorCode::io_failure, "The Clay render did not read back"});
  return Result<RenderedAudio>::success(RenderedAudio{published->release(), record.value()});
}

bool render_cached(const persistence::ProjectStore& store, const project::SourceRecord& source,
                   const project::ClayTake& take) {
  auto record = rendered_record(source, take);
  if (!record)
    return false;
  auto directory = render_directory_fd(store);
  if (!directory)
    return false;
  const auto name = render_name(record.value());
  struct stat status{};
  return ::fstatat(directory.value().get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(status.st_mode) &&
         static_cast<std::uint64_t>(status.st_size) == record.value().byte_length;
}

Result<project::SourceRecord> clip_audio_record(const project::Clip& clip,
                                                const project::SourceRecord& source) {
  if (!clip.clay)
    return Result<project::SourceRecord>::success(source);
  return rendered_record(source, *clip.clay);
}

} // namespace oscillate::processing::clay
