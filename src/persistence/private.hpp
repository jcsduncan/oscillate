#pragma once

#include "oscillate/persistence/store.hpp"

#include <string>
#include <string_view>

namespace oscillate::persistence::detail {

inline constexpr std::size_t max_manifest_bytes = std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t max_tracks = 4096;
inline constexpr std::size_t max_clips = 100'000;
inline constexpr std::size_t max_events = 100'000;
inline constexpr std::size_t max_sources = project::max_source_records;
// Random generation, stage and store-token names are this many bytes as hex.
inline constexpr std::size_t random_name_bytes = 16;

[[nodiscard]] bool hex_name(std::string_view value);
[[nodiscard]] foundation::Result<std::string> random_name();

[[nodiscard]] std::string encode_manifest(const project::Snapshot& snapshot,
                                          std::string_view generation,
                                          const std::optional<std::string>& parent, SaveKind kind);
[[nodiscard]] foundation::Result<LoadedGeneration> decode_manifest(std::string_view text);
[[nodiscard]] std::string encode_pointer(std::string_view generation, std::string_view hash);
[[nodiscard]] foundation::Result<std::pair<std::string, std::string>>
decode_pointer(std::string_view text);

struct NamedPointer {
  std::string name;
  std::string generation;
  std::string hash;
};
[[nodiscard]] bool valid_revision_name(std::string_view name);
[[nodiscard]] std::string encode_named_pointer(const NamedPointer& pointer);
[[nodiscard]] foundation::Result<NamedPointer> decode_named_pointer(std::string_view text);

} // namespace oscillate::persistence::detail
