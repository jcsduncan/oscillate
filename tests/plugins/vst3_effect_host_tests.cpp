#include "oscillate/plugins/vst3_effect_host.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "vst3_effect_host_internal.hpp"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using oscillate::test::counting_process_allocations;
using oscillate::test::process_allocations;

namespace {

using oscillate::test::require;

enum class Failure : std::uint8_t {
  none,
  create,
  processor_interface,
  initialize,
  sample_size,
  arrangement,
  bus_info,
  aux_buses,
  active_secondary_output,
  setup,
  activate,
  processing_activate,
  process,
  save,
  restore,
  stream_probe,
  throw_initialize,
  throw_activate,
  throw_process,
  throw_save,
  throw_restore,
  throw_suspend,
  controller_create,
  controller_initialize,
  controller_handler,
  parameter_count,
  parameter_duplicate,
  parameter_bad_title,
  parameter_low_surrogate,
  parameter_unterminated_title,
  parameter_control_title,
  parameter_bad_default,
  parameter_bad_step,
  parameter_bad_flags,
  parameter_query,
  controller_restore,
  controller_not_implemented,
  static_controller_set,
  static_flush,
  component_connect,
  controller_connect,
  component_no_connection,
  controller_no_connection
};

struct ObservedPoint {
  std::uint32_t id{0};
  std::int32_t offset{0};
  double value{0.0};
};

struct Stats {
  int created{0};
  int destroyed{0};
  int initialized{0};
  int terminated{0};
  int active{0};
  int processing{0};
  int processed_blocks{0};
  std::uint32_t declared_latency{7};
  bool throw_latency_query{false};
  bool sidechain_expected{false};
  bool aux_default_inactive{false};
  bool aux_incompatible{false};
  int aux_activations{0};
  Steinberg::Vst::IoMode io_mode{Steinberg::Vst::kSimple};
  Steinberg::int32 setup_mode{Steinberg::Vst::kRealtime};
  Steinberg::int32 process_mode{Steinberg::Vst::kRealtime};
  bool io_mode_before_initialize{false};
  std::int64_t project_sample{0};
  double quarter_position{0.0};
  double tempo_bpm{0.0};
  double sample_rate{0.0};
  std::uint32_t context_state{0};
  int static_flushes{0};
  bool enable_controller{false};
  int controller_created{0};
  int controller_destroyed{0};
  int controller_initialized{0};
  int controller_terminated{0};
  float controller_synced_gain{0.0F};
  bool controller_private_supported{false};
  bool controller_private_empty{false};
  bool controller_private_save_fails{false};
  bool controller_private_restore_fails{false};
  bool controller_private_oversized{false};
  std::uint8_t controller_private_value{0x6a};
  std::uint8_t controller_private_restored{0};
  int controller_private_restore_calls{0};
  std::array<ObservedPoint, 16> points{};
  std::size_t point_count{0};
  bool queue_probe_passed{false};
  bool handler_probe_passed{false};
  Steinberg::Vst::IComponentHandler* editor_handler{nullptr};
  bool enable_connection{false};
  bool editor_view{false};
  bool editor_x11{false};
  bool editor_throw_create{false};
  bool editor_throw_query{false};
  int editor_created{0};
  int editor_destroyed{0};
  int editor_attached{0};
  std::array<int, 8> connection_events{};
  std::size_t connection_event_count{0};
  int messages_to_component{0};
  int messages_to_controller{0};

  void note_connection(int event) { connection_events[connection_event_count++] = event; }
};

class FixtureMessage final : public Steinberg::Vst::IMessage {
public:
  explicit FixtureMessage(const char* id) : id_(id) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IMessage_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::FIDString PLUGIN_API getMessageID() override { return id_; }
  void PLUGIN_API setMessageID(Steinberg::FIDString id) override { id_ = id; }
  Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes() override { return nullptr; }

private:
  Steinberg::FIDString id_;
};

class FixtureEffect final : public Steinberg::Vst::IComponent,
                            public Steinberg::Vst::IAudioProcessor,
                            public Steinberg::Vst::IConnectionPoint {
public:
  FixtureEffect(Stats& stats, Failure failure) : stats_(stats), failure_(failure) {
    ++stats_.created;
  }
  ~FixtureEffect() { ++stats_.destroyed; }

  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(Steinberg::TUID)) == 0) {
      *object = static_cast<Steinberg::Vst::IComponent*>(this);
    } else if (failure_ != Failure::processor_interface &&
               std::memcmp(iid, Steinberg::Vst::IAudioProcessor_iid, sizeof(Steinberg::TUID)) ==
                   0) {
      *object = static_cast<Steinberg::Vst::IAudioProcessor*>(this);
    } else if (stats_.enable_connection && failure_ != Failure::component_no_connection &&
               std::memcmp(iid, Steinberg::Vst::IConnectionPoint_iid, sizeof(Steinberg::TUID)) ==
                   0) {
      *object = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
    } else {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override {
    const auto remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override {
    stats_.note_connection(1);
    if (failure_ == Failure::component_connect || !other)
      return Steinberg::kResultFalse;
    peer_ = other;
    peer_->addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) override {
    stats_.note_connection(4);
    if (other != peer_ && peer_ != nullptr)
      return Steinberg::kResultFalse;
    if (peer_)
      peer_->release();
    peer_ = nullptr;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override {
    if (!message || std::strcmp(message->getMessageID(), "controller-to-component") != 0)
      return Steinberg::kResultFalse;
    ++stats_.messages_to_component;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown*) override {
    if (failure_ == Failure::throw_initialize)
      throw std::runtime_error("fixture initialization exception");
    if (failure_ == Failure::initialize)
      return Steinberg::kResultFalse;
    ++stats_.initialized;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override {
    ++stats_.terminated;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getControllerClassId(Steinberg::TUID id) override {
    if (!stats_.enable_controller)
      return Steinberg::kNoInterface;
    const Steinberg::TUID controller_id = {33, 34, 35, 36, 37, 38, 39, 40,
                                           41, 42, 43, 44, 45, 46, 47, 48};
    std::memcpy(id, controller_id, sizeof(controller_id));
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setIoMode(Steinberg::Vst::IoMode mode) override {
    stats_.io_mode = mode;
    stats_.io_mode_before_initialize = stats_.initialized == 0;
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API getBusCount(Steinberg::Vst::MediaType type,
                                          Steinberg::Vst::BusDirection) override {
    return type == Steinberg::Vst::kAudio
               ? (failure_ == Failure::aux_buses || failure_ == Failure::active_secondary_output
                      ? 2
                      : 1)
               : 0;
  }
  Steinberg::tresult PLUGIN_API getBusInfo(Steinberg::Vst::MediaType,
                                           Steinberg::Vst::BusDirection direction,
                                           Steinberg::int32 index,
                                           Steinberg::Vst::BusInfo& info) override {
    if (failure_ == Failure::bus_info)
      return Steinberg::kResultFalse;
    info = {};
    info.direction = direction;
    info.mediaType = Steinberg::Vst::kAudio;
    info.channelCount =
        index == 1 && direction == Steinberg::Vst::kInput && stats_.aux_incompatible ? 1 : 2;
    info.busType = index == 1 && direction == Steinberg::Vst::kInput ? Steinberg::Vst::kAux
                                                                     : Steinberg::Vst::kMain;
    if (index == 1 && direction == Steinberg::Vst::kInput)
      info.flags = stats_.aux_default_inactive ? 0 : Steinberg::Vst::BusInfo::kDefaultActive;
    if (index == 1 && direction == Steinberg::Vst::kOutput &&
        failure_ == Failure::active_secondary_output)
      info.flags = Steinberg::Vst::BusInfo::kDefaultActive;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getRoutingInfo(Steinberg::Vst::RoutingInfo&,
                                               Steinberg::Vst::RoutingInfo&) override {
    return Steinberg::kNoInterface;
  }
  Steinberg::tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType, Steinberg::Vst::BusDirection,
                                            Steinberg::int32 index, Steinberg::TBool) override {
    if (index == 1)
      ++stats_.aux_activations;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool active) override {
    if (active && failure_ == Failure::throw_activate)
      throw std::runtime_error("fixture activation exception");
    if (active && failure_ == Failure::activate)
      return Steinberg::kResultFalse;
    stats_.active = active ? 1 : 0;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    if (failure_ == Failure::throw_restore)
      throw std::runtime_error("fixture restore exception");
    if (failure_ == Failure::restore)
      return Steinberg::kResultFalse;
    float incoming = 0.0F;
    Steinberg::int32 read = 0;
    if (stream->read(&incoming, sizeof(incoming), &read) != Steinberg::kResultOk ||
        read != sizeof(incoming) || !std::isfinite(incoming))
      return Steinberg::kResultFalse;
    gain_ = incoming;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    if (failure_ == Failure::throw_save)
      throw std::runtime_error("fixture save exception");
    if (failure_ == Failure::save)
      return Steinberg::kResultFalse;
    if (peer_) {
      FixtureMessage message("component-to-controller");
      if (peer_->notify(&message) != Steinberg::kResultOk)
        return Steinberg::kResultFalse;
    }
    if (failure_ == Failure::stream_probe) {
      void* interface_pointer = nullptr;
      if (stream->queryInterface(Steinberg::IBStream_iid, &interface_pointer) !=
              Steinberg::kResultOk ||
          !interface_pointer)
        return Steinberg::kResultFalse;
      static_cast<Steinberg::IBStream*>(interface_pointer)->release();
      if (stream->queryInterface(Steinberg::IPluginFactory_iid, &interface_pointer) !=
              Steinberg::kNoInterface ||
          interface_pointer ||
          stream->queryInterface(Steinberg::IBStream_iid, nullptr) != Steinberg::kInvalidArgument ||
          stream->read(nullptr, -1) != Steinberg::kInvalidArgument ||
          stream->write(nullptr, -1) != Steinberg::kInvalidArgument ||
          stream->tell(nullptr) != Steinberg::kInvalidArgument ||
          stream->seek(1, 99) != Steinberg::kInvalidArgument ||
          stream->seek(-1, Steinberg::IBStream::kIBSeekSet) != Steinberg::kResultFalse)
        return Steinberg::kResultFalse;
      Steinberg::int64 position = 0;
      Steinberg::int32 read = 1;
      std::uint8_t byte = 0;
      if (stream->seek(2, Steinberg::IBStream::kIBSeekSet, &position) != Steinberg::kResultOk ||
          position != 2 || stream->read(&byte, 1, &read) != Steinberg::kResultOk || read != 0)
        return Steinberg::kResultFalse;
      Steinberg::int32 written = 0;
      if (stream->write(&gain_, sizeof(gain_), &written) != Steinberg::kResultOk ||
          written != sizeof(gain_) || stream->tell(&position) != Steinberg::kResultOk ||
          position != 6 ||
          stream->seek(0, Steinberg::IBStream::kIBSeekEnd) != Steinberg::kResultOk ||
          stream->read(nullptr, 0) != Steinberg::kResultOk)
        return Steinberg::kResultFalse;
      return Steinberg::kResultOk;
    }
    return stream->write(&gain_, sizeof(gain_)) == Steinberg::kResultOk ? Steinberg::kResultOk
                                                                        : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                   Steinberg::int32 in_count,
                                                   Steinberg::Vst::SpeakerArrangement* outputs,
                                                   Steinberg::int32 out_count) override {
    const auto expected =
        failure_ == Failure::aux_buses || failure_ == Failure::active_secondary_output ? 2 : 1;
    if (failure_ == Failure::arrangement || in_count != expected || out_count != expected ||
        !std::ranges::all_of(
            std::span(inputs, static_cast<std::size_t>(in_count)),
            [](auto value) { return value == Steinberg::Vst::SpeakerArr::kStereo; }) ||
        !std::ranges::all_of(
            std::span(outputs, static_cast<std::size_t>(out_count)),
            [](auto value) { return value == Steinberg::Vst::SpeakerArr::kStereo; }))
      return Steinberg::kResultFalse;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API
  getBusArrangement(Steinberg::Vst::BusDirection, Steinberg::int32,
                    Steinberg::Vst::SpeakerArrangement& arrangement) override {
    arrangement = Steinberg::Vst::SpeakerArr::kStereo;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
    return failure_ != Failure::sample_size && size == Steinberg::Vst::kSample32
               ? Steinberg::kResultOk
               : Steinberg::kResultFalse;
  }
  Steinberg::uint32 PLUGIN_API getLatencySamples() override {
    if (stats_.throw_latency_query)
      throw std::runtime_error("fixture latency query failed");
    return stats_.declared_latency;
  }
  Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
    stats_.setup_mode = setup.processMode;
    if (failure_ == Failure::setup || setup.symbolicSampleSize != Steinberg::Vst::kSample32 ||
        setup.maxSamplesPerBlock != 128 || setup.sampleRate != 48000.0)
      return Steinberg::kResultFalse;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool active) override {
    if (!active && failure_ == Failure::throw_suspend)
      throw std::runtime_error("fixture suspension exception");
    if (active && failure_ == Failure::processing_activate)
      return Steinberg::kResultFalse;
    stats_.processing = active ? 1 : 0;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
    if (failure_ == Failure::throw_process || failure_ == Failure::process) {
      // Model a processor that faults after contaminating part of its output.
      if (data.numSamples > 0 && data.numOutputs > 0) {
        data.outputs[0].channelBuffers32[0][0] = 0.75F;
        data.outputs[0].channelBuffers32[1][0] = -0.75F;
      }
      if (failure_ == Failure::throw_process)
        throw std::runtime_error("fixture process exception");
      return Steinberg::kResultFalse;
    }
    if (data.numSamples == 0) {
      if (failure_ == Failure::static_flush || data.numInputs != 0 || data.numOutputs != 0 ||
          !data.inputParameterChanges || data.inputParameterChanges->getParameterCount() != 1)
        return Steinberg::kResultFalse;
      auto* queue = data.inputParameterChanges->getParameterData(0);
      Steinberg::int32 offset = -1;
      Steinberg::Vst::ParamValue value = -1.0;
      if (!queue || queue->getParameterId() != 101 || queue->getPointCount() != 1 ||
          queue->getPoint(0, offset, value) != Steinberg::kResultOk || offset != 0)
        return Steinberg::kResultFalse;
      gain_ = static_cast<float>(value * 4.0);
      ++stats_.static_flushes;
      return Steinberg::kResultOk;
    }
    const auto expected = failure_ == Failure::aux_buses ? 2 : 1;
    if (data.numInputs != expected || data.numOutputs != expected || data.numSamples > 128 ||
        data.inputs[0].numChannels != 2 || data.outputs[0].numChannels != 2 ||
        data.processContext == nullptr)
      return Steinberg::kResultFalse;
    if (failure_ == Failure::aux_buses) {
      if (data.inputs[1].numChannels != 2 || data.outputs[1].numChannels != 2)
        return Steinberg::kResultFalse;
      for (Steinberg::int32 frame = 0; frame < data.numSamples; ++frame) {
        const auto expected_left = stats_.sidechain_expected ? 0.125F : 0.0F;
        const auto expected_right = stats_.sidechain_expected ? -0.25F : 0.0F;
        if (data.inputs[1].channelBuffers32[0][frame] != expected_left ||
            data.inputs[1].channelBuffers32[1][frame] != expected_right)
          return Steinberg::kResultFalse;
        data.outputs[1].channelBuffers32[0][frame] = 42.0F;
        data.outputs[1].channelBuffers32[1][frame] = -42.0F;
      }
    }
    stats_.process_mode = data.processMode;
    stats_.project_sample = data.processContext->projectTimeSamples;
    stats_.quarter_position = data.processContext->projectTimeMusic;
    stats_.tempo_bpm = data.processContext->tempo;
    stats_.sample_rate = data.processContext->sampleRate;
    stats_.context_state = data.processContext->state;
    ++stats_.processed_blocks;
    stats_.point_count = 0;
    if (data.inputParameterChanges) {
      if (stats_.enable_controller) {
        void* interface_pointer = nullptr;
        Steinberg::int32 inserted = 0;
        const Steinberg::Vst::ParamID id = 101;
        stats_.queue_probe_passed =
            data.inputParameterChanges->queryInterface(Steinberg::Vst::IParameterChanges_iid,
                                                       &interface_pointer) ==
                Steinberg::kResultOk &&
            interface_pointer == data.inputParameterChanges &&
            data.inputParameterChanges->queryInterface(
                Steinberg::IPluginFactory_iid, &interface_pointer) == Steinberg::kNoInterface &&
            interface_pointer == nullptr &&
            data.inputParameterChanges->getParameterData(-1) == nullptr &&
            data.inputParameterChanges->getParameterData(99) == nullptr &&
            data.inputParameterChanges->addParameterData(id, inserted) == nullptr && inserted == -1;
        if (!stats_.queue_probe_passed)
          return Steinberg::kResultFalse;
      }
      for (int queue_index = 0; queue_index < data.inputParameterChanges->getParameterCount();
           ++queue_index) {
        auto* queue = data.inputParameterChanges->getParameterData(queue_index);
        if (!queue)
          return Steinberg::kResultFalse;
        if (stats_.enable_controller) {
          void* interface_pointer = nullptr;
          Steinberg::int32 offset = 0;
          Steinberg::int32 inserted = 0;
          Steinberg::Vst::ParamValue value = 0.0;
          stats_.queue_probe_passed =
              stats_.queue_probe_passed &&
              queue->queryInterface(Steinberg::Vst::IParamValueQueue_iid, &interface_pointer) ==
                  Steinberg::kResultOk &&
              interface_pointer == queue &&
              queue->queryInterface(Steinberg::IPluginFactory_iid, &interface_pointer) ==
                  Steinberg::kNoInterface &&
              interface_pointer == nullptr &&
              queue->getPoint(-1, offset, value) == Steinberg::kInvalidArgument &&
              queue->getPoint(queue->getPointCount(), offset, value) ==
                  Steinberg::kInvalidArgument &&
              queue->addPoint(0, 0.5, inserted) == Steinberg::kResultFalse;
          if (!stats_.queue_probe_passed)
            return Steinberg::kResultFalse;
        }
        for (int point_index = 0; point_index < queue->getPointCount(); ++point_index) {
          if (stats_.point_count >= stats_.points.size())
            return Steinberg::kResultFalse;
          auto& point = stats_.points[stats_.point_count++];
          point.id = queue->getParameterId();
          if (queue->getPoint(point_index, point.offset, point.value) != Steinberg::kResultOk)
            return Steinberg::kResultFalse;
        }
      }
    }
    const float initial_gain = gain_;
    for (int channel = 0; channel < 2; ++channel) {
      float running_gain = initial_gain;
      for (int frame = 0; frame < data.numSamples; ++frame) {
        for (std::size_t index = 0; index < stats_.point_count; ++index)
          if (stats_.points[index].id == 101 && stats_.points[index].offset == frame)
            running_gain = static_cast<float>(stats_.points[index].value * 4.0);
        data.outputs[0].channelBuffers32[channel][frame] =
            data.inputs[0].channelBuffers32[channel][frame] * running_gain +
            (stats_.sidechain_expected ? data.inputs[1].channelBuffers32[channel][frame] : 0.0F);
      }
      gain_ = running_gain;
    }
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
  Stats& stats_;
  Failure failure_;
  Steinberg::uint32 references_{1};
  float gain_{2.0F};
  Steinberg::Vst::IConnectionPoint* peer_{nullptr};
};

class FixtureEditorView final : public Steinberg::IPlugView {
public:
  explicit FixtureEditorView(Stats& stats) : stats_(stats) { ++stats_.editor_created; }
  ~FixtureEditorView() { ++stats_.editor_destroyed; }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (std::memcmp(iid, Steinberg::IPlugView_iid, sizeof(Steinberg::TUID)) != 0 &&
        std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) != 0) {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    *object = this;
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override {
    const auto remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override {
    if (stats_.editor_throw_query)
      throw std::runtime_error("editor query failure");
    return stats_.editor_x11 && type &&
                   std::strcmp(type, Steinberg::kPlatformTypeX11EmbedWindowID) == 0
               ? Steinberg::kResultTrue
               : Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API attached(void*, Steinberg::FIDString) override {
    ++stats_.editor_attached;
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API removed() override { return Steinberg::kResultFalse; }
  Steinberg::tresult PLUGIN_API onWheel(float) override { return Steinberg::kResultFalse; }
  Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16, Steinberg::int16,
                                          Steinberg::int16) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16, Steinberg::int16,
                                        Steinberg::int16) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect*) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect*) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame*) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultFalse; }
  Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect*) override {
    return Steinberg::kResultFalse;
  }

private:
  Stats& stats_;
  Steinberg::uint32 references_{1};
};

class FixtureController final : public Steinberg::Vst::IEditController,
                                public Steinberg::Vst::IConnectionPoint {
public:
  FixtureController(Stats& stats, Failure failure) : stats_(stats), failure_(failure) {
    ++stats_.controller_created;
  }
  ~FixtureController() { ++stats_.controller_destroyed; }
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
    if (!object)
      return Steinberg::kInvalidArgument;
    if (stats_.enable_connection && failure_ != Failure::controller_no_connection &&
        std::memcmp(iid, Steinberg::Vst::IConnectionPoint_iid, sizeof(Steinberg::TUID)) == 0) {
      *object = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
    } else if (std::memcmp(iid, Steinberg::Vst::IEditController_iid, sizeof(Steinberg::TUID)) ==
                   0 ||
               std::memcmp(iid, Steinberg::FUnknown_iid, sizeof(Steinberg::TUID)) == 0) {
      *object = static_cast<Steinberg::Vst::IEditController*>(this);
    } else {
      *object = nullptr;
      return Steinberg::kNoInterface;
    }
    addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override {
    const auto remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override {
    stats_.note_connection(2);
    if (failure_ == Failure::controller_connect || !other)
      return Steinberg::kResultFalse;
    peer_ = other;
    peer_->addRef();
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) override {
    stats_.note_connection(3);
    if (other != peer_ && peer_ != nullptr)
      return Steinberg::kResultFalse;
    if (peer_)
      peer_->release();
    peer_ = nullptr;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override {
    if (!message || std::strcmp(message->getMessageID(), "component-to-controller") != 0)
      return Steinberg::kResultFalse;
    ++stats_.messages_to_controller;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown*) override {
    if (failure_ == Failure::controller_initialize)
      return Steinberg::kResultFalse;
    ++stats_.controller_initialized;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API terminate() override {
    ++stats_.controller_terminated;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* stream) override {
    if (failure_ == Failure::controller_not_implemented)
      return Steinberg::kNotImplemented;
    if (failure_ == Failure::controller_restore)
      return Steinberg::kResultFalse;
    Steinberg::int32 count = 0;
    float gain = 0.0F;
    if (stream->read(&gain, sizeof(gain), &count) != Steinberg::kResultOk || count != sizeof(gain))
      return Steinberg::kResultFalse;
    stats_.controller_synced_gain = gain;
    if (peer_) {
      FixtureMessage message("controller-to-component");
      if (peer_->notify(&message) != Steinberg::kResultOk)
        return Steinberg::kResultFalse;
    }
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* stream) override {
    ++stats_.controller_private_restore_calls;
    if (stats_.controller_private_restore_fails || !stats_.controller_private_supported)
      return Steinberg::kResultFalse;
    if (stats_.controller_private_empty)
      return Steinberg::kResultOk;
    Steinberg::int32 count = 0;
    std::uint8_t value = 0;
    if (stream->read(&value, sizeof(value), &count) != Steinberg::kResultOk ||
        count != sizeof(value))
      return Steinberg::kResultFalse;
    stats_.controller_private_restored = value;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* stream) override {
    if (stats_.controller_private_save_fails)
      return Steinberg::kResultFalse;
    if (!stats_.controller_private_supported)
      return Steinberg::kNotImplemented;
    if (stats_.controller_private_empty)
      return Steinberg::kResultOk;
    if (stats_.controller_private_oversized) {
      std::array<std::uint8_t, 1025> bytes{};
      return stream->write(bytes.data(), static_cast<Steinberg::int32>(bytes.size()));
    }
    return stream->write(&stats_.controller_private_value, sizeof(stats_.controller_private_value));
  }
  Steinberg::int32 PLUGIN_API getParameterCount() override {
    return failure_ == Failure::parameter_count ? 8193 : 3;
  }
  Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 index,
                                                 Steinberg::Vst::ParameterInfo& info) override {
    if (index < 0 || index >= 3)
      return Steinberg::kInvalidArgument;
    if (failure_ == Failure::parameter_query)
      return Steinberg::kResultFalse;
    info = {};
    info.id = index == 0 || (index == 1 && failure_ == Failure::parameter_duplicate) ? 101U
              : index == 1                                                           ? 202U
                                                                                     : 303U;
    const auto* title = index == 0 ? u"\U0001F3B5 Gain" : index == 1 ? u"\u00D8 Mode" : u"Pan";
    for (std::size_t i = 0; title[i] != 0; ++i)
      info.title[i] = title[i];
    if (failure_ == Failure::parameter_bad_title) {
      info.title[0] = static_cast<Steinberg::Vst::TChar>(0xd800);
      info.title[1] = 0;
    }
    if (failure_ == Failure::parameter_low_surrogate)
      info.title[0] = static_cast<Steinberg::Vst::TChar>(0xdc00);
    if (failure_ == Failure::parameter_unterminated_title)
      for (auto& character : info.title)
        character = u'X';
    if (failure_ == Failure::parameter_control_title)
      info.title[0] = u'\n';
    info.shortTitle[0] = u'\u20AC';
    info.units[0] = u'd';
    info.units[1] = u'B';
    info.stepCount = failure_ == Failure::parameter_bad_step ? -1 : index == 1 ? 2 : 0;
    info.defaultNormalizedValue =
        failure_ == Failure::parameter_bad_default ? std::numeric_limits<double>::quiet_NaN() : 0.5;
    info.flags = index != 1 ? Steinberg::Vst::ParameterInfo::kCanAutomate : 0;
    if (failure_ == Failure::parameter_bad_flags)
      info.flags |= Steinberg::Vst::ParameterInfo::kIsReadOnly;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID,
                                                      Steinberg::Vst::ParamValue,
                                                      Steinberg::Vst::String128) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID,
                                                      Steinberg::Vst::TChar*,
                                                      Steinberg::Vst::ParamValue&) override {
    return Steinberg::kResultFalse;
  }
  Steinberg::Vst::ParamValue PLUGIN_API
  normalizedParamToPlain(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue value) override {
    return value;
  }
  Steinberg::Vst::ParamValue PLUGIN_API
  plainParamToNormalized(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue value) override {
    return value;
  }
  Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID) override {
    return 0.5;
  }
  // The SDK fixes this convertible parameter order.
  Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id,
                                                   Steinberg::Vst::ParamValue value) override {
    if (failure_ == Failure::static_controller_set || id != 101)
      return Steinberg::kResultFalse;
    stats_.controller_synced_gain = static_cast<float>(value * 4.0);
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API
  setComponentHandler(Steinberg::Vst::IComponentHandler* handler) override {
    if (handler && failure_ == Failure::controller_handler)
      return Steinberg::kResultFalse;
    if (handler) {
      void* interface_pointer = nullptr;
      stats_.handler_probe_passed =
          handler->queryInterface(Steinberg::Vst::IComponentHandler_iid, &interface_pointer) ==
              Steinberg::kResultOk &&
          interface_pointer == handler && handler->addRef() == 1 && handler->release() == 1 &&
          handler->queryInterface(Steinberg::IPluginFactory_iid, &interface_pointer) ==
              Steinberg::kNoInterface &&
          interface_pointer == nullptr && handler->beginEdit(101) == Steinberg::kResultFalse &&
          handler->performEdit(101, 0.5) == Steinberg::kResultFalse &&
          handler->endEdit(101) == Steinberg::kResultFalse &&
          handler->restartComponent(0) == Steinberg::kResultFalse;
      if (!stats_.handler_probe_passed)
        return Steinberg::kResultFalse;
    }
    stats_.editor_handler = handler;
    return Steinberg::kResultOk;
  }
  Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override {
    if (stats_.editor_throw_create)
      throw std::runtime_error("editor creation failure");
    if (!stats_.editor_view || !name || std::strcmp(name, Steinberg::Vst::ViewType::kEditor) != 0)
      return nullptr;
    return new FixtureEditorView(stats_);
  }

private:
  Stats& stats_;
  Failure failure_;
  Steinberg::uint32 references_{1};
  Steinberg::Vst::IConnectionPoint* peer_{nullptr};
};

class FixtureFactory final : public Steinberg::IPluginFactory {
public:
  FixtureFactory(Stats& stats, Failure failure) : stats_(stats), failure_(failure) {}
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void** object) override {
    if (object)
      *object = nullptr;
    return Steinberg::kNoInterface;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
  Steinberg::tresult PLUGIN_API getFactoryInfo(Steinberg::PFactoryInfo*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::int32 PLUGIN_API countClasses() override { return 1; }
  Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32, Steinberg::PClassInfo*) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid,
                                               void** object) override {
    static const Steinberg::TUID expected = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static const Steinberg::TUID controller_id = {33, 34, 35, 36, 37, 38, 39, 40,
                                                  41, 42, 43, 44, 45, 46, 47, 48};
    if (stats_.enable_controller && std::memcmp(cid, controller_id, sizeof(controller_id)) == 0 &&
        std::memcmp(iid, Steinberg::Vst::IEditController_iid, sizeof(controller_id)) == 0) {
      if (failure_ == Failure::controller_create)
        return Steinberg::kNoInterface;
      *object =
          static_cast<Steinberg::Vst::IEditController*>(new FixtureController(stats_, failure_));
      return Steinberg::kResultOk;
    }
    if (failure_ == Failure::create || std::memcmp(cid, expected, sizeof(expected)) != 0 ||
        std::memcmp(iid, Steinberg::Vst::IComponent_iid, sizeof(expected)) != 0)
      return Steinberg::kNoInterface;
    *object = static_cast<Steinberg::Vst::IComponent*>(new FixtureEffect(stats_, failure_));
    return Steinberg::kResultOk;
  }

private:
  Stats& stats_;
  Failure failure_;
};

oscillate::plugins::Vst3EffectConfig config() {
  return {.identity = {.id = "0102030405060708090a0b0c0d0e0f10",
                       .category = "Audio Module Class",
                       .name = "Fixture",
                       .vendor = "Fixture",
                       .version = "1",
                       .subcategories = "Fx"},
          .sample_rate = oscillate::test::sample_rate_48k_hz,
          .max_block_frames = 128,
          .max_state_bytes = 1024};
}

void test_process_state_and_lifecycle() {
  Stats stats;
  FixtureFactory factory(stats, Failure::none);
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
  REQUIRE_NE(host, nullptr, "effect host must instantiate");
  REQUIRE_EQ(host->latency_samples(), 7, "latency must be exposed");
  REQUIRE_EQ(host->check_prepared_latency().status,
             oscillate::plugins::Vst3LatencyCheckStatus::stable,
             "unchanged processor latency must be independently observed");
  REQUIRE_EQ(host->finite_tail_samples(), 0, "finite zero tail must be exposed");
  REQUIRE_EQ(host->identity().id, config().identity.id, "scanned class identity must persist");
  std::array<float, 64> left{}, right{}, output_left{}, output_right{};
  left[0] = 0.25F;
  right[0] = -0.5F;
  oscillate::plugins::Vst3StereoBlock block{left, right, output_left, output_right};
  REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::inactive,
             "unactivated host must not process");
  require(host->activate(error), "activation must succeed");
  REQUIRE_EQ(stats.active, 1, "fixture must be processing");
  REQUIRE_EQ(stats.processing, 1, "fixture must be processing");
  block.project_sample = 48'000;
  block.quarter_position = 4.0;
  block.tempo_bpm = 96.0;
  process_allocations = 0;
  counting_process_allocations = true;
  const auto status = host->process(block);
  counting_process_allocations = false;
  REQUIRE_EQ(status, oscillate::plugins::Vst3ProcessStatus::ok, "stereo process must succeed");
  REQUIRE_EQ(process_allocations, 0, "host callback path must not allocate");
  REQUIRE_EQ(output_left[0], 0.5F, "gain oracle failed");
  REQUIRE_EQ(output_right[0], -1.0F, "gain oracle failed");
  REQUIRE_EQ(stats.processed_blocks, 1, "one block expected");
  constexpr auto timing_flags = Steinberg::Vst::ProcessContext::kTempoValid |
                                Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
                                Steinberg::Vst::ProcessContext::kPlaying;
  REQUIRE_EQ(stats.project_sample, 48'000, "effect plugin did not receive block project timing");
  REQUIRE_EQ(stats.quarter_position, 4.0, "effect plugin did not receive block project timing");
  REQUIRE_EQ(stats.tempo_bpm, 96.0, "effect plugin did not receive block project timing");
  REQUIRE_EQ(stats.sample_rate, 48'000.0, "effect plugin did not receive block project timing");
  REQUIRE_EQ((stats.context_state & timing_flags), timing_flags,
             "effect plugin did not receive block project timing");
  require(!host->save_component_state(error), "active state save must be rejected");
  host->suspend();
  REQUIRE_EQ(stats.active, 0, "suspension must stop plugin");
  REQUIRE_EQ(stats.processing, 0, "suspension must stop plugin");
  auto state = host->save_component_state(error);
  if (!state)
    throw std::runtime_error("component state missing");
  REQUIRE_EQ(state->size(), sizeof(float), "component state must save");
  float new_gain = 0.5F;
  std::memcpy(state.value().data(), &new_gain, sizeof(new_gain));
  require(host->restore_component_state(state.value(), error), "component state must restore");
  require(host->activate(error), "reactivation must succeed");
  REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::ok,
             "processing after restore must succeed");
  REQUIRE_EQ(output_left[0], 0.125F, "restored component gain oracle failed");
  REQUIRE_EQ(output_right[0], -0.25F, "restored component gain oracle failed");
  host.reset();
  REQUIRE_EQ(stats.created, 1, "component must be released");
  REQUIRE_EQ(stats.destroyed, 1, "component must be released");
  REQUIRE_EQ(stats.initialized, 1, "component must terminate once");
  REQUIRE_EQ(stats.terminated, 1, "component must terminate once");
}

void test_bounded_auxiliary_buses() {
  Stats stats;
  FixtureFactory factory(stats, Failure::aux_buses);
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
  REQUIRE_NE(host, nullptr, "stereo effect with one auxiliary input and output must load");
  REQUIRE_TRUE(host->activate(error),
               "stereo effect with one auxiliary input and output must load");
  const auto limits = host->audio_bus_limits();
  REQUIRE_EQ(limits.silent_auxiliary_inputs, 1, "host did not report its unconnected buses");
  REQUIRE_EQ(limits.discarded_secondary_outputs, 1, "host did not report its unconnected buses");
  std::array<float, 128> left{}, right{}, output_left{}, output_right{};
  left.fill(0.25F);
  right.fill(-0.5F);
  for (std::size_t frames : {64UL, 128UL}) {
    process_allocations = 0;
    counting_process_allocations = true;
    const auto result = host->process(
        {std::span(left).first(frames), std::span(right).first(frames),
         std::span(output_left).first(frames), std::span(output_right).first(frames)});
    counting_process_allocations = false;
    REQUIRE_EQ(result, oscillate::plugins::Vst3ProcessStatus::ok,
               "silent auxiliary input and discarded output must not alter main stereo audio");
    REQUIRE_EQ(process_allocations, 0,
               "silent auxiliary input and discarded output must not alter main stereo audio");
    REQUIRE_EQ(output_left[0], 0.5F,
               "silent auxiliary input and discarded output must not alter main stereo audio");
    REQUIRE_EQ(output_right[0], -1.0F,
               "silent auxiliary input and discarded output must not alter main stereo audio");
  }
  Stats unsupported_stats;
  FixtureFactory unsupported(unsupported_stats, Failure::active_secondary_output);
  REQUIRE_TRUE(!oscillate::plugins::detail::create_effect_host(unsupported, config(), error),
               "active secondary output must fail instead of being silently discarded");
  REQUIRE_EQ(error, "VST3 secondary output bus is unsupported",
             "active secondary output must fail instead of being silently discarded");
}

void test_routed_sidechain_bus() {
  Stats stats;
  stats.sidechain_expected = true;
  stats.aux_default_inactive = true;
  FixtureFactory factory(stats, Failure::aux_buses);
  auto requested = config();
  requested.routed_sidechain = true;
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, requested, error);
  REQUIRE_NE(host, nullptr,
             "routed detector must activate a compatible inactive auxiliary input off callback");
  REQUIRE_EQ(stats.aux_activations, 1,
             "routed detector must activate a compatible inactive auxiliary input off callback");
  REQUIRE_TRUE(host->activate(error),
               "routed detector must activate a compatible inactive auxiliary input off callback");
  REQUIRE_EQ(host->audio_bus_limits().silent_auxiliary_inputs, 0,
             "connected detector bus must not be reported as silent");
  REQUIRE_EQ(host->audio_bus_limits().discarded_secondary_outputs, 1,
             "connected detector bus must not be reported as silent");
  REQUIRE_TRUE(host->routed_sidechain_enabled(),
               "connected detector bus must not be reported as silent");
  std::array<float, 128> left{}, right{}, detector_left{}, detector_right{}, output_left{},
      output_right{};
  left.fill(0.25F);
  right.fill(-0.5F);
  detector_left.fill(0.125F);
  detector_right.fill(-0.25F);
  for (const std::size_t frames : {64UL, 128UL}) {
    oscillate::plugins::Vst3StereoBlock block{
        std::span(left).first(frames), std::span(right).first(frames),
        std::span(output_left).first(frames), std::span(output_right).first(frames)};
    REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::invalid_block,
               "routed host accepted a missing detector span");
    block.sidechain_left = std::span(detector_left).first(frames);
    REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::invalid_block,
               "routed host accepted a one-channel detector");
    block.sidechain_right = std::span(detector_right).first(frames);
    process_allocations = 0;
    counting_process_allocations = true;
    const auto status = host->process(block);
    counting_process_allocations = false;
    REQUIRE_EQ(status, oscillate::plugins::Vst3ProcessStatus::ok,
               "routed auxiliary signal was not delivered exactly at 64/128 frames");
    REQUIRE_EQ(process_allocations, 0,
               "routed auxiliary signal was not delivered exactly at 64/128 frames");
    REQUIRE_TRUE(std::ranges::all_of(std::span(output_left).first(frames),
                                     [](float value) { return value == 0.625F; }),
                 "routed auxiliary signal was not delivered exactly at 64/128 frames");
    REQUIRE_TRUE(std::ranges::all_of(std::span(output_right).first(frames),
                                     [](float value) { return value == -1.25F; }),
                 "routed auxiliary signal was not delivered exactly at 64/128 frames");
  }
  Stats missing_stats;
  FixtureFactory missing(missing_stats, Failure::none);
  REQUIRE_TRUE(!oscillate::plugins::detail::create_effect_host(missing, requested, error),
               "missing detector bus must fail preflight without changing another effect");
  REQUIRE_EQ(error, "VST3 routed sidechain requires exactly one stereo auxiliary input bus",
             "missing detector bus must fail preflight without changing another effect");
  Stats incompatible_stats;
  incompatible_stats.aux_incompatible = true;
  FixtureFactory incompatible(incompatible_stats, Failure::aux_buses);
  REQUIRE_TRUE(!oscillate::plugins::detail::create_effect_host(incompatible, requested, error),
               "incompatible detector layout must fail before graph admission");
  REQUIRE_EQ(error, "VST3 auxiliary input bus is unsupported",
             "incompatible detector layout must fail before graph admission");
}

void test_offline_processing_mode() {
  Stats stats;
  FixtureFactory factory(stats, Failure::none);
  auto offline = config();
  offline.processing_mode = oscillate::plugins::Vst3ProcessingMode::offline;
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, offline, error);
  REQUIRE_NE(host, nullptr, "offline effect did not activate");
  REQUIRE_TRUE(host->activate(error), "offline effect did not activate");
  std::array<float, 64> input{}, output_left{}, output_right{};
  const oscillate::plugins::Vst3StereoBlock block{input, input, output_left, output_right};
  REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::ok,
             "offline effect did not process");
  REQUIRE_EQ(stats.io_mode, Steinberg::Vst::kOfflineProcessing,
             "offline mode did not reach component, setup and process calls");
  REQUIRE_TRUE(stats.io_mode_before_initialize,
               "offline mode did not reach component, setup and process calls");
  REQUIRE_EQ(stats.setup_mode, Steinberg::Vst::kOffline,
             "offline mode did not reach component, setup and process calls");
  REQUIRE_EQ(stats.process_mode, Steinberg::Vst::kOffline,
             "offline mode did not reach component, setup and process calls");
}

void test_failures() {
  for (const auto failure :
       {Failure::create, Failure::processor_interface, Failure::initialize, Failure::sample_size,
        Failure::arrangement, Failure::bus_info, Failure::setup, Failure::throw_initialize}) {
    Stats stats;
    FixtureFactory factory(stats, failure);
    std::string error;
    require(!oscillate::plugins::detail::create_effect_host(factory, config(), error),
            "invalid effect must be rejected");
    require(!error.empty(), "host setup failure must report reason");
    REQUIRE_EQ(stats.created, stats.destroyed, "failed effect must release component");
    REQUIRE_EQ(stats.initialized, stats.terminated, "initialized effect must terminate");
  }
  for (const auto failure :
       {Failure::activate, Failure::processing_activate, Failure::throw_activate}) {
    Stats stats;
    FixtureFactory factory(stats, failure);
    std::string error;
    auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
    REQUIRE_NE(host, nullptr, "activation failure must propagate");
    REQUIRE_TRUE(!host->activate(error), "activation failure must propagate");
    REQUIRE_EQ(stats.active, 0, "activation must roll back");
    REQUIRE_EQ(stats.processing, 0, "activation must roll back");
  }
  Stats stats;
  FixtureFactory factory(stats, Failure::none);
  std::string error;
  auto invalid = config();
  invalid.identity.id = "bad";
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "invalid scanned ID must be rejected");
  invalid = config();
  invalid.identity.id = "0102030405060708090A0B0C0D0E0F10";
  REQUIRE_NE(oscillate::plugins::detail::create_effect_host(factory, invalid, error), nullptr,
             "uppercase hex class IDs must be accepted");
  invalid = config();
  invalid.identity.id[0] = 'g';
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "nonhex class ID must be rejected");
  invalid = config();
  invalid.max_block_frames = 0;
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "zero maximum block must be rejected");
  invalid = config();
  invalid.max_parameter_events = 0;
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "zero event capacity must be rejected");
  invalid.max_parameter_events = oscillate::plugins::max_vst3_parameter_events + 1;
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "unbounded event capacity must be rejected");
  invalid.max_parameter_events = oscillate::plugins::max_vst3_parameter_events;
  REQUIRE_NE(oscillate::plugins::detail::create_effect_host(factory, invalid, error), nullptr,
             "the documented maximum event capacity must be accepted");
  invalid = config();
  invalid.sample_rate = std::numeric_limits<double>::quiet_NaN();
  require(!oscillate::plugins::detail::create_effect_host(factory, invalid, error),
          "nonfinite sample rate must be rejected");
  auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
  REQUIRE_NE(host, nullptr, "valid host must open");
  require(!host->restore_component_state(std::vector<std::uint8_t>(1025), error),
          "oversized state must be rejected");
  std::array<std::uint8_t, 1> bad_state{0};
  require(!host->restore_component_state(bad_state, error), "truncated state must fail");
  std::array<float, 129> in{}, out{};
  require(host->activate(error), "fixture activation must succeed");
  require(host->activate(error), "already active fixture must remain active");
  REQUIRE_EQ(host->process({in, in, out, out}),
             oscillate::plugins::Vst3ProcessStatus::invalid_block,
             "oversized block must be rejected");

  Stats process_stats;
  FixtureFactory failing_factory(process_stats, Failure::process);
  auto failing = oscillate::plugins::detail::create_effect_host(failing_factory, config(), error);
  REQUIRE_NE(failing, nullptr, "failing fixture must activate");
  REQUIRE_TRUE(failing->activate(error), "failing fixture must activate");
  std::array<float, 64> short_in{}, short_out_left{}, short_out_right{};
  short_out_left.fill(0.5F);
  short_out_right.fill(-0.5F);
  const auto silent = [&] {
    return std::ranges::all_of(short_out_left, [](float sample) { return sample == 0.0F; }) &&
           std::ranges::all_of(short_out_right, [](float sample) { return sample == 0.0F; });
  };
  REQUIRE_EQ(failing->process({short_in, short_in, short_out_left, short_out_right}),
             oscillate::plugins::Vst3ProcessStatus::plugin_error,
             "failed effect must silence the full contaminated output block");
  REQUIRE_TRUE(silent(), "failed effect must silence the full contaminated output block");
  short_out_left.fill(0.5F);
  short_out_right.fill(-0.5F);
  REQUIRE_EQ(failing->process({short_in, short_in, short_out_left, short_out_right}),
             oscillate::plugins::Vst3ProcessStatus::plugin_error,
             "faulted plugin must stay silent");
  REQUIRE_TRUE(silent(), "faulted plugin must stay silent");
  failing->suspend();
  require(!failing->save_component_state(error), "faulted processor state must not be trusted");

  Stats throw_process_stats;
  FixtureFactory throwing_factory(throw_process_stats, Failure::throw_process);
  auto throwing = oscillate::plugins::detail::create_effect_host(throwing_factory, config(), error);
  REQUIRE_NE(throwing, nullptr, "throwing fixture must activate");
  REQUIRE_TRUE(throwing->activate(error), "throwing fixture must activate");
  short_out_left.fill(0.5F);
  short_out_right.fill(-0.5F);
  REQUIRE_EQ(throwing->process({short_in, short_in, short_out_left, short_out_right}),
             oscillate::plugins::Vst3ProcessStatus::plugin_error,
             "plugin process exception must be contained and silenced");
  REQUIRE_TRUE(silent(), "plugin process exception must be contained and silenced");

  for (const auto failure :
       {Failure::save, Failure::restore, Failure::throw_save, Failure::throw_restore}) {
    Stats state_stats;
    FixtureFactory state_factory(state_stats, failure);
    auto state_host =
        oscillate::plugins::detail::create_effect_host(state_factory, config(), error);
    REQUIRE_NE(state_host, nullptr, "state fixture must instantiate");
    if (failure == Failure::save || failure == Failure::throw_save) {
      require(!state_host->save_component_state(error), "failing component state save must report");
    } else {
      std::array<std::uint8_t, 4> bytes{};
      require(!state_host->restore_component_state(bytes, error),
              "failing component state restore must report");
    }
  }
  auto tiny_state_config = config();
  tiny_state_config.max_state_bytes = 2;
  auto tiny_state_host =
      oscillate::plugins::detail::create_effect_host(factory, tiny_state_config, error);
  REQUIRE_NE(tiny_state_host, nullptr, "small state budget host must instantiate");
  require(!tiny_state_host->save_component_state(error), "oversized saved state must be rejected");

  Stats suspend_stats;
  FixtureFactory suspend_factory(suspend_stats, Failure::throw_suspend);
  auto suspend_host =
      oscillate::plugins::detail::create_effect_host(suspend_factory, config(), error);
  REQUIRE_NE(suspend_host, nullptr, "suspension fixture must activate");
  REQUIRE_TRUE(suspend_host->activate(error), "suspension fixture must activate");
  suspend_host->suspend();
  REQUIRE_EQ(suspend_stats.active, 0, "component must deactivate after processing-stop exception");
  require(!suspend_host->activate(error), "failed suspension must fault the host");

  Stats stream_stats;
  FixtureFactory stream_factory(stream_stats, Failure::stream_probe);
  auto stream_host =
      oscillate::plugins::detail::create_effect_host(stream_factory, config(), error);
  REQUIRE_NE(stream_host, nullptr, "stream probe fixture must instantiate");
  const auto stream_state = stream_host->save_component_state(error);
  REQUIRE_TRUE(stream_state, "bounded stream must support sparse seek and report its position");
  REQUIRE_EQ(stream_state->size(), 6,
             "bounded stream must support sparse seek and report its position");
  REQUIRE_EQ((*stream_state)[0], 0,
             "bounded stream must support sparse seek and report its position");
  REQUIRE_EQ((*stream_state)[1], 0,
             "bounded stream must support sparse seek and report its position");
}

void test_parameters() {
  Stats stats;
  stats.enable_controller = true;
  FixtureFactory factory(stats, Failure::none);
  auto parameter_config = config();
  parameter_config.max_parameter_events = 4;
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, parameter_config, error);
  REQUIRE_NE(host, nullptr, "parameter controller must instantiate");
  REQUIRE_EQ(host->parameters().size(), 3, "parameter metadata count must be preserved");
  REQUIRE_EQ(host->parameters()[0].id, 101, "gain metadata must be exposed");
  REQUIRE_EQ(host->parameters()[0].title, "\xF0\x9F\x8E\xB5 Gain", "gain metadata must be exposed");
  REQUIRE_EQ(host->parameters()[0].short_title, "\xE2\x82\xAC", "gain metadata must be exposed");
  REQUIRE_EQ(host->parameters()[0].units, "dB", "gain metadata must be exposed");
  REQUIRE_TRUE(host->parameters()[0].automatable, "gain metadata must be exposed");
  REQUIRE_EQ(host->parameters()[1].id, 202, "discrete metadata must be exposed");
  REQUIRE_EQ(host->parameters()[1].step_count, 2, "discrete metadata must be exposed");
  REQUIRE_EQ(host->parameters()[1].title, "\xC3\x98 Mode", "discrete metadata must be exposed");
  REQUIRE_EQ(host->parameters()[2].id, 303, "second automatable parameter must be exposed");
  REQUIRE_TRUE(host->parameters()[2].automatable, "second automatable parameter must be exposed");
  REQUIRE_EQ(host->parameter_precision(101),
             oscillate::plugins::Vst3ParameterPrecision::sample_offsets_forwarded,
             "parameter precision must be explicit");
  REQUIRE_EQ(host->parameter_precision(202),
             oscillate::plugins::Vst3ParameterPrecision::control_only,
             "parameter precision must be explicit");
  REQUIRE_EQ(host->parameter_precision(999),
             oscillate::plugins::Vst3ParameterPrecision::unavailable,
             "parameter precision must be explicit");
  require(host->activate(error), "parameter fixture activation must succeed");
  std::array<float, 64> input{}, output_left{}, output_right{};
  input.fill(1.0F);
  oscillate::plugins::Vst3StereoBlock block{input, input, output_left, output_right};
  const std::array<oscillate::plugins::Vst3ParameterEvent, 3> changes{
      {{101, 0, 0.25}, {101, 63, 0.75}, {303, 10, 0.5}}};
  process_allocations = 0;
  counting_process_allocations = true;
  const auto status = host->process(block, changes);
  counting_process_allocations = false;
  REQUIRE_EQ(status, oscillate::plugins::Vst3ProcessStatus::ok,
             "parameter callback must process without allocations");
  REQUIRE_EQ(process_allocations, 0, "parameter callback must process without allocations");
  REQUIRE_TRUE(stats.queue_probe_passed,
               "bounded queue and metadata handler interfaces must behave correctly");
  REQUIRE_TRUE(stats.handler_probe_passed,
               "bounded queue and metadata handler interfaces must behave correctly");
  REQUIRE_EQ(output_left[0], 1.0F, "sample offset processing oracle failed");
  REQUIRE_EQ(output_left[62], 1.0F, "sample offset processing oracle failed");
  REQUIRE_EQ(output_left[63], 3.0F, "sample offset processing oracle failed");
  REQUIRE_EQ(output_right[0], 1.0F, "sample offset processing oracle failed");
  REQUIRE_EQ(output_right[63], 3.0F, "sample offset processing oracle failed");
  REQUIRE_EQ(stats.point_count, 3, "VST3 input queue order or values incorrect");
  REQUIRE_EQ(stats.points[0].offset, 0, "VST3 input queue order or values incorrect");
  REQUIRE_EQ(stats.points[1].offset, 63, "VST3 input queue order or values incorrect");
  REQUIRE_EQ(stats.points[1].value, 0.75, "VST3 input queue order or values incorrect");
  REQUIRE_EQ(stats.points[2].id, 303, "VST3 input queue order or values incorrect");
  REQUIRE_EQ(stats.points[2].offset, 10, "VST3 input queue order or values incorrect");
  const auto processed = stats.processed_blocks;
  using Event = oscillate::plugins::Vst3ParameterEvent;
  using Status = oscillate::plugins::Vst3ProcessStatus;
  const std::array<Event, 1> at_end{{{101, 64, 0.5}}};
  const std::array<Event, 1> nan{{{101, 0, std::numeric_limits<double>::quiet_NaN()}}};
  const std::array<Event, 1> below_range{{{101, 0, -0.01}}};
  const std::array<Event, 1> above_range{{{101, 0, 1.01}}};
  const std::array<Event, 1> unknown{{{999, 0, 0.5}}};
  const std::array<Event, 1> control_only{{{202, 0, 0.5}}};
  const std::array<Event, 2> unordered{{{101, 20, 0.5}, {101, 10, 0.25}}};
  const std::array<Event, 2> duplicate{{{101, 20, 0.5}, {101, 20, 0.25}}};
  const std::array<Event, 2> reversed_ids{{{303, 0, 0.5}, {101, 1, 0.25}}};
  const std::array<Event, 5> overflowing{
      {{101, 0, 0.1}, {101, 1, 0.2}, {101, 2, 0.3}, {101, 3, 0.4}, {101, 4, 0.5}}};
  REQUIRE_EQ(host->process(block, at_end), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, nan), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, below_range), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, above_range), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, unordered), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, duplicate), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process(block, reversed_ids), Status::invalid_parameter_events,
             "malformed parameter events must be rejected before DSP");
  REQUIRE_EQ(host->process({{}, {}, {}, {}}, changes), Status::invalid_block,
             "empty audio blocks must be rejected before event delivery");
  REQUIRE_EQ(host->process(block, unknown), Status::parameter_unavailable,
             "unknown and control-only parameters must report unavailable processing");
  REQUIRE_EQ(host->process(block, control_only), Status::parameter_unavailable,
             "unknown and control-only parameters must report unavailable processing");
  REQUIRE_EQ(host->process(block, overflowing), Status::parameter_capacity_exceeded,
             "event capacity must be enforced");
  REQUIRE_EQ(stats.processed_blocks, processed,
             "rejected event blocks must never reach the processor");
  host->suspend();
  auto state = host->save_component_state(error);
  if (!state)
    throw std::runtime_error("parameter fixture state missing");
  const float restored_gain = 0.75F;
  std::memcpy(state.value().data(), &restored_gain, sizeof(restored_gain));
  require(host->restore_component_state(state.value(), error),
          "component and controller state must restore together");
  REQUIRE_EQ(host->controller_state_sync(),
             oscillate::plugins::Vst3ControllerStateSync::synchronized,
             "successful controller restore must be reported");
  REQUIRE_EQ(stats.controller_synced_gain, restored_gain,
             "controller must receive restored component state");
  REQUIRE_TRUE(host->activate(error), "restored state must precede resumed processing");
  REQUIRE_EQ(host->process(block), Status::ok, "restored state must precede resumed processing");
  REQUIRE_EQ(output_left[0], restored_gain, "restored state must precede resumed processing");
  host.reset();
  REQUIRE_EQ(stats.controller_created, stats.controller_destroyed,
             "controller lifetime must be balanced");
  REQUIRE_EQ(stats.controller_initialized, stats.controller_terminated,
             "controller lifetime must be balanced");

  for (const auto failure :
       {Failure::controller_create, Failure::controller_initialize, Failure::controller_handler,
        Failure::parameter_count, Failure::parameter_duplicate, Failure::parameter_bad_title,
        Failure::parameter_low_surrogate, Failure::parameter_unterminated_title,
        Failure::parameter_control_title, Failure::parameter_bad_default,
        Failure::parameter_bad_step, Failure::parameter_bad_flags, Failure::parameter_query}) {
    Stats failed_stats;
    failed_stats.enable_controller = true;
    FixtureFactory failed_factory(failed_stats, failure);
    require(!oscillate::plugins::detail::create_effect_host(failed_factory, config(), error),
            "invalid controller metadata must fail construction");
    REQUIRE_TRUE(!error.empty(), "failed controller construction must clean up all instances");
    REQUIRE_EQ(failed_stats.created, failed_stats.destroyed,
               "failed controller construction must clean up all instances");
    REQUIRE_EQ(failed_stats.controller_created, failed_stats.controller_destroyed,
               "failed controller construction must clean up all instances");
    REQUIRE_EQ(failed_stats.controller_initialized, failed_stats.controller_terminated,
               "failed controller construction must clean up all instances");
  }
  Stats restore_stats;
  restore_stats.enable_controller = true;
  FixtureFactory restore_factory(restore_stats, Failure::controller_restore);
  auto restore_host =
      oscillate::plugins::detail::create_effect_host(restore_factory, config(), error);
  REQUIRE_NE(restore_host, nullptr, "restore failure fixture must construct");
  const std::array<std::uint8_t, sizeof(float)> bytes{};
  require(!restore_host->restore_component_state(bytes, error),
          "controller state failure must be explicit");
  require(!restore_host->activate(error), "partial restore must fault the runtime");

  Stats optional_stats;
  optional_stats.enable_controller = true;
  FixtureFactory optional_factory(optional_stats, Failure::controller_not_implemented);
  auto optional_host =
      oscillate::plugins::detail::create_effect_host(optional_factory, config(), error);
  REQUIRE_NE(optional_host, nullptr,
             "optional controller synchronization must not reject valid DSP state");
  REQUIRE_TRUE(optional_host->restore_component_state(bytes, error),
               "optional controller synchronization must not reject valid DSP state");
  REQUIRE_EQ(optional_host->controller_state_sync(),
             oscillate::plugins::Vst3ControllerStateSync::not_implemented,
             "optional controller synchronization must not reject valid DSP state");
  REQUIRE_TRUE(optional_host->activate(error),
               "optional controller synchronization must not reject valid DSP state");
}

void test_independent_controller_state() {
  using oscillate::plugins::Vst3ControllerStateSync;
  using oscillate::plugins::Vst3StatePair;
  std::string error;
  Stats source_stats;
  source_stats.enable_controller = true;
  source_stats.controller_private_supported = true;
  FixtureFactory source_factory(source_stats, Failure::controller_not_implemented);
  auto source = oscillate::plugins::detail::create_effect_host(source_factory, config(), error);
  REQUIRE_NE(source, nullptr, "split-state source must construct");
  const auto pair = source->save_state_pair(error);
  REQUIRE_TRUE(pair, "component and private controller bytes must capture independently");
  REQUIRE_EQ(pair->component_state.size(), sizeof(float),
             "component and private controller bytes must capture independently");
  REQUIRE_TRUE(pair->controller_state,
               "component and private controller bytes must capture independently");
  REQUIRE_EQ(*pair->controller_state, std::vector<std::uint8_t>{0x6a},
             "component and private controller bytes must capture independently");
  REQUIRE_TRUE(source->activate(error), "active pair capture must be rejected");
  REQUIRE_TRUE(!source->save_state_pair(error), "active pair capture must be rejected");
  source->suspend();

  Stats target_stats;
  target_stats.enable_controller = true;
  target_stats.controller_private_supported = true;
  FixtureFactory target_factory(target_stats, Failure::controller_not_implemented);
  auto target = oscillate::plugins::detail::create_effect_host(target_factory, config(), error);
  REQUIRE_NE(target, nullptr,
             "private controller state must restore after optional component synchronization");
  REQUIRE_TRUE(target->restore_state_pair(*pair, error),
               "private controller state must restore after optional component synchronization");
  REQUIRE_EQ(target->controller_state_sync(), Vst3ControllerStateSync::not_implemented,
             "private controller state must restore after optional component synchronization");
  REQUIRE_EQ(target_stats.controller_private_restore_calls, 1,
             "private controller state must restore after optional component synchronization");
  REQUIRE_EQ(target_stats.controller_private_restored, 0x6a,
             "private controller state must restore after optional component synchronization");
  REQUIRE_TRUE(target->activate(error),
               "private controller state must restore after optional component synchronization");

  Stats absent_stats;
  absent_stats.enable_controller = true;
  FixtureFactory absent_factory(absent_stats, Failure::none);
  auto absent = oscillate::plugins::detail::create_effect_host(absent_factory, config(), error);
  const auto absent_pair = absent->save_state_pair(error);
  REQUIRE_TRUE(absent_pair, "kNotImplemented controller getState must mean absent private state");
  REQUIRE_TRUE(!absent_pair->controller_state,
               "kNotImplemented controller getState must mean absent private state");

  Stats empty_stats;
  empty_stats.enable_controller = true;
  empty_stats.controller_private_supported = true;
  empty_stats.controller_private_empty = true;
  FixtureFactory empty_factory(empty_stats, Failure::none);
  auto empty = oscillate::plugins::detail::create_effect_host(empty_factory, config(), error);
  const auto empty_pair = empty->save_state_pair(error);
  REQUIRE_TRUE(empty_pair,
               "present-empty private controller state must round-trip and call setState");
  REQUIRE_TRUE(empty_pair->controller_state,
               "present-empty private controller state must round-trip and call setState");
  REQUIRE_TRUE(empty_pair->controller_state->empty(),
               "present-empty private controller state must round-trip and call setState");
  REQUIRE_TRUE(empty->restore_state_pair(*empty_pair, error),
               "present-empty private controller state must round-trip and call setState");
  REQUIRE_EQ(empty_stats.controller_private_restore_calls, 1,
             "present-empty private controller state must round-trip and call setState");

  Stats failed_save_stats;
  failed_save_stats.enable_controller = true;
  failed_save_stats.controller_private_save_fails = true;
  FixtureFactory failed_save_factory(failed_save_stats, Failure::none);
  auto failed_save =
      oscillate::plugins::detail::create_effect_host(failed_save_factory, config(), error);
  REQUIRE_TRUE(failed_save, "controller getState failure must fail the complete pair capture");
  REQUIRE_TRUE(!failed_save->save_state_pair(error),
               "controller getState failure must fail the complete pair capture");
  REQUIRE_TRUE(!error.empty(), "controller getState failure must fail the complete pair capture");

  Stats oversized_stats;
  oversized_stats.enable_controller = true;
  oversized_stats.controller_private_supported = true;
  oversized_stats.controller_private_oversized = true;
  FixtureFactory oversized_factory(oversized_stats, Failure::none);
  auto oversized =
      oscillate::plugins::detail::create_effect_host(oversized_factory, config(), error);
  REQUIRE_TRUE(oversized, "oversized controller state must fail the complete pair capture");
  REQUIRE_TRUE(!oversized->save_state_pair(error),
               "oversized controller state must fail the complete pair capture");

  Stats refused_stats;
  refused_stats.enable_controller = true;
  refused_stats.controller_private_supported = true;
  refused_stats.controller_private_restore_fails = true;
  FixtureFactory refused_factory(refused_stats, Failure::none);
  auto refused = oscillate::plugins::detail::create_effect_host(refused_factory, config(), error);
  REQUIRE_TRUE(refused, "controller setState refusal must fault the host before activation");
  REQUIRE_TRUE(!refused->restore_state_pair(*pair, error),
               "controller setState refusal must fault the host before activation");
  REQUIRE_TRUE(!refused->activate(error),
               "controller setState refusal must fault the host before activation");

  Stats controller_only_stats;
  controller_only_stats.enable_controller = true;
  controller_only_stats.controller_private_supported = true;
  FixtureFactory controller_only_factory(controller_only_stats, Failure::none);
  auto controller_only =
      oscillate::plugins::detail::create_effect_host(controller_only_factory, config(), error);
  REQUIRE_TRUE(controller_only,
               "controller-only restore must not feed an empty component stream to setState");
  REQUIRE_TRUE(controller_only->restore_state_pair(
                   Vst3StatePair{.component_state = {}, .controller_state = {{0x7b}}}, error),
               "controller-only restore must not feed an empty component stream to setState");
  REQUIRE_EQ(controller_only_stats.controller_private_restored, 0x7b,
             "controller-only restore must not feed an empty component stream to setState");
  REQUIRE_EQ(controller_only->controller_state_sync(), Vst3ControllerStateSync::not_attempted,
             "controller-only restore must not feed an empty component stream to setState");

  Stats missing_stats;
  FixtureFactory missing_factory(missing_stats, Failure::none);
  auto missing = oscillate::plugins::detail::create_effect_host(missing_factory, config(), error);
  REQUIRE_TRUE(missing, "saved controller state without a controller must fail before publication");
  REQUIRE_TRUE(!missing->restore_state_pair(*pair, error),
               "saved controller state without a controller must fail before publication");
  REQUIRE_TRUE(!missing->activate(error),
               "saved controller state without a controller must fail before publication");
}

void test_connection_points() {
  Stats stats;
  stats.enable_controller = true;
  stats.enable_connection = true;
  FixtureFactory factory(stats, Failure::none);
  std::string error;
  auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
  REQUIRE_NE(host, nullptr, "distinct connection points must connect component before controller");
  REQUIRE_EQ(stats.connection_event_count, 2,
             "distinct connection points must connect component before controller");
  REQUIRE_EQ(stats.connection_events[0], 1,
             "distinct connection points must connect component before controller");
  REQUIRE_EQ(stats.connection_events[1], 2,
             "distinct connection points must connect component before controller");
  const auto state = host->save_component_state(error);
  REQUIRE_TRUE(state, "connected peers did not exchange bidirectional VST3 messages");
  REQUIRE_TRUE(host->restore_component_state(*state, error),
               "connected peers did not exchange bidirectional VST3 messages");
  REQUIRE_EQ(stats.messages_to_controller, 1,
             "connected peers did not exchange bidirectional VST3 messages");
  REQUIRE_EQ(stats.messages_to_component, 1,
             "connected peers did not exchange bidirectional VST3 messages");
  require(host->activate(error), "connected fixture failed to activate");
  std::array<float, 64> input{}, output{};
  input.fill(0.25F);
  process_allocations = 0;
  counting_process_allocations = true;
  const auto status = host->process({input, input, output, output});
  counting_process_allocations = false;
  REQUIRE_EQ(status, oscillate::plugins::Vst3ProcessStatus::ok,
             "connection lifecycle performed callback work");
  REQUIRE_EQ(process_allocations, 0, "connection lifecycle performed callback work");
  REQUIRE_EQ(stats.connection_event_count, 2, "connection lifecycle performed callback work");
  host.reset();
  REQUIRE_EQ(stats.connection_event_count, 4,
             "teardown must disconnect controller before component and release both");
  REQUIRE_EQ(stats.connection_events[2], 3,
             "teardown must disconnect controller before component and release both");
  REQUIRE_EQ(stats.connection_events[3], 4,
             "teardown must disconnect controller before component and release both");
  REQUIRE_EQ(stats.created, stats.destroyed,
             "teardown must disconnect controller before component and release both");
  REQUIRE_EQ(stats.controller_created, stats.controller_destroyed,
             "teardown must disconnect controller before component and release both");

  for (const auto failure : {Failure::component_connect, Failure::controller_connect}) {
    Stats rejected;
    rejected.enable_controller = true;
    rejected.enable_connection = true;
    FixtureFactory rejected_factory(rejected, failure);
    error.clear();
    auto failed = oscillate::plugins::detail::create_effect_host(rejected_factory, config(), error);
    REQUIRE_TRUE(!failed, "connection rejection leaked a component or controller");
    REQUIRE_TRUE(!error.empty(), "connection rejection leaked a component or controller");
    REQUIRE_EQ(rejected.created, rejected.destroyed,
               "connection rejection leaked a component or controller");
    REQUIRE_EQ(rejected.controller_created, rejected.controller_destroyed,
               "connection rejection leaked a component or controller");
    REQUIRE_EQ(rejected.initialized, rejected.terminated,
               "connection rejection leaked a component or controller");
    REQUIRE_EQ(rejected.controller_initialized, rejected.controller_terminated,
               "connection rejection leaked a component or controller");
    if (failure == Failure::component_connect)
      require(rejected.connection_event_count == 2 && rejected.connection_events[0] == 1 &&
                  rejected.connection_events[1] == 4,
              "component rejection did not attempt cleanup");
    else
      require(rejected.connection_event_count == 4 && rejected.connection_events[0] == 1 &&
                  rejected.connection_events[1] == 2 && rejected.connection_events[2] == 3 &&
                  rejected.connection_events[3] == 4,
              "controller rejection did not disconnect in reverse order");
  }
  for (const auto failure : {Failure::component_no_connection, Failure::controller_no_connection}) {
    Stats optional;
    optional.enable_controller = true;
    optional.enable_connection = true;
    FixtureFactory optional_factory(optional, failure);
    error.clear();
    auto unpaired =
        oscillate::plugins::detail::create_effect_host(optional_factory, config(), error);
    REQUIRE_NE(unpaired, nullptr, "one absent endpoint must not create a half-connection");
    REQUIRE_EQ(optional.connection_event_count, 0,
               "one absent endpoint must not create a half-connection");
    const std::array<oscillate::plugins::Vst3ParameterEvent, 1> saved{{{101, 0, 0.25}}};
    REQUIRE_TRUE(!unpaired->stage_static_parameter_values(saved, error),
                 "unconnected separate controller accepted static value restoration");
    REQUIRE_TRUE(!unpaired->static_parameter_values_ready(),
                 "unconnected separate controller accepted static value restoration");
    unpaired.reset();
    REQUIRE_EQ(optional.created, optional.destroyed,
               "unpaired endpoint reference was not released");
    REQUIRE_EQ(optional.controller_created, optional.controller_destroyed,
               "unpaired endpoint reference was not released");
  }
}

void test_staged_static_values() {
  using Event = oscillate::plugins::Vst3ParameterEvent;
  using Status = oscillate::plugins::Vst3ProcessStatus;
  constexpr std::array<Event, 1> saved{{{101, 0, 0.25}}};
  for (const std::size_t frames : {64UL, 128UL}) {
    Stats stats;
    stats.enable_controller = true;
    stats.enable_connection = true;
    FixtureFactory factory(stats, Failure::none);
    std::string error;
    auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
    REQUIRE_NE(host, nullptr, "staged fixture failed to construct");
    REQUIRE_TRUE(host->stage_static_parameter_values(saved, error),
                 "UI-stage controller value was not applied and gated");
    REQUIRE_EQ(stats.controller_synced_gain, 1.0F,
               "UI-stage controller value was not applied and gated");
    REQUIRE_TRUE(!host->static_parameter_values_ready(),
                 "UI-stage controller value was not applied and gated");
    std::array<float, 128> input{}, output_left{}, output_right{};
    input.fill(0.5F);
    oscillate::plugins::Vst3StereoBlock block{
        std::span(input).first(frames), std::span(input).first(frames),
        std::span(output_left).first(frames), std::span(output_right).first(frames)};
    bool active = false;
    Status before_flush = Status::ok;
    bool flushed = false;
    Status processed = Status::inactive;
    std::thread processing_thread([&] {
      active = host->activate(error);
      if (active) {
        before_flush = host->process(block);
        flushed = host->flush_static_parameter_values(error);
      }
      process_allocations = 0;
      counting_process_allocations = true;
      if (flushed)
        processed = host->process(block);
      counting_process_allocations = false;
    });
    processing_thread.join();
    REQUIRE_TRUE(active, "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(before_flush, Status::inactive,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(stats.processed_blocks, 1,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_TRUE(flushed, "processor flush did not precede allocation-free audible processing");
    REQUIRE_TRUE(host->static_parameter_values_ready(),
                 "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(stats.static_flushes, 1,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(processed, Status::ok,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(output_left[0], 0.5F,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(output_right[frames - 1], 0.5F,
               "processor flush did not precede allocation-free audible processing");
    REQUIRE_EQ(process_allocations, 0,
               "processor flush did not precede allocation-free audible processing");
  }
  for (const Event bad : {Event{999, 0, 0.25}, Event{101, 1, 0.25},
                          Event{101, 0, std::numeric_limits<double>::quiet_NaN()}}) {
    Stats stats;
    stats.enable_controller = true;
    stats.enable_connection = true;
    FixtureFactory factory(stats, Failure::none);
    std::string error;
    auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
    REQUIRE_NE(host, nullptr, "invalid-batch fixture failed to construct");
    REQUIRE_TRUE(!host->stage_static_parameter_values(std::span(&bad, 1), error),
                 "rejected static batch must fault before controller mutation or activation");
    REQUIRE_TRUE(!host->static_parameter_values_ready(),
                 "rejected static batch must fault before controller mutation or activation");
    REQUIRE_EQ(stats.controller_synced_gain, 0.0F,
               "rejected static batch must fault before controller mutation or activation");
    REQUIRE_TRUE(!host->activate(error),
                 "rejected static batch must fault before controller mutation or activation");
  }
  for (const auto failure : {Failure::static_controller_set, Failure::static_flush}) {
    Stats stats;
    stats.enable_controller = true;
    stats.enable_connection = true;
    FixtureFactory factory(stats, failure);
    std::string error;
    auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
    REQUIRE_NE(host, nullptr, "failing static fixture failed to construct");
    const bool staged = host->stage_static_parameter_values(saved, error);
    if (failure == Failure::static_controller_set) {
      REQUIRE_TRUE(!staged, "controller refusal left a publishable processor");
      REQUIRE_TRUE(!host->static_parameter_values_ready(),
                   "controller refusal left a publishable processor");
      REQUIRE_TRUE(!host->activate(error), "controller refusal left a publishable processor");
    } else {
      bool active = false;
      bool flushed = true;
      std::thread processing_thread([&] {
        active = host->activate(error);
        if (active)
          flushed = host->flush_static_parameter_values(error);
      });
      processing_thread.join();
      REQUIRE_TRUE(staged, "zero-sample processor refusal left a publishable processor");
      REQUIRE_TRUE(active, "zero-sample processor refusal left a publishable processor");
      REQUIRE_TRUE(!flushed, "zero-sample processor refusal left a publishable processor");
      REQUIRE_TRUE(!host->static_parameter_values_ready(),
                   "zero-sample processor refusal left a publishable processor");
    }
    require(!error.empty(), "failed static restore lacks diagnostic");
  }
}

void benchmark() {
  for (const std::size_t frames : {64UL, 128UL}) {
    Stats stats;
    FixtureFactory factory(stats, Failure::none);
    std::string error;
    auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
    REQUIRE_NE(host, nullptr, "benchmark fixture must activate");
    REQUIRE_TRUE(host->activate(error), "benchmark fixture must activate");
    std::array<float, 128> input{}, output{};
    input[0] = 0.25F;
    oscillate::plugins::Vst3StereoBlock block{
        std::span<float>(input.data(), frames), std::span<float>(input.data(), frames),
        std::span<float>(output.data(), frames), std::span<float>(output.data(), frames)};
    for (int warmup = 0; warmup < 2000; ++warmup)
      REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::ok,
                 "benchmark warmup process failed");
    process_allocations = 0;
    counting_process_allocations = true;
    const auto start = std::chrono::steady_clock::now();
    constexpr int iterations = 100000;
    for (int index = 0; index < iterations; ++index)
      REQUIRE_EQ(host->process(block), oscillate::plugins::Vst3ProcessStatus::ok,
                 "benchmark process failed");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    counting_process_allocations = false;
    REQUIRE_EQ(process_allocations, 0, "benchmark host callback allocated");
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::cout << frames << " frames: " << (nanos / iterations)
              << " ns/block; allocations: " << process_allocations << '\n';

    Stats parameter_stats;
    parameter_stats.enable_controller = true;
    FixtureFactory parameter_factory(parameter_stats, Failure::none);
    auto parameter_host =
        oscillate::plugins::detail::create_effect_host(parameter_factory, config(), error);
    REQUIRE_NE(parameter_host, nullptr, "parameter benchmark fixture must activate");
    REQUIRE_TRUE(parameter_host->activate(error), "parameter benchmark fixture must activate");
    const std::array<oscillate::plugins::Vst3ParameterEvent, 2> changes{
        {{101, 0, 0.25}, {101, static_cast<std::uint32_t>(frames - 1), 0.75}}};
    for (int warmup = 0; warmup < 2000; ++warmup)
      REQUIRE_EQ(parameter_host->process(block, changes), oscillate::plugins::Vst3ProcessStatus::ok,
                 "parameter benchmark warmup failed");
    process_allocations = 0;
    counting_process_allocations = true;
    const auto parameter_start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index)
      REQUIRE_EQ(parameter_host->process(block, changes), oscillate::plugins::Vst3ProcessStatus::ok,
                 "parameter benchmark process failed");
    const auto parameter_elapsed = std::chrono::steady_clock::now() - parameter_start;
    counting_process_allocations = false;
    REQUIRE_EQ(process_allocations, 0, "parameter benchmark callback allocated");
    const auto parameter_nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(parameter_elapsed).count();
    std::cout << frames << " frames with two parameter points: " << (parameter_nanos / iterations)
              << " ns/block; allocations: " << process_allocations << '\n';
  }
}

} // namespace

void test_editor_probe_fallback() {
  using oscillate::plugins::Vst3EditorPlatform;
  using oscillate::plugins::Vst3EditorProbe;
  std::string error;
  Stats no_controller;
  FixtureFactory bare_factory(no_controller, Failure::none);
  auto bare = oscillate::plugins::detail::create_effect_host(bare_factory, config(), error);
  REQUIRE_TRUE(bare, "missing controller needs generic editor");
  REQUIRE_EQ(bare->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::no_controller,
             "missing controller needs generic editor");

  Stats stats;
  stats.enable_controller = true;
  FixtureFactory factory(stats, Failure::none);
  auto host = oscillate::plugins::detail::create_effect_host(factory, config(), error);
  REQUIRE_NE(host, nullptr, "editor fixture host created");
  REQUIRE_NE(stats.editor_handler, nullptr, "editor fixture handler is unavailable");
  stats.declared_latency = 12;
  auto changed_latency = host->check_prepared_latency();
  REQUIRE_EQ(changed_latency.status, oscillate::plugins::Vst3LatencyCheckStatus::changed,
             "a fresh latency observation must not silently replace prepared PDC");
  REQUIRE_EQ(changed_latency.prepared_samples, 7,
             "a fresh latency observation must not silently replace prepared PDC");
  REQUIRE_EQ(changed_latency.observed_samples, 12,
             "a fresh latency observation must not silently replace prepared PDC");
  REQUIRE_EQ(host->latency_samples(), 7,
             "a fresh latency observation must not silently replace prepared PDC");
  stats.declared_latency = 7;
  stats.throw_latency_query = true;
  REQUIRE_EQ(host->check_prepared_latency().status,
             oscillate::plugins::Vst3LatencyCheckStatus::unavailable,
             "a failing processor latency query must fail closed");
  stats.throw_latency_query = false;
  REQUIRE_EQ(stats.editor_handler->restartComponent(Steinberg::Vst::kParamValuesChanged),
             Steinberg::kResultFalse,
             "an unrelated restart must not invalidate a stable latency receipt");
  REQUIRE_EQ(host->take_restart_requests(),
             static_cast<std::uint32_t>(Steinberg::Vst::kParamValuesChanged),
             "an unrelated restart must not invalidate a stable latency receipt");
  REQUIRE_EQ(host->check_prepared_latency().status,
             oscillate::plugins::Vst3LatencyCheckStatus::stable,
             "an unrelated restart must not invalidate a stable latency receipt");
  REQUIRE_EQ(host->take_restart_requests(), 0,
             "restart requests must be observed once without claiming they were fulfilled");
  REQUIRE_EQ(stats.editor_handler->restartComponent(Steinberg::Vst::kLatencyChanged |
                                                    Steinberg::Vst::kParamValuesChanged),
             Steinberg::kResultFalse,
             "restart requests must be observed once without claiming they were fulfilled");
  REQUIRE_EQ(stats.editor_handler->restartComponent(Steinberg::Vst::kIoChanged),
             Steinberg::kResultFalse,
             "restart requests must be observed once without claiming they were fulfilled");
  REQUIRE_EQ(host->take_restart_requests(),
             (Steinberg::Vst::kLatencyChanged | Steinberg::Vst::kParamValuesChanged |
              Steinberg::Vst::kIoChanged),
             "restart requests must be observed once without claiming they were fulfilled");
  REQUIRE_EQ(host->take_restart_requests(), 0,
             "restart requests must be observed once without claiming they were fulfilled");
  REQUIRE_EQ(host->latency_samples(), 7,
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(stats.editor_handler->restartComponent(Steinberg::Vst::kLatencyChanged),
             Steinberg::kResultFalse,
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(host->latency_samples(), 7,
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(host->take_restart_requests(),
             static_cast<std::uint32_t>(Steinberg::Vst::kLatencyChanged),
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(host->take_restart_requests(), 0,
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(host->check_prepared_latency().status,
             oscillate::plugins::Vst3LatencyCheckStatus::restart_requested,
             "a latency restart request must remain an unfulfilled signal, not dynamic PDC");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.5), Steinberg::kResultFalse,
             "attach-time callbacks must not be recorded before capture begins");
  host->begin_editor_parameter_capture();
  REQUIRE_EQ(stats.editor_handler->beginEdit(101), Steinberg::kResultOk,
             "native editor callback values were rejected");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.25), Steinberg::kResultOk,
             "native editor callback values were rejected");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.75), Steinberg::kResultOk,
             "native editor callback values were rejected");
  auto previews = host->take_editor_parameter_previews();
  REQUIRE_TRUE(previews,
               "active native drag did not expose its latest preview without ending the gesture");
  REQUIRE_EQ(previews->size(), 1,
             "active native drag did not expose its latest preview without ending the gesture");
  REQUIRE_EQ(previews->front().id, 101,
             "active native drag did not expose its latest preview without ending the gesture");
  REQUIRE_EQ(previews->front().normalized_value, 0.75,
             "active native drag did not expose its latest preview without ending the gesture");
  REQUIRE_TRUE(host->take_editor_parameter_previews()->empty(),
               "active native drag did not expose its latest preview without ending the gesture");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.6), Steinberg::kResultOk,
             "completed native gesture left a stale preview");
  REQUIRE_EQ(stats.editor_handler->endEdit(101), Steinberg::kResultOk,
             "completed native gesture left a stale preview");
  REQUIRE_TRUE(host->take_editor_parameter_previews()->empty(),
               "completed native gesture left a stale preview");
  auto completed = host->take_completed_editor_gestures();
  REQUIRE_TRUE(completed, "native drag did not yield one final-value gesture at endEdit");
  REQUIRE_EQ(completed->size(), 1, "native drag did not yield one final-value gesture at endEdit");
  REQUIRE_EQ(completed->front().id, 101,
             "native drag did not yield one final-value gesture at endEdit");
  REQUIRE_EQ(completed->front().normalized_value, 0.6,
             "native drag did not yield one final-value gesture at endEdit");
  REQUIRE_TRUE(host->take_completed_editor_gestures()->empty(),
               "native drag did not yield one final-value gesture at endEdit");
  REQUIRE_EQ(stats.editor_handler->beginEdit(101), Steinberg::kResultOk,
             "second native gesture was rejected");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.6), Steinberg::kResultOk,
             "second native gesture was rejected");
  REQUIRE_EQ(stats.editor_handler->endEdit(101), Steinberg::kResultOk,
             "second native gesture was rejected");
  completed = host->take_completed_editor_gestures();
  REQUIRE_TRUE(completed, "second native drag did not produce its own undo-sized gesture");
  REQUIRE_EQ(completed->size(), 1, "second native drag did not produce its own undo-sized gesture");
  REQUIRE_EQ(completed->front().normalized_value, 0.6,
             "second native drag did not produce its own undo-sized gesture");
  auto captured = host->take_editor_parameter_edits();
  REQUIRE_TRUE(captured, "bounded native editor callback capture did not end cleanly");
  REQUIRE_EQ(captured->size(), 1, "bounded native editor callback capture did not end cleanly");
  REQUIRE_EQ(captured->front().id, 101,
             "bounded native editor callback capture did not end cleanly");
  REQUIRE_EQ(captured->front().normalized_value, 0.6,
             "bounded native editor callback capture did not end cleanly");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.5), Steinberg::kResultFalse,
             "bounded native editor callback capture did not end cleanly");
  host->begin_editor_parameter_capture();
  REQUIRE_EQ(stats.editor_handler->beginEdit(101), Steinberg::kResultOk,
             "native gesture became live before endEdit or disappeared after a drain");
  REQUIRE_EQ(stats.editor_handler->performEdit(101, 0.4), Steinberg::kResultOk,
             "native gesture became live before endEdit or disappeared after a drain");
  REQUIRE_TRUE(host->take_completed_editor_gestures()->empty(),
               "native gesture became live before endEdit or disappeared after a drain");
  REQUIRE_EQ(stats.editor_handler->endEdit(101), Steinberg::kResultOk,
             "native gesture became live before endEdit or disappeared after a drain");
  REQUIRE_EQ(host->take_completed_editor_gestures()->size(), 1,
             "native gesture became live before endEdit or disappeared after a drain");
  host->begin_editor_parameter_capture();
  for (std::uint32_t id = 0; id < 256; ++id)
    REQUIRE_EQ(stats.editor_handler->performEdit(id, 0.5), Steinberg::kResultOk,
               "bounded native editor callback rejected a permitted value");
  REQUIRE_EQ(stats.editor_handler->performEdit(256, 0.5), Steinberg::kResultFalse,
             "native editor callback overflow did not fail closed");
  REQUIRE_TRUE(!host->take_editor_parameter_edits(),
               "native editor callback overflow did not fail closed");
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::no_view,
             "missing view needs generic editor");
  stats.editor_view = true;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed),
             Vst3EditorProbe::unsupported_platform, "unsupported X11 view needs generic editor");
  stats.editor_x11 = true;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::x11_candidate,
             "X11 support is only an attachment candidate");
  const auto created = stats.editor_created;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::native_wayland),
             Vst3EditorProbe::wayland_bridge_unavailable,
             "native Wayland must fall back before creating a view");
  REQUIRE_EQ(stats.editor_created, created, "native Wayland must fall back before creating a view");
  stats.editor_throw_query = true;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::plugin_exception,
             "editor query exception must report fallback");
  stats.editor_throw_query = false;
  stats.editor_throw_create = true;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::plugin_exception,
             "editor creation exception must report fallback");
  REQUIRE_EQ(stats.editor_created, stats.editor_destroyed,
             "probe must release every view without attaching it");
  REQUIRE_EQ(stats.editor_attached, 0, "probe must release every view without attaching it");
  stats.editor_throw_create = false;
  REQUIRE_EQ(host->probe_editor(Vst3EditorPlatform::x11_xembed), Vst3EditorProbe::x11_candidate,
             "editor failure must not fault audio host");
  REQUIRE_EQ(stats.editor_created, stats.editor_destroyed,
             "successful probe must release its view");
  {
    Vst3EditorProbe editor_status{};
    auto editor = host->create_editor_view(&editor_status);
    REQUIRE_NE(editor, nullptr, "X11 candidate may create a retained editor view");
    REQUIRE_EQ(editor_status, Vst3EditorProbe::x11_candidate,
               "opened editor reports candidate status");
    REQUIRE_EQ(host->create_editor_view(&editor_status), nullptr,
               "host permits only one editor lease with reason");
    REQUIRE_EQ(editor_status, Vst3EditorProbe::host_unavailable,
               "host permits only one editor lease with reason");
    REQUIRE_EQ(stats.editor_created, stats.editor_destroyed + 1,
               "editor lease keeps view alive until closed");
  }
  REQUIRE_EQ(stats.editor_created, stats.editor_destroyed,
             "closing editor lease releases the view");
  REQUIRE_NE(host->create_editor_view(), nullptr, "editor lease can reopen after close");
  REQUIRE_EQ(stats.editor_created, stats.editor_destroyed,
             "temporary editor lease releases its view");
  std::array<float, 1> input_left{0.25F};
  std::array<float, 1> input_right{-0.5F};
  std::array<float, 1> output_left{};
  std::array<float, 1> output_right{};
  require(host->activate(error), "editor exception must leave audio host activatable");
  REQUIRE_EQ(host->process({input_left, input_right, output_left, output_right}),
             oscillate::plugins::Vst3ProcessStatus::ok,
             "editor exception must not silence audio processing");
  REQUIRE_EQ(output_left[0], 0.5F, "editor exception must not silence audio processing");
  REQUIRE_EQ(output_right[0], -1.0F, "editor exception must not silence audio processing");
  host->suspend();
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
      benchmark();
    } else {
      test_process_state_and_lifecycle();
      test_bounded_auxiliary_buses();
      test_routed_sidechain_bus();
      test_offline_processing_mode();
      test_failures();
      test_parameters();
      test_independent_controller_state();
      test_connection_points();
      test_staged_static_values();
      test_editor_probe_fallback();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
