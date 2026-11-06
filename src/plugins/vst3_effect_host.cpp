#include "oscillate/foundation/midi.hpp"
#include "vst3_effect_host_internal.hpp"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace oscillate::plugins {
namespace {

constexpr std::size_t max_host_audio_buses = 8;
constexpr Steinberg::int32 max_host_parameter_metadata = 8192;

[[nodiscard]] bool parse_class_id(std::string_view text, Steinberg::TUID& output) {
  if (text.size() != 32)
    return false;
  const auto digit = [](char character) -> int {
    if (character >= '0' && character <= '9')
      return character - '0';
    if (character >= 'a' && character <= 'f')
      return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
      return character - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < 16; ++index) {
    const auto high = digit(text[index * 2]);
    const auto low = digit(text[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    output[index] = static_cast<Steinberg::int8>(high * 16 + low);
  }
  return true;
}

class BoundedStream final : public Steinberg::IBStream {
public:
  explicit BoundedStream(std::size_t limit) : limit_(limit) {}
  BoundedStream(std::span<const std::uint8_t> bytes, std::size_t limit)
      : bytes_(bytes.begin(), bytes.end()), limit_(limit) {}

  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::IBStream_iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0) {
      *object = this;
      addRef();
      return Steinberg::kResultOk;
    }
    *object = nullptr;
    return Steinberg::kNoInterface;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }

  Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 count,
                                     Steinberg::int32* bytes_read) override {
    if (count < 0 || (count > 0 && !buffer))
      return Steinberg::kInvalidArgument;
    const auto available = position_ < bytes_.size() ? bytes_.size() - position_ : 0;
    const auto amount = std::min(static_cast<std::size_t>(count), available);
    if (amount > 0)
      std::memcpy(buffer, bytes_.data() + position_, amount);
    position_ += amount;
    if (bytes_read)
      *bytes_read = static_cast<Steinberg::int32>(amount);
    return Steinberg::kResultOk;
  }

  Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 count,
                                      Steinberg::int32* bytes_written) override {
    if (bytes_written)
      *bytes_written = 0;
    if (count < 0 || (count > 0 && !buffer))
      return Steinberg::kInvalidArgument;
    const auto amount = static_cast<std::size_t>(count);
    if (position_ > limit_ || amount > limit_ - position_) {
      failed_ = true;
      return Steinberg::kResultFalse;
    }
    if (position_ + amount > bytes_.size())
      bytes_.resize(position_ + amount);
    if (amount > 0)
      std::memcpy(bytes_.data() + position_, buffer, amount);
    position_ += amount;
    if (bytes_written)
      *bytes_written = count;
    return Steinberg::kResultOk;
  }

  // SDK IBStream fixes this ABI parameter order and the integral parameter types.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Steinberg::tresult PLUGIN_API seek(Steinberg::int64 distance, Steinberg::int32 mode,
                                     Steinberg::int64* result) override {
    Steinberg::int64 base = 0;
    if (mode == kIBSeekCur)
      base = static_cast<Steinberg::int64>(position_);
    else if (mode == kIBSeekEnd)
      base = static_cast<Steinberg::int64>(bytes_.size());
    else if (mode != kIBSeekSet)
      return Steinberg::kInvalidArgument;
    const auto limit = static_cast<Steinberg::int64>(limit_);
    if (distance < -base || distance > limit - base)
      return Steinberg::kResultFalse;
    position_ = static_cast<std::size_t>(base + distance);
    if (result)
      *result = static_cast<Steinberg::int64>(position_);
    return Steinberg::kResultOk;
  }

  Steinberg::tresult PLUGIN_API tell(Steinberg::int64* position) override {
    if (!position)
      return Steinberg::kInvalidArgument;
    *position = static_cast<Steinberg::int64>(position_);
    return Steinberg::kResultOk;
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
  std::vector<std::uint8_t> bytes_;
  std::size_t position_{0};
  std::size_t limit_;
  Steinberg::uint32 references_{1};
  bool failed_{false};
};

std::optional<std::string> utf8_text(const Steinberg::Vst::TChar (&text)[128], bool required) {
  std::string result;
  result.reserve(128);
  bool terminated = false;
  for (std::size_t index = 0; index < 128; ++index) {
    const auto unit = static_cast<std::uint16_t>(text[index]);
    if (unit == 0) {
      terminated = true;
      break;
    }
    std::uint32_t codepoint = unit;
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++index >= 128)
        return std::nullopt;
      const auto low = static_cast<std::uint16_t>(text[index]);
      if (low < 0xdc00 || low > 0xdfff)
        return std::nullopt;
      codepoint = 0x10000U + ((unit - 0xd800U) << 10U) + (low - 0xdc00U);
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return std::nullopt;
    }
    if (codepoint < 0x20 && codepoint != '\t')
      return std::nullopt;
    if (codepoint < 0x80) {
      result.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
      result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
      result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint < 0x10000) {
      result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
      result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
  }
  if (!terminated || (required && result.empty()))
    return std::nullopt;
  return result;
}

class ParameterQueue final : public Steinberg::Vst::IParamValueQueue {
public:
  void assign(Steinberg::Vst::ParamID id, std::span<const Vst3ParameterEvent> points) noexcept {
    id_ = id;
    points_ = points;
  }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IParamValueQueue_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
  Steinberg::int32 PLUGIN_API getPointCount() override {
    return static_cast<Steinberg::int32>(points_.size());
  }
  // The VST3 interface fixes this parameter order and these convertible types.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& offset,
                                         Steinberg::Vst::ParamValue& value) override {
    if (index < 0 || static_cast<std::size_t>(index) >= points_.size())
      return Steinberg::kInvalidArgument;
    const auto& point = points_[static_cast<std::size_t>(index)];
    offset = static_cast<Steinberg::int32>(point.sample_offset);
    value = point.normalized_value;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32, Steinberg::Vst::ParamValue,
                                         Steinberg::int32&) override {
    return Steinberg::kResultFalse;
  }

private:
  Steinberg::Vst::ParamID id_{0};
  std::span<const Vst3ParameterEvent> points_{};
};

class ParameterChanges final : public Steinberg::Vst::IParameterChanges {
public:
  explicit ParameterChanges(std::size_t capacity) : queues_(capacity) {}
  void clear() noexcept { active_ = 0; }
  void add(Steinberg::Vst::ParamID id, std::span<const Vst3ParameterEvent> points) noexcept {
    queues_[active_++].assign(id, points);
  }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IParameterChanges_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::int32 PLUGIN_API getParameterCount() override {
    return static_cast<Steinberg::int32>(active_);
  }
  Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
    return index >= 0 && static_cast<std::size_t>(index) < active_
               ? &queues_[static_cast<std::size_t>(index)]
               : nullptr;
  }
  Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&,
                                                                Steinberg::int32& index) override {
    index = -1;
    return nullptr;
  }

private:
  std::vector<ParameterQueue> queues_;
  std::size_t active_{0};
};

class NoteEventList final : public Steinberg::Vst::IEventList {
public:
  explicit NoteEventList(std::span<const Vst3NoteEvent> notes) : notes_(notes) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IEventList_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::int32 PLUGIN_API getEventCount() override {
    return static_cast<Steinberg::int32>(notes_.size());
  }
  Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index,
                                         Steinberg::Vst::Event& result) override {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size())
      return Steinberg::kInvalidArgument;
    const auto& note = notes_[static_cast<std::size_t>(index)];
    result = {};
    result.busIndex = 0;
    result.sampleOffset = static_cast<Steinberg::int32>(note.sample_offset);
    if (note.kind == Vst3NoteKind::on) {
      result.type = Steinberg::Vst::Event::kNoteOnEvent;
      result.noteOn = {static_cast<Steinberg::int16>(note.channel),
                       static_cast<Steinberg::int16>(note.pitch),
                       0.0F,
                       note.velocity,
                       0,
                       note.note_id};
    } else {
      result.type = Steinberg::Vst::Event::kNoteOffEvent;
      result.noteOff = {static_cast<Steinberg::int16>(note.channel),
                        static_cast<Steinberg::int16>(note.pitch), note.velocity, note.note_id,
                        0.0F};
    }
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event&) override {
    return Steinberg::kResultFalse;
  }

private:
  std::span<const Vst3NoteEvent> notes_;
};

class MetadataHandler final : public Steinberg::Vst::IComponentHandler {
public:
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
  explicit MetadataHandler(std::size_t capacity) {
    edits_.reserve(capacity);
    previews_.reserve(capacity);
    completed_.reserve(capacity);
    active_.reserve(capacity);
  }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IComponentHandler_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
    if (!capturing_ || overflow_)
      return Steinberg::kResultFalse;
    if (std::ranges::find(active_, id) == active_.end()) {
      if (active_.size() == active_.capacity()) {
        overflow_ = true;
        return Steinberg::kResultFalse;
      }
      active_.push_back(id);
    }
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                            Steinberg::Vst::ParamValue value) override {
    if (!capturing_ || !std::isfinite(value) || value < 0.0 || value > 1.0 || overflow_)
      return Steinberg::kResultFalse;
    if (std::ranges::find(active_, id) != active_.end()) {
      auto preview = std::ranges::find(previews_, id, &Vst3ParameterEvent::id);
      if (preview != previews_.end()) {
        preview->normalized_value = value;
      } else {
        if (previews_.size() == previews_.capacity()) {
          overflow_ = true;
          return Steinberg::kResultFalse;
        }
        previews_.push_back({id, 0, value});
      }
    }
    auto found = std::ranges::find(edits_, id, &Vst3ParameterEvent::id);
    if (found != edits_.end()) {
      found->normalized_value = value;
      return Steinberg::kResultOk;
    }
    if (edits_.size() == edits_.capacity()) {
      overflow_ = true;
      return Steinberg::kResultFalse;
    }
    edits_.push_back({id, 0, value});
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
    if (!capturing_ || overflow_)
      return Steinberg::kResultFalse;
    const auto active = std::ranges::find(active_, id);
    if (active == active_.end())
      return Steinberg::kResultFalse;
    active_.erase(active);
    const auto preview = std::ranges::find(previews_, id, &Vst3ParameterEvent::id);
    if (preview != previews_.end())
      previews_.erase(preview);
    const auto value = std::ranges::find(edits_, id, &Vst3ParameterEvent::id);
    if (value != edits_.end()) {
      if (completed_.size() == completed_.capacity()) {
        overflow_ = true;
        return Steinberg::kResultFalse;
      }
      completed_.push_back(*value);
    }
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
    if ((flags & Steinberg::Vst::kLatencyChanged) != 0)
      latency_restart_requested_.store(true, std::memory_order_release);
    if (flags != 0)
      restart_requests_.fetch_or(static_cast<std::uint32_t>(flags), std::memory_order_relaxed);
    return Steinberg::kResultFalse;
  }
  [[nodiscard]] bool latency_restart_requested() const noexcept {
    return latency_restart_requested_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint32_t take_restart_requests() noexcept {
    return restart_requests_.exchange(0, std::memory_order_acq_rel);
  }
  [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
  [[nodiscard]] const std::vector<Vst3ParameterEvent>& edits() const noexcept { return edits_; }
  [[nodiscard]] std::vector<Vst3ParameterEvent> take_previews() {
    if (previews_.empty())
      return {};
    std::vector<Vst3ParameterEvent> result;
    result.swap(previews_);
    previews_.reserve(edits_.capacity());
    return result;
  }
  [[nodiscard]] std::vector<Vst3ParameterEvent> take_completed() {
    if (completed_.empty())
      return {};
    std::vector<Vst3ParameterEvent> result;
    result.swap(completed_);
    completed_.reserve(edits_.capacity());
    return result;
  }
  void begin_capture() noexcept {
    edits_.clear();
    previews_.clear();
    completed_.clear();
    active_.clear();
    overflow_ = false;
    capturing_ = true;
  }
  void end_capture() noexcept {
    capturing_ = false;
    previews_.clear();
  }

private:
  std::vector<Vst3ParameterEvent> edits_;
  std::vector<Vst3ParameterEvent> previews_;
  std::vector<Vst3ParameterEvent> completed_;
  std::vector<Steinberg::Vst::ParamID> active_;
  bool overflow_{false};
  bool capturing_{false};
  std::atomic<std::uint32_t> restart_requests_{0};
  std::atomic<bool> latency_restart_requested_{false};
};

} // namespace

struct Vst3EffectHost::Impl {
  explicit Impl(Vst3EffectConfig configuration)
      : config(std::move(configuration)), metadata_handler(config.max_parameter_events),
        parameter_changes(config.max_parameter_events) {
    active_notes.reserve(config.max_note_events);
    next_active_notes.reserve(config.max_note_events);
    block_notes.reserve(config.max_note_events);
  }
  ~Impl() {
    const auto safe = [this](auto operation) {
      try {
        operation();
      } catch (...) {
        faulted = true;
      }
    };
    if (processing && processor)
      safe([this] { processor->setProcessing(false); });
    if (active && component)
      safe([this] { component->setActive(false); });
    if (controller_connect_attempted && controller_connection)
      safe([this] { controller_connection->disconnect(component_connection); });
    if (component_connect_attempted && component_connection)
      safe([this] { component_connection->disconnect(controller_connection); });
    if (controller_connection)
      safe([this] { controller_connection->release(); });
    if (component_connection)
      safe([this] { component_connection->release(); });
    if (controller) {
      safe([this] { controller->setComponentHandler(nullptr); });
      if (controller_initialized)
        safe([this] { controller->terminate(); });
      safe([this] { controller->release(); });
    }
    if (initialized && component)
      safe([this] { component->terminate(); });
    if (processor)
      safe([this] { processor->release(); });
    if (component)
      safe([this] { component->release(); });
  }

  Vst3EffectConfig config;
  Steinberg::Vst::IComponent* component{nullptr};
  Steinberg::Vst::IAudioProcessor* processor{nullptr};
  Steinberg::Vst::IEditController* controller{nullptr};
  Steinberg::Vst::IConnectionPoint* component_connection{nullptr};
  Steinberg::Vst::IConnectionPoint* controller_connection{nullptr};
  MetadataHandler metadata_handler;
  std::vector<Vst3ParameterInfo> parameters;
  std::vector<std::uint32_t> midi_controller_ids; // Sorted; see map_midi_controller.
  ParameterChanges parameter_changes;
  std::vector<Vst3ParameterEvent> static_values;
  std::vector<Vst3NoteEvent> active_notes;
  std::vector<Vst3NoteEvent> next_active_notes;
  std::vector<Vst3NoteEvent> block_notes;
  std::uint64_t transport_generation{0};
  bool has_transport_generation{false};
  bool flush_on_resume{false};
  std::uint32_t latency{0};
  std::uint32_t event_channels{0};
  Vst3ControllerStateSync controller_state_sync{Vst3ControllerStateSync::not_attempted};
  Steinberg::int32 audio_input_buses{0};
  Steinberg::int32 audio_output_buses{0};
  // Auxiliary buffers are prepared on the control thread. The public host
  // remains stereo; unconnected auxiliary inputs receive silence and extra
  // outputs are discarded until a routed multi-bus interface exists.
  std::vector<std::array<std::vector<float>, 2>> auxiliary_inputs;
  std::vector<std::array<std::vector<float>, 2>> auxiliary_outputs;
  bool initialized{false};
  bool controller_initialized{false};
  bool component_connect_attempted{false};
  bool controller_connect_attempted{false};
  bool active{false};
  bool processing{false};
  std::atomic<bool> faulted{false};
  bool static_flush_pending{false};
  std::shared_ptr<bool> editor_open{std::make_shared<bool>(false)};
};

namespace {
struct ProcessBusBuffers {
  std::array<Steinberg::Vst::AudioBusBuffers, max_host_audio_buses> inputs{};
  std::array<Steinberg::Vst::AudioBusBuffers, max_host_audio_buses> outputs{};
  std::array<std::array<float*, 2>, max_host_audio_buses> input_channels{};
  std::array<std::array<float*, 2>, max_host_audio_buses> output_channels{};
};

void prepare_process_buses(Vst3EffectHost::Impl& host, ProcessBusBuffers& buses,
                           std::span<float> input_left, std::span<float> input_right,
                           std::span<float> output_left, std::span<float> output_right,
                           std::span<const float> sidechain_left = {},
                           std::span<const float> sidechain_right = {}) noexcept {
  const auto frames = output_left.size();
  for (Steinberg::int32 index = 0; index < host.audio_input_buses; ++index) {
    auto& channels = buses.input_channels[static_cast<std::size_t>(index)];
    if (index == 0 && host.config.kind == Vst3HostKind::effect) {
      channels = {input_left.data(), input_right.data()};
    } else if (index == 1 && host.config.kind == Vst3HostKind::effect &&
               host.config.routed_sidechain) {
      // ProcessData uses mutable pointers even for inputs. Copy into the
      // preallocated host buffer so a plug-in cannot mutate the graph's bus.
      auto& detector = host.auxiliary_inputs[static_cast<std::size_t>(index)];
      std::copy_n(sidechain_left.data(), frames, detector[0].data());
      std::copy_n(sidechain_right.data(), frames, detector[1].data());
      channels = {detector[0].data(), detector[1].data()};
    } else {
      auto& silent = host.auxiliary_inputs[static_cast<std::size_t>(index)];
      std::fill_n(silent[0].data(), frames, 0.0F);
      std::fill_n(silent[1].data(), frames, 0.0F);
      channels = {silent[0].data(), silent[1].data()};
    }
    buses.inputs[static_cast<std::size_t>(index)].numChannels = 2;
    buses.inputs[static_cast<std::size_t>(index)].channelBuffers32 = channels.data();
  }
  for (Steinberg::int32 index = 0; index < host.audio_output_buses; ++index) {
    auto& channels = buses.output_channels[static_cast<std::size_t>(index)];
    if (index == 0) {
      channels = {output_left.data(), output_right.data()};
    } else {
      auto& discarded = host.auxiliary_outputs[static_cast<std::size_t>(index)];
      std::fill_n(discarded[0].data(), frames, 0.0F);
      std::fill_n(discarded[1].data(), frames, 0.0F);
      channels = {discarded[0].data(), discarded[1].data()};
    }
    buses.outputs[static_cast<std::size_t>(index)].numChannels = 2;
    buses.outputs[static_cast<std::size_t>(index)].channelBuffers32 = channels.data();
  }
}
} // namespace

Vst3EffectHost::Vst3EffectHost(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
Vst3EffectHost::~Vst3EffectHost() = default;

bool Vst3EffectHost::activate(std::string& error) {
  auto& host = *implementation_;
  if (host.faulted) {
    error = "VST3 processor faulted";
    return false;
  }
  if (host.processing)
    return true;
  try {
    if (host.component->setActive(true) != Steinberg::kResultOk) {
      error = "VST3 activation failed";
      return false;
    }
    host.active = true;
    if (host.processor->setProcessing(true) != Steinberg::kResultOk) {
      suspend();
      error = "VST3 processing activation failed";
      return false;
    }
    host.processing = true;
    return true;
  } catch (...) {
    suspend();
    host.faulted = true;
    error = "VST3 activation threw an exception";
    return false;
  }
}

void Vst3EffectHost::suspend() noexcept {
  auto& host = *implementation_;
  if (host.config.kind == Vst3HostKind::instrument && !host.active_notes.empty())
    host.flush_on_resume = true;
  if (host.processing) {
    try {
      host.processor->setProcessing(false);
    } catch (...) {
      host.faulted = true;
    }
    host.processing = false;
  }
  if (host.active) {
    try {
      host.component->setActive(false);
    } catch (...) {
      host.faulted = true;
    }
    host.active = false;
  }
}

Vst3ProcessStatus Vst3EffectHost::process(Vst3StereoBlock block,
                                          std::span<const Vst3ParameterEvent> events) noexcept {
  auto& host = *implementation_;
  if (host.config.kind != Vst3HostKind::effect)
    return Vst3ProcessStatus::invalid_block;
  if (!host.processing)
    return Vst3ProcessStatus::inactive;
  if (host.faulted) {
    std::ranges::fill(block.output_left, 0.0F);
    std::ranges::fill(block.output_right, 0.0F);
    return Vst3ProcessStatus::plugin_error;
  }
  if (host.static_flush_pending)
    return Vst3ProcessStatus::inactive;
  const auto frames = block.input_left.size();
  if (frames == 0 || frames > host.config.max_block_frames || block.input_right.size() != frames ||
      block.output_left.size() != frames || block.output_right.size() != frames)
    return Vst3ProcessStatus::invalid_block;
  if (host.config.routed_sidechain
          ? block.sidechain_left.size() != frames || block.sidechain_right.size() != frames
          : !block.sidechain_left.empty() || !block.sidechain_right.empty())
    return Vst3ProcessStatus::invalid_block;
  if (block.project_sample < 0 ||
      block.project_sample >
          std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(frames) ||
      !std::isfinite(block.quarter_position) || block.quarter_position < 0.0 ||
      !std::isfinite(block.tempo_bpm) || block.tempo_bpm <= 0.0)
    return Vst3ProcessStatus::invalid_transport;
  if (events.size() > host.config.max_parameter_events)
    return Vst3ProcessStatus::parameter_capacity_exceeded;
  std::uint32_t previous_id = 0;
  std::uint32_t previous_offset = 0;
  bool first = true;
  for (const auto& event : events) {
    if (!std::isfinite(event.normalized_value) || event.normalized_value < 0.0 ||
        event.normalized_value > 1.0 || event.sample_offset >= frames ||
        (!first && (event.id < previous_id ||
                    (event.id == previous_id && event.sample_offset <= previous_offset))))
      return Vst3ProcessStatus::invalid_parameter_events;
    if (parameter_precision(event.id) != Vst3ParameterPrecision::sample_offsets_forwarded)
      return Vst3ProcessStatus::parameter_unavailable;
    previous_id = event.id;
    previous_offset = event.sample_offset;
    first = false;
  }
  host.parameter_changes.clear();
  for (std::size_t begin = 0; begin < events.size();) {
    std::size_t end = begin + 1;
    while (end < events.size() && events[end].id == events[begin].id)
      ++end;
    host.parameter_changes.add(events[begin].id, events.subspan(begin, end - begin));
    begin = end;
  }

  ProcessBusBuffers buses;
  prepare_process_buses(host, buses, block.input_left, block.input_right, block.output_left,
                        block.output_right, block.sidechain_left, block.sidechain_right);
  Steinberg::Vst::ProcessData data{};
  Steinberg::Vst::ProcessContext context{};
  context.state = Steinberg::Vst::ProcessContext::kTempoValid |
                  Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
  if (block.playing)
    context.state |= Steinberg::Vst::ProcessContext::kPlaying;
  context.sampleRate = host.config.sample_rate;
  context.projectTimeSamples = block.project_sample;
  context.projectTimeMusic = block.quarter_position;
  context.tempo = block.tempo_bpm;
  data.processMode = host.config.processing_mode == Vst3ProcessingMode::offline
                         ? Steinberg::Vst::kOffline
                         : Steinberg::Vst::kRealtime;
  data.symbolicSampleSize = Steinberg::Vst::kSample32;
  data.numSamples = static_cast<Steinberg::int32>(frames);
  data.numInputs = host.audio_input_buses;
  data.numOutputs = host.audio_output_buses;
  data.inputs = buses.inputs.data();
  data.outputs = buses.outputs.data();
  data.inputParameterChanges = events.empty() ? nullptr : &host.parameter_changes;
  data.processContext = &context;
  try {
    if (host.processor->process(data) == Steinberg::kResultOk)
      return Vst3ProcessStatus::ok;
  } catch (...) {
    host.faulted = true;
  }
  host.faulted = true;
  // A plug-in may have written only part of either output before failing.
  // Never expose that partial block to the graph, including on later calls.
  std::ranges::fill(block.output_left, 0.0F);
  std::ranges::fill(block.output_right, 0.0F);
  return Vst3ProcessStatus::plugin_error;
}

Vst3ProcessStatus
Vst3EffectHost::process_instrument(Vst3InstrumentBlock block, std::span<const Vst3NoteEvent> notes,
                                   Vst3InstrumentTransport transport,
                                   std::span<const Vst3ParameterEvent> parameters) noexcept {
  auto& host = *implementation_;
  if (host.config.kind != Vst3HostKind::instrument)
    return Vst3ProcessStatus::invalid_block;
  if (!host.processing || host.static_flush_pending)
    return Vst3ProcessStatus::inactive;
  if (host.faulted) {
    std::ranges::fill(block.output_left, 0.0F);
    std::ranges::fill(block.output_right, 0.0F);
    return Vst3ProcessStatus::plugin_error;
  }
  const auto frames = block.output_left.size();
  if (frames == 0 || frames > host.config.max_block_frames || block.output_right.size() != frames)
    return Vst3ProcessStatus::invalid_block;
  if (transport.project_sample < 0 ||
      transport.project_sample >
          std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(frames) ||
      !std::isfinite(transport.quarter_position) || transport.quarter_position < 0.0 ||
      !std::isfinite(transport.tempo_bpm) || transport.tempo_bpm <= 0.0 ||
      (!transport.playing && !notes.empty()))
    return Vst3ProcessStatus::invalid_transport;
  if (notes.size() > host.config.max_note_events)
    return Vst3ProcessStatus::note_capacity_exceeded;
  if (parameters.size() > host.config.max_parameter_events)
    return Vst3ProcessStatus::parameter_capacity_exceeded;
  std::uint32_t previous_id = 0;
  std::uint32_t previous_parameter_offset = 0;
  bool first_parameter = true;
  for (const auto& event : parameters) {
    if (!std::isfinite(event.normalized_value) || event.normalized_value < 0.0 ||
        event.normalized_value > 1.0 || event.sample_offset >= frames ||
        (!first_parameter &&
         (event.id < previous_id ||
          (event.id == previous_id && event.sample_offset <= previous_parameter_offset))))
      return Vst3ProcessStatus::invalid_parameter_events;
    if (parameter_precision(event.id) != Vst3ParameterPrecision::sample_offsets_forwarded &&
        !std::ranges::binary_search(host.midi_controller_ids, event.id))
      return Vst3ProcessStatus::parameter_unavailable;
    previous_id = event.id;
    previous_parameter_offset = event.sample_offset;
    first_parameter = false;
  }
  std::uint32_t previous_offset = 0;
  for (const auto& note : notes) {
    if ((note.kind != Vst3NoteKind::on && note.kind != Vst3NoteKind::off) ||
        note.sample_offset >= frames || note.sample_offset < previous_offset ||
        note.channel >= host.event_channels || note.pitch > foundation::max_midi_key ||
        !std::isfinite(note.velocity) || note.velocity < 0.0F || note.velocity > 1.0F)
      return Vst3ProcessStatus::invalid_note_events;
    previous_offset = note.sample_offset;
  }
  const bool flush =
      !transport.playing || host.flush_on_resume ||
      (host.has_transport_generation && transport.generation != host.transport_generation);
  if (host.active_notes.size() * static_cast<std::size_t>(flush) + notes.size() >
      host.config.max_note_events)
    return Vst3ProcessStatus::note_capacity_exceeded;
  host.block_notes.clear();
  host.next_active_notes = host.active_notes;
  if (flush) {
    for (const auto& active : host.active_notes) {
      host.block_notes.push_back({.kind = Vst3NoteKind::off,
                                  .sample_offset = 0,
                                  .channel = active.channel,
                                  .pitch = active.pitch,
                                  .velocity = 0.0F,
                                  .note_id = active.note_id});
    }
    host.next_active_notes.clear();
  }
  for (const auto& note : notes) {
    host.block_notes.push_back(note);
    const auto matches = [&note](const Vst3NoteEvent& active) {
      return active.channel == note.channel && active.pitch == note.pitch &&
             active.note_id == note.note_id;
    };
    const auto found = std::ranges::find_if(host.next_active_notes, matches);
    if (note.kind == Vst3NoteKind::off) {
      if (found != host.next_active_notes.end())
        host.next_active_notes.erase(found);
    } else {
      if (note.note_id >= 0 &&
          std::ranges::any_of(host.next_active_notes, [&note](const Vst3NoteEvent& active) {
            return active.note_id == note.note_id;
          }))
        return Vst3ProcessStatus::invalid_note_events;
      if (host.next_active_notes.size() == host.config.max_note_events)
        return Vst3ProcessStatus::note_capacity_exceeded;
      host.next_active_notes.push_back(note);
    }
  }
  host.parameter_changes.clear();
  for (std::size_t begin = 0; begin < parameters.size();) {
    std::size_t end = begin + 1;
    while (end < parameters.size() && parameters[end].id == parameters[begin].id)
      ++end;
    host.parameter_changes.add(parameters[begin].id, parameters.subspan(begin, end - begin));
    begin = end;
  }
  ProcessBusBuffers buses;
  prepare_process_buses(host, buses, {}, {}, block.output_left, block.output_right);
  NoteEventList event_list(host.block_notes);
  Steinberg::Vst::ProcessContext context{};
  context.state = Steinberg::Vst::ProcessContext::kTempoValid |
                  Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
  if (transport.playing)
    context.state |= Steinberg::Vst::ProcessContext::kPlaying;
  context.sampleRate = host.config.sample_rate;
  context.projectTimeSamples = transport.project_sample;
  context.projectTimeMusic = transport.quarter_position;
  context.tempo = transport.tempo_bpm;
  Steinberg::Vst::ProcessData data{};
  data.processMode = host.config.processing_mode == Vst3ProcessingMode::offline
                         ? Steinberg::Vst::kOffline
                         : Steinberg::Vst::kRealtime;
  data.symbolicSampleSize = Steinberg::Vst::kSample32;
  data.numSamples = static_cast<Steinberg::int32>(frames);
  data.numInputs = host.audio_input_buses;
  data.numOutputs = host.audio_output_buses;
  data.inputs = host.audio_input_buses == 0 ? nullptr : buses.inputs.data();
  data.outputs = buses.outputs.data();
  data.inputEvents = &event_list;
  data.inputParameterChanges = parameters.empty() ? nullptr : &host.parameter_changes;
  data.processContext = &context;
  try {
    if (host.processor->process(data) == Steinberg::kResultOk) {
      host.active_notes.swap(host.next_active_notes);
      host.transport_generation = transport.generation;
      host.has_transport_generation = true;
      host.flush_on_resume = false;
      return Vst3ProcessStatus::ok;
    }
  } catch (...) {
    // A plug-in exception must not escape through the audio callback.
    host.faulted = true;
  }
  host.faulted = true;
  std::ranges::fill(block.output_left, 0.0F);
  std::ranges::fill(block.output_right, 0.0F);
  return Vst3ProcessStatus::plugin_error;
}

std::uint32_t Vst3EffectHost::latency_samples() const noexcept { return implementation_->latency; }
Vst3LatencyCheck Vst3EffectHost::check_prepared_latency() const noexcept {
  const auto& host = *implementation_;
  Vst3LatencyCheck check{.status = Vst3LatencyCheckStatus::unavailable,
                         .prepared_samples = host.latency,
                         .observed_samples = std::nullopt};
  if (host.faulted || !host.processor)
    return check;
  try {
    check.observed_samples = host.processor->getLatencySamples();
  } catch (...) {
    return check;
  }
  if (host.metadata_handler.latency_restart_requested())
    check.status = Vst3LatencyCheckStatus::restart_requested;
  else if (*check.observed_samples != host.latency)
    check.status = Vst3LatencyCheckStatus::changed;
  else
    check.status = Vst3LatencyCheckStatus::stable;
  return check;
}
std::uint32_t Vst3EffectHost::take_restart_requests() noexcept {
  return implementation_->metadata_handler.take_restart_requests();
}
std::optional<std::uint32_t> Vst3EffectHost::finite_tail_samples() const noexcept {
  if (!implementation_->processor)
    return std::nullopt;
  try {
    const auto tail = implementation_->processor->getTailSamples();
    if (tail != std::numeric_limits<Steinberg::uint32>::max())
      return tail;
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}
const Vst3Class& Vst3EffectHost::identity() const noexcept {
  return implementation_->config.identity;
}
const std::vector<Vst3ParameterInfo>& Vst3EffectHost::parameters() const noexcept {
  return implementation_->parameters;
}
Vst3ParameterPrecision Vst3EffectHost::parameter_precision(std::uint32_t id) const noexcept {
  const auto& params = implementation_->parameters;
  const auto found = std::ranges::lower_bound(params, id, {}, &Vst3ParameterInfo::id);
  if (found == params.end() || found->id != id)
    return Vst3ParameterPrecision::unavailable;
  return found->automatable && !found->read_only ? Vst3ParameterPrecision::sample_offsets_forwarded
                                                 : Vst3ParameterPrecision::control_only;
}

std::optional<std::uint32_t> Vst3EffectHost::map_midi_controller(std::int16_t controller) {
  auto& host = *implementation_;
  if (!host.controller || host.faulted)
    return std::nullopt;
  void* object = nullptr;
  Steinberg::Vst::IMidiMapping* mapping = nullptr;
  try {
    if (host.controller->queryInterface(Steinberg::Vst::IMidiMapping_iid, &object) !=
            Steinberg::kResultOk ||
        object == nullptr)
      return std::nullopt;
    mapping = static_cast<Steinberg::Vst::IMidiMapping*>(object);
    Steinberg::Vst::ParamID id = 0;
    const bool assigned =
        mapping->getMidiControllerAssignment(0, 0, controller, id) == Steinberg::kResultTrue;
    auto* released = mapping;
    mapping = nullptr;
    released->release();
    if (!assigned)
      return std::nullopt;
    const auto at = std::ranges::lower_bound(host.midi_controller_ids, id);
    if (at == host.midi_controller_ids.end() || *at != id)
      host.midi_controller_ids.insert(at, id);
    return id;
  } catch (...) {
    if (mapping) {
      try {
        mapping->release();
      } catch (...) {
      }
    }
    return std::nullopt;
  }
}

Vst3EditorProbe Vst3EffectHost::probe_editor(Vst3EditorPlatform platform) noexcept {
  // A native Wayland claim would also require IWaylandHost and an owned
  // wl_surface lifecycle. The current host has neither.
  if (platform == Vst3EditorPlatform::native_wayland)
    return Vst3EditorProbe::wayland_bridge_unavailable;
  if (platform != Vst3EditorPlatform::x11_xembed || implementation_->faulted)
    return Vst3EditorProbe::host_unavailable;
  if (!implementation_->controller)
    return Vst3EditorProbe::no_controller;
  Steinberg::IPlugView* view = nullptr;
  try {
    view = implementation_->controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view)
      return Vst3EditorProbe::no_view;
    const auto supported = view->isPlatformTypeSupported(
                               Steinberg::kPlatformTypeX11EmbedWindowID) == Steinberg::kResultTrue;
    auto* released_view = view;
    view = nullptr;
    released_view->release();
    return supported ? Vst3EditorProbe::x11_candidate : Vst3EditorProbe::unsupported_platform;
  } catch (...) {
    if (view) {
      try {
        auto* released_view = view;
        view = nullptr;
        released_view->release();
      } catch (...) {
        return Vst3EditorProbe::plugin_exception;
      }
    }
    return Vst3EditorProbe::plugin_exception;
  }
}

std::unique_ptr<Vst3EditorView>
Vst3EffectHost::create_editor_view(Vst3EditorProbe* status) noexcept {
  const auto report = [status](Vst3EditorProbe value) {
    if (status)
      *status = value;
  };
  auto& host = *implementation_;
  if (host.faulted || *host.editor_open) {
    report(Vst3EditorProbe::host_unavailable);
    return nullptr;
  }
  if (!host.controller) {
    report(Vst3EditorProbe::no_controller);
    return nullptr;
  }
  Steinberg::IPlugView* view = nullptr;
  try {
    view = host.controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view) {
      report(Vst3EditorProbe::no_view);
      return nullptr;
    }
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeX11EmbedWindowID) !=
        Steinberg::kResultTrue) {
      auto* rejected = view;
      view = nullptr;
      rejected->release();
      report(Vst3EditorProbe::unsupported_platform);
      return nullptr;
    }
    auto result = std::make_unique<Vst3EditorView>(view, host.editor_open);
    *host.editor_open = true;
    report(Vst3EditorProbe::x11_candidate);
    return result;
  } catch (...) {
    if (view) {
      try {
        view->release();
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
    report(Vst3EditorProbe::plugin_exception);
    return nullptr;
  }
}

void Vst3EffectHost::begin_editor_parameter_capture() noexcept {
  implementation_->metadata_handler.begin_capture();
}

std::optional<std::vector<Vst3ParameterEvent>> Vst3EffectHost::take_editor_parameter_edits() {
  auto& handler = implementation_->metadata_handler;
  handler.end_capture();
  if (handler.overflowed())
    return std::nullopt;
  return handler.edits();
}

std::optional<std::vector<Vst3ParameterEvent>> Vst3EffectHost::take_editor_parameter_previews() {
  auto& handler = implementation_->metadata_handler;
  if (handler.overflowed())
    return std::nullopt;
  return handler.take_previews();
}

std::optional<std::vector<Vst3ParameterEvent>> Vst3EffectHost::take_completed_editor_gestures() {
  auto& handler = implementation_->metadata_handler;
  if (handler.overflowed())
    return std::nullopt;
  return handler.take_completed();
}

std::optional<std::vector<std::uint8_t>> Vst3EffectHost::save_component_state(std::string& error) {
  auto& host = *implementation_;
  if (host.processing || host.faulted || host.static_flush_pending) {
    error = "Suspend VST3 processing before state save";
    return std::nullopt;
  }
  BoundedStream stream(host.config.max_state_bytes);
  try {
    if (host.component->getState(&stream) == Steinberg::kResultOk && !stream.failed())
      return stream.bytes();
  } catch (...) {
    host.faulted = true;
  }
  error = "VST3 component state save failed or exceeded its size limit";
  return std::nullopt;
}

std::optional<Vst3StatePair> Vst3EffectHost::save_state_pair(std::string& error) {
  auto component = save_component_state(error);
  if (!component)
    return std::nullopt;
  Vst3StatePair pair{.component_state = std::move(*component), .controller_state = std::nullopt};
  auto& host = *implementation_;
  if (!host.controller)
    return pair;
  BoundedStream stream(host.config.max_state_bytes);
  try {
    const auto result = host.controller->getState(&stream);
    if (result == Steinberg::kResultOk && !stream.failed()) {
      pair.controller_state = stream.bytes();
      return pair;
    }
    if (result == Steinberg::kNotImplemented && stream.bytes().empty() && !stream.failed())
      return pair;
  } catch (...) {
    host.faulted = true;
  }
  error = "VST3 controller state save failed or exceeded its size limit";
  return std::nullopt;
}

bool Vst3EffectHost::restore_component_state(std::span<const std::uint8_t> state,
                                             std::string& error) {
  auto& host = *implementation_;
  if (host.processing || host.faulted || host.static_flush_pending ||
      state.size() > host.config.max_state_bytes) {
    error = "VST3 state restore requires a suspended host and bounded state";
    return false;
  }
  BoundedStream stream(state, host.config.max_state_bytes);
  host.controller_state_sync = Vst3ControllerStateSync::not_attempted;
  try {
    if (host.component->setState(&stream) == Steinberg::kResultOk && !stream.failed()) {
      if (host.controller) {
        BoundedStream controller_stream(state, host.config.max_state_bytes);
        const auto result = host.controller->setComponentState(&controller_stream);
        if (result == Steinberg::kNotImplemented && !controller_stream.failed()) {
          host.controller_state_sync = Vst3ControllerStateSync::not_implemented;
        } else if (result == Steinberg::kResultOk && !controller_stream.failed()) {
          host.controller_state_sync = Vst3ControllerStateSync::synchronized;
        } else {
          Steinberg::int64 consumed = -1;
          (void)controller_stream.tell(&consumed);
          host.faulted = true;
          error = "VST3 controller rejected restored component state (result " +
                  std::to_string(result) + ", consumed " + std::to_string(consumed) + "/" +
                  std::to_string(state.size()) + " bytes)";
          return false;
        }
      }
      // Component state can change a processor's declared latency before it
      // is activated. Graph/PDC preparation must see the restored value.
      host.latency = host.processor->getLatencySamples();
      return true;
    }
  } catch (...) {
    host.faulted = true;
  }
  error = "VST3 component state restore failed";
  return false;
}

bool Vst3EffectHost::restore_state_pair(const Vst3StatePair& state, std::string& error) {
  auto& host = *implementation_;
  if (host.processing || host.faulted || host.static_flush_pending ||
      state.component_state.size() > host.config.max_state_bytes ||
      (state.controller_state && state.controller_state->size() > host.config.max_state_bytes)) {
    error = "VST3 state restore requires a suspended host and bounded state";
    return false;
  }
  if (state.controller_state && !host.controller) {
    host.faulted = true;
    error = "Saved VST3 controller state has no matching controller";
    return false;
  }
  // A rejected component state may have changed the plugin part-way, so the
  // pair's contract faults the host on any failure.
  if (!state.component_state.empty() && !restore_component_state(state.component_state, error)) {
    host.faulted = true;
    return false;
  }
  if (!state.controller_state)
    return true;
  BoundedStream stream(*state.controller_state, host.config.max_state_bytes);
  try {
    if (host.controller->setState(&stream) == Steinberg::kResultOk && !stream.failed())
      return true;
  } catch (...) {
  }
  host.faulted = true;
  error = "VST3 controller rejected its independent state";
  return false;
}

Vst3ControllerStateSync Vst3EffectHost::controller_state_sync() const noexcept {
  return implementation_->controller_state_sync;
}

Vst3AudioBusLimits Vst3EffectHost::audio_bus_limits() const noexcept {
  const auto& host = *implementation_;
  const auto primary_inputs =
      host.config.kind == Vst3HostKind::effect ? (host.config.routed_sidechain ? 2 : 1) : 0;
  return {static_cast<std::uint32_t>(host.audio_input_buses - primary_inputs),
          static_cast<std::uint32_t>(host.audio_output_buses - 1)};
}

bool Vst3EffectHost::routed_sidechain_enabled() const noexcept {
  return implementation_->config.routed_sidechain;
}

bool Vst3EffectHost::stage_static_parameter_values(std::span<const Vst3ParameterEvent> values,
                                                   std::string& error) {
  auto& host = *implementation_;
  if (!host.controller ||
      (host.controller_initialized &&
       (!host.component_connect_attempted || !host.controller_connect_attempted)) ||
      host.active || host.processing || host.faulted || host.static_flush_pending ||
      values.empty() || values.size() > host.config.max_parameter_events) {
    host.faulted = true;
    error = "VST3 static values require an inactive controller and bounded nonempty batch";
    return false;
  }
  std::uint32_t prior_id = 0;
  bool first = true;
  for (const auto& value : values) {
    if (!std::isfinite(value.normalized_value) || value.normalized_value < 0.0 ||
        value.normalized_value > 1.0 || value.sample_offset != 0 ||
        (!first && value.id <= prior_id) ||
        parameter_precision(value.id) != Vst3ParameterPrecision::sample_offsets_forwarded) {
      host.faulted = true;
      error = "VST3 static parameter batch contains an unavailable or invalid value";
      return false;
    }
    prior_id = value.id;
    first = false;
  }
  try {
    host.static_values.assign(values.begin(), values.end());
    host.static_flush_pending = true;
    for (const auto& value : host.static_values) {
      if (host.controller->setParamNormalized(value.id, value.normalized_value) !=
          Steinberg::kResultOk) {
        host.faulted = true;
        error = "VST3 controller rejected a saved static parameter value";
        return false;
      }
    }
    return true;
  } catch (...) {
    host.faulted = true;
    error = "VST3 controller static parameter restore failed";
    return false;
  }
}

bool Vst3EffectHost::flush_static_parameter_values(std::string& error) {
  auto& host = *implementation_;
  if (!host.processing || host.faulted || !host.static_flush_pending) {
    host.faulted = true;
    error = "VST3 static parameter flush requires an active staged processor";
    return false;
  }
  host.parameter_changes.clear();
  for (const auto& value : host.static_values)
    host.parameter_changes.add(value.id, std::span(&value, 1));
  Steinberg::Vst::ProcessData data{};
  data.processMode = host.config.processing_mode == Vst3ProcessingMode::offline
                         ? Steinberg::Vst::kOffline
                         : Steinberg::Vst::kRealtime;
  data.symbolicSampleSize = Steinberg::Vst::kSample32;
  data.numSamples = 0;
  data.inputParameterChanges = &host.parameter_changes;
  try {
    if (host.processor->process(data) == Steinberg::kResultOk) {
      host.static_flush_pending = false;
      host.static_values.clear();
      return true;
    }
  } catch (...) {
    host.faulted = true;
    error = "VST3 processor threw during zero-sample static parameter flush";
    return false;
  }
  host.faulted = true;
  error = "VST3 processor rejected zero-sample static parameter flush";
  return false;
}

bool Vst3EffectHost::static_parameter_values_ready() const noexcept {
  return !implementation_->faulted && !implementation_->static_flush_pending;
}

namespace detail {

std::unique_ptr<Vst3EffectHost> create_effect_host(Steinberg::IPluginFactory& factory,
                                                   Vst3EffectConfig config, std::string& error) {
  try {
    Steinberg::TUID id{};
    if (!parse_class_id(config.identity.id, id) ||
        (config.kind != Vst3HostKind::effect && config.kind != Vst3HostKind::instrument) ||
        (config.processing_mode != Vst3ProcessingMode::realtime &&
         config.processing_mode != Vst3ProcessingMode::offline) ||
        !std::isfinite(config.sample_rate) || config.sample_rate < 8000.0 ||
        config.sample_rate > 384000.0 || config.max_block_frames == 0 ||
        config.max_block_frames > 8192 || config.max_state_bytes == 0 ||
        config.max_state_bytes > 16UL * 1024UL * 1024UL || config.max_parameter_events == 0 ||
        config.max_parameter_events > max_vst3_parameter_events || config.max_note_events == 0 ||
        config.max_note_events > 1024 ||
        (config.routed_sidechain && config.kind != Vst3HostKind::effect)) {
      error = "Invalid VST3 host configuration or class identifier";
      return nullptr;
    }
    auto implementation = std::make_unique<Vst3EffectHost::Impl>(std::move(config));
    void* object = nullptr;
    if (factory.createInstance(id, Steinberg::Vst::IComponent_iid, &object) !=
            Steinberg::kResultOk ||
        !object) {
      error = "VST3 component creation failed";
      return nullptr;
    }
    implementation->component = static_cast<Steinberg::Vst::IComponent*>(object);
    if (implementation->config.processing_mode == Vst3ProcessingMode::offline &&
        implementation->component->setIoMode(Steinberg::Vst::kOfflineProcessing) !=
            Steinberg::kResultOk) {
      error = "VST3 component rejected offline I/O mode";
      return nullptr;
    }
    Steinberg::TUID controller_id{};
    const bool separate_controller =
        implementation->component->getControllerClassId(controller_id) == Steinberg::kResultOk;
    object = nullptr;
    if (implementation->component->queryInterface(Steinberg::Vst::IAudioProcessor_iid, &object) !=
            Steinberg::kResultOk ||
        !object) {
      error = "VST3 audio processor interface is missing";
      return nullptr;
    }
    implementation->processor = static_cast<Steinberg::Vst::IAudioProcessor*>(object);
    if (implementation->component->initialize(nullptr) != Steinberg::kResultOk) {
      error = "VST3 component initialization failed";
      return nullptr;
    }
    implementation->initialized = true;
    const bool instrument = implementation->config.kind == Vst3HostKind::instrument;
    const auto audio_inputs =
        implementation->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    const auto audio_outputs =
        implementation->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    const auto event_inputs =
        implementation->component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput);
    const auto event_outputs =
        implementation->component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kOutput);
    if (implementation->config.routed_sidechain && audio_inputs != 2) {
      error = "VST3 routed sidechain requires exactly one stereo auxiliary input bus";
      return nullptr;
    }
    const bool sample32 = implementation->processor->canProcessSampleSize(
                              Steinberg::Vst::kSample32) == Steinberg::kResultOk;
    if (!sample32 || audio_inputs < (instrument ? 0 : 1) || audio_outputs < 1 ||
        audio_inputs > static_cast<Steinberg::int32>(max_host_audio_buses) ||
        audio_outputs > static_cast<Steinberg::int32>(max_host_audio_buses) ||
        event_inputs != (instrument ? 1 : 0) || event_outputs != 0) {
      const auto bus_detail = [&](Steinberg::Vst::BusDirection direction, Steinberg::int32 count) {
        std::string details;
        for (Steinberg::int32 index = 0;
             index < std::min(count, static_cast<Steinberg::int32>(max_host_audio_buses));
             ++index) {
          Steinberg::Vst::BusInfo info{};
          if (implementation->component->getBusInfo(Steinberg::Vst::kAudio, direction, index,
                                                    info) != Steinberg::kResultOk)
            return details + " ?";
          details +=
              " " + std::to_string(index) + ":" +
              (info.busType == Steinberg::Vst::kMain ? "main" : "aux") + ":" +
              std::to_string(info.channelCount) + ":" +
              ((info.flags & Steinberg::Vst::BusInfo::kDefaultActive) != 0 ? "active" : "inactive");
        }
        if (count > static_cast<Steinberg::int32>(max_host_audio_buses))
          details += " ...";
        return details;
      };
      error = "VST3 audio/event bus topology is unsupported (audio in/out " +
              std::to_string(audio_inputs) + "/" + std::to_string(audio_outputs) +
              ", event in/out " + std::to_string(event_inputs) + "/" +
              std::to_string(event_outputs) + ", float32 " + (sample32 ? "yes" : "no") +
              "; inputs" + bus_detail(Steinberg::Vst::kInput, audio_inputs) + "; outputs" +
              bus_detail(Steinberg::Vst::kOutput, audio_outputs) + ")";
      return nullptr;
    }
    std::array<Steinberg::Vst::SpeakerArrangement, max_host_audio_buses> stereo_inputs{};
    std::array<Steinberg::Vst::SpeakerArrangement, max_host_audio_buses> stereo_outputs{};
    stereo_inputs.fill(Steinberg::Vst::SpeakerArr::kStereo);
    stereo_outputs.fill(Steinberg::Vst::SpeakerArr::kStereo);
    if (implementation->processor->setBusArrangements(
            audio_inputs == 0 ? nullptr : stereo_inputs.data(), audio_inputs, stereo_outputs.data(),
            audio_outputs) != Steinberg::kResultOk) {
      error = "VST3 stereo bus arrangement was rejected";
      return nullptr;
    }
    Steinberg::Vst::BusInfo input_info{};
    Steinberg::Vst::BusInfo output_info{};
    if ((!instrument &&
         implementation->component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0,
                                               input_info) != Steinberg::kResultOk) ||
        implementation->component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0,
                                              output_info) != Steinberg::kResultOk ||
        (!instrument &&
         (input_info.channelCount != 2 || input_info.busType != Steinberg::Vst::kMain ||
          input_info.mediaType != Steinberg::Vst::kAudio ||
          input_info.direction != Steinberg::Vst::kInput)) ||
        output_info.channelCount != 2 || output_info.busType != Steinberg::Vst::kMain ||
        output_info.mediaType != Steinberg::Vst::kAudio ||
        output_info.direction != Steinberg::Vst::kOutput) {
      error = "VST3 effect reported incompatible stereo buses";
      return nullptr;
    }
    if (instrument) {
      Steinberg::Vst::BusInfo event_info{};
      if (implementation->component->getBusInfo(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0,
                                                event_info) != Steinberg::kResultOk ||
          event_info.busType != Steinberg::Vst::kMain ||
          event_info.mediaType != Steinberg::Vst::kEvent ||
          event_info.direction != Steinberg::Vst::kInput || event_info.channelCount < 1 ||
          event_info.channelCount > 16) {
        error = "VST3 instrument event bus is unsupported";
        return nullptr;
      }
      implementation->event_channels = static_cast<std::uint32_t>(event_info.channelCount);
    }
    for (Steinberg::int32 index = instrument ? 0 : 1; index < audio_inputs; ++index) {
      Steinberg::Vst::BusInfo info{};
      if (implementation->component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput,
                                                index, info) != Steinberg::kResultOk ||
          info.channelCount != 2 || info.busType != Steinberg::Vst::kAux ||
          info.mediaType != Steinberg::Vst::kAudio || info.direction != Steinberg::Vst::kInput) {
        error = "VST3 auxiliary input bus is unsupported";
        return nullptr;
      }
    }
    for (Steinberg::int32 index = 1; index < audio_outputs; ++index) {
      Steinberg::Vst::BusInfo info{};
      if (implementation->component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput,
                                                index, info) != Steinberg::kResultOk ||
          info.channelCount != 2 || info.mediaType != Steinberg::Vst::kAudio ||
          info.direction != Steinberg::Vst::kOutput ||
          (info.flags & Steinberg::Vst::BusInfo::kDefaultActive) != 0) {
        error = "VST3 secondary output bus is unsupported";
        return nullptr;
      }
    }
    implementation->audio_input_buses = audio_inputs;
    implementation->audio_output_buses = audio_outputs;
    implementation->auxiliary_inputs.resize(static_cast<std::size_t>(audio_inputs));
    implementation->auxiliary_outputs.resize(static_cast<std::size_t>(audio_outputs));
    for (Steinberg::int32 index = instrument ? 0 : 1; index < audio_inputs; ++index)
      for (auto& channel : implementation->auxiliary_inputs[static_cast<std::size_t>(index)])
        channel.resize(implementation->config.max_block_frames);
    for (Steinberg::int32 index = 1; index < audio_outputs; ++index)
      for (auto& channel : implementation->auxiliary_outputs[static_cast<std::size_t>(index)])
        channel.resize(implementation->config.max_block_frames);
    Steinberg::Vst::ProcessSetup setup{
        implementation->config.processing_mode == Vst3ProcessingMode::offline
            ? Steinberg::Vst::kOffline
            : Steinberg::Vst::kRealtime,
        Steinberg::Vst::kSample32,
        static_cast<Steinberg::int32>(implementation->config.max_block_frames),
        implementation->config.sample_rate};
    if (implementation->processor->setupProcessing(setup) != Steinberg::kResultOk ||
        (!instrument &&
         implementation->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0,
                                                true) != Steinberg::kResultOk) ||
        implementation->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0,
                                               true) != Steinberg::kResultOk ||
        (instrument &&
         implementation->component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0,
                                                true) != Steinberg::kResultOk)) {
      error = "VST3 effect processing setup failed";
      return nullptr;
    }
    for (Steinberg::int32 index = instrument ? 0 : 1; index < audio_inputs; ++index) {
      Steinberg::Vst::BusInfo info{};
      if (implementation->component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput,
                                                index, info) != Steinberg::kResultOk ||
          (((info.flags & Steinberg::Vst::BusInfo::kDefaultActive) != 0 ||
            (implementation->config.routed_sidechain && index == 1)) &&
           implementation->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput,
                                                  index, true) != Steinberg::kResultOk)) {
        error = "VST3 auxiliary input activation failed";
        return nullptr;
      }
    }
    implementation->latency = implementation->processor->getLatencySamples();
    if (separate_controller) {
      object = nullptr;
      if (factory.createInstance(controller_id, Steinberg::Vst::IEditController_iid, &object) !=
              Steinberg::kResultOk ||
          !object) {
        error = "VST3 declared controller could not be created";
        return nullptr;
      }
      implementation->controller = static_cast<Steinberg::Vst::IEditController*>(object);
      if (implementation->controller->initialize(nullptr) != Steinberg::kResultOk) {
        error = "VST3 controller initialization failed";
        return nullptr;
      }
      implementation->controller_initialized = true;
    } else {
      object = nullptr;
      if (implementation->component->queryInterface(Steinberg::Vst::IEditController_iid, &object) ==
              Steinberg::kResultOk &&
          object)
        implementation->controller = static_cast<Steinberg::Vst::IEditController*>(object);
    }
    if (implementation->controller) {
      if (implementation->controller->setComponentHandler(&implementation->metadata_handler) !=
          Steinberg::kResultOk) {
        error = "VST3 controller rejected metadata handler";
        return nullptr;
      }
      if (separate_controller) {
        void* component_endpoint = nullptr;
        void* controller_endpoint = nullptr;
        const auto component_result = implementation->component->queryInterface(
            Steinberg::Vst::IConnectionPoint_iid, &component_endpoint);
        implementation->component_connection =
            static_cast<Steinberg::Vst::IConnectionPoint*>(component_endpoint);
        const auto controller_result = implementation->controller->queryInterface(
            Steinberg::Vst::IConnectionPoint_iid, &controller_endpoint);
        implementation->controller_connection =
            static_cast<Steinberg::Vst::IConnectionPoint*>(controller_endpoint);
        if ((component_result == Steinberg::kResultOk) !=
                (implementation->component_connection != nullptr) ||
            (controller_result == Steinberg::kResultOk) !=
                (implementation->controller_connection != nullptr)) {
          error = "VST3 connection-point interface query was inconsistent";
          return nullptr;
        }
        if (implementation->component_connection && implementation->controller_connection) {
          implementation->component_connect_attempted = true;
          if (implementation->component_connection->connect(
                  implementation->controller_connection) != Steinberg::kResultOk) {
            error = "VST3 component connection was rejected";
            return nullptr;
          }
          implementation->controller_connect_attempted = true;
          if (implementation->controller_connection->connect(
                  implementation->component_connection) != Steinberg::kResultOk) {
            error = "VST3 controller connection was rejected";
            return nullptr;
          }
        }
      }
      const auto count = implementation->controller->getParameterCount();
      if (count < 0 || count > max_host_parameter_metadata) {
        error = "VST3 parameter count exceeds the host limit (" + std::to_string(count) + " > " +
                std::to_string(max_host_parameter_metadata) + ")";
        return nullptr;
      }
      std::set<std::uint32_t> seen_ids;
      implementation->parameters.reserve(static_cast<std::size_t>(count));
      for (Steinberg::int32 index = 0; index < count; ++index) {
        Steinberg::Vst::ParameterInfo info{};
        if (implementation->controller->getParameterInfo(index, info) != Steinberg::kResultOk) {
          error = "VST3 parameter metadata query failed";
          return nullptr;
        }
        const auto title = utf8_text(info.title, true);
        const auto short_title = utf8_text(info.shortTitle, false);
        const auto units = utf8_text(info.units, false);
        const auto flags = static_cast<std::uint32_t>(info.flags);
        const bool automatable =
            (flags & static_cast<std::uint32_t>(Steinberg::Vst::ParameterInfo::kCanAutomate)) != 0;
        const bool read_only =
            (flags & static_cast<std::uint32_t>(Steinberg::Vst::ParameterInfo::kIsReadOnly)) != 0;
        if (!title || !short_title || !units || !seen_ids.insert(info.id).second ||
            info.stepCount < 0 || info.stepCount > 65535 ||
            !std::isfinite(info.defaultNormalizedValue) || info.defaultNormalizedValue < 0.0 ||
            info.defaultNormalizedValue > 1.0 || (automatable && read_only)) {
          error = "VST3 parameter metadata is malformed";
          return nullptr;
        }
        implementation->parameters.push_back({.id = info.id,
                                              .title = *title,
                                              .short_title = *short_title,
                                              .units = *units,
                                              .step_count = info.stepCount,
                                              .default_normalized = info.defaultNormalizedValue,
                                              .automatable = automatable,
                                              .read_only = read_only});
      }
      std::ranges::sort(implementation->parameters, {}, &Vst3ParameterInfo::id);
    }
    return std::make_unique<Vst3EffectHost>(std::move(implementation));
  } catch (...) {
    error = "VST3 effect setup threw an exception";
    return nullptr;
  }
}

} // namespace detail
} // namespace oscillate::plugins
