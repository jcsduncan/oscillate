#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace oscillate::quick {

// One audio file in the user's sample folder.
struct SampleEntry {
  std::filesystem::path path;
  std::string name;          // File stem.
  std::string folder;        // Parent folder, relative to the library root ("" at the top).
  std::optional<double> bpm; // From the name only; previews also read the length.
  bool operator==(const SampleEntry&) const = default;
};

inline constexpr std::size_t max_library_samples = 5'000;
inline constexpr int max_library_depth = 6;

// Bounded, sorted scan of `root` for audio files (wav, aiff, flac, mp3; the
// encoded ones only when `encoded` is true). Hidden entries and symlinked
// directories are skipped; unreadable folders are passed over.
[[nodiscard]] std::vector<SampleEntry> scan_sample_folder(const std::filesystem::path& root,
                                                          bool encoded,
                                                          const std::stop_token& stop = {});

// Scans on a worker; the owner polls for the result on its own thread.
class SampleLibrary final {
public:
  SampleLibrary() = default;
  SampleLibrary(const SampleLibrary&) = delete;
  SampleLibrary& operator=(const SampleLibrary&) = delete;
  ~SampleLibrary() = default;

  // Starts a new scan, cancelling any earlier one.
  void scan(std::filesystem::path root, bool encoded);
  // The finished scan's entries, once; empty while scanning.
  [[nodiscard]] std::optional<std::vector<SampleEntry>> take();
  [[nodiscard]] bool scanning() const noexcept { return scanning_.load(std::memory_order_acquire); }

private:
  struct Shared {
    std::mutex mutex;
    std::optional<std::vector<SampleEntry>> ready;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::atomic<bool> scanning_{false};
  std::jthread worker_;
};

} // namespace oscillate::quick
