#include "oscillate/midi_file/smf.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace oscillate::midi_file {
namespace {

using foundation::Error;
using foundation::ErrorCode;

// A failure carried out of the nested readers; never thrown past parse().
struct Failure : std::exception {
  explicit Failure(Error failure) : error(std::move(failure)) {}
  Error error;
};

[[noreturn]] void fail(ErrorCode code, std::string message) {
  throw Failure({code, std::move(message)});
}

[[noreturn]] void corrupt(std::string message) {
  fail(ErrorCode::corrupt_data, std::move(message));
}

// Bounds-checked big-endian reads over one span.
class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool done() const noexcept { return position_ >= bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

  std::uint8_t byte(const char* what) {
    if (done())
      corrupt(std::string("MIDI file ends inside ") + what);
    return bytes_[position_++];
  }
  [[nodiscard]] std::uint8_t peek(const char* what) const {
    if (done())
      corrupt(std::string("MIDI file ends inside ") + what);
    return bytes_[position_];
  }
  std::uint16_t u16(const char* what) {
    const auto high = byte(what);
    return static_cast<std::uint16_t>((high << 8U) | byte(what));
  }
  std::uint32_t u32(const char* what) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
      value = (value << 8U) | byte(what);
    return value;
  }
  // A variable-length quantity: at most four bytes, at most 0x0FFFFFFF.
  std::uint32_t vlq(const char* what) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const auto next = byte(what);
      value = (value << 7U) | (next & 0x7FU);
      if ((next & 0x80U) == 0)
        return value;
    }
    corrupt(std::string("MIDI variable-length value is too long in ") + what);
  }
  std::span<const std::uint8_t> take(std::size_t count, const char* what) {
    if (count > remaining())
      corrupt(std::string("MIDI ") + what + " runs past the end of its chunk");
    auto out = bytes_.subspan(position_, count);
    position_ += count;
    return out;
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ = 0;
};

[[nodiscard]] bool tag_is(std::span<const std::uint8_t> tag, std::string_view expected) {
  return tag.size() == 4 && std::memcmp(tag.data(), expected.data(), 4) == 0;
}

struct Pending {
  std::uint64_t start;
  std::uint8_t velocity;
};

// Gathered across tracks, then sorted by tick with file order breaking ties.
template <typename T> struct Ordered {
  T value;
  std::size_t order;
};

struct Counters {
  std::size_t events = 0;
  std::size_t order = 0;
};

// The width of a well-formed UTF-8 sequence at `at`, or zero.
std::size_t utf8_width(std::span<const std::uint8_t> text, std::size_t at) {
  const unsigned first = text[at];
  const std::size_t width = first >= 0xC2U && first <= 0xDFU   ? 2
                            : first >= 0xE0U && first <= 0xEFU ? 3
                            : first >= 0xF0U && first <= 0xF4U ? 4
                                                               : 0;
  if (width == 0 || at + width > text.size()) {
    return 0;
  }
  const unsigned second = text[at + 1];
  constexpr unsigned continuation_mask = 0xC0U;
  constexpr unsigned continuation = 0x80U;
  if ((second & continuation_mask) != continuation || (first == 0xE0U && second < 0xA0U) ||
      (first == 0xEDU && second > 0x9FU) || (first == 0xF0U && second < 0x90U) ||
      (first == 0xF4U && second > 0x8FU)) {
    return 0;
  }
  for (std::size_t j = 2; j < width; ++j) {
    if ((text[at + j] & continuation_mask) != continuation) {
      return 0;
    }
  }
  return width;
}

// Track names are free bytes in a MIDI file. The name kept is valid UTF-8
// of at most max_track_name_bytes: well-formed UTF-8 is kept, any other byte
// is read as Latin-1, and control characters (C0, DEL, C1) are dropped.
std::string printable_name(std::span<const std::uint8_t> text) {
  constexpr unsigned first_printable = 0x20U;
  constexpr unsigned del = 0x7FU;
  constexpr unsigned first_latin1_printable = 0xA0U;
  constexpr unsigned latin1_lead = 0xC0U;
  constexpr unsigned latin1_shift = 6U;
  constexpr unsigned latin1_low_bits = 0x3FU;
  constexpr unsigned continuation = 0x80U;
  std::string name;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned c = text[i];
    if (c < continuation) {
      if (c >= first_printable && c != del) {
        if (name.size() + 1 > max_track_name_bytes)
          break;
        name.push_back(static_cast<char>(c));
      }
      ++i;
      continue;
    }
    if (const auto width = utf8_width(text, i); width != 0) {
      if (name.size() + width > max_track_name_bytes)
        break;
      name.append(reinterpret_cast<const char*>(text.data() + i), width);
      i += width;
      continue;
    }
    if (c >= first_latin1_printable) {
      if (name.size() + 2 > max_track_name_bytes)
        break;
      name.push_back(static_cast<char>(latin1_lead | (c >> latin1_shift)));
      name.push_back(static_cast<char>(continuation | (c & latin1_low_bits)));
    }
    ++i;
  }
  return name;
}

Track read_track(std::span<const std::uint8_t> chunk, Counters& counters,
                 std::vector<Ordered<TempoChange>>& tempo,
                 std::vector<Ordered<TimeSignature>>& meter, Repairs& repairs) {
  Reader in(chunk);
  Track track;
  // FIFO per channel and key: 16 channels x 128 keys.
  std::vector<std::deque<Pending>> open(std::size_t{16} * 128U);
  std::uint64_t tick = 0;
  std::uint8_t running = 0;
  bool ended = false;
  bool named = false;
  const auto close = [&](std::uint8_t channel, std::uint8_t key) {
    auto& queue = open[channel * 128U + key];
    if (queue.empty()) {
      ++repairs.unmatched_note_offs;
      return;
    }
    const auto pending = queue.front();
    queue.pop_front();
    std::uint64_t end = tick;
    if (end <= pending.start) {
      end = pending.start + 1;
      ++repairs.zero_length_notes;
    }
    track.notes.push_back({pending.start, end, channel, key, pending.velocity});
  };
  while (!in.done()) {
    if (++counters.events > max_events)
      fail(ErrorCode::resource_exhausted, "MIDI file has too many events");
    tick += in.vlq("an event time");
    if (tick > max_tick)
      fail(ErrorCode::resource_exhausted, "MIDI file is too long");
    std::uint8_t status = in.peek("an event");
    if (status >= 0x80U) {
      (void)in.byte("an event");
    } else {
      if (running == 0)
        corrupt("MIDI data byte without a status (running status is not set)");
      status = running;
    }
    if (status < 0xF0U) {
      running = status;
      const auto type = static_cast<std::uint8_t>(status & 0xF0U);
      const auto channel = static_cast<std::uint8_t>(status & 0x0FU);
      const auto data_byte = [&] {
        const auto value = in.byte("a channel message");
        if (value >= 0x80U)
          corrupt("MIDI channel message has a status byte where data belongs");
        return value;
      };
      const auto first = data_byte();
      const auto second = (type == 0xC0U || type == 0xD0U) ? std::uint8_t{0} : data_byte();
      switch (type) {
      case 0x80U:
        close(channel, first);
        break;
      case 0x90U:
        if (second == 0)
          close(channel, first);
        else
          open[channel * 128U + first].push_back({tick, second});
        break;
      case 0xB0U:
        track.controllers.push_back({tick, channel, first, second});
        break;
      case 0xC0U:
        track.programs.push_back({tick, channel, first});
        break;
      default: // Poly and channel pressure, pitch bend: not imported.
        break;
      }
      continue;
    }
    running = 0; // System and meta events cancel running status.
    if (status == 0xF0U || status == 0xF7U) {
      (void)in.take(in.vlq("a SysEx length"), "SysEx event");
      continue;
    }
    if (status != 0xFFU)
      corrupt("MIDI file contains a system message that files may not carry");
    const auto type = in.byte("a meta event");
    const auto data = in.take(in.vlq("a meta event length"), "meta event");
    if (type == 0x2FU) {
      ended = true;
      break;
    }
    if (type == 0x51U) {
      if (data.size() != 3)
        corrupt("MIDI tempo event must carry three bytes");
      const auto microseconds = (static_cast<std::uint32_t>(data[0]) << 16U) |
                                (static_cast<std::uint32_t>(data[1]) << 8U) | data[2];
      if (microseconds == 0)
        corrupt("MIDI tempo event has zero length quarter notes");
      tempo.push_back({{tick, microseconds}, counters.order++});
    } else if (type == 0x58U) {
      if (data.size() < 4)
        corrupt("MIDI time signature event is too short");
      if (data[0] == 0 || data[1] > 15)
        corrupt("MIDI time signature is invalid");
      meter.push_back(
          {{tick, data[0], static_cast<std::uint16_t>(1U << data[1])}, counters.order++});
    } else if (type == 0x03U && !named) {
      track.name = printable_name(data);
      named = true;
    }
  }
  if (!ended)
    ++repairs.missing_end_of_track;
  track.end_tick = tick;
  // Notes still held at the end of the track end there.
  for (std::size_t index = 0; index < open.size(); ++index) {
    for (const auto& pending : open[index]) {
      std::uint64_t end = tick;
      if (end <= pending.start)
        end = pending.start + 1;
      track.notes.push_back({pending.start, end, static_cast<std::uint8_t>(index / 128U),
                             static_cast<std::uint8_t>(index % 128U), pending.velocity});
      ++repairs.notes_closed_at_track_end;
    }
  }
  std::ranges::stable_sort(track.notes, [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.key < b.key;
  });
  return track;
}

template <typename T, typename Key>
std::vector<T> collapse(std::vector<Ordered<T>> items, Key tick_of) {
  std::ranges::stable_sort(items, [&](const auto& a, const auto& b) {
    return tick_of(a.value) != tick_of(b.value) ? tick_of(a.value) < tick_of(b.value)
                                                : a.order < b.order;
  });
  std::vector<T> out;
  out.reserve(items.size());
  for (const auto& item : items) {
    if (!out.empty() && tick_of(out.back()) == tick_of(item.value))
      out.back() = item.value;
    else
      out.push_back(item.value);
  }
  return out;
}

File read_file(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > max_file_bytes)
    fail(ErrorCode::resource_exhausted, "MIDI file is larger than 32 MiB");
  Reader in(bytes);
  if (in.remaining() < 4 || !tag_is(in.take(4, "header"), "MThd"))
    corrupt("Not a Standard MIDI File (no MThd header)");
  const auto header_length = in.u32("the header");
  if (header_length < 6)
    corrupt("MIDI header is too short");
  Reader header(in.take(header_length, "header"));
  File file;
  file.format = header.u16("the header");
  const auto tracks = header.u16("the header");
  const auto division = header.u16("the header");
  if (file.format == 2)
    fail(ErrorCode::unsupported,
         "MIDI format 2 (independent sequences) is not supported; save as format 0 or 1");
  if (file.format > 2)
    corrupt("Unknown MIDI file format " + std::to_string(file.format));
  if (tracks == 0)
    corrupt("MIDI file declares no tracks");
  if (file.format == 0 && tracks != 1)
    corrupt("MIDI format 0 must have exactly one track");
  if (tracks > max_tracks)
    fail(ErrorCode::resource_exhausted, "MIDI file has too many tracks");
  if ((division & 0x8000U) != 0) {
    const auto rate = static_cast<int>(-static_cast<std::int8_t>(division >> 8U));
    const auto per_frame = static_cast<std::uint8_t>(division & 0xFFU);
    if ((rate != 24 && rate != 25 && rate != 29 && rate != 30) || per_frame == 0)
      corrupt("MIDI SMPTE division is invalid");
    file.division = {true, 0, static_cast<std::uint8_t>(rate), per_frame};
  } else {
    if (division == 0)
      corrupt("MIDI division is zero");
    file.division = {false, division, 0, 0};
  }
  Counters counters;
  std::vector<Ordered<TempoChange>> tempo;
  std::vector<Ordered<TimeSignature>> meter;
  file.tracks.reserve(tracks);
  while (file.tracks.size() < tracks) {
    if (in.remaining() < 8)
      corrupt("MIDI file ends before all declared tracks");
    const auto tag = in.take(4, "chunk");
    const auto length = in.u32("a chunk header");
    const auto body = in.take(length, "chunk");
    if (tag_is(tag, "MTrk"))
      file.tracks.push_back(read_track(body, counters, tempo, meter, file.repairs));
    // Unknown chunk types are skipped, as the format requires.
  }
  file.tempo = collapse(std::move(tempo), [](const TempoChange& t) { return t.tick; });
  file.meter = collapse(std::move(meter), [](const TimeSignature& t) { return t.tick; });
  return file;
}

} // namespace

foundation::Result<File> parse(std::span<const std::uint8_t> bytes) {
  try {
    return foundation::Result<File>::success(read_file(bytes));
  } catch (const Failure& failure) {
    return foundation::Result<File>::failure(failure.error);
  } catch (const std::bad_alloc&) {
    return foundation::Result<File>::failure(
        {ErrorCode::resource_exhausted, "Not enough memory to read the MIDI file"});
  }
}

} // namespace oscillate::midi_file
