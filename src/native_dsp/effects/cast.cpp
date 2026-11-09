#include "oscillate/native_dsp/effects/cast.hpp"

#include "cast_convolver.hpp"
#include "cast_ir.hpp"
#include "cast_presets.hpp"
#include "cast_table.hpp"
#include "dsp/biquad.hpp"
#include "dsp/crossover.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::native_dsp::effects::cast {
namespace {

using cast_table::index_of;
using foundation::ErrorCode;
using foundation::Result;
namespace bq = native_dsp::dsp;

namespace p {
constexpr std::size_t start = index_of("cast.start");
constexpr std::size_t length = index_of("cast.length");
constexpr std::size_t stretch = index_of("cast.stretch");
constexpr std::size_t decay = index_of("cast.decay");
constexpr std::size_t fade_in = index_of("cast.fade_in");
constexpr std::size_t reverse = index_of("cast.reverse");
constexpr std::size_t normalise = index_of("cast.normalise");
constexpr std::size_t predelay = index_of("cast.predelay");
constexpr std::size_t width = index_of("cast.width");
constexpr std::size_t low_cut = index_of("cast.low_cut");
constexpr std::size_t high_cut = index_of("cast.high_cut");
constexpr std::size_t tilt = index_of("cast.tilt");
constexpr std::size_t duck = index_of("cast.duck");
constexpr std::size_t duck_threshold = index_of("cast.duck_threshold");
constexpr std::size_t duck_release = index_of("cast.duck_release");
constexpr std::size_t duck_key = index_of("cast.duck_key");
constexpr std::size_t gain = index_of("cast.gain");
constexpr std::size_t mix = index_of("cast.mix");
} // namespace p

constexpr double seconds_per_millisecond = 1e-3;
// The tilt pivots on two shelves here, half the tilt each way.
constexpr double tilt_pivot_hz = 1'000.0;
// The duck's detector: a fast attack, then the Release parameter.
constexpr double duck_attack_seconds = 0.002;
// Meters hold their peak and fall by 20 dB in this long.
constexpr double meter_fall_seconds = 1.0;
constexpr double meter_fall_db = 20.0;
constexpr double floor_db = -120.0;
// The tone filters' ring after the IR ends, in the reported tail.
constexpr double tone_ring_seconds = 0.05;
// The kernel keeps every IR it may still read; at most a few are in use
// (each tier's previous and next, the head's and the newest).
constexpr std::size_t held_irs = 16;
constexpr std::size_t retired_irs = 32;

// The bundled set, with each model's length for the tail before the IR is
// built.
struct Bundled {
  BuiltinIr ir;
  double seconds;
};
constexpr std::array<Bundled, 10> bundled{{
    {{"booth", "Vocal Booth", "Rooms"}, 0.4},
    {{"small_room", "Small Room", "Rooms"}, 1.0},
    {{"live_room", "Live Room", "Rooms"}, 1.6},
    {{"chamber", "Echo Chamber", "Chambers"}, 3.0},
    {{"hall", "Concert Hall", "Halls"}, 3.8},
    {{"cathedral", "Cathedral", "Halls"}, 9.5},
    {{"warehouse", "Warehouse", "Halls"}, 6.0},
    {{"plate", "Plate", "Plates"}, 3.5},
    {{"bright_plate", "Bright Plate", "Plates"}, 2.0},
    {{"spring", "Spring", "Springs"}, 3.0},
}};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): built from a constant table.
const std::array<BuiltinIr, bundled.size()> builtin_list = [] {
  std::array<BuiltinIr, bundled.size()> list{};
  for (std::size_t i = 0; i < bundled.size(); ++i) {
    list[i] = bundled[i].ir;
  }
  return list;
}();

[[nodiscard]] const Bundled* find_bundled(std::string_view id) noexcept {
  const auto* found = std::ranges::find(bundled, id, [](const Bundled& b) { return b.ir.id; });
  return found == bundled.end() ? nullptr : found;
}

[[nodiscard]] std::size_t capacity_frames(double sample_rate) noexcept {
  return static_cast<std::size_t>(std::ceil(max_ir_seconds * sample_rate));
}

// Bundled IRs at the session rate, built once per rate (control only).
struct BuiltinCache {
  std::mutex mutex;
  std::map<std::pair<std::string, double>, std::shared_ptr<const cast_ir::Responses>> irs;
};

BuiltinCache& builtin_cache() {
  static BuiltinCache cache;
  return cache;
}

std::shared_ptr<const cast_ir::Responses> builtin_at(std::string_view id, double sample_rate) {
  if (find_bundled(id) == nullptr) {
    return nullptr;
  }
  auto& cache = builtin_cache();
  const std::scoped_lock lock(cache.mutex);
  const auto key = std::pair{std::string(id), sample_rate};
  if (const auto found = cache.irs.find(key); found != cache.irs.end()) {
    return found->second;
  }
  auto designed = cast_ir::synthesise(id);
  cast_ir::Responses resampled;
  for (const auto& response : designed) {
    resampled.push_back(cast_ir::resample(response, cast_ir::design_rate, sample_rate,
                                          capacity_frames(sample_rate)));
  }
  auto shared = std::make_shared<const cast_ir::Responses>(std::move(resampled));
  cache.irs.emplace(key, shared);
  return shared;
}

// IRs the callback no longer reads, handed back to control to free: one
// producer (the kernel), one consumer (the next compile, or destruction).
class Retired {
public:
  // Callback: false when full (the caller keeps it).
  bool push(std::shared_ptr<const cast_dsp::CompiledIr>& ir) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= retired_irs) {
      return false;
    }
    slots_[head % retired_irs] = std::move(ir);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }
  // Control: frees what the callback handed back.
  void drain() noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    auto tail = tail_.load(std::memory_order_relaxed);
    for (; tail != head; ++tail) {
      slots_[tail % retired_irs].reset();
    }
    tail_.store(tail, std::memory_order_release);
  }

private:
  std::array<std::shared_ptr<const cast_dsp::CompiledIr>, retired_irs> slots_{};
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

// What a compile keeps for the next: the IR source and its trimmed and
// stretched form, each with the inputs it was made from.
struct SourceKey {
  std::string identity;
  double sample_rate = 0.0;
  bool operator==(const SourceKey&) const = default;
};
struct TrimKey {
  double start = 0.0;
  double length = 0.0;
  double stretch = 0.0;
  bool operator==(const TrimKey&) const = default;
};

struct CastState final : DocumentState {
  std::shared_ptr<const cast_dsp::CompiledIr> ir;
  std::shared_ptr<Retired> retired;
  SourceKey source_key;
  std::shared_ptr<const cast_ir::Responses> source;
  TrimKey trim_key;
  std::shared_ptr<const cast_ir::Responses> trimmed;
  cast_ir::Overview overview;
  double length_seconds = 0.0;
  double rt60_seconds = 0.0;
  std::size_t responses = 0;
};

[[nodiscard]] const project::NativeMediaSlot*
find_slot(const project::NativeSynthDocument& document, std::string_view id) {
  const auto found = std::ranges::find(document.media, id, &project::NativeMediaSlot::id);
  return found == document.media.end() ? nullptr : &*found;
}

Result<void> validate_document(const project::NativeSynthDocument& document) {
  using R = Result<void>;
  if (!document.routes.empty() || !document.curves.empty() || !document.chains.empty() ||
      !document.sequences.empty() || !document.labels.empty() || !document.regions.empty() ||
      !document.voice_lanes.empty()) {
    return R::failure({ErrorCode::invalid_argument, "Cast's document holds only its IR"});
  }
  for (const auto& slot : document.media) {
    if (slot.id != ir_slot && slot.id != ir_right_slot) {
      return R::failure({ErrorCode::invalid_argument, "Cast plays only its ir and ir.right slots"});
    }
    if (slot.frame_length != 0) {
      return R::failure({ErrorCode::invalid_argument, "Cast's IR is not cut into frames"});
    }
  }
  const auto* left = find_slot(document, ir_slot);
  const auto* right = find_slot(document, ir_right_slot);
  if (left != nullptr) {
    if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&left->media);
        builtin != nullptr && find_bundled(builtin->name) == nullptr) {
      return R::failure({ErrorCode::invalid_argument, "Cast has no bundled IR of that name"});
    }
  }
  if (right != nullptr) {
    const auto* left_file =
        left != nullptr ? std::get_if<project::ManagedAudioReference>(&left->media) : nullptr;
    const auto* right_file = std::get_if<project::ManagedAudioReference>(&right->media);
    if (left_file == nullptr || right_file == nullptr || left_file->channels != 2 ||
        right_file->channels != 2) {
      return R::failure({ErrorCode::invalid_argument,
                         "A true-stereo IR is two stereo files, one for each input"});
    }
  }
  return R::success();
}

// The managed slot's decoded audio as one response per file channel.
[[nodiscard]] std::optional<cast_ir::Responses> file_responses(const project::NativeMediaSlot& slot,
                                                               const engine::SynthMedia* media,
                                                               double sample_rate) {
  const auto* reference = std::get_if<project::ManagedAudioReference>(&slot.media);
  if (reference == nullptr || media == nullptr) {
    return std::nullopt;
  }
  const auto entry = std::ranges::find(*media, slot.id, &engine::SynthMediaEntry::slot);
  if (entry == media->end() || !entry->asset || entry->asset->frames.empty()) {
    return std::nullopt;
  }
  const auto& asset = *entry->asset;
  // Room for the shortest stretch to fill the longest IR.
  const auto cap = static_cast<std::size_t>(
      std::ceil(2.0 * max_ir_seconds * static_cast<double>(asset.source_rate)));
  const std::size_t frames = std::min(asset.frames.size(), cap);
  const std::size_t channels = reference->channels == 1 ? 1 : 2;
  cast_ir::Responses out;
  for (std::size_t c = 0; c < channels; ++c) {
    std::vector<float> channel(frames);
    for (std::size_t i = 0; i < frames; ++i) {
      channel[i] = c == 0 ? asset.frames[i].left : asset.frames[i].right;
    }
    out.push_back(cast_ir::resample(channel, asset.source_rate, sample_rate,
                                    2 * capacity_frames(sample_rate)));
  }
  return out;
}

[[nodiscard]] std::string source_identity(const project::NativeSynthDocument& document) {
  std::string identity;
  for (const auto id : {ir_slot, ir_right_slot}) {
    const auto* slot = find_slot(document, id);
    if (slot == nullptr) {
      continue;
    }
    identity += std::string(id) + '=';
    if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot->media)) {
      identity += "builtin:" + builtin->name;
    } else {
      identity += "file:" + std::get<project::ManagedAudioReference>(slot->media).sha256;
    }
    identity += ';';
  }
  return identity.empty() ? "builtin:" + std::string(default_ir) : identity;
}

Result<std::shared_ptr<const cast_ir::Responses>>
load_source(const project::NativeSynthDocument& document, const DocumentInputs& inputs) {
  using R = Result<std::shared_ptr<const cast_ir::Responses>>;
  const auto* left = find_slot(document, ir_slot);
  const auto* right = find_slot(document, ir_right_slot);
  if (left == nullptr || std::holds_alternative<project::NativeBuiltinMedia>(left->media)) {
    const std::string_view id =
        left == nullptr ? default_ir : std::get<project::NativeBuiltinMedia>(left->media).name;
    auto built = builtin_at(id, inputs.sample_rate);
    if (!built || built->empty()) {
      return R::failure({ErrorCode::invalid_argument, "Cast has no bundled IR of that name"});
    }
    return R::success(std::move(built));
  }
  auto first = file_responses(*left, inputs.media, inputs.sample_rate);
  if (!first) {
    return R::failure({ErrorCode::unavailable, "Cast's IR file is not loaded"});
  }
  cast_ir::Responses responses = std::move(*first);
  if (right != nullptr) {
    auto second = file_responses(*right, inputs.media, inputs.sample_rate);
    if (!second) {
      return R::failure({ErrorCode::unavailable, "Cast's right-input IR file is not loaded"});
    }
    for (auto& response : *second) {
      responses.push_back(std::move(response));
    }
  }
  std::size_t length = 0;
  for (const auto& response : responses) {
    length = std::max(length, response.size());
  }
  for (auto& response : responses) {
    response.resize(length, 0.0F);
  }
  return R::success(std::make_shared<const cast_ir::Responses>(std::move(responses)));
}

Result<std::shared_ptr<const DocumentState>>
prepare_document(const project::NativeSynthDocument& document, const DocumentInputs& inputs) {
  using R = Result<std::shared_ptr<const DocumentState>>;
  if (auto valid = validate_document(document); !valid) {
    return R::failure(valid.error());
  }
  const auto& values = inputs.values;
  if (values.size() != cast_table::parameters.size() || !(inputs.sample_rate > 0.0)) {
    return R::failure({ErrorCode::invalid_argument, "Cast's values do not match its table"});
  }
  const auto* previous = dynamic_cast<const CastState*>(inputs.previous);
  auto state = std::make_shared<CastState>();
  state->retired = previous != nullptr ? previous->retired : std::make_shared<Retired>();
  state->retired->drain();

  state->source_key = {source_identity(document), inputs.sample_rate};
  if (previous != nullptr && previous->source_key == state->source_key) {
    state->source = previous->source;
  } else {
    auto loaded = load_source(document, inputs);
    if (!loaded) {
      return R::failure(loaded.error());
    }
    state->source = std::move(loaded).value();
  }
  state->trim_key = {values[p::start], values[p::length], values[p::stretch]};
  if (previous != nullptr && previous->source == state->source &&
      previous->trim_key == state->trim_key) {
    state->trimmed = previous->trimmed;
  } else {
    state->trimmed = std::make_shared<const cast_ir::Responses>(cast_ir::trim_and_stretch(
        *state->source, inputs.sample_rate,
        {values[p::start] * seconds_per_millisecond, values[p::length], values[p::stretch]},
        capacity_frames(inputs.sample_rate)));
  }
  auto shaped = *state->trimmed;
  cast_ir::shape(shaped, inputs.sample_rate,
                 {values[p::decay], values[p::fade_in] * seconds_per_millisecond,
                  values[p::reverse] >= 1.0, values[p::normalise] >= 1.0});
  state->ir = cast_dsp::CompiledIr::build(shaped, capacity_frames(inputs.sample_rate));
  state->overview = cast_ir::overview(shaped, overview_points);
  state->responses = shaped.size();
  state->length_seconds = static_cast<double>(state->ir->length()) / inputs.sample_rate;
  state->rt60_seconds = cast_ir::rt60(shaped, inputs.sample_rate);
  return R::success(std::shared_ptr<const DocumentState>(std::move(state)));
}

project::NativeSynthDocument preset_document(std::optional<std::size_t> preset) {
  const std::size_t index = preset.value_or(0);
  return builtin_document(index < cast_presets::preset_irs.size() ? cast_presets::preset_irs[index]
                                                                  : default_ir);
}

// The IR's length in seconds for a document, before shaping.
[[nodiscard]] double source_seconds(const project::NativeSynthDocument& document) {
  double seconds = 0.0;
  for (const auto id : {ir_slot, ir_right_slot}) {
    const auto* slot = find_slot(document, id);
    if (slot == nullptr) {
      continue;
    }
    if (const auto* file = std::get_if<project::ManagedAudioReference>(&slot->media)) {
      if (file->sample_rate > 0) {
        seconds = std::max(seconds, static_cast<double>(file->frames) /
                                        static_cast<double>(file->sample_rate));
      }
    } else if (const auto* found =
                   find_bundled(std::get<project::NativeBuiltinMedia>(slot->media).name)) {
      seconds = std::max(seconds, found->seconds);
    }
  }
  if (find_slot(document, ir_slot) == nullptr) {
    seconds = find_bundled(default_ir)->seconds;
  }
  return seconds;
}

std::uint64_t tail_for(double ir_seconds, std::span<const double> values, double sample_rate) {
  const double predelay = values.size() > p::predelay
                              ? values[p::predelay] * seconds_per_millisecond
                              : max_predelay_seconds;
  const double seconds = std::min(ir_seconds, max_ir_seconds) + predelay + tone_ring_seconds;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  return tail_for(max_ir_seconds, values, sample_rate);
}

std::uint64_t document_tail_frames(const project::NativeSynthDocument& document,
                                   std::span<const double> values, double sample_rate) {
  double seconds = source_seconds(document);
  if (values.size() == cast_table::parameters.size()) {
    seconds = std::max(0.0, seconds - values[p::start] * seconds_per_millisecond) *
              values[p::length] * values[p::stretch];
  }
  return tail_for(seconds, values, sample_rate);
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

class CastKernel final : public EffectKernel {
public:
  explicit CastKernel(double sample_rate)
      : rate_(sample_rate), convolver_(capacity_frames(sample_rate)),
        fall_(std::pow(10.0, -meter_fall_db / 20.0 / (meter_fall_seconds * sample_rate))),
        attack_(1.0 - std::exp(-1.0 / (duck_attack_seconds * sample_rate))) {
    for (auto& line : predelay_) {
      line.prepare(static_cast<std::size_t>(std::ceil(max_predelay_seconds * sample_rate)) + 1);
    }
  }

  void adopt_document(const DocumentState& state) noexcept override {
    const auto& cast = static_cast<const CastState&>(state);
    if (!retired_) {
      retired_ = cast.retired;
    }
    state_ = &cast;
    if (hold(cast.ir)) {
      convolver_.set_ir(cast.ir.get());
    }
    release_unused();
  }

  void control(std::span<const double> v, std::uint32_t frames_to_next_tick,
               const Transport&) noexcept override {
    const auto steps = static_cast<double>(frames_to_next_tick);
    const bool jump = !primed_;
    predelay_frames_.to(v[p::predelay] * seconds_per_millisecond * rate_, steps, jump);
    width_.settle(v[p::width], steps, jump);
    wet_gain_.settle(bq::db_to_gain(v[p::gain]), steps, jump);
    const double mix = v[p::mix];
    const double angle = mix * std::numbers::pi / 2.0;
    dry_.settle(mix >= 1.0 ? 0.0 : std::cos(angle), steps, jump);
    wet_.settle(mix <= 0.0 ? 0.0 : std::sin(angle), steps, jump);
    duck_db_ = v[p::duck];
    duck_threshold_db_ = v[p::duck_threshold];
    release_ = 1.0 - std::exp(-1.0 / (v[p::duck_release] * seconds_per_millisecond * rate_));
    sidechain_key_ = v[p::duck_key] >= 1.0;
    set_tone(v[p::low_cut], v[p::high_cut], v[p::tilt]);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    const bool keyed = sidechain_key_ && !sidechain.empty();
    for (std::size_t at = 0; at < audio.size(); at += scratch_frames) {
      const std::size_t count = std::min(scratch_frames, audio.size() - at);
      render(audio.subspan(at, count),
             keyed ? sidechain.subspan(at, count) : std::span<const engine::StereoFrame>{});
    }
    const auto frames = audio.size();
    const double fall = std::pow(fall_, static_cast<double>(frames));
    for (std::size_t c = 0; c < 2; ++c) {
      input_meter_[c] = std::max(input_meter_[c] * fall, input_peak_[c]);
      wet_meter_[c] = std::max(wet_meter_[c] * fall, wet_peak_[c]);
      input_peak_[c] = 0.0;
      wet_peak_[c] = 0.0;
    }
    duck_meter_ = std::max(duck_meter_ * fall, duck_peak_);
    duck_peak_ = 0.0;
    release_unused();
  }

  void render(std::span<engine::StereoFrame> audio,
              std::span<const engine::StereoFrame> key_audio) noexcept {
    const std::size_t frames = audio.size();
    for (std::size_t i = 0; i < frames; ++i) {
      const auto in = audio[i];
      input_peak_[0] = std::max(input_peak_[0], static_cast<double>(std::fabs(in.left)));
      input_peak_[1] = std::max(input_peak_[1], static_cast<double>(std::fabs(in.right)));
      predelay_[0].write(in.left);
      predelay_[1].write(in.right);
      const double delay = predelay_frames_.next();
      // Under a frame of pre-delay the input passes through untouched.
      send_[0][i] = delay < 1.0 ? in.left : static_cast<float>(predelay_[0].read(delay));
      send_[1][i] = delay < 1.0 ? in.right : static_cast<float>(predelay_[1].read(delay));
    }
    convolver_.process(std::span(send_[0]).first(frames), std::span(send_[1]).first(frames),
                       std::span(wet_out_[0]).first(frames), std::span(wet_out_[1]).first(frames));
    for (std::size_t i = 0; i < frames; ++i) {
      double left = wet_out_[0][i];
      double right = wet_out_[1][i];
      tone(left, right);
      const double width = width_.next();
      if (width != 1.0) {
        const double mid = 0.5 * (left + right);
        const double side = 0.5 * (left - right) * width;
        left = mid + side;
        right = mid - side;
      }
      const auto key = key_audio.empty() ? audio[i] : key_audio[i];
      const double gain = wet_gain_.next() * duck_gain(key);
      left *= gain;
      right *= gain;
      wet_peak_[0] = std::max(wet_peak_[0], std::fabs(left));
      wet_peak_[1] = std::max(wet_peak_[1], std::fabs(right));
      const double dry = dry_.next();
      const double wet = wet_.next();
      audio[i] = {static_cast<float>(dry * audio[i].left + wet * left),
                  static_cast<float>(dry * audio[i].right + wet * right)};
    }
  }

  void reset() noexcept override {
    convolver_.reset();
    for (auto& line : predelay_) {
      line.reset();
    }
    for (auto& channel : filters_) {
      for (auto& filter : channel) {
        filter.reset();
      }
    }
    envelope_ = 0.0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto db = [](double gain) { return static_cast<float>(bq::gain_to_db(gain, floor_db)); };
    out[telemetry::input_peak] = db(input_meter_[0]);
    out[telemetry::input_peak + 1] = db(input_meter_[1]);
    out[telemetry::wet_peak] = db(wet_meter_[0]);
    out[telemetry::wet_peak + 1] = db(wet_meter_[1]);
    out[telemetry::duck] = static_cast<float>(duck_meter_);
    const auto fill = [&](std::size_t at, const std::vector<float>& values) {
      for (std::size_t i = 0; i < overview_points; ++i) {
        out[at + i] = i < values.size() ? values[i] : static_cast<float>(floor_db);
      }
    };
    if (state_ != nullptr) {
      out[telemetry::length_seconds] = static_cast<float>(state_->length_seconds);
      out[telemetry::responses] = static_cast<float>(state_->responses);
      out[telemetry::rt60_seconds] = static_cast<float>(state_->rt60_seconds);
      fill(telemetry::envelope, state_->overview.envelope);
      fill(telemetry::decay_curve, state_->overview.decay_curve);
    } else {
      out[telemetry::length_seconds] = 0.0F;
      out[telemetry::responses] = 0.0F;
      out[telemetry::rt60_seconds] = 0.0F;
      fill(telemetry::envelope, {});
      fill(telemetry::decay_curve, {});
    }
    return telemetry::size;
  }

private:
  static constexpr std::size_t scratch_frames = control_interval_frames;
  enum Filter : std::uint8_t { low_cut, high_cut, low_shelf, high_shelf, filter_count };

  // Holds a new IR; false (keeping the current one) when every slot is
  // still read, which the slot count rules out.
  bool hold(const std::shared_ptr<const cast_dsp::CompiledIr>& ir) noexcept {
    for (auto& slot : held_) {
      if (slot == ir) {
        return true;
      }
    }
    for (auto& slot : held_) {
      if (!slot) {
        slot = ir;
        return true;
      }
    }
    return false;
  }

  // Hands back the IRs the convolver no longer reads.
  void release_unused() noexcept {
    if (!retired_) {
      return;
    }
    for (auto& slot : held_) {
      if (slot && !convolver_.references(slot.get())) {
        retired_->push(slot);
      }
    }
  }

  void set_tone(double low_hz, double high_hz, double tilt_db) noexcept {
    const double nyquist = bq::nyquist_guard * rate_;
    use_low_cut_ = low_hz > cast_table::low_cut_off_hz;
    use_high_cut_ = high_hz < cast_table::high_cut_off_hz;
    use_tilt_ = tilt_db != 0.0;
    std::array<bq::BiquadCoefficients, filter_count> designs{
        bq::rbj_highpass({low_hz, rate_}, bq::butterworth_q),
        bq::rbj_lowpass({std::min(high_hz, nyquist), rate_}, bq::butterworth_q),
        bq::rbj_shelf(false, bq::db_to_gain(-tilt_db / 2.0), {tilt_pivot_hz, rate_}),
        bq::rbj_shelf(true, bq::db_to_gain(tilt_db / 2.0), {tilt_pivot_hz, rate_})};
    for (auto& channel : filters_) {
      for (std::size_t f = 0; f < filter_count; ++f) {
        channel[f].set(designs[f]);
      }
    }
  }

  void tone(double& left, double& right) noexcept {
    std::array<double*, 2> io{&left, &right};
    for (std::size_t c = 0; c < 2; ++c) {
      auto& f = filters_[c];
      double x = *io[c];
      // Every section runs, so switching one in starts from its history.
      const double cut_low = f[low_cut].process(x);
      x = use_low_cut_ ? cut_low : x;
      const double cut_high = f[high_cut].process(x);
      x = use_high_cut_ ? cut_high : x;
      const double tilted = f[high_shelf].process(f[low_shelf].process(x));
      *io[c] = use_tilt_ ? tilted : x;
    }
  }

  [[nodiscard]] double duck_gain(engine::StereoFrame key) noexcept {
    const double level = std::max(std::fabs(key.left), std::fabs(key.right));
    envelope_ += (level > envelope_ ? attack_ : release_) * (level - envelope_);
    if (duck_db_ <= 0.0) {
      return 1.0;
    }
    const double over = bq::gain_to_db(envelope_, floor_db) - duck_threshold_db_;
    const double reduction = std::clamp(over, 0.0, duck_db_);
    duck_peak_ = std::max(duck_peak_, reduction);
    return reduction > 0.0 ? bq::db_to_gain(-reduction) : 1.0;
  }

  double rate_;
  cast_dsp::Convolver convolver_;
  std::array<std::shared_ptr<const cast_dsp::CompiledIr>, held_irs> held_{};
  std::shared_ptr<Retired> retired_;
  const CastState* state_ = nullptr;
  std::array<dsp::Delay, 2> predelay_{};
  std::array<std::array<float, scratch_frames>, 2> send_{};
  std::array<std::array<float, scratch_frames>, 2> wet_out_{};
  std::array<std::array<bq::Biquad, filter_count>, 2> filters_{};
  bool use_low_cut_ = false;
  bool use_high_cut_ = false;
  bool use_tilt_ = false;
  dsp::Ramp predelay_frames_;
  dsp::Ramp width_{1.0, 0.0};
  dsp::Ramp wet_gain_{1.0, 0.0};
  dsp::Ramp dry_{1.0, 0.0};
  dsp::Ramp wet_;
  double duck_db_ = 0.0;
  double duck_threshold_db_ = 0.0;
  double release_ = 0.0;
  bool sidechain_key_ = false;
  double envelope_ = 0.0;
  double fall_;
  double attack_;
  bool primed_ = false;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> wet_peak_{};
  std::array<double, 2> input_meter_{};
  std::array<double, 2> wet_meter_{};
  double duck_peak_ = 0.0;
  double duck_meter_ = 0.0;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
    return nullptr;
  }
  return std::make_unique<CastKernel>(sample_rate);
}

const EngineDescriptor descriptor{"cast", "Cast", 1, cast_table::parameters, cast_presets::presets};

EffectEngine make_engine() {
  EffectEngine engine;
  engine.descriptor = &descriptor;
  engine.create_kernel = &create_kernel;
  engine.tail_frames = &tail_frames;
  engine.latency_frames = &latency_frames;
  engine.sidechain = true;
  engine.validate_document = &validate_document;
  engine.prepare_document = &prepare_document;
  engine.preset_document = &preset_document;
  engine.media = true;
  engine.document_parameters = cast_table::document_parameters;
  engine.document_tail_frames = &document_tail_frames;
  return engine;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): built from constant tables.
const EffectEngine cast_engine = make_engine();

} // namespace

std::span<const BuiltinIr> builtin_irs() noexcept { return builtin_list; }

project::NativeSynthDocument builtin_document(std::string_view id) {
  project::NativeSynthDocument document;
  document.media.push_back({std::string(ir_slot), project::NativeBuiltinMedia{std::string(id)}, 0});
  return document;
}

Responses builtin_responses(std::string_view id, double sample_rate) {
  Responses out;
  const auto built = builtin_at(id, sample_rate);
  if (!built) {
    return out;
  }
  for (std::size_t r = 0; r < out.paths.size() && r < built->size(); ++r) {
    out.paths[r] = (*built)[r];
  }
  return out;
}

const EffectEngine& engine() { return cast_engine; }

} // namespace oscillate::native_dsp::effects::cast
