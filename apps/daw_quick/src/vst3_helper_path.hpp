#pragma once

#include <filesystem>
#include <system_error>

namespace oscillate::quick::detail {

[[nodiscard]] inline std::filesystem::path vst3_helper_path() {
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error)
    return {};
  return executable.parent_path() / "oscillate_vst3_scan_helper";
}

} // namespace oscillate::quick::detail
