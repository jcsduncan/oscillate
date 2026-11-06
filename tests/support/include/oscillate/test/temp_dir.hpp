#pragma once

// A real, uniquely named directory under $TMPDIR for a test's files, removed
// with everything in it when the TempDir goes out of scope. Tests that check
// persistence use this rather than a mocked filesystem.
//
//   const oscillate::test::TempDir directory("oscillate-store");
//   write(directory / "project.json");

#include "oscillate/test/check.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oscillate::test {

class TempDir {
public:
  // `prefix` names the directory, so a leftover one says which test made it.
  explicit TempDir(std::string_view prefix = "oscillate-test") {
    std::string pattern =
        (std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
    const char* made = ::mkdtemp(pattern.data());
    require(made != nullptr, "temporary directory could not be created from " + pattern);
    path_ = made;
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::exchange(other.path_, {});
    }
    return *this;
  }
  ~TempDir() { remove(); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& child) const {
    return path_ / child;
  }

private:
  void remove() noexcept {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  std::filesystem::path path_;
};

} // namespace oscillate::test
