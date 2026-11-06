#pragma once

#include "oscillate/project/model.hpp"

#include <memory>
#include <utility>

namespace oscillate::application {

// Control-side arbitration between playback and stopped-project operations.
// Neither this gate nor its tokens may be used from the audio callback.
class ControlTransportGate final : public project::TransportGate {
  struct State;

public:
  class PlaybackToken {
  public:
    PlaybackToken(const PlaybackToken&) = delete;
    PlaybackToken& operator=(const PlaybackToken&) = delete;
    PlaybackToken(PlaybackToken&&) noexcept = default;
    PlaybackToken& operator=(PlaybackToken&&) noexcept = delete;
    ~PlaybackToken() = default;

    // Caller must first stop/quiesce and join the device callback. The gate
    // deliberately remains running if an unfinished token is destroyed.
    void finish_after_join() noexcept;

  private:
    friend class ControlTransportGate;
    explicit PlaybackToken(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
  };

  ControlTransportGate();
  ControlTransportGate(const ControlTransportGate&) = delete;
  ControlTransportGate& operator=(const ControlTransportGate&) = delete;
  ~ControlTransportGate() override;

  // Called before device start; hold the token until stop/join completes.
  [[nodiscard]] foundation::Result<PlaybackToken> try_start();
  [[nodiscard]] foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
  acquire_stopped() override;

private:
  class Lease;
  std::shared_ptr<State> state_;
};

} // namespace oscillate::application
