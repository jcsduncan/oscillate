#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace oscillate;

using oscillate::test::require;

using oscillate::test::take;

template <std::size_t width>
void put(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t index = 0; index < width; ++index) {
    bytes[at + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

// Independent FNV-1a fixture helper; mutation tests must pass the checksum so
// they exercise structural validation rather than only the outer digest.
void resign(std::vector<std::byte>& bytes) {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  for (std::size_t index = 0; index + 8 < bytes.size(); ++index) {
    hash ^= std::to_integer<std::uint8_t>(bytes[index]);
    hash *= 1'099'511'628'211ULL;
  }
  put<8>(bytes, bytes.size() - 8, hash);
}

std::vector<std::byte> from_hex(std::string_view hex) {
  auto nibble = [](char digit) -> std::uint8_t {
    if (digit >= '0' && digit <= '9') {
      return static_cast<std::uint8_t>(digit - '0');
    }
    return static_cast<std::uint8_t>(digit - 'a' + 10);
  };
  REQUIRE_EQ(hex.size() % 2, 0, "bad golden hex");
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    bytes.push_back(static_cast<std::byte>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
  }
  return bytes;
}

analysis::WaveformSummary waveform() {
  analysis::WaveformSummary result{.identity = {"w", 7},
                                   .sample_rate = oscillate::test::sample_rate_48k,
                                   .channels = 1,
                                   .frames = 3,
                                   .levels = {}};
  result.levels.push_back(
      {.frames_per_bucket = 2,
       .buckets = {{.begin_frame = 0, .end_frame = 2, .channels = {{{-0.5F, 0.5F, 0.5}, {}}}},
                   {.begin_frame = 2, .end_frame = 3, .channels = {{{0.25F, 0.25F, 0.25}, {}}}}}});
  result.levels.push_back({.frames_per_bucket = 3,
                           .buckets = {{.begin_frame = 0,
                                        .end_frame = 3,
                                        .channels = {{{-0.5F, 0.5F, 0.4330127018922193}, {}}}}}});
  return result;
}

void waveform_roundtrip_and_golden() {
  const auto source = waveform();
  const auto encoded = take(analysis::encode_waveform_cache(source));
  const auto decoded =
      take(analysis::decode_waveform_cache(encoded, source.identity, {48'000, 1, 3}));
  REQUIRE_EQ(decoded.identity.source_key, "w", "waveform identity, partial bucket or values lost");
  REQUIRE_EQ(decoded.identity.generation, 7, "waveform identity, partial bucket or values lost");
  REQUIRE_EQ(decoded.levels.size(), 2, "waveform identity, partial bucket or values lost");
  REQUIRE_EQ(decoded.levels[0].buckets.size(), 2,
             "waveform identity, partial bucket or values lost");
  REQUIRE_EQ(decoded.levels[0].buckets[1].end_frame, 3,
             "waveform identity, partial bucket or values lost");
  REQUIRE_EQ(decoded.levels[1].buckets[0].channels[0].rms, 0.4330127018922193,
             "waveform identity, partial bucket or values lost");

  const analysis::WaveformSummary empty{.identity = {"x", 1},
                                        .sample_rate = oscillate::test::sample_rate_48k,
                                        .channels = 1,
                                        .frames = 0,
                                        .levels = {}};
  const auto empty_bytes = take(analysis::encode_waveform_cache(empty));
  // Fixed v1 fixture produced independently from the documented field layout.
  const auto golden = from_hex("4c44414e4c595331010001003900000001000000000000000000000080bb0000010"
                               "0000000000000000000007800000000c35a936b89e190a4");
  REQUIRE_EQ(empty_bytes, golden, "waveform v1 golden bytes changed");
  require(take(analysis::decode_waveform_cache(golden, {"x", 1}, {48'000, 1, 0})).levels.empty(),
          "golden fixture failed to decode");
}

void waveform_rejection() {
  auto source = waveform();
  const auto encoded = take(analysis::encode_waveform_cache(source));
  auto decode = [&](const std::vector<std::byte>& bytes) {
    return analysis::decode_waveform_cache(bytes, source.identity, {48'000, 1, 3});
  };
  const auto reopened = take(analysis::decode_waveform_cache(encoded, {"w", 8}, {48'000, 1, 3}));
  REQUIRE_EQ(reopened.identity.generation, 8,
             "reopened cache did not adopt current worker generation");
  REQUIRE_EQ(reopened.levels.size(), 2, "reopened cache did not adopt current worker generation");
  REQUIRE_TRUE(!analysis::decode_waveform_cache(encoded, {"other", 7}, {48'000, 1, 3}),
               "stale identity or source metadata accepted");
  REQUIRE_TRUE(!analysis::decode_waveform_cache(encoded, source.identity, {44'100, 1, 3}),
               "stale identity or source metadata accepted");
  REQUIRE_TRUE(!analysis::decode_waveform_cache(encoded, source.identity, {48'000, 2, 3}),
               "stale identity or source metadata accepted");
  REQUIRE_TRUE(!analysis::decode_waveform_cache(encoded, source.identity, {48'000, 1, 4}),
               "stale identity or source metadata accepted");
  auto broken = encoded;
  broken[0] = std::byte{0};
  require(!decode(broken), "magic corruption accepted");
  broken = encoded;
  put<8>(broken, 20, 7); // transient generation field must remain reserved zero
  resign(broken);
  require(!decode(broken), "persistent generation accepted");
  broken = encoded;
  broken[8] = std::byte{2};
  resign(broken);
  require(!decode(broken), "unsupported version accepted");
  broken = encoded;
  broken[10] = std::byte{2};
  resign(broken);
  require(!decode(broken), "wrong kind accepted");
  broken = encoded;
  broken[11] = std::byte{1};
  resign(broken);
  require(!decode(broken), "reserved flags accepted");
  broken = encoded;
  put<4>(broken, 12, 1);
  resign(broken);
  require(!decode(broken), "length mismatch accepted");
  broken = encoded;
  put<2>(broken, 16, 129);
  resign(broken);
  require(!decode(broken), "oversize identity accepted");
  broken = encoded;
  put<8>(broken, 49, 0); // first level width
  resign(broken);
  require(!decode(broken), "zero bucket width accepted");
  broken = encoded;
  put<4>(broken, 57, 1'000'001); // first level count
  resign(broken);
  require(!decode(broken), "forged allocation count accepted");
  broken = encoded;
  put<8>(broken, 61, 1); // first bucket starts at frame 1
  resign(broken);
  require(!decode(broken), "discontinuous source-frame range accepted");
  broken = encoded;
  put<4>(broken, 77, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
  resign(broken);
  require(!decode(broken), "non-finite waveform value accepted");
  broken = encoded;
  broken.pop_back();
  require(!decode(broken), "truncated payload accepted");
  broken = encoded;
  broken.push_back(std::byte{0});
  require(!decode(broken), "trailing payload accepted");
  broken = encoded;
  broken.back() ^= std::byte{1};
  require(!decode(broken), "checksum mismatch accepted");
  source.levels.front().buckets.front().channels[0].minimum = 1.0F;
  require(!analysis::encode_waveform_cache(source), "invalid value order encoded");
  source = waveform();
  source.levels.front().buckets.pop_back();
  require(!analysis::encode_waveform_cache(source), "missing partial bucket encoded");
}

void tempo_roundtrip_and_rejection() {
  const analysis::TempoIdentity identity{"beat", 5};
  analysis::TempoEstimate source{.identity = identity,
                                 .sample_rate = oscillate::test::sample_rate_48k,
                                 .frames = 120'000,
                                 .bpm = 120.0,
                                 .confidence = 0.75,
                                 .suggested_beat_frames = {0, 24'000, 48'000, 72'000},
                                 .suggestions_truncated = true,
                                 .suggested_onset_frames = {12'000, 36'000, 60'000},
                                 .onsets_truncated = true};
  const auto bytes = take(analysis::encode_tempo_cache(source, 2));
  auto decoded = take(analysis::decode_tempo_cache(bytes, source.identity, {48'000, 2, 120'000}));
  REQUIRE_EQ(decoded.bpm, 120.0, "tempo cache values lost");
  REQUIRE_EQ(decoded.confidence, 0.75, "tempo cache values lost");
  REQUIRE_EQ(decoded.suggested_beat_frames, source.suggested_beat_frames,
             "tempo cache values lost");
  REQUIRE_TRUE(decoded.suggestions_truncated, "tempo cache values lost");
  REQUIRE_EQ(decoded.suggested_onset_frames, source.suggested_onset_frames,
             "tempo cache values lost");
  REQUIRE_TRUE(decoded.onsets_truncated, "tempo cache values lost");
  auto old_version = bytes;
  old_version[8] = std::byte{1};
  resign(old_version);
  require(!analysis::decode_tempo_cache(old_version, source.identity, {48'000, 2, 120'000}),
          "old tempo cache must rebuild to acquire onset suggestions");
  REQUIRE_EQ(take(analysis::decode_tempo_cache(bytes, {"beat", 6}, {48'000, 2, 120'000}))
                 .identity.generation,
             6, "tempo cache did not adopt current generation");
  REQUIRE_TRUE(!analysis::decode_tempo_cache(bytes, {"other", 5}, {48'000, 2, 120'000}),
               "stale tempo source identity or channels accepted");
  REQUIRE_TRUE(!analysis::decode_tempo_cache(bytes, source.identity, {48'000, 1, 120'000}),
               "stale tempo source identity or channels accepted");
  auto broken = bytes;
  put<4>(broken, 44 + 4 + 8 + 8 + 1, 50'001); // forged suggestion count
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "forged tempo count accepted");
  broken = bytes;
  put<8>(broken, 44 + 4 + 8 + 8 + 1 + 4 + 8, 0); // second marker duplicates first
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "unordered beat grid accepted");
  broken = bytes;
  constexpr std::size_t onset_count_at = 44 + 4 + 8 + 8 + 1 + 4 + 4 * 8 + 1;
  put<4>(broken, onset_count_at, 50'001);
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "forged onset count accepted");
  broken = bytes;
  put<8>(broken, onset_count_at + 4 + 8, 12'000);
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "unordered onsets accepted");
  broken = bytes;
  put<8>(broken, 44 + 4, std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()));
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "non-finite BPM accepted");
  broken = bytes;
  broken[44 + 4 + 16] = std::byte{2};
  resign(broken);
  require(!analysis::decode_tempo_cache(broken, source.identity, {48'000, 2, 120'000}),
          "noncanonical truncation flag accepted");
  source.suggested_beat_frames = {0, 0};
  require(!analysis::encode_tempo_cache(source, 2), "duplicate beat frames encoded");
  source.suggested_beat_frames.clear();
  source.suggested_onset_frames = {12'000, 12'000};
  require(!analysis::encode_tempo_cache(source, 2), "duplicate onsets encoded");
  source.suggested_onset_frames = {12'000};
  source.onsets_truncated = false;
  source.bpm = 0.0;
  source.confidence = 0.0;
  source.suggestions_truncated = false;
  decoded = take(analysis::decode_tempo_cache(take(analysis::encode_tempo_cache(source, 1)),
                                              source.identity, {48'000, 1, 120'000}));
  REQUIRE_EQ(decoded.bpm, 0.0, "low-confidence transients did not survive cache round trip");
  REQUIRE_TRUE(decoded.suggested_beat_frames.empty(),
               "low-confidence transients did not survive cache round trip");
  REQUIRE_EQ(decoded.suggested_onset_frames, source.suggested_onset_frames,
             "low-confidence transients did not survive cache round trip");
}

analysis::KeyEstimate known_key() {
  return {.identity = {"encoded-source-v1-key-algorithm-v1", 5},
          .sample_rate = oscillate::test::sample_rate_48k,
          .frames = 384'000,
          .key = analysis::MusicalKey{0, analysis::KeyMode::major},
          .confidence = 0.625,
          .compatible_keys = {{{9, analysis::KeyMode::minor}, analysis::KeyRelation::relative},
                              {{7, analysis::KeyMode::major}, analysis::KeyRelation::dominant},
                              {{5, analysis::KeyMode::major}, analysis::KeyRelation::subdominant}}};
}

void key_roundtrip_and_stale_source() {
  const auto source = known_key();
  const auto bytes = take(analysis::encode_key_cache(source, 2));
  const auto decoded = take(
      analysis::decode_key_cache(bytes, {source.identity.source_key, 7}, {48'000, 2, 384'000}));
  REQUIRE_EQ(decoded.identity.generation, 7,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_TRUE(decoded.key, "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.key->tonic, 0,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.key->mode, analysis::KeyMode::major,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.confidence, 0.625,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.compatible_keys.size(), 3,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.compatible_keys[0].key.tonic, 9,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.compatible_keys[0].key.mode, analysis::KeyMode::minor,
             "key identity, source generation, confidence or related keys lost");
  REQUIRE_EQ(decoded.compatible_keys[2].relation, analysis::KeyRelation::subdominant,
             "key identity, source generation, confidence or related keys lost");
  const auto changed_bytes = analysis::decode_key_cache(
      bytes, {"encoded-source-v2-key-algorithm-v1", 8}, {48'000, 2, 384'000});
  REQUIRE_TRUE(!changed_bytes, "changed source-byte identity reused key analysis");
  REQUIRE_EQ(changed_bytes.error().code, foundation::ErrorCode::conflict,
             "changed source-byte identity reused key analysis");
  REQUIRE_TRUE(!analysis::decode_key_cache(bytes, source.identity, {44'100, 2, 384'000}),
               "changed decoded source metadata reused key analysis");
  REQUIRE_TRUE(!analysis::decode_key_cache(bytes, source.identity, {48'000, 1, 384'000}),
               "changed decoded source metadata reused key analysis");
  REQUIRE_TRUE(!analysis::decode_key_cache(bytes, source.identity, {48'000, 2, 384'001}),
               "changed decoded source metadata reused key analysis");

  analysis::KeyEstimate unknown{.identity = {"ambiguous-source", 11},
                                .sample_rate = oscillate::test::sample_rate_48k,
                                .frames = 384'000,
                                .key = std::nullopt,
                                .confidence = 0.0,
                                .compatible_keys = {}};
  const auto unknown_bytes = take(analysis::encode_key_cache(unknown, 1));
  auto reopened = take(
      analysis::decode_key_cache(unknown_bytes, {"ambiguous-source", 12}, {48'000, 1, 384'000}));
  REQUIRE_TRUE(!reopened.key, "explicit unknown key did not survive cache reopen");
  REQUIRE_EQ(reopened.confidence, 0.0, "explicit unknown key did not survive cache reopen");
  REQUIRE_TRUE(reopened.compatible_keys.empty(),
               "explicit unknown key did not survive cache reopen");
  REQUIRE_EQ(reopened.identity.generation, 12, "explicit unknown key did not survive cache reopen");
  unknown.compatible_keys.push_back(source.compatible_keys[0]);
  require(!analysis::encode_key_cache(unknown, 1), "unknown estimate with a related key encoded");
}

void key_corruption_and_semantics() {
  const auto source = known_key();
  const auto bytes = take(analysis::encode_key_cache(source, 2));
  const auto decode = [&](const std::vector<std::byte>& payload) {
    return analysis::decode_key_cache(payload, source.identity, {48'000, 2, 384'000});
  };
  const auto body = std::size_t{44} + source.identity.source_key.size();
  auto broken = bytes;
  broken.back() ^= std::byte{1};
  require(!decode(broken), "mutated key payload checksum accepted");
  broken = bytes;
  broken.pop_back();
  require(!decode(broken), "truncated key payload accepted");
  broken = bytes;
  broken[8] = std::byte{2}; // unsupported key payload version
  resign(broken);
  require(!decode(broken), "unsupported key payload version accepted");
  broken = bytes;
  broken[10] = std::byte{2}; // tempo kind under key decoder
  resign(broken);
  require(!decode(broken), "wrong analysis artifact kind accepted");
  broken = bytes;
  put<8>(broken, 20, 5); // generation must never be persisted
  resign(broken);
  require(!decode(broken), "persistent generation accepted");
  broken = bytes;
  broken[body] = std::byte{2}; // noncanonical key-presence flag
  resign(broken);
  require(!decode(broken), "invalid key-presence flag accepted");
  broken = bytes;
  broken[body + 1] = std::byte{12};
  resign(broken);
  require(!decode(broken), "invalid tonic accepted");
  broken = bytes;
  broken[body + 2] = std::byte{2};
  resign(broken);
  require(!decode(broken), "invalid mode accepted");
  broken = bytes;
  put<8>(broken, body + 3, std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
  resign(broken);
  require(!decode(broken), "non-finite confidence accepted");
  broken = bytes;
  broken[body + 11] = std::byte{200}; // forged related-key count
  resign(broken);
  require(!decode(broken), "forged compatible-key count accepted");
  broken = bytes;
  broken[body + 12] = std::byte{8}; // relative minor must be A, not G sharp
  resign(broken);
  require(!decode(broken), "semantically wrong relative key accepted");
  broken = bytes;
  broken[body + 14] = std::byte{2}; // wrong relation ordering
  resign(broken);
  require(!decode(broken), "semantically wrong relation accepted");
  auto invalid = source;
  invalid.key = analysis::MusicalKey{12, analysis::KeyMode::major};
  require(!analysis::encode_key_cache(invalid, 2), "invalid tonic encoded");
  invalid = source;
  invalid.confidence = 0.0;
  require(!analysis::encode_key_cache(invalid, 2), "known key with zero confidence encoded");
  invalid = source;
  invalid.compatible_keys[1].key.tonic = 8;
  require(!analysis::encode_key_cache(invalid, 2), "incorrect compatible key encoded");
}

} // namespace

int main() {
  try {
    waveform_roundtrip_and_golden();
    waveform_rejection();
    tempo_roundtrip_and_rejection();
    key_roundtrip_and_stale_source();
    key_corruption_and_semantics();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
