#include "oscillate/foundation/zip_writer.hpp"

#include "oscillate/foundation/file_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <format>
#include <utility>

#include <unistd.h>

namespace oscillate::foundation::zip {
namespace {

constexpr std::uint32_t local_header_signature = 0x04034b50;
constexpr std::uint32_t central_header_signature = 0x02014b50;
constexpr std::uint32_t end_signature = 0x06054b50;
// Version 1.0 suffices to extract a stored entry; "made by" 2.0 on Unix
// (3), so readers take the external attributes as a Unix mode.
constexpr std::uint16_t version_needed = 10;
constexpr std::uint16_t version_made_by = (3U << 8U) | 20U;
// General-purpose flag bit 11: names are UTF-8.
constexpr std::uint16_t utf8_names = 1U << 11U;
constexpr std::uint16_t method_stored = 0;
// Regular file, rw-r--r--, in the external attributes' high half.
constexpr std::uint32_t unix_file_attributes = 0100644U << 16U;
// Offset of the CRC-32 field inside a local header; both sizes follow it.
constexpr std::uint64_t local_crc_offset = 14;
constexpr std::uint32_t crc_polynomial = 0xEDB88320; // Reflected 0x04C11DB7.
constexpr std::uint32_t bits_per_byte = 8;
constexpr std::size_t byte_values = 256;
constexpr std::uint32_t byte_mask = 0xFF;
// MS-DOS dates start in 1980; seconds are kept in two-second steps.
constexpr int dos_epoch_year = 1980;
constexpr int tm_epoch_year = 1900;
constexpr unsigned dos_year_shift = 9;
constexpr unsigned dos_month_shift = 5;
constexpr unsigned dos_hour_shift = 11;
constexpr unsigned dos_minute_shift = 5;
constexpr int dos_seconds_step = 2;
constexpr std::uint16_t dos_epoch_date = (1U << dos_month_shift) | 1U; // 1980-01-01.

constexpr std::array<std::uint32_t, byte_values> crc_table = [] {
  std::array<std::uint32_t, byte_values> table{};
  for (std::uint32_t value = 0; value < byte_values; ++value) {
    std::uint32_t crc = value;
    for (std::uint32_t bit = 0; bit < bits_per_byte; ++bit)
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ crc_polynomial : crc >> 1U;
    table[value] = crc;
  }
  return table;
}();

class Record {
public:
  void u16(std::uint16_t value) {
    const std::uint32_t wide = value;
    for (std::size_t index = 0; index < sizeof value; ++index)
      bytes_.push_back(static_cast<std::byte>((wide >> (index * bits_per_byte)) & byte_mask));
  }
  void u32(std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof value; ++index)
      bytes_.push_back(static_cast<std::byte>((value >> (index * bits_per_byte)) & byte_mask));
  }
  void text(std::string_view value) {
    for (const char c : value)
      bytes_.push_back(static_cast<std::byte>(c));
  }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
  std::vector<std::byte> bytes_;
};

Error format_error(std::string message) {
  return {ErrorCode::invalid_argument, "zip: " + std::move(message)};
}

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > max_entries || name.front() == '/')
    return false;
  if (name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start <= name.size()) {
    const auto end = std::min(name.find('/', start), name.size());
    const auto part = name.substr(start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}

} // namespace

std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t previous) noexcept {
  std::uint32_t crc = ~previous;
  for (const auto byte : bytes)
    crc = crc_table[(crc ^ static_cast<std::uint32_t>(byte)) & byte_mask] ^ (crc >> bits_per_byte);
  return ~crc;
}

Writer::Writer(int fd, std::int64_t unix_seconds) : fd_(fd) {
  const auto value = static_cast<std::time_t>(unix_seconds);
  std::tm utc{};
  if (::gmtime_r(&value, &utc) == nullptr || utc.tm_year + tm_epoch_year < dos_epoch_year) {
    dos_time_ = 0;
    dos_date_ = dos_epoch_date;
    return;
  }
  dos_time_ = static_cast<std::uint16_t>((static_cast<unsigned>(utc.tm_hour) << dos_hour_shift) |
                                         (static_cast<unsigned>(utc.tm_min) << dos_minute_shift) |
                                         static_cast<unsigned>(utc.tm_sec / dos_seconds_step));
  dos_date_ = static_cast<std::uint16_t>(
      (static_cast<unsigned>(utc.tm_year + tm_epoch_year - dos_epoch_year) << dos_year_shift) |
      (static_cast<unsigned>(utc.tm_mon + 1) << dos_month_shift) |
      static_cast<unsigned>(utc.tm_mday));
}

Result<void> Writer::write(std::span<const std::byte> bytes) {
  if (offset_ + bytes.size() > max_bytes)
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "zip: the archive would exceed 4 GiB"});
  if (auto written = write_all(fd_, bytes, "zip: write"); !written)
    return written;
  offset_ += bytes.size();
  return Result<void>::success();
}

Result<void> Writer::begin(std::string_view name) {
  if (finished_ || open_)
    return Result<void>::failure(format_error("an entry is already open"));
  if (!valid_name(name))
    return Result<void>::failure(format_error(std::format("unusable entry name \"{}\"", name)));
  if (entries_.size() == max_entries)
    return Result<void>::failure({ErrorCode::resource_exhausted, "zip: too many entries"});
  Record header;
  header.u32(local_header_signature);
  header.u16(version_needed);
  header.u16(utf8_names);
  header.u16(method_stored);
  header.u16(dos_time_);
  header.u16(dos_date_);
  header.u32(0); // CRC-32, written by end().
  header.u32(0); // Compressed size.
  header.u32(0); // Uncompressed size.
  header.u16(static_cast<std::uint16_t>(name.size()));
  header.u16(0); // No extra field.
  header.text(name);
  Entry entry{.name = std::string(name), .header_offset = offset_};
  if (auto written = write(header.bytes()); !written)
    return written;
  entries_.push_back(std::move(entry));
  open_ = true;
  return Result<void>::success();
}

Result<void> Writer::append(std::span<const std::byte> bytes) {
  if (!open_)
    return Result<void>::failure(format_error("no entry is open"));
  if (auto written = write(bytes); !written)
    return written;
  auto& entry = entries_.back();
  entry.crc = crc32(bytes, entry.crc);
  entry.size += bytes.size();
  return Result<void>::success();
}

Result<void> Writer::append(std::string_view text) {
  return append(std::as_bytes(std::span(text.data(), text.size())));
}

Result<void> Writer::end() {
  if (!open_)
    return Result<void>::failure(format_error("no entry is open"));
  const auto& entry = entries_.back();
  Record fields;
  fields.u32(entry.crc);
  fields.u32(static_cast<std::uint32_t>(entry.size));
  fields.u32(static_cast<std::uint32_t>(entry.size));
  const auto bytes = fields.bytes();
  auto position = static_cast<off_t>(entry.header_offset + local_crc_offset);
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto written = ::pwrite(fd_, bytes.data() + done, bytes.size() - done,
                                  position + static_cast<off_t>(done));
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return Result<void>::failure(io_error("zip: write entry header"));
    done += static_cast<std::size_t>(written);
  }
  open_ = false;
  return Result<void>::success();
}

Result<void> Writer::add(std::string_view name, // NOLINT(bugprone-easily-swappable-parameters)
                         std::string_view content) {
  if (auto begun = begin(name); !begun)
    return begun;
  if (auto appended = append(content); !appended)
    return appended;
  return end();
}

Result<void> Writer::finish() {
  if (finished_ || open_)
    return Result<void>::failure(format_error("finish() needs every entry ended, once"));
  const auto directory_offset = offset_;
  for (const auto& entry : entries_) {
    Record header;
    header.u32(central_header_signature);
    header.u16(version_made_by);
    header.u16(version_needed);
    header.u16(utf8_names);
    header.u16(method_stored);
    header.u16(dos_time_);
    header.u16(dos_date_);
    header.u32(entry.crc);
    header.u32(static_cast<std::uint32_t>(entry.size));
    header.u32(static_cast<std::uint32_t>(entry.size));
    header.u16(static_cast<std::uint16_t>(entry.name.size()));
    header.u16(0); // Extra field.
    header.u16(0); // Comment.
    header.u16(0); // Disk number.
    header.u16(0); // Internal attributes.
    header.u32(unix_file_attributes);
    header.u32(static_cast<std::uint32_t>(entry.header_offset));
    header.text(entry.name);
    if (auto written = write(header.bytes()); !written)
      return written;
  }
  Record end;
  end.u32(end_signature);
  end.u16(0); // This disk.
  end.u16(0); // The central directory's disk.
  end.u16(static_cast<std::uint16_t>(entries_.size()));
  end.u16(static_cast<std::uint16_t>(entries_.size()));
  end.u32(static_cast<std::uint32_t>(offset_ - directory_offset));
  end.u32(static_cast<std::uint32_t>(directory_offset));
  end.u16(0); // Comment.
  if (auto written = write(end.bytes()); !written)
    return written;
  finished_ = true;
  return Result<void>::success();
}

} // namespace oscillate::foundation::zip
