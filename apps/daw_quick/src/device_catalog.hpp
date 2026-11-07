#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// What every built-in instrument and effect is, in one place: the browser
// lists each by what it does ("Compressor"), grouped by job ("Dynamics"),
// with its own name beside it ("Press"). Searches also read its keywords,
// so "comp", "glue" or "verb" find what a producer means.
namespace oscillate::quick {

// Browser sections, in the order they are listed: the ones reached for most
// come first within their tab.
enum class DeviceSection : std::uint8_t {
  synths,
  keys,
  drums,
  samplers,
  winds,
  dynamics,
  eq_filter,
  drive,
  space,
  texture,
  modulation,
  metering,
  tools,
};

struct DeviceLook {
  std::string_view id;
  std::string_view category; // Browser tab: Instruments, Drums, Effects or Utilities.
  DeviceSection section;
  std::string_view kind;         // What it is: "Compressor".
  std::string_view abbreviation; // Two characters for its tile: "Cp".
  std::string_view color;
  std::string_view blurb;    // One line for the browser list.
  std::string_view detail;   // One technical line for effect lists; may be empty.
  std::string_view about;    // A paragraph for the full browser's preview; may be empty.
  std::string_view keywords; // Lower-case words a search also matches.
};

// In listing order: by section, then within it.
[[nodiscard]] std::span<const DeviceLook> device_looks() noexcept;
[[nodiscard]] const DeviceLook* device_look(std::string_view id) noexcept;
[[nodiscard]] std::string_view section_name(DeviceSection section) noexcept;
// Utilities measure or route rather than colour the sound.
[[nodiscard]] bool is_utility(const DeviceLook& look) noexcept;
// The look's position in device_looks(), for sorting.
[[nodiscard]] std::optional<std::size_t> device_rank(std::string_view id) noexcept;

} // namespace oscillate::quick
