#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oscillate::plugins {

struct Vst3Class {
  std::string id;
  std::string category;
  std::string name;
  std::string vendor;
  std::string version;
  std::string subcategories;
};

struct Vst3Metadata {
  std::string vendor;
  std::vector<Vst3Class> classes;
};

// A scanner receipt pins the exact x86-64 bundle binary observed during a
// successful scan. It is verified again before in-process loading.
struct Vst3BinaryIdentity {
  std::filesystem::path canonical_bundle;
  std::filesystem::path canonical_binary;
  std::uint64_t byte_size{0};
  std::string sha256;
};

enum class ScanStatus : std::uint8_t {
  ok,
  invalid_bundle,
  invalid_metadata,
  scanner_error,
  crashed,
  timed_out,
  oversized_response,
  quarantined,
};

struct ScanResult {
  ScanStatus status{ScanStatus::scanner_error};
  Vst3Metadata metadata;
  std::string diagnostic;
  Vst3BinaryIdentity binary;
  [[nodiscard]] bool ok() const noexcept { return status == ScanStatus::ok; }
};

struct ScanLimits {
  std::chrono::milliseconds timeout{std::chrono::seconds(3)};
  std::size_t max_response_bytes{64UL * 1024UL};
  unsigned quarantine_after_failures{2};
};

// Lists bundle paths only; loading and metadata inspection require Vst3Scanner.
[[nodiscard]] std::vector<std::filesystem::path>
discover_vst3_bundles(std::span<const std::filesystem::path> roots, std::size_t max_bundles = 4096,
                      std::size_t max_entries = 100000);

// Worker/control-side service. It must never be called from the audio callback.
class Vst3Scanner {
public:
  explicit Vst3Scanner(std::filesystem::path helper, ScanLimits limits = {});
  [[nodiscard]] ScanResult scan(const std::filesystem::path& bundle);
  void retry(const std::filesystem::path& bundle);
  [[nodiscard]] bool is_quarantined(const std::filesystem::path& bundle) const;

private:
  [[nodiscard]] static std::string key(const std::filesystem::path& bundle);
  std::filesystem::path helper_;
  ScanLimits limits_;
  mutable std::mutex failures_mutex_;
  std::unordered_map<std::string, unsigned> failures_;
  std::unordered_map<std::string, std::uint64_t> retry_epochs_;
};

} // namespace oscillate::plugins
