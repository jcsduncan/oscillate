#include "oscillate/foundation/diagnostic.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/hex.hpp"
#include "oscillate/foundation/id.hpp"
#include "oscillate/foundation/sha256.hpp"
#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/test/check.hpp"

#include <compare>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <span>
#include <unistd.h>

namespace {

using oscillate::foundation::DiagnosticCategory;
using oscillate::foundation::DiagnosticEvent;
using oscillate::foundation::DiagnosticSink;
using oscillate::foundation::Error;
using oscillate::foundation::ErrorCode;
using oscillate::foundation::format_id_bits;
using oscillate::foundation::Id;
using oscillate::foundation::IdBits;
using oscillate::foundation::parse_id_bits;
using oscillate::foundation::Result;

struct TrackTag {};
struct ClipTag {};

static_assert(!std::is_convertible_v<Id<TrackTag>, Id<ClipTag>>);
static_assert(!std::is_default_constructible_v<Id<TrackTag>>);
static_assert(std::is_trivially_copyable_v<DiagnosticEvent>);

using oscillate::test::require;

void id_contract() {
  constexpr std::string_view canonical = "01234567-89ab-cdef-fedc-ba9876543210";
  auto parsed = Id<TrackTag>::parse(canonical);
  REQUIRE_OK(parsed, "canonical ID rejected");
  REQUIRE_EQ(parsed.value().bits(), (IdBits{0x0123456789abcdefULL, 0xfedcba9876543210ULL}),
             "ID words changed during parse");
  REQUIRE_EQ(parsed.value().to_string(), canonical, "ID text changed during round trip");

  auto uppercase = Id<TrackTag>::parse("01234567-89AB-CDEF-FEDC-BA9876543210");
  REQUIRE_OK(uppercase, "uppercase ID did not normalize");
  REQUIRE_EQ(uppercase.value(), parsed.value(), "uppercase ID did not normalize");
  REQUIRE_EQ(uppercase.value().to_string(), canonical, "ID output not canonical lowercase");

  auto low_only = Id<TrackTag>::from_bits({0, 1});
  REQUIRE_OK(low_only, "low-only ID rejected");
  REQUIRE_EQ(low_only.value().to_string(), "00000000-0000-0000-0000-000000000001",
             "low-only ID formatted incorrectly");
  auto high_only = Id<TrackTag>::from_bits({1, 0});
  REQUIRE_OK(high_only, "high-only ID rejected");
  REQUIRE_EQ(high_only.value().to_string(), "00000000-0000-0001-0000-000000000000",
             "high-only ID formatted incorrectly");
  require((high_only.value() <=> low_only.value()) > 0, "ID ordering is not word order");
  REQUIRE_EQ(format_id_bits(parsed.value().bits()), canonical, "bit formatter differs from ID");

  auto zero = Id<TrackTag>::from_bits({});
  REQUIRE_TRUE(!zero, "nil ID was accepted");
  REQUIRE_EQ(zero.error().code, ErrorCode::invalid_argument, "nil ID was accepted");
  auto clip_id = Id<ClipTag>::from_bits(parsed.value().bits());
  REQUIRE_OK(clip_id, "different entity type changed stable ID words");
  REQUIRE_EQ(clip_id.value().bits(), parsed.value().bits(),
             "different entity type changed stable ID words");
  auto zero_text = parse_id_bits("00000000-0000-0000-0000-000000000000");
  REQUIRE_TRUE(!zero_text, "nil text was accepted");
  REQUIRE_EQ(zero_text.error().code, ErrorCode::invalid_argument, "nil text was accepted");
}

void malformed_ids() {
  constexpr std::string_view malformed[] = {
      "01234567-89ab-cdef-fedc-ba987654321",   "01234567-89ab-cdef-fedc-ba98765432100",
      "0123456789ab-cdef-fedc-ba9876543210",   "01234567-89ab-cdef-fEdc-ba987654321g",
      "01234567x89ab-cdef-fedc-ba9876543210",  " 1234567-89ab-cdef-fedc-ba9876543210",
      "01234567-89ab-cdef-fedc-ba9876543210 ",
  };
  for (std::string_view text : malformed) {
    auto result = Id<ClipTag>::parse(text);
    REQUIRE_TRUE(!result, "malformed ID was accepted");
    REQUIRE_EQ(result.error().code, ErrorCode::invalid_argument, "malformed ID was accepted");
  }
}

void result_contract() {
  auto good = Result<std::string>::success("saved");
  REQUIRE_TRUE(good, "successful result lost value");
  REQUIRE_EQ(good.value(), "saved", "successful result lost value");
  auto failed = Result<std::string>::failure({ErrorCode::io_failure, "disk full"});
  REQUIRE_TRUE(!failed, "result lost failure details");
  REQUIRE_EQ(failed.error().code, ErrorCode::io_failure, "result lost failure details");
  REQUIRE_EQ(failed.error().message, "disk full", "result lost failure details");
  auto moved = Result<std::string>::success("move-safe");
  std::string moved_value = std::move(moved).value();
  REQUIRE_EQ(moved_value, "move-safe", "result did not move payload");

  auto void_ok = Result<void>::success();
  auto void_error = Result<void>::failure(Error{ErrorCode::conflict, "stale revision"});
  REQUIRE_TRUE(void_ok, "void result did not distinguish success from failure");
  REQUIRE_TRUE(!void_error, "void result did not distinguish success from failure");
  REQUIRE_EQ(void_error.error().message, "stale revision",
             "void result did not distinguish success from failure");
}

class RejectingSink final : public DiagnosticSink {
public:
  bool try_publish(DiagnosticEvent event) noexcept override {
    last_ = event;
    return false;
  }
  DiagnosticEvent last_{};
};

void diagnostic_contract() {
  RejectingSink sink;
  DiagnosticEvent event{.subject = {0x10, 0x20},
                        .correlation = 77,
                        .sample_position = -128,
                        .detail = 4,
                        .code = ErrorCode::unavailable,
                        .category = DiagnosticCategory::media};
  require(!sink.try_publish(event), "sink rejection was not visible");
  REQUIRE_EQ(sink.last_.subject, event.subject, "fixed diagnostic fields did not survive capture");
  REQUIRE_EQ(sink.last_.correlation, 77, "fixed diagnostic fields did not survive capture");
  REQUIRE_EQ(sink.last_.sample_position, -128, "fixed diagnostic fields did not survive capture");
  REQUIRE_EQ(sink.last_.detail, 4, "fixed diagnostic fields did not survive capture");
  REQUIRE_EQ(sink.last_.code, ErrorCode::unavailable,
             "fixed diagnostic fields did not survive capture");
  REQUIRE_EQ(sink.last_.category, DiagnosticCategory::media,
             "fixed diagnostic fields did not survive capture");
}

std::size_t occurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
    ++count;
  return count;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}

void trace_contract() {
  namespace trace = oscillate::foundation::trace;
  const auto path = std::filesystem::temp_directory_path() /
                    ("oscillate-trace-test-" + std::to_string(::getpid()) + ".json");
  require(!trace::enabled(), "tracing starts off");
  {
    OSCILLATE_TRACE_SPAN("before");
  }
  // The four threads below and this one each take one chunk of the pool.
  constexpr std::size_t pool_chunks = 5;
  constexpr std::size_t overflow_events = 3 * trace::chunk_events;
  REQUIRE_TRUE(trace::start(pool_chunks * trace::chunk_bytes),
               "a second start is refused while recording");
  REQUIRE_TRUE(!trace::start(), "a second start is refused while recording");
  REQUIRE_EQ(trace::capacity_events(), pool_chunks * trace::chunk_events,
             "the pool holds whole chunks of the budget");
  trace::label("node", 7, "Kick \"808\"");
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([t] {
      trace::name_thread(t == 0 ? "audio" : "worker");
      for (int i = 0; i < 100; ++i) {
        OSCILLATE_TRACE_SPAN("node", t == 0 ? 7 : 1);
      }
      trace::instant("over budget", 3);
      trace::counter("load", 0.5);
    });
  }
  for (auto& thread : threads)
    thread.join();
  // One thread overflows the spent pool: its newest chunk of events stays.
  for (std::size_t i = 0; i < overflow_events; ++i) {
    OSCILLATE_TRACE_SPAN("overflow");
  }
  const auto written = trace::stop_and_write(path);
  REQUIRE_TRUE(written, "every event, newest first, was written");
  REQUIRE_EQ(written.value(), 4 * 102 + trace::chunk_events,
             "every event, newest first, was written");
  {
    OSCILLATE_TRACE_SPAN("after");
  }
  require(!trace::enabled(), "tracing stops");
  const auto json = read_file(path);
  REQUIRE_EQ(occurrences(json, "\"name\":\"Kick \\\"808\\\"\""), 100,
             "a label names its spans, escaped");
  REQUIRE_EQ(occurrences(json, "\"kind\":\"node\""), 400, "spans keep their kind");
  REQUIRE_EQ(occurrences(json, "\"ph\":\"i\""), 4, "instants and counters are written");
  REQUIRE_EQ(occurrences(json, "\"ph\":\"C\""), 4, "instants and counters are written");
  REQUIRE_EQ(occurrences(json, "\"thread_name\""), 4, "threads carry their names");
  REQUIRE_EQ(occurrences(json, "\"name\":\"audio\""), 1, "threads carry their names");
  REQUIRE_EQ(json.find("before"), std::string::npos, "nothing is recorded while tracing is off");
  REQUIRE_EQ(json.find("after"), std::string::npos, "nothing is recorded while tracing is off");
  REQUIRE_EQ(occurrences(json, "\"name\":\"overflow\""), trace::chunk_events,
             "a full pool keeps the newest");
  REQUIRE_NE(
      json.find("\"dropped_events\":" + std::to_string(overflow_events - trace::chunk_events)),
      std::string::npos, "dropped events are counted in the trace");
  std::filesystem::remove(path);
}

// Fault-injection writer: short writes, one EINTR, then the scripted failure.
enum class FakeWrite : std::uint8_t { short_then_interrupt, zero, no_space };
FakeWrite fake_mode = FakeWrite::short_then_interrupt;
int fake_calls = 0;
std::string fake_output;
constexpr std::size_t fake_short_write_bytes = 2;
constexpr int fake_interrupt_call = 2;

ssize_t fake_write(int, const void* data, std::size_t size) {
  ++fake_calls;
  if (fake_mode == FakeWrite::zero)
    return 0;
  if (fake_mode == FakeWrite::no_space) {
    errno = ENOSPC;
    return -1;
  }
  if (fake_calls == fake_interrupt_call) {
    errno = EINTR;
    return -1;
  }
  const auto count = std::min(size, fake_short_write_bytes);
  fake_output.append(static_cast<const char*>(data), count);
  return static_cast<ssize_t>(count);
}

std::string read_all(int directory, const std::string& name) {
  oscillate::foundation::UniqueFd file(::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC));
  require(file.valid(), "the published file opens");
  std::string text;
  std::array<char, 64> buffer{};
  for (ssize_t count = 0; (count = ::read(file.get(), buffer.data(), buffer.size())) > 0;)
    text.append(buffer.data(), static_cast<std::size_t>(count));
  return text;
}

void file_io_contract() {
  using oscillate::foundation::Durability;
  using oscillate::foundation::PublishMode;
  using oscillate::foundation::UniqueFd;
  namespace io = oscillate::foundation;
  constexpr std::string_view payload = "hello";
  const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));

  fake_mode = FakeWrite::short_then_interrupt;
  fake_calls = 0;
  fake_output.clear();
  REQUIRE_TRUE(io::write_all(-1, bytes, "write fake", fake_write),
               "short writes and EINTR are retried until every byte is written");
  REQUIRE_EQ(fake_output, payload,
             "short writes and EINTR are retried until every byte is written");
  fake_mode = FakeWrite::zero;
  const auto stalled = io::write_all(-1, bytes, "write fake", fake_write);
  REQUIRE_TRUE(!stalled, "a stalled write fails");
  REQUIRE_EQ(stalled.error().code, ErrorCode::io_failure, "a stalled write fails");
  fake_mode = FakeWrite::no_space;
  const auto full = io::write_all(-1, bytes, "write fake", fake_write);
  REQUIRE_TRUE(!full, "a failed write names its action");
  REQUIRE_EQ(full.error().code, ErrorCode::io_failure, "a failed write names its action");
  REQUIRE_NE(full.error().message.find("write fake"), std::string::npos,
             "a failed write names its action");

  const auto root = std::filesystem::temp_directory_path() /
                    ("oscillate-file-io-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  UniqueFd directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  require(directory.valid(), "the test directory opens");

  const auto replaced =
      io::replace_file(directory.get(), "state.tmp", "state", bytes, "test state");
  REQUIRE_TRUE(replaced, "replace_file publishes the whole file and leaves no temporary");
  REQUIRE_EQ(replaced.value(), Durability::durable,
             "replace_file publishes the whole file and leaves no temporary");
  REQUIRE_EQ(read_all(directory.get(), "state"), payload,
             "replace_file publishes the whole file and leaves no temporary");
  REQUIRE_TRUE(!std::filesystem::exists(root / "state.tmp"),
               "replace_file publishes the whole file and leaves no temporary");
  constexpr std::string_view second = "second";
  REQUIRE_TRUE(io::replace_file(directory.get(), "state.tmp", "state",
                                std::as_bytes(std::span(second.data(), second.size())),
                                "test state"),
               "replace_file replaces an existing target");
  REQUIRE_EQ(read_all(directory.get(), "state"), second,
             "replace_file replaces an existing target");

  std::filesystem::create_directory(root / "blocked");
  const auto blocked = io::replace_file(directory.get(), "blocked.tmp", "blocked", bytes, "block");
  REQUIRE_TRUE(!blocked, "a failed rename removes the temporary and keeps the target");
  REQUIRE_EQ(blocked.error().code, ErrorCode::io_failure,
             "a failed rename removes the temporary and keeps the target");
  REQUIRE_TRUE(!std::filesystem::exists(root / "blocked.tmp"),
               "a failed rename removes the temporary and keeps the target");
  REQUIRE_TRUE(std::filesystem::is_directory(root / "blocked"),
               "a failed rename removes the temporary and keeps the target");

  REQUIRE_OK(io::write_new_synced(directory.get(), "staged", bytes, "staged file"),
             "write_new_synced creates a new file");
  const auto again = io::write_new_synced(directory.get(), "staged", bytes, "staged file");
  REQUIRE_TRUE(!again, "write_new_synced refuses an existing name");
  REQUIRE_NE(again.error().message.find("create staged file"), std::string::npos,
             "write_new_synced refuses an existing name");
  const auto clash =
      io::rename_in(directory.get(), "staged", "state", PublishMode::no_replace, "publish");
  REQUIRE_TRUE(!clash, "no_replace keeps the existing target and reports EEXIST");
  REQUIRE_EQ(errno, EEXIST, "no_replace keeps the existing target and reports EEXIST");
  REQUIRE_EQ(read_all(directory.get(), "state"), second,
             "no_replace keeps the existing target and reports EEXIST");

  UniqueFd streamed(
      ::openat(directory.get(), "stream.tmp", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
  REQUIRE_TRUE(streamed.valid(), "a streamed temporary is written");
  REQUIRE_TRUE(io::write_all(streamed.get(), payload, "write stream"),
               "a streamed temporary is written");
  const auto published = io::publish_synced(std::move(streamed), directory.get(), "stream.tmp",
                                            "stream", PublishMode::no_replace, "stream");
  REQUIRE_TRUE(published, "publish_synced flushes, closes and publishes");
  REQUIRE_EQ(published.value(), Durability::durable,
             "publish_synced flushes, closes and publishes");
  REQUIRE_EQ(read_all(directory.get(), "stream"), payload,
             "publish_synced flushes, closes and publishes");

  UniqueFd moved(::dup(directory.get()));
  UniqueFd target;
  target = std::move(moved);
  REQUIRE_TRUE(target.valid(), "UniqueFd moves and closes");
  REQUIRE_TRUE(!moved.valid(), "UniqueFd moves and closes");
  REQUIRE_OK(target.close("close"), "UniqueFd moves and closes");
  std::filesystem::remove_all(root);
}

void encoding_contract() {
  namespace foundation = oscillate::foundation;
  constexpr std::array<std::uint8_t, 4> bytes{0x00, 0x0f, 0xa5, 0xff};
  REQUIRE_EQ(foundation::to_hex(std::span(bytes)), "000fa5ff",
             "hex is lowercase, high nibble first");
  constexpr std::size_t hex_length = 8;
  REQUIRE_TRUE(foundation::is_lowercase_hex("000fa5ff", hex_length),
               "lowercase hex validation checks digits and length");
  REQUIRE_TRUE(!foundation::is_lowercase_hex("000FA5FF", hex_length),
               "lowercase hex validation checks digits and length");
  REQUIRE_TRUE(!foundation::is_lowercase_hex("000fa5f", hex_length),
               "lowercase hex validation checks digits and length");

  // SHA-256("abc") from FIPS 180-2 appendix B.1.
  const auto abc = foundation::sha256(std::string_view("abc"));
  constexpr std::string_view abc_digest =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  REQUIRE_TRUE(abc, "one-shot SHA-256");
  REQUIRE_EQ(foundation::to_hex(std::span(abc.value())), abc_digest, "one-shot SHA-256");
  auto streamed = foundation::Sha256::create();
  REQUIRE_OK(streamed, "SHA-256 context");
  constexpr std::string_view first = "a";
  constexpr std::string_view rest = "bc";
  REQUIRE_TRUE(streamed.value().update(std::as_bytes(std::span(first.data(), first.size()))),
               "SHA-256 accepts chunks");
  REQUIRE_TRUE(streamed.value().update(std::as_bytes(std::span(rest.data(), rest.size()))),
               "SHA-256 accepts chunks");
  const auto finished = streamed.value().finish();
  REQUIRE_TRUE(finished, "chunked SHA-256 matches one-shot");
  REQUIRE_EQ(finished.value(), abc.value(), "chunked SHA-256 matches one-shot");
  require(!streamed.value().finish(), "a finished hasher refuses more work");

  // Reference outputs of the SplitMix64 generator seeded with zero.
  constexpr std::uint64_t first_output = 0xe220a8397b1dcdafULL;
  constexpr std::uint64_t second_output = 0x6e789e6aa1b965f4ULL;
  foundation::SplitMix64 random(0);
  REQUIRE_EQ(random.next(), first_output, "SplitMix64 matches the reference sequence");
  REQUIRE_EQ(random.next(), second_output, "SplitMix64 matches the reference sequence");
  foundation::SplitMix64 unit_random(0);
  const auto unit = unit_random.unit();
  REQUIRE_GE(unit, 0.0, "unit draws are in [0, 1)");
  REQUIRE_LT(unit, 1.0, "unit draws are in [0, 1)");
}

} // namespace

int main() {
  try {
    id_contract();
    malformed_ids();
    result_contract();
    diagnostic_contract();
    trace_contract();
    file_io_contract();
    encoding_contract();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
