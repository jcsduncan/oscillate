#include "oscillate/foundation/crash_report.hpp"
#include "oscillate/foundation/log.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace crash = oscillate::foundation::crash_report;
using oscillate::test::take;

// The killed child is sent SIGILL: sanitizers leave it alone by default,
// where they catch SIGSEGV and SIGBUS themselves and exit instead of dying.
constexpr int kill_signal = SIGILL;
// A child that could not arm its handlers exits with this.
constexpr int arm_failed_status = 3;
constexpr std::string_view home = "/home/listener";
constexpr std::string_view application_name = "crash-report-test";
constexpr std::string_view older_log_line = "older session line in /home/listener/Music/song";
constexpr std::string_view newer_log_line = "newer session line";
constexpr std::string_view audio_payload = "RIFF-audio-payload-that-must-stay-local";
constexpr std::string_view fatal_message = "Qt fatal: scene graph lost\nsecond line";
constexpr std::string_view variable_name = "OSCILLATE_LOG";
constexpr std::string_view variable_value = "audio=debug";

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path reports;
  std::filesystem::path logs;
};

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

// A state directory with two log files, and audio next to them and in a
// project folder that a report must never pick up.
Fixture make_fixture(std::string_view name) {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() /
                 ("oscillate-" + std::string(name) + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(fixture.root);
  fixture.reports = fixture.root / "crash-reports";
  fixture.logs = fixture.root / "logs";
  std::filesystem::create_directories(fixture.logs);
  std::filesystem::create_directories(fixture.root / "projects" / "song");
  write_text(fixture.logs / "oscillate.1.log", std::string(older_log_line) + "\n");
  write_text(fixture.logs / "oscillate.log", std::string(newer_log_line) + "\n");
  write_text(fixture.logs / "take.wav", audio_payload);
  write_text(fixture.root / "projects" / "song" / "take.wav", audio_payload);
  return fixture;
}

crash::CollectOptions collect_options(const Fixture& fixture) {
  return {.directory = fixture.reports, .log_directory = fixture.logs, .home = std::string(home)};
}

// Forks a child that arms the handlers, then runs `crash` (or waits to be
// killed when it returns). Returns the child's wait status.
template <typename Crash>
int run_child(const Fixture& fixture, std::optional<int> kill_with, Crash crash) {
  std::array<int, 2> ready{};
  REQUIRE_EQ(::pipe(ready.data()), 0);
  const pid_t child = ::fork();
  REQUIRE_GE(child, 0);
  if (child == 0) {
    ::close(ready[0]);
    const rlimit no_core{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &no_core);
    const std::array facts{crash::Fact{"application", std::string(application_name)}};
    if (!crash::arm(fixture.reports, crash::describe_session(facts)))
      ::_exit(arm_failed_status);
    const char byte = 1;
    if (::write(ready[1], &byte, 1) != 1)
      ::_exit(arm_failed_status);
    crash();
    for (;;)
      ::pause();
  }
  ::close(ready[1]);
  char byte = 0;
  const auto got = ::read(ready[0], &byte, 1);
  ::close(ready[0]);
  if (got == 1 && kill_with)
    REQUIRE_EQ(::kill(child, *kill_with), 0);
  int status = 0;
  REQUIRE_EQ(::waitpid(child, &status, 0), child);
  REQUIRE_EQ(got, 1, "the child armed its handlers");
  return status;
}

void killed_child_leaves_a_local_report() {
  const auto fixture = make_fixture("crash-killed");
  const int status = run_child(fixture, kill_signal, [] {});
  REQUIRE_TRUE(WIFSIGNALED(status), "the handler re-raises, so the child still dies by signal");
  REQUIRE_EQ(WTERMSIG(status), kill_signal);
  REQUIRE_TRUE(std::filesystem::exists(fixture.reports / crash::marker_name));

  const auto report = take(crash::collect(collect_options(fixture)));
  REQUIRE_TRUE(report.has_value(), "the next start finds the marker");
  REQUIRE_EQ(report->parent_path(), fixture.reports);
  REQUIRE_TRUE(report->filename().string().starts_with(crash::report_prefix));
  REQUIRE_FALSE(std::filesystem::exists(fixture.reports / crash::marker_name),
                "a crash is reported once");

  const auto text = read_text(*report);
  REQUIRE_NE(text.find("SIGILL"), std::string::npos);
  REQUIRE_NE(text.find(std::string("application: ") + std::string(application_name)),
             std::string::npos, "the crashed session's description is in the report");
  const auto older = text.find("older session line");
  const auto newer = text.find(newer_log_line);
  REQUIRE_NE(older, std::string::npos);
  REQUIRE_NE(newer, std::string::npos);
  REQUIRE_LT(older, newer, "log files run oldest first");
  REQUIRE_NE(text.find("~/Music/song"), std::string::npos);
  REQUIRE_EQ(text.find(home), std::string::npos, "the home folder is redacted");
  REQUIRE_EQ(text.find(audio_payload), std::string::npos, "no media goes into a report");

  REQUIRE_FALSE(take(crash::collect(collect_options(fixture))).has_value(),
                "a second start has nothing to report");
  std::filesystem::remove_all(fixture.root);
}

void fatal_error_is_reported_with_its_message() {
  const auto fixture = make_fixture("crash-fatal");
  const int status = run_child(fixture, std::nullopt, [] {
    crash::note_fatal_error(fatal_message);
    std::abort();
  });
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);

  const auto report = take(crash::collect(collect_options(fixture)));
  REQUIRE_TRUE(report.has_value());
  const auto text = read_text(*report);
  REQUIRE_NE(text.find("SIGABRT"), std::string::npos);
  REQUIRE_NE(text.find("fatal error: Qt fatal: scene graph lost second line"), std::string::npos,
             "the message is kept on one line");
  std::filesystem::remove_all(fixture.root);
}

void clean_exit_leaves_no_report() {
  const auto fixture = make_fixture("crash-clean");
  const int status = run_child(fixture, std::nullopt, [] { ::_exit(EXIT_SUCCESS); });
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
  REQUIRE_TRUE(std::filesystem::exists(fixture.reports / crash::session_name));
  REQUIRE_FALSE(take(crash::collect(collect_options(fixture))).has_value());
  std::filesystem::remove_all(fixture.root);
}

void session_lists_only_relevant_variables() {
  REQUIRE_EQ(::setenv(std::string(variable_name).c_str(), std::string(variable_value).c_str(), 1),
             0);
  const std::array facts{crash::Fact{"application", std::string(application_name)}};
  const auto session = crash::describe_session(facts);
  REQUIRE_TRUE(session.starts_with("application: crash-report-test\n"));
  REQUIRE_NE(session.find("kernel: "), std::string::npos);
  REQUIRE_NE(session.find("  OSCILLATE_LOG=audio=debug\n"), std::string::npos);
  REQUIRE_EQ(session.find("\n  HOME="), std::string::npos, "unrelated variables stay out");
  REQUIRE_EQ(::unsetenv(std::string(variable_name).c_str()), 0);
}

// The log's rotation limits, which bound a report's log section.
constexpr std::size_t log_files_kept = oscillate::foundation::log::Options::default_max_files;
constexpr std::uint64_t log_bytes_kept =
    oscillate::foundation::log::Options::default_max_file_bytes;
constexpr std::string_view dropped_head = "head-of-an-oversized-log";
constexpr char filler = 'x';

void log_section_is_bounded() {
  const auto fixture = make_fixture("crash-bounded");
  for (std::size_t number = 1; number <= log_files_kept; ++number)
    write_text(fixture.logs / ("oscillate." + std::to_string(number) + ".log"),
               "numbered log " + std::to_string(number) + "\n");
  write_text(fixture.logs / "oscillate.log",
             std::string(dropped_head) + std::string(log_bytes_kept, filler));
  std::filesystem::create_directories(fixture.reports);
  write_text(fixture.reports / crash::marker_name, "signal 4 0\n");

  const auto report = take(crash::collect(collect_options(fixture)));
  REQUIRE_TRUE(report.has_value());
  const auto text = read_text(*report);
  REQUIRE_EQ(text.find("numbered log " + std::to_string(log_files_kept)), std::string::npos,
             "the oldest file beyond the rotation limit is left out");
  REQUIRE_NE(text.find("numbered log " + std::to_string(log_files_kept - 1)), std::string::npos);
  REQUIRE_EQ(text.find(dropped_head), std::string::npos, "only the tail of a large file is kept");
  REQUIRE_NE(text.find(std::string(log_bytes_kept, filler)), std::string::npos);
  std::filesystem::remove_all(fixture.root);
}

void redaction_replaces_whole_home_paths() {
  REQUIRE_EQ(crash::redact("/home/me/a and /home/meow and /home/me", "/home/me/"),
             std::string("~/a and /home/meow and ~"));
  REQUIRE_EQ(crash::redact("/home/me/a", ""), std::string("/home/me/a"), "no home, no change");
  REQUIRE_EQ(crash::redact("/etc", "/"), std::string("/etc"), "a root home is not redacted");
  REQUIRE_EQ(crash::redact("/mnt/backup/home/me/x", "/home/me"),
             std::string("/mnt/backup/home/me/x"), "only a path that starts with home");
}

void directory_fallback() {
  REQUIRE_EQ(crash::default_directory("/state", "/home/me"),
             std::filesystem::path("/state/oscillate/crash-reports"));
  REQUIRE_EQ(crash::default_directory("relative", "/home/me"),
             std::filesystem::path("/home/me/.local/state/oscillate/crash-reports"));
  REQUIRE_TRUE(crash::default_directory(nullptr, nullptr).empty());
}

} // namespace

int main() {
  try {
    killed_child_leaves_a_local_report();
    fatal_error_is_reported_with_its_message();
    clean_exit_leaves_no_report();
    session_lists_only_relevant_variables();
    log_section_is_bounded();
    redaction_replaces_whole_home_paths();
    directory_fallback();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
