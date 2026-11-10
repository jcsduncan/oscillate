#pragma once

#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/sha256.hpp"
#include "oscillate/test/check.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

// Rewrites a saved project's current generation as another Oscillate release
// would have written it, for tests of opening older and newer schemas. Users
// link nlohmann_json and oscillate::foundation_digest.
namespace oscillate::test {

namespace detail {
inline std::string read_saved_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "saved file is readable: " + path.string());
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

inline void write_saved_file(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  require(out.good(), "saved file is writable: " + path.string());
}
} // namespace detail

// `edit` changes the parsed manifest of the generation CURRENT names; the
// manifest's checksum file and CURRENT then match the new bytes.
inline void rewrite_current_manifest(const std::filesystem::path& root,
                                     const std::function<void(nlohmann::json&)>& edit) {
  const auto state = root / "state";
  const auto pointer = nlohmann::json::parse(detail::read_saved_file(state / "CURRENT"));
  const auto generation = pointer.at("generation").get<std::string>();
  const auto directory = state / "generations" / generation;
  auto manifest = nlohmann::json::parse(detail::read_saved_file(directory / "manifest.json"));
  edit(manifest);
  const auto bytes = manifest.dump() + '\n';
  const auto digest = take(foundation::sha256(bytes));
  const auto hash = foundation::to_hex(std::span(digest));
  detail::write_saved_file(directory / "manifest.json", bytes);
  detail::write_saved_file(directory / "manifest.sha256", hash + '\n');
  detail::write_saved_file(state / "CURRENT",
                           nlohmann::json{{"generation", generation}, {"sha256", hash}}.dump() +
                               '\n');
}

// Sets the manifest's saved schema.
inline void set_manifest_schema(nlohmann::json& manifest, std::uint32_t major,
                                std::uint32_t minor) {
  manifest["schema"] = {{"major", major}, {"minor", minor}};
}

} // namespace oscillate::test
