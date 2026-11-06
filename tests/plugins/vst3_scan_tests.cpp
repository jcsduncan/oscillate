#include "oscillate/foundation/log.hpp"
#include "oscillate/plugins/vst3_scan.hpp"
#include "oscillate/test/check.hpp"
#include "scan_protocol.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

using oscillate::plugins::ScanLimits;
using oscillate::plugins::ScanStatus;
using oscillate::plugins::Vst3Scanner;
namespace fs = std::filesystem;

void run_tests() {
  const fs::path root = VST3_FIXTURE_ROOT;
  const fs::path helper = VST3_HELPER_PATH;
  const std::array<fs::path, 2> roots{root, root};
  const auto bundles = oscillate::plugins::discover_vst3_bundles(roots);
  REQUIRE_EQ(bundles.size(), 7);
  REQUIRE_TRUE(oscillate::plugins::discover_vst3_bundles(roots, 0).empty());
  REQUIRE_TRUE(oscillate::plugins::discover_vst3_bundles(roots, 4096, 0).empty());
  REQUIRE_LE(oscillate::plugins::discover_vst3_bundles(roots, 4096, 2).size(), 2);
  const std::array<fs::path, 1> absent_roots{root / "absent"};
  REQUIRE_TRUE(oscillate::plugins::discover_vst3_bundles(absent_roots).empty());
  Vst3Scanner scanner(helper, ScanLimits{.timeout = std::chrono::seconds(10),
                                         .max_response_bytes = 64UL * 1024UL,
                                         .quarantine_after_failures = 2});
  const auto valid = root / "Valid.vst3";
  const auto good = scanner.scan(valid);
  REQUIRE_TRUE(good.ok());
  REQUIRE_EQ(good.metadata.vendor, "Fixture Vendor");
  REQUIRE_EQ(good.metadata.classes.size(), 1);
  REQUIRE_EQ(good.metadata.classes[0].id, "0102030405060708090a0b0c0d0e0f10");
  REQUIRE_EQ(good.metadata.classes[0].name, "Fixture Effect");
  REQUIRE_EQ(good.metadata.classes[0].version, "1.2.3");
  REQUIRE_EQ(good.binary.canonical_bundle, fs::canonical(valid));
  REQUIRE_EQ(good.binary.canonical_binary, fs::canonical(valid / "Contents/x86_64-linux/Valid.so"));
  REQUIRE_EQ(good.binary.byte_size, fs::file_size(good.binary.canonical_binary));
  REQUIRE_EQ(good.binary.sha256.size(), 64);
  REQUIRE_TRUE(!scanner.is_quarantined(valid));
  Vst3Scanner large_scanner(helper, ScanLimits{.timeout = std::chrono::seconds(10),
                                               .max_response_bytes = 128UL * 1024UL,
                                               .quarantine_after_failures = 2});
  const auto large = large_scanner.scan(root / "Large.vst3");
  REQUIRE_TRUE(large.ok());
  REQUIRE_EQ(large.metadata.classes.size(), 256);
  REQUIRE_NE(large.metadata.classes.front().id, large.metadata.classes.back().id);
  const auto encoded = oscillate::plugins::detail::encode_scan_result(good);
  REQUIRE_OK(encoded);
  auto wire = *encoded;
  const auto decoded = oscillate::plugins::detail::decode_scan_result(wire);
  REQUIRE_OK(decoded);
  REQUIRE_EQ(decoded->metadata.classes[0].id, good.metadata.classes[0].id);
  wire.push_back(0);
  REQUIRE_TRUE(!oscillate::plugins::detail::decode_scan_result(wire));
  wire.pop_back();
  wire[0] = 0;
  REQUIRE_TRUE(!oscillate::plugins::detail::decode_scan_result(wire));
  wire[0] = 'L';
  wire[4] = 99;
  REQUIRE_TRUE(!oscillate::plugins::detail::decode_scan_result(wire));
  wire[4] = oscillate::plugins::detail::scan_version;
  wire[5] = 255;
  REQUIRE_TRUE(!oscillate::plugins::detail::decode_scan_result(wire));
  wire[5] = static_cast<std::uint8_t>(ScanStatus::ok);
  wire.resize(7);
  REQUIRE_TRUE(!oscillate::plugins::detail::decode_scan_result(wire));

  auto oversized = good;
  oversized.metadata.classes[0].name.assign(oscillate::plugins::detail::max_field_bytes + 1, 'x');
  REQUIRE_TRUE(!oscillate::plugins::detail::encode_scan_result(oversized));
  oversized.metadata.classes[0].name.assign(oscillate::plugins::detail::max_field_bytes, 'x');
  REQUIRE_TRUE(oscillate::plugins::detail::encode_scan_result(oversized));
  oversized.metadata.classes.resize(oscillate::plugins::detail::max_classes + 1);
  REQUIRE_TRUE(!oscillate::plugins::detail::encode_scan_result(oversized));

  const auto malformed = root / "Malformed.vst3";
  REQUIRE_EQ(scanner.scan(malformed).status, ScanStatus::invalid_metadata);
  REQUIRE_EQ(scanner.scan(malformed).status, ScanStatus::invalid_metadata);
  REQUIRE_EQ(scanner.scan(malformed).status, ScanStatus::quarantined);
  scanner.retry(malformed);
  REQUIRE_EQ(scanner.scan(malformed).status, ScanStatus::invalid_metadata);

  const auto crashed = root / "Crash.vst3";
  // The plugin's stderr reaches the log instead of /dev/null.
  const auto log_directory =
      fs::temp_directory_path() / ("oscillate-scan-log-" + std::to_string(::getpid()));
  fs::remove_all(log_directory);
  oscillate::foundation::log::configure(
      {.filter = {}, .to_stderr = false, .directory = log_directory});
  REQUIRE_EQ(scanner.scan(crashed).status, ScanStatus::crashed);
  {
    std::ifstream log_file(log_directory / "oscillate.log");
    const std::string logged{std::istreambuf_iterator<char>(log_file), {}};
    REQUIRE_NE(logged.find("fixture crash: about to fault"), std::string::npos);
    REQUIRE_NE(logged.find("warning plugin ["), std::string::npos);
  }
  oscillate::foundation::log::configure({.filter = {}, .to_stderr = false, .directory = {}});
  fs::remove_all(log_directory);
  REQUIRE_EQ(scanner.scan(crashed).status, ScanStatus::crashed);
  REQUIRE_TRUE(scanner.is_quarantined(crashed));
  const auto crash_alias = root.parent_path() / "CrashAlias.vst3";
  fs::remove(crash_alias);
  fs::create_directory_symlink(crashed, crash_alias);
  REQUIRE_TRUE(scanner.is_quarantined(crash_alias));
  REQUIRE_EQ(scanner.scan(crash_alias).status, ScanStatus::quarantined);
  scanner.retry(crash_alias);
  REQUIRE_TRUE(!scanner.is_quarantined(crashed));
  fs::remove(crash_alias);
  REQUIRE_EQ(scanner.scan(root / "NullFactory.vst3").status, ScanStatus::invalid_metadata);
  REQUIRE_EQ(scanner.scan(root / "NoExports.vst3").status, ScanStatus::invalid_bundle);

  const auto hung = root / "Hang.vst3";
  Vst3Scanner short_timeout(helper, ScanLimits{.timeout = std::chrono::milliseconds(100),
                                               .max_response_bytes = 64UL * 1024UL,
                                               .quarantine_after_failures = 2});
  REQUIRE_EQ(short_timeout.scan(hung).status, ScanStatus::timed_out);
  REQUIRE_EQ(short_timeout.scan(hung).status, ScanStatus::timed_out);
  REQUIRE_TRUE(short_timeout.is_quarantined(hung));

  // A scanner helper can exit while a plugin-spawned descendant retains its
  // stdout pipe. The supervisor must kill that process group on timeout.
  const auto descendant_pid_file = root.parent_path() / "scan-descendant.pid";
  fs::remove(descendant_pid_file);
  REQUIRE_EQ(setenv("OSCILLATE_SCAN_DESCENDANT_PID_FILE", descendant_pid_file.c_str(), 1), 0);
  Vst3Scanner descendant_scanner(VST3_DESCENDANT_HELPER_PATH,
                                 ScanLimits{.timeout = std::chrono::milliseconds(100),
                                            .max_response_bytes = 64UL * 1024UL,
                                            .quarantine_after_failures = 2});
  REQUIRE_EQ(descendant_scanner.scan(valid).status, ScanStatus::timed_out);
  REQUIRE_EQ(unsetenv("OSCILLATE_SCAN_DESCENDANT_PID_FILE"), 0);
  std::ifstream pid_input(descendant_pid_file);
  pid_t descendant_pid = 0;
  REQUIRE_TRUE((pid_input >> descendant_pid) && descendant_pid > 0);
  const auto pid_fd = static_cast<int>(syscall(SYS_pidfd_open, descendant_pid, 0));
  if (pid_fd >= 0) {
    pollfd process{.fd = pid_fd, .events = POLLIN, .revents = 0};
    REQUIRE_TRUE(poll(&process, 1, 250) == 1 && (process.revents & POLLIN) != 0);
    close(pid_fd);
  } else {
    REQUIRE_EQ(errno, ESRCH);
  }
  fs::remove(descendant_pid_file);

  REQUIRE_EQ(scanner.scan(root / "Missing.vst3").status, ScanStatus::invalid_bundle);
  REQUIRE_EQ(scanner.scan(root / "Missing.vst3").status, ScanStatus::invalid_bundle);
  REQUIRE_TRUE(!scanner.is_quarantined(root / "Missing.vst3"));
  const auto empty = root.parent_path() / "Empty.vst3";
  fs::create_directory(empty);
  REQUIRE_EQ(scanner.scan(empty).status, ScanStatus::invalid_bundle);
  const auto invalid_binary =
      root.parent_path() / "InvalidBinary.vst3" / "Contents" / "x86_64-linux";
  fs::create_directories(invalid_binary);
  std::ofstream(invalid_binary / "InvalidBinary.so") << "not an ELF library";
  REQUIRE_EQ(scanner.scan(root.parent_path() / "InvalidBinary.vst3").status,
             ScanStatus::invalid_bundle);
  const auto empty_binary = root.parent_path() / "ZeroBinary.vst3/Contents/x86_64-linux";
  fs::create_directories(empty_binary);
  {
    std::ofstream file(empty_binary / "ZeroBinary.so");
    REQUIRE_TRUE(file.good());
  }
  REQUIRE_EQ(scanner.scan(root.parent_path() / "ZeroBinary.vst3").status,
             ScanStatus::invalid_bundle);
  const auto escaped_binary = root.parent_path() / "Escaped.vst3/Contents/x86_64-linux";
  fs::create_directories(escaped_binary);
  fs::remove(escaped_binary / "Escaped.so");
  fs::create_symlink(good.binary.canonical_binary, escaped_binary / "Escaped.so");
  REQUIRE_EQ(scanner.scan(root.parent_path() / "Escaped.vst3").status, ScanStatus::invalid_bundle);
  const auto oversized_binary = root.parent_path() / "Oversized.vst3/Contents/x86_64-linux";
  fs::create_directories(oversized_binary);
  {
    std::ofstream file(oversized_binary / "Oversized.so");
    REQUIRE_TRUE(file.good());
  }
  fs::resize_file(oversized_binary / "Oversized.so", 513ULL * 1024ULL * 1024ULL);
  REQUIRE_EQ(scanner.scan(root.parent_path() / "Oversized.vst3").status,
             ScanStatus::invalid_bundle);
  Vst3Scanner tiny(helper, ScanLimits{.timeout = std::chrono::seconds(10),
                                      .max_response_bytes = 16,
                                      .quarantine_after_failures = 2});
  REQUIRE_EQ(tiny.scan(valid).status, ScanStatus::oversized_response);
  Vst3Scanner missing_helper(root / "missing-helper");
  REQUIRE_EQ(missing_helper.scan(valid).status, ScanStatus::scanner_error);
  Vst3Scanner no_helper({});
  REQUIRE_EQ(no_helper.scan(valid).status, ScanStatus::scanner_error);
  Vst3Scanner malformed_response("/bin/true");
  REQUIRE_EQ(malformed_response.scan(valid).status, ScanStatus::invalid_metadata);
  Vst3Scanner failed_helper("/bin/false");
  REQUIRE_EQ(failed_helper.scan(valid).status, ScanStatus::scanner_error);
}

int main() {
  try {
    run_tests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
