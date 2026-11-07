#include "oscillate/foundation/diagnostics_bundle.hpp"
#include "oscillate/foundation/zip_writer.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

namespace bundle = oscillate::foundation::diagnostics_bundle;
namespace zip = oscillate::foundation::zip;
using oscillate::test::take;

// An independent reading of the ZIP format (APPNOTE 6.3.10, sections 4.3.7,
// 4.3.12 and 4.3.16), so the writer is not checked against itself.
constexpr std::uint32_t local_signature = 0x04034b50;
constexpr std::uint32_t central_signature = 0x02014b50;
constexpr std::uint32_t end_signature = 0x06054b50;
constexpr std::size_t end_record_bytes = 22;
constexpr std::size_t end_entries_at = 10;
constexpr std::size_t end_directory_size_at = 12;
constexpr std::size_t end_directory_offset_at = 16;
constexpr std::size_t central_header_bytes = 46;
constexpr std::size_t central_method_at = 10;
constexpr std::size_t central_crc_at = 16;
constexpr std::size_t central_compressed_at = 20;
constexpr std::size_t central_size_at = 24;
constexpr std::size_t central_name_length_at = 28;
constexpr std::size_t central_extra_length_at = 30;
constexpr std::size_t central_comment_length_at = 32;
constexpr std::size_t central_attributes_at = 38;
constexpr std::size_t central_offset_at = 42;
constexpr std::size_t local_header_bytes = 30;
constexpr std::size_t local_crc_at = 14;
constexpr std::size_t local_compressed_at = 18;
constexpr std::size_t local_size_at = 22;
constexpr std::size_t local_name_length_at = 26;
constexpr std::size_t local_extra_length_at = 28;
constexpr std::uint16_t method_stored = 0;
constexpr unsigned attribute_shift = 16;
constexpr std::uint32_t regular_file_mode = 0100644;
constexpr unsigned bits_per_byte = 8;
constexpr std::uint32_t crc_polynomial = 0xEDB88320;
// CRC-32/ISO-HDLC of "123456789" (the catalogue's check value).
constexpr std::string_view crc_check_input = "123456789";
constexpr std::uint32_t crc_check_value = 0xCBF43926;

constexpr std::string_view home = "/home/listener";
constexpr std::string_view audio_payload = "RIFF-audio-payload-that-must-stay-local";
constexpr std::string_view older_log_line = "older line /home/listener/Music/song";
constexpr std::string_view newer_log_line = "newer line";
constexpr std::string_view older_report_name = "oscillate-crash-20261101T100000Z.txt";
constexpr std::string_view newer_report_name = "oscillate-crash-20261105T100000Z.txt";
constexpr std::string_view newer_report_text = "newer crash in /home/listener/projects\n";
constexpr std::string_view trace_name = "trace-20261107-120000.json";
constexpr std::string_view trace_text = "{\"traceEvents\":[\n{\"name\":\"/home/listener/x\"}\n]}\n";
constexpr std::string_view plugin_name = "Glass Synth";
constexpr std::string_view plugin_version = "1.2.3";
constexpr std::string_view plugin_status = "ready";
constexpr std::string_view plugin_note = "One bundle failed.";
constexpr std::string_view feature_name = "vst3 host";
constexpr std::string_view feature_value = "yes";
constexpr std::string_view application_fact = "application";
constexpr std::string_view application_value = "Oscillate test";
constexpr auto older_age = std::chrono::hours(2);
constexpr auto newer_age = std::chrono::hours(1);
constexpr std::filesystem::perms owner_only = std::filesystem::perms::owner_all;
constexpr std::filesystem::perms owner_read_write =
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
constexpr std::size_t trace_boundary_lead = 9;

struct Entry {
  std::string data;
  std::uint32_t attributes = 0;
};

std::uint32_t bitwise_crc(std::string_view data) {
  std::uint32_t crc = ~std::uint32_t{0};
  for (const char c : data) {
    crc ^= static_cast<std::uint8_t>(c);
    for (unsigned bit = 0; bit < bits_per_byte; ++bit)
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ crc_polynomial : crc >> 1U;
  }
  return ~crc;
}

std::uint32_t little_endian(std::string_view bytes, std::size_t at, std::size_t width) {
  REQUIRE_LE(at + width, bytes.size(), "a field inside the archive");
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < width; ++index)
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + index]))
             << (index * bits_per_byte);
  return value;
}
std::uint32_t u16(std::string_view bytes, std::size_t at) {
  return little_endian(bytes, at, sizeof(std::uint16_t));
}
std::uint32_t u32(std::string_view bytes, std::size_t at) {
  return little_endian(bytes, at, sizeof(std::uint32_t));
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << text;
}

// Every entry by name, each checked against its local header and CRC.
std::map<std::string, Entry> read_zip(const std::filesystem::path& path) {
  const auto bytes = read_text(path);
  REQUIRE_GE(bytes.size(), end_record_bytes);
  const auto end = bytes.size() - end_record_bytes;
  REQUIRE_EQ(u32(bytes, end), end_signature, "the archive ends with its end record");
  const auto count = u16(bytes, end + end_entries_at);
  const auto directory_size = u32(bytes, end + end_directory_size_at);
  auto at = static_cast<std::size_t>(u32(bytes, end + end_directory_offset_at));
  REQUIRE_EQ(at + directory_size, end, "the central directory precedes the end record");
  std::map<std::string, Entry> entries;
  for (std::uint32_t index = 0; index < count; ++index) {
    REQUIRE_EQ(u32(bytes, at), central_signature);
    REQUIRE_EQ(u16(bytes, at + central_method_at), std::uint32_t{method_stored});
    const auto crc = u32(bytes, at + central_crc_at);
    const auto size = u32(bytes, at + central_size_at);
    REQUIRE_EQ(u32(bytes, at + central_compressed_at), size, "stored entries are not compressed");
    const auto name_length = u16(bytes, at + central_name_length_at);
    const auto skip =
        u16(bytes, at + central_extra_length_at) + u16(bytes, at + central_comment_length_at);
    const auto attributes = u32(bytes, at + central_attributes_at);
    const auto local = static_cast<std::size_t>(u32(bytes, at + central_offset_at));
    const auto name = bytes.substr(at + central_header_bytes, name_length);
    at += central_header_bytes + name_length + skip;

    REQUIRE_EQ(u32(bytes, local), local_signature);
    REQUIRE_EQ(u32(bytes, local + local_crc_at), crc, "the local header has the CRC");
    REQUIRE_EQ(u32(bytes, local + local_compressed_at), size);
    REQUIRE_EQ(u32(bytes, local + local_size_at), size);
    const auto local_name_length = u16(bytes, local + local_name_length_at);
    REQUIRE_EQ(bytes.substr(local + local_header_bytes, local_name_length), name);
    const auto data_at =
        local + local_header_bytes + local_name_length + u16(bytes, local + local_extra_length_at);
    REQUIRE_LE(data_at + size, bytes.size());
    auto data = bytes.substr(data_at, size);
    REQUIRE_EQ(bitwise_crc(data), crc, "the CRC matches the data");
    REQUIRE_TRUE(entries.emplace(name, Entry{std::move(data), attributes}).second,
                 "names are unique");
  }
  return entries;
}

std::filesystem::path fresh_root(std::string_view name) {
  auto root = std::filesystem::temp_directory_path() /
              ("oscillate-" + std::string(name) + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

std::vector<std::string> names(const std::map<std::string, Entry>& entries) {
  std::vector<std::string> result;
  result.reserve(entries.size());
  for (const auto& [name, entry] : entries)
    result.push_back(name);
  return result;
}

void crc_matches_the_check_value_and_continues() {
  const auto bytes = std::as_bytes(std::span(crc_check_input.data(), crc_check_input.size()));
  REQUIRE_EQ(zip::crc32(bytes), crc_check_value);
  const auto split = crc_check_input.size() / 2;
  REQUIRE_EQ(zip::crc32(bytes.subspan(split), zip::crc32(bytes.first(split))), crc_check_value,
             "a running checksum equals the whole one");
  REQUIRE_EQ(zip::crc32(bytes), bitwise_crc(crc_check_input));
}

void zip_round_trip() {
  const auto root = fresh_root("zip-round-trip");
  const auto path = root / "test.zip";
  {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE_TRUE(fd >= 0);
    zip::Writer writer(fd, 0);
    take(writer.add("a.txt", "hello"));
    take(writer.begin("folder/b.txt"));
    take(writer.append("first "));
    take(writer.append("second"));
    take(writer.end());
    take(writer.add("empty.txt", ""));
    take(writer.finish());
    REQUIRE_FALSE(writer.add("late.txt", "x").has_value(), "nothing follows finish()");
    ::close(fd);
  }
  const auto entries = read_zip(path);
  REQUIRE_EQ(entries.size(), std::size_t{3});
  REQUIRE_EQ(entries.at("a.txt").data, std::string("hello"));
  REQUIRE_EQ(entries.at("folder/b.txt").data, std::string("first second"));
  REQUIRE_EQ(entries.at("empty.txt").data, std::string{});
  REQUIRE_EQ(entries.at("a.txt").attributes >> attribute_shift, regular_file_mode,
             "entries extract as ordinary files");
  std::filesystem::remove_all(root);
}

void zip_refuses_unsafe_names_and_misuse() {
  const auto root = fresh_root("zip-misuse");
  const auto path = root / "test.zip";
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  REQUIRE_TRUE(fd >= 0);
  zip::Writer writer(fd, 0);
  for (const std::string_view name : {"", "/abs", "../up", "a/../b", "a//b", "a/./b", "a\\b", "a/"})
    REQUIRE_FALSE(writer.begin(name).has_value(), "unsafe entry name refused");
  REQUIRE_FALSE(writer.append("x").has_value(), "no entry is open");
  REQUIRE_FALSE(writer.end().has_value(), "no entry is open");
  take(writer.begin("open.txt"));
  REQUIRE_FALSE(writer.begin("second.txt").has_value(), "one entry at a time");
  REQUIRE_FALSE(writer.finish().has_value(), "an open entry blocks finish()");
  take(writer.end());
  take(writer.finish());
  ::close(fd);
  REQUIRE_EQ(names(read_zip(path)), std::vector<std::string>{"open.txt"});
  std::filesystem::remove_all(root);
}

struct Fixture {
  std::filesystem::path root;
  bundle::Options options;
};

// A state directory with two logs, two crash reports and a trace, and audio
// beside the logs and in a project that a bundle must never pick up.
Fixture full_fixture(std::string_view name) {
  Fixture fixture;
  fixture.root = fresh_root(name);
  const auto logs = fixture.root / "logs";
  const auto reports = fixture.root / "crash-reports";
  write_text(logs / "oscillate.1.log", std::string(older_log_line) + "\n");
  write_text(logs / "oscillate.log", std::string(newer_log_line) + "\n");
  write_text(logs / "take.wav", audio_payload);
  write_text(reports / "take.wav", audio_payload);
  write_text(fixture.root / "projects" / "song" / "take.wav", audio_payload);
  write_text(reports / older_report_name, "older crash\n");
  write_text(reports / newer_report_name, newer_report_text);
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(reports / older_report_name, now - older_age);
  std::filesystem::last_write_time(reports / newer_report_name, now - newer_age);
  write_text(fixture.root / "traces" / trace_name, trace_text);

  auto& options = fixture.options;
  options.directory = fixture.root / "diagnostics";
  options.crash_directory = reports;
  options.log_directory = logs;
  options.home = std::string(home);
  options.application = {{std::string(application_fact), std::string(application_value)}};
  options.build = {{std::string(feature_name), std::string(feature_value)}};
  options.plugins = {
      {std::string(plugin_name), std::string(plugin_version), std::string(plugin_status)}};
  options.plugin_note = std::string(plugin_note);
  options.trace = fixture.root / "traces" / trace_name;
  return fixture;
}

void bundle_holds_exactly_the_documented_entries() {
  const auto fixture = full_fixture("diagnostics-full");
  const auto path = take(bundle::write(fixture.options));
  REQUIRE_EQ(path.parent_path(), fixture.options.directory);
  const auto file_name = path.filename().string();
  REQUIRE_TRUE(file_name.starts_with(bundle::bundle_prefix) &&
               file_name.ends_with(bundle::bundle_extension));
  REQUIRE_EQ(std::filesystem::status(path).permissions(), owner_read_write,
             "only the user reads it");
  REQUIRE_EQ(std::filesystem::status(fixture.options.directory).permissions(), owner_only);
  REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(fixture.options.directory),
                           std::filesystem::directory_iterator{}),
             std::ptrdiff_t{1}, "no partial file is left beside the bundle");

  const auto entries = read_zip(path);
  const std::vector<std::string> expected{
      std::string(bundle::readme_entry),
      std::string(bundle::build_entry),
      std::string(bundle::crash_folder) + std::string(newer_report_name),
      std::string(bundle::logs_folder) + "oscillate.1.log",
      std::string(bundle::logs_folder) + "oscillate.log",
      std::string(bundle::plugins_entry),
      std::string(bundle::session_entry),
      std::string(bundle::trace_folder) + std::string(trace_name),
  };
  auto sorted_expected = expected;
  std::ranges::sort(sorted_expected);
  REQUIRE_EQ(names(entries), sorted_expected, "logs, the newest crash report and the trace only");

  for (const auto& [name, entry] : entries) {
    REQUIRE_EQ(entry.data.find(audio_payload), std::string::npos, "no audio in " + name);
    REQUIRE_EQ(entry.data.find(home), std::string::npos, "the home path is redacted in " + name);
  }
  REQUIRE_NE(entries.at(std::string(bundle::logs_folder) + "oscillate.1.log")
                 .data.find("older line ~/Music/song"),
             std::string::npos);
  REQUIRE_NE(entries.at(std::string(bundle::crash_folder) + std::string(newer_report_name))
                 .data.find("newer crash in ~/projects"),
             std::string::npos);
  REQUIRE_NE(entries.at(std::string(bundle::trace_folder) + std::string(trace_name))
                 .data.find("{\"name\":\"~/x\"}"),
             std::string::npos, "the trace is included, redacted");
  REQUIRE_EQ(entries.at(std::string(bundle::plugins_entry)).data,
             std::string(plugin_note) + "\n" + std::string(plugin_name) + " | " +
                 std::string(plugin_version) + " | " + std::string(plugin_status) + "\n");
  REQUIRE_EQ(entries.at(std::string(bundle::build_entry)).data,
             std::string(feature_name) + ": " + std::string(feature_value) + "\n");
  REQUIRE_TRUE(entries.at(std::string(bundle::session_entry))
                   .data.starts_with(std::string(application_fact) + ": " +
                                     std::string(application_value) + "\n"),
               "the session description starts with the application's facts");
  REQUIRE_EQ(entries.at(std::string(bundle::readme_entry)).data.find("Left out"), std::string::npos,
             "nothing was left out");
  std::filesystem::remove_all(fixture.root);
}

void missing_inputs_are_noted_in_the_readme() {
  auto fixture = full_fixture("diagnostics-empty");
  fixture.options.log_directory = fixture.root / "no-logs";
  fixture.options.crash_directory = fixture.root / "no-reports";
  fixture.options.trace.clear();
  fixture.options.plugins.clear();
  fixture.options.plugin_note.clear();
  const auto entries = read_zip(take(bundle::write(fixture.options)));
  std::vector<std::string> expected{
      std::string(bundle::readme_entry), std::string(bundle::build_entry),
      std::string(bundle::plugins_entry), std::string(bundle::session_entry)};
  std::ranges::sort(expected);
  REQUIRE_EQ(names(entries), expected);
  const auto& readme = entries.at(std::string(bundle::readme_entry)).data;
  for (const std::string_view note :
       {"No log files were found.", "No crash report", "No performance trace: none was requested."})
    REQUIRE_NE(readme.find(note), std::string::npos, std::string(note));
  REQUIRE_EQ(entries.at(std::string(bundle::plugins_entry)).data,
             std::string("No plug-ins found.\n"));
  std::filesystem::remove_all(fixture.root);
}

void trace_is_redacted_across_its_read_boundary() {
  auto fixture = full_fixture("diagnostics-trace-boundary");
  // The second line starts just before the first read ends, so the home path
  // straddles two reads.
  std::string text(bundle::trace_read_bytes - trace_boundary_lead, 'x');
  text += '\n';
  text += std::string(home) + "/straddling\n";
  write_text(fixture.options.trace, text);
  const auto entries = read_zip(take(bundle::write(fixture.options)));
  const auto& trace = entries.at(std::string(bundle::trace_folder) + std::string(trace_name)).data;
  REQUIRE_EQ(trace.size(), text.size() - home.size() + 1, "only the home path shrank to ~");
  REQUIRE_TRUE(trace.ends_with("\n~/straddling\n"));
  std::filesystem::remove_all(fixture.root);
}

void trace_with_an_overlong_line_is_left_out() {
  auto fixture = full_fixture("diagnostics-trace-line");
  write_text(fixture.options.trace, std::string(bundle::max_trace_line_bytes + 1, 'x'));
  const auto entries = read_zip(take(bundle::write(fixture.options)));
  REQUIRE_FALSE(entries.contains(std::string(bundle::trace_folder) + std::string(trace_name)));
  REQUIRE_NE(entries.at(std::string(bundle::readme_entry)).data.find("has a line longer than"),
             std::string::npos);
  std::filesystem::remove_all(fixture.root);
}

void unusable_directory_fails_without_a_file() {
  auto fixture = full_fixture("diagnostics-unusable");
  write_text(fixture.root / "occupied", "a file where the folder should be");
  fixture.options.directory = fixture.root / "occupied" / "diagnostics";
  REQUIRE_FALSE(bundle::write(fixture.options).has_value());
  fixture.options.directory.clear();
  REQUIRE_FALSE(bundle::write(fixture.options).has_value(), "no directory, no bundle");
  std::filesystem::remove_all(fixture.root);
}

void directory_fallback() {
  REQUIRE_EQ(bundle::default_directory("/state", "/home/me"),
             std::filesystem::path("/state/oscillate/diagnostics"));
  REQUIRE_EQ(bundle::default_directory("relative", "/home/me"),
             std::filesystem::path("/home/me/.local/state/oscillate/diagnostics"));
  REQUIRE_TRUE(bundle::default_directory(nullptr, nullptr).empty());
}

} // namespace

int main() {
  try {
    crc_matches_the_check_value_and_continues();
    zip_round_trip();
    zip_refuses_unsafe_names_and_misuse();
    bundle_holds_exactly_the_documented_entries();
    missing_inputs_are_noted_in_the_readme();
    trace_is_redacted_across_its_read_boundary();
    trace_with_an_overlong_line_is_left_out();
    unusable_directory_fails_without_a_file();
    directory_fallback();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
