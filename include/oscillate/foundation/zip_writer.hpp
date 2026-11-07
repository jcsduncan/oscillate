#pragma once

#include "oscillate/foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A ZIP archive writer for small archives of uncompressed ("stored") entries,
// such as a diagnostics bundle: no compression library, no ZIP64. Each entry
// streams through begin()/append()/end(); its CRC-32 and size are written
// back into its local header when it ends, so the file must be seekable.
// Control and worker threads only: it performs blocking I/O.
namespace oscillate::foundation::zip {

// The archive's limits without ZIP64: entries, and bytes of any offset or size.
inline constexpr std::size_t max_entries = 0xFFFF;
inline constexpr std::uint64_t max_bytes = 0xFFFFFFFF;

// CRC-32 (ISO-HDLC, as ZIP uses it). Pass the previous result to continue a
// running checksum; start from zero.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes,
                                  std::uint32_t previous = 0) noexcept;

class Writer {
public:
  // Writes to `fd`, an empty regular file opened for writing; the caller
  // keeps ownership. Entries are stamped with `unix_seconds` (UTC; ZIP keeps
  // two-second steps from 1980).
  Writer(int fd, std::int64_t unix_seconds);

  // Starts an entry named `name`: a relative '/'-separated path with no
  // empty, "." or ".." part. Fails while another entry is open.
  [[nodiscard]] Result<void> begin(std::string_view name);
  [[nodiscard]] Result<void> append(std::span<const std::byte> bytes);
  [[nodiscard]] Result<void> append(std::string_view text);
  // Ends the open entry and writes its checksum and size into its header.
  [[nodiscard]] Result<void> end();
  // begin(name), append(content), end().
  [[nodiscard]] Result<void> add(std::string_view name, std::string_view content);
  // Writes the central directory. No entry may be open; nothing may follow.
  [[nodiscard]] Result<void> finish();

  // Bytes written so far.
  [[nodiscard]] std::uint64_t size() const noexcept { return offset_; }

private:
  struct Entry {
    std::string name;
    std::uint64_t header_offset = 0;
    std::uint32_t crc = 0;
    std::uint64_t size = 0;
  };

  [[nodiscard]] Result<void> write(std::span<const std::byte> bytes);

  int fd_;
  std::uint16_t dos_time_;
  std::uint16_t dos_date_;
  std::uint64_t offset_ = 0;
  std::vector<Entry> entries_;
  bool open_ = false;
  bool finished_ = false;
};

} // namespace oscillate::foundation::zip
