#include "parser_exercise.hpp"

#include "oscillate/media/prefetch.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "private.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace oscillate::fuzz {
namespace {

constexpr std::size_t max_input_bytes = 1024U * 1024U;

void write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
  std::size_t written = 0;
  while (written != size) {
    const auto count = ::write(fd, bytes + written, size - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("fuzz memfd write failed");
    written += static_cast<std::size_t>(count);
  }
}

} // namespace

void midi(const std::uint8_t* bytes, std::size_t size) {
  if (size > max_input_bytes)
    return;
  auto file = midi_file::parse(std::span(bytes, size));
  if (!file)
    return;
  // Accepted files keep every invariant the importer relies on.
  for (const auto& track : file.value().tracks) {
    for (const auto& note : track.notes) {
      if (note.end <= note.start || note.velocity == 0 || note.key > 127 || note.channel > 15)
        throw std::logic_error("MIDI reader accepted a malformed note");
    }
  }
}

void manifest(const std::uint8_t* bytes, std::size_t size) {
  if (size > max_input_bytes)
    return;
  const auto text = std::string_view(reinterpret_cast<const char*>(bytes), size);
  (void)persistence::detail::decode_manifest(text);
}

void wav(const std::uint8_t* bytes, std::size_t size) {
  if (size > max_input_bytes)
    return;
  // WavReader needs a pinned regular-file descriptor. An anonymous memfd
  // exercises the real fd parser without creating any filesystem entry.
  const int fd = ::memfd_create("oscillate-fuzz-wav", MFD_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("memfd_create unavailable");
  try {
    write_all(fd, bytes, size);
    auto source = media::WavReader::open_fd(fd);
    if (source && source.value().frame_count() != 0) {
      std::array<media::StereoFrame, 4> frames{};
      (void)source.value().read_at(0, std::span(frames));
      (void)source.value().read_at(source.value().frame_count() - 1, std::span(frames).first(1));
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
}

} // namespace oscillate::fuzz
