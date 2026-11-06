#include "oscillate/persistence/store.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace p = oscillate::processing;
namespace m = oscillate::media;
namespace pr = oscillate::project;
namespace ps = oscillate::persistence;
namespace t = oscillate::timeline;
namespace w = oscillate::warp_dsp;

using oscillate::test::require;
using oscillate::test::take;
template <typename T> T id(std::uint64_t value) { return take(T::from_bits({0, value})); }
template <typename T> T expect(std::optional<T> value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
  return std::move(*value);
}

void u16(std::ofstream& file, std::uint16_t value) {
  for (unsigned shift = 0; shift < 16; shift += 8)
    file.put(static_cast<char>(value >> shift));
}
void u32(std::ofstream& file, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    file.put(static_cast<char>(value >> shift));
}
void wav(const std::filesystem::path& path, std::span<const m::StereoFrame> frames,
         std::uint32_t rate) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto data_bytes = static_cast<std::uint32_t>(frames.size() * 8U);
  out.write("RIFF", 4);
  u32(out, 36 + data_bytes);
  out.write("WAVEfmt ", 8);
  u32(out, 16);
  u16(out, 3);
  u16(out, 2);
  u32(out, rate);
  u32(out, rate * 8);
  u16(out, 8);
  u16(out, 32);
  out.write("data", 4);
  u32(out, data_bytes);
  for (const auto frame : frames) {
    u32(out, std::bit_cast<std::uint32_t>(frame.left));
    u32(out, std::bit_cast<std::uint32_t>(frame.right));
  }
  require(bool(out), "WAV fixture write failed");
}

constexpr double tone_hz = 440.0;

// One second of a 440 Hz tone (right channel at half level, inverted).
std::vector<m::StereoFrame> tone(std::uint32_t rate) {
  std::vector<m::StereoFrame> samples(rate);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double value =
        0.4 * std::sin(2.0 * std::numbers::pi * tone_hz * static_cast<double>(i) / rate);
    samples[i] = {static_cast<float>(value), static_cast<float>(-0.5 * value)};
  }
  return samples;
}

// A saved 48 kHz project holding managed 48 kHz and 44.1 kHz tone sources.
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("oscillate-warp-cache-" + std::to_string(::getpid()));
  std::filesystem::path project_root = root / "project";
  // records[0] is the 48 kHz source, records[1] the 44.1 kHz one.
  std::vector<pr::SourceRecord> records;
  std::unique_ptr<m::CacheStore> cache;

  Fixture() {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto store = take(ps::ProjectStore::open_writer(project_root, true));
    std::vector<ps::SourceReceipt> receipts;
    for (const auto [rate, number] : {std::pair{48'000U, 1U}, std::pair{44'100U, 5U}}) {
      const auto path = root / ("tone-" + std::to_string(rate) + ".wav");
      wav(path, tone(rate), rate);
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      REQUIRE_GE(fd, 0, "fixture source open failed");
      auto receipt = store.stage_wav(id<pr::SourceId>(number), fd);
      (void)::close(fd);
      REQUIRE_OK(receipt, "source staging failed");
      records.push_back(receipt.value().record());
      receipts.push_back(std::move(receipt).value());
    }
    auto saved = store.save(snapshot(), ps::SaveKind::explicit_save, receipts);
    REQUIRE_OK(saved, saved ? "" : "source fixture save failed: " + saved.error().message);
    cache = std::make_unique<m::CacheStore>(
        take(m::CacheStore::adopt_directory_fd(take(store.open_cache_directory()))));
  }
  ~Fixture() { std::filesystem::remove_all(root); }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  // Clip 3 (track 2) stretches the native source to 1.25 s; clip 6 (track 7)
  // stretches the 44.1 kHz source to the same 60,000 project frames.
  [[nodiscard]] pr::Snapshot snapshot(const pr::ClipProcessing& processing = {},
                                      std::uint64_t generation = 1) const {
    pr::Clip first{id<pr::ClipId>(3),
                   records[0].id,
                   {{0}, {48'000}},
                   pr::ElapsedInterval{{0}, {60'000}},
                   generation};
    pr::Clip second{id<pr::ClipId>(6),
                    records[1].id,
                    {{0}, {44'100}},
                    pr::ElapsedInterval{{0}, {60'000}},
                    generation};
    first.processing = processing;
    second.processing = processing;
    return {id<pr::ProjectId>(4),
            1,
            48'000,
            take(t::TempoMap::create({{{0}, 120.0, t::TempoShape::step}}, 1)),
            take(t::MeterMap::create({{{0}, 4, 4}}, 1)),
            {{id<pr::TrackId>(2), {first}}, {id<pr::TrackId>(7), {second}}},
            records};
  }
};

pr::ClipProcessing with_mode(pr::WarpMode mode, double pitch = 0.0) {
  pr::ClipProcessing processing;
  processing.pitch_semitones = pitch;
  processing.warp.mode = mode;
  processing.warp.mode_overridden = true;
  return processing;
}

struct Rendered {
  m::CacheKey key;
  std::vector<float> left;
  std::vector<float> right;
};

Rendered render(Fixture& fixture, const p::WarpRenderRequest& request) {
  const auto key = take(p::render_warp_to_cache(*fixture.cache, fixture.project_root, request));
  REQUIRE_EQ(key, take(p::warp_render_cache_key(request)), "render published under another key");
  const auto reader =
      expect(take(p::WarpRenderCacheReader::open(
                 *fixture.cache, key,
                 {request.plan.warp.output_frames(), request.settings.mode, request.quality})),
             "rendered entry failed validation");
  Rendered out{key, {}, {}};
  std::array<m::StereoFrame, 4'096> block{};
  for (std::uint64_t frame = 0; frame < reader.frame_count();) {
    const auto count = take(reader.read_at(frame, block));
    REQUIRE_GT(count, 0, "seek read stalled");
    for (std::size_t i = 0; i < count; ++i) {
      out.left.push_back(block[i].left);
      out.right.push_back(block[i].right);
    }
    frame += count;
  }
  return out;
}

// Independent pitch measure: positive-going zero crossings with linear
// interpolation, over the middle of the render (away from both ends).
double frequency(const std::vector<float>& signal, std::uint32_t rate) {
  const auto begin = signal.size() / 5;
  const auto end = signal.size() * 4 / 5;
  double first = -1.0;
  double last = -1.0;
  int crossings = 0;
  for (auto i = begin; i + 1 < end; ++i) {
    if (signal[i] <= 0.0F && signal[i + 1] > 0.0F) {
      const double at = static_cast<double>(i) + signal[i] / (signal[i] - signal[i + 1]);
      if (first < 0.0)
        first = at;
      last = at;
      ++crossings;
    }
  }
  REQUIRE_GT(crossings, 10, "too few crossings to measure pitch");
  return (crossings - 1) * static_cast<double>(rate) / (last - first);
}

double cents_between(double measured, double expected) {
  return 1'200.0 * std::log2(measured / expected);
}

// --- Cent rounding -----------------------------------------------------------

// Oracle: on x86-64 a long double has a 64-bit significand, so 100 x a
// double (53 + 7 bits) is exact; roundl() rounds half away from zero.
std::int32_t exact_cents(double semitones) {
  return static_cast<std::int32_t>(std::roundl(static_cast<long double>(semitones) * 100.0L));
}

void cent_rounding_matches_exact_oracle() {
  static_assert(std::numeric_limits<long double>::digits >= 64, "oracle needs extended precision");
  struct Case {
    double semitones;
    std::int32_t cents;
  };
  // Decimal inputs whose stored doubles sit just above or below a half cent,
  // exact ties, extremes and signed zero.
  for (const auto& item : std::array<Case, 14>{{{0.0, 0},
                                                {-0.0, 0},
                                                {0.125, 13},
                                                {-0.125, -13},
                                                {0.005, 1},
                                                {-0.005, -1},
                                                {0.015, 1},
                                                {0.025, 3},
                                                {12.345, 1'235},
                                                {0.29, 29},
                                                {7.004999, 700},
                                                {48.0, 4'800},
                                                {-48.0, -4'800},
                                                {-36.5, -3'650}}}) {
    require(p::clip_transpose_cents(item.semitones) == item.cents &&
                exact_cents(item.semitones) == item.cents,
            "cent rounding differs from the specified table at " + std::to_string(item.semitones));
  }
  // Every thousandth of a semitone and their half-cent neighbours.
  for (int milli = -48'000; milli <= 48'000; ++milli) {
    const double value = milli / 1'000.0;
    for (const double probe : {value, std::nextafter(value, 0.0), std::nextafter(value, 100.0)}) {
      if (std::fabs(probe) <= 48.0)
        REQUIRE_EQ(p::clip_transpose_cents(probe), exact_cents(probe),
                   "cent rounding differs from the exact oracle at " + std::to_string(probe));
    }
  }
}

// --- Applied settings and requests -----------------------------------------

void applied_settings_follow_intent_rules() {
  auto repitch = with_mode(pr::WarpMode::repitch, 7.0);
  REQUIRE_EQ(p::applied_warp_settings(repitch).transpose_cents, 0,
             "Re-Pitch applied an inactive transpose");
  auto complex = with_mode(pr::WarpMode::complex, 7.004);
  REQUIRE_EQ(p::applied_warp_settings(complex).transpose_cents, 700, "Complex transpose rounding");

  auto pro = with_mode(pr::WarpMode::complex_pro, -3.0);
  pro.warp.complex_pro = {60.0, 64.0};
  pro.preserve_formants = true;
  const auto preserved = p::applied_warp_settings(pro);
  REQUIRE_EQ(preserved.formants, 60.0F,
             "Complex Pro did not apply its formant amount and envelope");
  REQUIRE_EQ(preserved.complex_pro_envelope, 64.0F,
             "Complex Pro did not apply its formant amount and envelope");
  REQUIRE_EQ(preserved.transpose_cents, -300,
             "Complex Pro did not apply its formant amount and envelope");
  pro.preserve_formants = false;
  REQUIRE_EQ(p::applied_warp_settings(pro).formants, 0.0F,
             "Complex Pro applied formants without formant intent");
  // Formant intent never reaches another mode.
  complex.preserve_formants = true;
  complex.warp.complex_pro = {90.0, 200.0};
  REQUIRE_EQ(p::applied_warp_settings(complex),
             p::applied_warp_settings(with_mode(pr::WarpMode::complex, 7.004)),
             "inactive Complex Pro parameters changed Complex settings");

  auto beats = with_mode(pr::WarpMode::beats, 2.0);
  beats.warp.beats = {pr::WarpBeatsLoop::back_and_forth, 35.5};
  const auto applied_beats = p::applied_warp_settings(beats);
  REQUIRE_EQ(applied_beats.beats_loop, w::BeatsLoop::back_and_forth,
             "Beats settings were not applied");
  REQUIRE_EQ(applied_beats.beats_envelope, 35.5F, "Beats settings were not applied");
  REQUIRE_EQ(applied_beats.transpose_cents, 200, "Beats settings were not applied");
  auto tones = with_mode(pr::WarpMode::tones);
  tones.warp.tones.grain_ms = 25.0;
  REQUIRE_EQ(p::applied_warp_settings(tones).tones_grain_ms, 25.0F, "Tones grain not applied");
  auto texture = with_mode(pr::WarpMode::texture);
  texture.warp.texture = {250.0, 40.0};
  const auto applied_texture = p::applied_warp_settings(texture);
  REQUIRE_EQ(applied_texture.texture_grain_ms, 250.0F, "Texture settings were not applied");
  REQUIRE_EQ(applied_texture.texture_flux, 40.0F, "Texture settings were not applied");
  REQUIRE_EQ(applied_texture.texture_seed, p::texture_render_seed,
             "Texture settings were not applied");
}

void requests_resolve_from_the_saved_project() {
  Fixture fixture;
  const auto stretched = take(p::warp_render_request(fixture.snapshot(), id<pr::ClipId>(3)));
  REQUIRE_EQ(stretched.plan.warp.output_frames(), 60'000,
             "stretched clip request does not follow the frame plan and defaults");
  REQUIRE_EQ(stretched.project_rate, 48'000,
             "stretched clip request does not follow the frame plan and defaults");
  REQUIRE_EQ(stretched.settings.mode, pr::WarpMode::complex,
             "stretched clip request does not follow the frame plan and defaults");
  REQUIRE_EQ(stretched.quality, w::QualityTier::q0,
             "stretched clip request does not follow the frame plan and defaults");
  REQUIRE_EQ(stretched.settings_generation, 1,
             "stretched clip request does not follow the frame plan and defaults");
  // An unstretched clip with inactive pitch plays without processing.
  auto unprocessed = fixture.snapshot(with_mode(pr::WarpMode::repitch, 5.0));
  std::get<pr::ElapsedInterval>(unprocessed.tracks[0].clips[0].placement).end = {48'000};
  auto refused = p::warp_render_request(unprocessed, id<pr::ClipId>(3));
  REQUIRE_TRUE(!refused, "unprocessed clip produced a render request");
  REQUIRE_EQ(refused.error().code, oscillate::foundation::ErrorCode::unsupported,
             "unprocessed clip produced a render request");
  // The same unstretched clip with active pitch in Complex renders.
  auto pitched = fixture.snapshot(with_mode(pr::WarpMode::complex, 5.0));
  std::get<pr::ElapsedInterval>(pitched.tracks[0].clips[0].placement).end = {48'000};
  REQUIRE_OK(p::warp_render_request(pitched, id<pr::ClipId>(3)), "pitched clip was not rendered");
  // The 44.1 kHz source keeps its rate-converted span: 44,100 source frames
  // are 48,000 project frames, stretched to 60,000.
  const auto other = take(p::warp_render_request(fixture.snapshot(), id<pr::ClipId>(6)));
  REQUIRE_EQ(other.source.sample_rate, 44'100, "different-rate request lost its plan");
  REQUIRE_EQ(other.plan.warp.output_frames(), 60'000, "different-rate request lost its plan");
  auto linked = fixture.snapshot();
  linked.sources[0].external_link = pr::ExternalSourceLink{"/elsewhere/tone.wav"};
  auto external = p::warp_render_request(linked, id<pr::ClipId>(3));
  REQUIRE_TRUE(!external, "externally linked source was accepted for processing");
  REQUIRE_EQ(external.error().code, oscillate::foundation::ErrorCode::unsupported,
             "externally linked source was accepted for processing");
  require(!p::warp_render_request(fixture.snapshot(), id<pr::ClipId>(99)),
          "missing clip produced a request");
}

// --- Cache key ---------------------------------------------------------------

void key_tracks_every_rendering_input() {
  Fixture fixture;
  auto pro = with_mode(pr::WarpMode::complex_pro, 3.0);
  const auto base = take(p::warp_render_request(fixture.snapshot(pro), id<pr::ClipId>(3)));
  const auto key = take(p::warp_render_cache_key(base));
  REQUIRE_EQ(take(p::warp_render_cache_key(base)), key, "key is not deterministic");
  const auto differs = [&](const std::function<void(p::WarpRenderRequest&)>& change,
                           std::string_view what) {
    auto changed = base;
    change(changed);
    REQUIRE_NE(take(p::warp_render_cache_key(changed)), key,
               std::string(what) + " missing from key");
  };
  differs([](auto& r) { r.source.sha256[0] = r.source.sha256[0] == 'a' ? 'b' : 'a'; },
          "source identity");
  differs([](auto& r) { r.project_rate = oscillate::test::sample_rate_96k; }, "project rate");
  differs([](auto& r) { r.clip = id<pr::ClipId>(42); }, "clip ID");
  {
    // A clip moved along the timeline (a new edit count) plays the same render.
    auto moved = base;
    moved.settings_generation = 2;
    REQUIRE_EQ(take(p::warp_render_cache_key(moved)), key, "the edit count re-keyed the render");
  }
  differs([](auto& r) { r.quality = w::QualityTier::q1; }, "quality tier");
  differs([](auto& r) { r.settings.transpose_cents = 301; }, "transpose");
  differs([](auto& r) { r.settings.formants = 99.0F; }, "Complex Pro formants");
  differs([](auto& r) { r.settings.complex_pro_envelope = 100.0F; }, "Complex Pro envelope");
  differs([](auto& r) { r.settings.mode = pr::WarpMode::complex; }, "mode");
  // Canonical map: moving a marker changes the plan's segments.
  const std::array markers{p::WarpAnchor{0, 0}, p::WarpAnchor{24'000, 20'000},
                           p::WarpAnchor{48'000, 60'000}};
  differs(
      [&](auto& r) { r.plan.warp = take(p::PreparedWarpMap::prepare(markers, 48'000, 60'000)); },
      "canonical map");
  const std::array trimmed{p::WarpAnchor{0, 0}, p::WarpAnchor{47'990, 60'000}};
  differs(
      [&](auto& r) {
        r.plan.source_start_frame = 10;
        r.plan.warp = take(p::PreparedWarpMap::prepare(trimmed, 47'990, 60'000));
      },
      "trimmed source start");
  // Another mode's parameters do not affect this mode's key.
  auto unaffected = base;
  unaffected.settings.tones_grain_ms = 12.0F;
  unaffected.settings.texture_flux = 50.0F;
  unaffected.settings.beats_envelope = 3.0F;
  REQUIRE_EQ(take(p::warp_render_cache_key(unaffected)), key,
             "inactive mode parameters changed the key");
  // Each mode's own parameters are keyed.
  const auto mode_key = [&](pr::WarpMode mode,
                            const std::function<void(p::AppliedWarpSettings&)>& change) {
    auto request = base;
    request.settings = {};
    request.settings.mode = mode;
    const auto before = take(p::warp_render_cache_key(request));
    change(request.settings);
    return before != take(p::warp_render_cache_key(request));
  };
  REQUIRE_TRUE(mode_key(pr::WarpMode::beats, [](auto& s) { s.beats_loop = w::BeatsLoop::off; }),
               "a mode parameter is missing from the key");
  REQUIRE_TRUE(mode_key(pr::WarpMode::beats, [](auto& s) { s.beats_envelope = 20.0F; }),
               "a mode parameter is missing from the key");
  REQUIRE_TRUE(mode_key(pr::WarpMode::tones, [](auto& s) { s.tones_grain_ms = 20.0F; }),
               "a mode parameter is missing from the key");
  REQUIRE_TRUE(mode_key(pr::WarpMode::texture, [](auto& s) { s.texture_grain_ms = 30.0F; }),
               "a mode parameter is missing from the key");
  REQUIRE_TRUE(mode_key(pr::WarpMode::texture, [](auto& s) { s.texture_flux = 5.0F; }),
               "a mode parameter is missing from the key");
  REQUIRE_TRUE(mode_key(pr::WarpMode::texture, [](auto& s) { s.texture_seed = 9; }),
               "a mode parameter is missing from the key");
  // Every mode has a distinct engine identity and a current version.
  std::vector<std::string_view> ids;
  for (const auto mode :
       {pr::WarpMode::repitch, pr::WarpMode::beats, pr::WarpMode::tones, pr::WarpMode::texture,
        pr::WarpMode::complex, pr::WarpMode::complex_pro}) {
    ids.push_back(p::warp_algorithm_id(mode));
    REQUIRE_GE(p::warp_algorithm_version(mode), 1, "mode has no algorithm version");
  }
  std::ranges::sort(ids);
  REQUIRE_EQ(std::ranges::adjacent_find(ids), ids.end(), "two modes share an engine identity");
  REQUIRE_EQ(p::warp_algorithm_version(pr::WarpMode::repitch), 2,
             "Re-Pitch did not move past the LDW1 version");
  // Level-neutral formant shaping changed Complex Pro's frames: its cached
  // renders must miss.
  require(p::warp_algorithm_version(pr::WarpMode::complex_pro) == 2 &&
              p::warp_algorithm_version(pr::WarpMode::complex) == 1,
          "Complex Pro renders from before level-neutral formants would still hit");
  auto invalid = base;
  invalid.settings.mode = pr::WarpMode::repitch;
  invalid.settings.transpose_cents = 100;
  require(!p::warp_render_cache_key(invalid), "Re-Pitch with an applied transpose was keyed");
}

// --- Rendering all modes -----------------------------------------------------

void every_mode_renders_with_preserved_duration_and_pitch() {
  Fixture fixture;
  for (const auto mode : {pr::WarpMode::beats, pr::WarpMode::tones, pr::WarpMode::texture,
                          pr::WarpMode::complex, pr::WarpMode::complex_pro}) {
    auto processing = with_mode(mode);
    processing.warp.tones.grain_ms = 30.0;
    processing.warp.texture = {80.0, 10.0};
    const auto request =
        take(p::warp_render_request(fixture.snapshot(processing), id<pr::ClipId>(3)));
    const auto out = render(fixture, request);
    REQUIRE_EQ(out.left.size(), 60'000,
               std::string(p::warp_algorithm_id(mode)) + " changed duration");
    // Stretching to 1.25x keeps the 440 Hz pitch in every pitch-preserving mode.
    const double error = cents_between(frequency(out.left, 48'000), tone_hz);
    REQUIRE_LT(std::fabs(error), 5.0,
               std::string(p::warp_algorithm_id(mode)) +
                   " changed pitch while stretching: " + std::to_string(error) + " cents");
  }
  // Re-Pitch stretches as varispeed: 1.25x longer is 1.25x lower.
  const auto varispeed =
      render(fixture, take(p::warp_render_request(
                          fixture.snapshot(with_mode(pr::WarpMode::repitch)), id<pr::ClipId>(3))));
  REQUIRE_LT(std::fabs(cents_between(frequency(varispeed.left, 48'000), tone_hz / 1.25)), 2.0,
             "Re-Pitch did not follow speed");
}

void transpose_reaches_the_stream_in_cents() {
  Fixture fixture;
  for (const auto mode : {pr::WarpMode::complex, pr::WarpMode::complex_pro, pr::WarpMode::tones}) {
    const auto request =
        take(p::warp_render_request(fixture.snapshot(with_mode(mode, 7.004)), id<pr::ClipId>(3)));
    REQUIRE_EQ(request.settings.transpose_cents, 700, "transpose not rounded to cents");
    const double error =
        cents_between(frequency(render(fixture, request).left, 48'000), tone_hz) - 700.0;
    REQUIRE_LT(std::fabs(error), 5.0,
               std::string(p::warp_algorithm_id(mode)) + " missed +700 cents by " +
                   std::to_string(error));
  }
}

void repitch_ignores_inactive_transpose() {
  Fixture fixture;
  auto plain = with_mode(pr::WarpMode::repitch);
  auto pitched = with_mode(pr::WarpMode::repitch, 9.0);
  const auto first = take(p::warp_render_request(fixture.snapshot(plain), id<pr::ClipId>(3)));
  const auto second = take(p::warp_render_request(fixture.snapshot(pitched), id<pr::ClipId>(3)));
  REQUIRE_EQ(take(p::warp_render_cache_key(first)), take(p::warp_render_cache_key(second)),
             "inactive Re-Pitch transpose changed the key");
  const auto a = render(fixture, first);
  const auto b = render(fixture, second);
  REQUIRE_EQ(a.left, b.left, "inactive Re-Pitch transpose changed audio");
  REQUIRE_EQ(a.right, b.right, "inactive Re-Pitch transpose changed audio");
}

void complex_pro_formant_intent_controls_the_stream() {
  Fixture fixture;
  auto off = with_mode(pr::WarpMode::complex_pro, 5.0);
  off.preserve_formants = false;
  auto zero = with_mode(pr::WarpMode::complex_pro, 5.0);
  zero.warp.complex_pro.formants = 0.0;
  auto full = with_mode(pr::WarpMode::complex_pro, 5.0);
  const auto off_request = take(p::warp_render_request(fixture.snapshot(off), id<pr::ClipId>(3)));
  const auto zero_request = take(p::warp_render_request(fixture.snapshot(zero), id<pr::ClipId>(3)));
  const auto full_request = take(p::warp_render_request(fixture.snapshot(full), id<pr::ClipId>(3)));
  REQUIRE_EQ(take(p::warp_render_cache_key(off_request)),
             take(p::warp_render_cache_key(zero_request)),
             "formant intent off is not the same render as formants 0");
  const auto without = render(fixture, off_request);
  const auto with = render(fixture, full_request);
  REQUIRE_NE(without.left, with.left, "formant preservation did not change Complex Pro output");
  // Pitch is the same either way; only the spectral envelope moves.
  require(std::fabs(cents_between(frequency(with.left, 48'000), tone_hz) - 500.0) < 5.0 &&
              std::fabs(cents_between(frequency(without.left, 48'000), tone_hz) - 500.0) < 5.0,
          "formant setting changed transposed pitch");
}

void different_rate_source_keeps_pitch_and_time() {
  Fixture fixture;
  for (const auto mode : {pr::WarpMode::complex, pr::WarpMode::repitch}) {
    // An unstretched 44.1 kHz clip: 44,100 source frames placed as 48,000.
    auto processing = with_mode(mode);
    processing.reviewed_warp_markers.push_back({{22'050}, t::SamplePosition{24'000}});
    auto snapshot = fixture.snapshot(processing);
    std::get<pr::ElapsedInterval>(snapshot.tracks[1].clips[0].placement).end = {48'000};
    const auto request = take(p::warp_render_request(snapshot, id<pr::ClipId>(6)));
    const auto out = render(fixture, request);
    REQUIRE_EQ(out.left.size(), 48'000, "different-rate clip changed elapsed duration");
    const double error = cents_between(frequency(out.left, 48'000), tone_hz);
    REQUIRE_LT(std::fabs(error), 2.0,
               std::string(p::warp_algorithm_id(mode)) + " shifted a 44.1 kHz source by " +
                   std::to_string(error) + " cents");
    if (mode == pr::WarpMode::repitch) {
      // Unity Re-Pitch through the converter is the analytic tone at 48 kHz.
      double worst = 0.0;
      for (std::size_t i = 2'000; i < 46'000; ++i) {
        const double expected =
            0.4 * std::sin(2.0 * std::numbers::pi * tone_hz * static_cast<double>(i) / 48'000.0);
        worst = std::max(worst, std::fabs(out.left[i] - expected));
      }
      REQUIRE_LT(worst, 1e-3, "converted Re-Pitch differs from the analytic tone");
    }
  }
}

// --- Payload validation, corruption and failures -----------------------------

void store_u32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes[at + i] = static_cast<std::byte>(value >> (8U * i));
}

void corrupt_and_ldw1_payloads_miss() {
  Fixture fixture;
  auto& cache = *fixture.cache;
  const auto request = take(p::warp_render_request(
      fixture.snapshot(with_mode(pr::WarpMode::repitch)), id<pr::ClipId>(3)));
  const p::WarpPayloadShape shape{60'000, pr::WarpMode::repitch, w::QualityTier::q0};
  const auto key = render(fixture, request).key;
  const auto valid = expect(take(cache.get(key)), "rendered entry is missing");
  REQUIRE_EQ(valid.size(), 96 + 60'000 * 8, "LDW2 payload size is not header plus samples");
  REQUIRE_TRUE(!take(p::WarpRenderCacheReader::open(
                   cache, key, {60'000, pr::WarpMode::complex, w::QualityTier::q0})),
               "reader accepted a mismatched mode, tier or frame count");
  REQUIRE_TRUE(!take(p::WarpRenderCacheReader::open(
                   cache, key, {60'000, pr::WarpMode::repitch, w::QualityTier::q1})),
               "reader accepted a mismatched mode, tier or frame count");
  REQUIRE_TRUE(!take(p::WarpRenderCacheReader::open(
                   cache, key, {59'999, pr::WarpMode::repitch, w::QualityTier::q0})),
               "reader accepted a mismatched mode, tier or frame count");
  const auto misses = [&](const std::function<void(std::vector<std::byte>&)>& damage,
                          std::string_view what) {
    auto bytes = valid;
    damage(bytes);
    REQUIRE_OK(cache.put(key, bytes), "corrupt fixture publish failed");
    require(!take(p::WarpRenderCacheReader::open(cache, key, shape)),
            std::string(what) + " was accepted");
  };
  misses([](auto& b) { b[3] = std::byte{'1'}; }, "LDW1 magic");
  misses([](auto& b) { store_u32(b, 4, 1); }, "format version 1");
  misses([](auto& b) { b[16] = std::byte{4}; }, "wrong header mode");
  misses([](auto& b) { b[17] = std::byte{2}; }, "wrong header tier");
  misses([](auto& b) { b[18] = std::byte{1}; }, "mono channel count");
  misses([](auto& b) { b[20] = std::byte{1}; }, "nonzero reserved field");
  misses([](auto& b) { b[24] = b[24] == std::byte{'0'} ? std::byte{'1'} : std::byte{'0'}; },
         "another key's digest");
  misses([](auto& b) { b[90] = std::byte{1}; }, "nonzero trailing reserved bytes");
  misses(
      [](auto& b) {
        store_u32(b, 96 + 8 * 1'000,
                  std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
      },
      "nonfinite sample");
  misses([](auto& b) { b.resize(b.size() - 8); }, "truncated payload");
  // An LDW1-format entry under the key misses, and a render replaces it.
  std::vector<std::byte> ldw1(16 + 60'000 * 8);
  ldw1[0] = std::byte{'L'};
  ldw1[1] = std::byte{'D'};
  ldw1[2] = std::byte{'W'};
  ldw1[3] = std::byte{'1'};
  ldw1[4] = std::byte{1};
  REQUIRE_OK(cache.put(key, ldw1), "LDW1 fixture publish failed");
  require(!take(p::WarpRenderCacheReader::open(cache, key, shape)), "LDW1 entry was accepted");
  REQUIRE_OK(p::render_warp_to_cache(cache, fixture.project_root, request),
             "render did not replace an LDW1 entry with a valid render");
  REQUIRE_OK(take(p::WarpRenderCacheReader::open(cache, key, shape)),
             "render did not replace an LDW1 entry with a valid render");
}

void failures_keep_the_previous_entry() {
  Fixture fixture;
  auto& cache = *fixture.cache;
  const auto request = take(p::warp_render_request(
      fixture.snapshot(with_mode(pr::WarpMode::complex)), id<pr::ClipId>(3)));
  const p::WarpPayloadShape shape{60'000, pr::WarpMode::complex, w::QualityTier::q0};
  const auto key = render(fixture, request).key;
  const auto still_valid = [&](std::string_view what) {
    REQUIRE_OK(take(p::WarpRenderCacheReader::open(cache, key, shape)),
               std::string(what) + " replaced the previous valid entry");
  };
  m::CompletionGate gate;
  auto stale = take(gate.issue(key));
  (void)take(gate.issue(key));
  require(!p::render_warp_to_cache(cache, fixture.project_root, request, {}, &stale),
          "superseded ticket rendered");
  still_valid("superseded ticket");
  std::stop_source stop;
  stop.request_stop();
  require(!p::render_warp_to_cache(cache, fixture.project_root, request, stop.get_token()),
          "cancelled render published");
  still_valid("cancelled render");
  auto damaged = request;
  damaged.source.sha256[0] = damaged.source.sha256[0] == 'a' ? 'b' : 'a';
  require(!p::render_warp_to_cache(cache, fixture.project_root, damaged),
          "damaged source identity rendered");
  still_valid("damaged source");
  auto other_ticket = take(gate.issue(take(p::warp_render_cache_key(damaged))));
  require(!p::render_warp_to_cache(cache, fixture.project_root, request, {}, &other_ticket),
          "ticket for another key rendered");
  // A marker beyond Re-Pitch's 16x speed limit fails with an actionable status.
  auto extreme = with_mode(pr::WarpMode::repitch);
  extreme.reviewed_warp_markers.push_back({{47'000}, t::SamplePosition{100}});
  const auto too_fast = take(p::warp_render_request(fixture.snapshot(extreme), id<pr::ClipId>(3)));
  auto refused = p::render_warp_to_cache(cache, fixture.project_root, too_fast);
  REQUIRE_TRUE(!refused, "over-speed Re-Pitch map was not refused");
  REQUIRE_EQ(refused.error().code, oscillate::foundation::ErrorCode::unsupported,
             "over-speed Re-Pitch map was not refused");
}

void prefetch_fill_uses_processed_output_frames() {
  Fixture fixture;
  const auto request = take(p::warp_render_request(
      fixture.snapshot(with_mode(pr::WarpMode::repitch)), id<pr::ClipId>(3)));
  const auto out = render(fixture, request);
  const auto reader =
      expect(take(p::WarpRenderCacheReader::open(
                 *fixture.cache, out.key, {60'000, pr::WarpMode::repitch, w::QualityTier::q0})),
             "rendered entry failed validation");
  m::PrefetchRing ring(id<pr::ClipId>(3), 128);
  std::array<m::StereoFrame, 64> scratch{};
  REQUIRE_EQ(take(p::fill_from_warp_render_cache(reader, ring, {7, 4096}, scratch)), 64,
             "warp prefetch did not respect ring capacity");
  REQUIRE_EQ(take(p::fill_from_warp_render_cache(reader, ring, {7, 4160}, scratch)), 64,
             "warp prefetch did not respect ring capacity");
  REQUIRE_EQ(take(p::fill_from_warp_render_cache(reader, ring, {7, 4224}, scratch)), 0,
             "warp prefetch did not respect ring capacity");
  REQUIRE_EQ(ring.prime_handoff_window(7, 4096, 128), m::HandoffWindowStatus::ready,
             "warp ring cannot prime the processed frame window");
  std::array<m::StereoFrame, 128> from_ring{};
  const auto consumed = ring.consume(7, 4096, from_ring, true);
  REQUIRE_EQ(consumed.copied, from_ring.size(), "warp prefetch gap");
  REQUIRE_EQ(consumed.silent, 0, "warp prefetch gap");
  for (std::size_t i = 0; i < from_ring.size(); ++i)
    require(from_ring[i].left == out.left[4096 + i] && from_ring[i].right == out.right[4096 + i],
            "ring frames differ from the rendered output frames");
  REQUIRE_TRUE(!p::fill_from_warp_render_cache(reader, ring, {0, 0}, scratch),
               "invalid warp prefetch cursor accepted");
  REQUIRE_TRUE(!p::fill_from_warp_render_cache(
                   reader, ring, {9, std::numeric_limits<std::uint64_t>::max()}, scratch),
               "invalid warp prefetch cursor accepted");
}

} // namespace

int main() {
  try {
    cent_rounding_matches_exact_oracle();
    applied_settings_follow_intent_rules();
    requests_resolve_from_the_saved_project();
    key_tracks_every_rendering_input();
    every_mode_renders_with_preserved_duration_and_pitch();
    transpose_reaches_the_stream_in_cents();
    repitch_ignores_inactive_transpose();
    complex_pro_formant_intent_controls_the_stream();
    different_rate_source_keeps_pitch_and_time();
    corrupt_and_ldw1_payloads_miss();
    failures_keep_the_previous_entry();
    prefetch_fill_uses_processed_output_frames();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
