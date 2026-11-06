#include "scan_protocol.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace oscillate::plugins::detail {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_string(std::vector<std::uint8_t>& out, std::string_view value) {
  const auto size = static_cast<std::uint16_t>(value.size());
  put_u16(out, size);
  out.insert(out.end(), value.begin(), value.begin() + size);
}

class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  [[nodiscard]] std::optional<std::uint8_t> byte() {
    if (offset_ >= bytes_.size())
      return std::nullopt;
    return bytes_[offset_++];
  }
  [[nodiscard]] std::optional<std::uint16_t> u16() {
    const auto lo = byte();
    const auto hi = byte();
    if (!lo || !hi)
      return std::nullopt;
    return static_cast<std::uint16_t>(*lo | (static_cast<std::uint16_t>(*hi) << 8));
  }
  [[nodiscard]] std::optional<std::string> str() {
    const auto size = u16();
    if (!size || *size > max_field_bytes || bytes_.size() - offset_ < *size)
      return std::nullopt;
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    offset_ += *size;
    return std::string(begin, *size);
  }
  [[nodiscard]] bool done() const { return offset_ == bytes_.size(); }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_{0};
};

} // namespace

std::optional<std::vector<std::uint8_t>> encode_scan_result(const ScanResult& result) {
  if (result.diagnostic.size() > max_field_bytes ||
      result.metadata.vendor.size() > max_field_bytes ||
      result.metadata.classes.size() > max_classes)
    return std::nullopt;
  for (const auto& item : result.metadata.classes) {
    if (item.id.size() > max_field_bytes || item.category.size() > max_field_bytes ||
        item.name.size() > max_field_bytes || item.vendor.size() > max_field_bytes ||
        item.version.size() > max_field_bytes || item.subcategories.size() > max_field_bytes)
      return std::nullopt;
  }
  std::vector<std::uint8_t> out;
  out.reserve(256);
  out.insert(out.end(), scan_magic.begin(), scan_magic.end());
  out.push_back(scan_version);
  out.push_back(static_cast<std::uint8_t>(result.status));
  put_string(out, result.diagnostic);
  put_string(out, result.metadata.vendor);
  put_u16(out, static_cast<std::uint16_t>(result.metadata.classes.size()));
  for (const auto& item : result.metadata.classes) {
    put_string(out, item.id);
    put_string(out, item.category);
    put_string(out, item.name);
    put_string(out, item.vendor);
    put_string(out, item.version);
    put_string(out, item.subcategories);
  }
  return out;
}

std::optional<ScanResult> decode_scan_result(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  for (const auto expected : scan_magic) {
    if (reader.byte() != static_cast<std::uint8_t>(expected))
      return std::nullopt;
  }
  if (reader.byte() != scan_version)
    return std::nullopt;
  const auto status = reader.byte();
  if (!status || *status > static_cast<std::uint8_t>(ScanStatus::quarantined))
    return std::nullopt;
  const auto diagnostic = reader.str();
  const auto vendor = reader.str();
  const auto count = reader.u16();
  if (!diagnostic || !vendor || !count || *count > max_classes)
    return std::nullopt;
  ScanResult result;
  result.status = static_cast<ScanStatus>(*status);
  result.diagnostic = *diagnostic;
  result.metadata.vendor = *vendor;
  result.metadata.classes.reserve(*count);
  for (std::uint16_t i = 0; i < *count; ++i) {
    auto id = reader.str();
    auto category = reader.str();
    auto name = reader.str();
    auto class_vendor = reader.str();
    auto version = reader.str();
    auto subcategories = reader.str();
    if (!id || !category || !name || !class_vendor || !version || !subcategories)
      return std::nullopt;
    result.metadata.classes.push_back(
        {*id, *category, *name, *class_vendor, *version, *subcategories});
  }
  if (!reader.done())
    return std::nullopt;
  return result;
}

} // namespace oscillate::plugins::detail
