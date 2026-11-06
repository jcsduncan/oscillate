#pragma once

#include "oscillate/plugins/vst3_scan.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oscillate::plugins::detail {

inline constexpr std::string_view scan_magic{"LDV3"};
inline constexpr std::uint8_t scan_version = 1;
inline constexpr std::size_t max_field_bytes = 512;
inline constexpr std::size_t max_classes = 256;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_scan_result(const ScanResult& result);
[[nodiscard]] std::optional<ScanResult> decode_scan_result(std::span<const std::uint8_t> bytes);

} // namespace oscillate::plugins::detail
