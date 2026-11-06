#include "oscillate/application/control_transport_gate.hpp"

#include <cstddef>
#include <limits>
#include <mutex>
#include <utility>

namespace oscillate::application {

struct ControlTransportGate::State {
  std::mutex mutex;
  bool running = false;
  std::size_t stopped_leases = 0;
};

class ControlTransportGate::Lease final : public project::StoppedTransportLease {
public:
  explicit Lease(std::shared_ptr<State> state) : state_(std::move(state)) {}
  ~Lease() override {
    const std::lock_guard guard(state_->mutex);
    --state_->stopped_leases;
  }

private:
  std::shared_ptr<State> state_;
};

ControlTransportGate::ControlTransportGate() : state_(std::make_shared<State>()) {}
ControlTransportGate::~ControlTransportGate() = default;

foundation::Result<ControlTransportGate::PlaybackToken> ControlTransportGate::try_start() {
  const std::lock_guard guard(state_->mutex);
  if (state_->running || state_->stopped_leases != 0) {
    return foundation::Result<PlaybackToken>::failure(
        {foundation::ErrorCode::conflict, "Transport start requires no playback or stopped lease"});
  }
  state_->running = true;
  return foundation::Result<PlaybackToken>::success(PlaybackToken(state_));
}

foundation::Result<std::unique_ptr<project::StoppedTransportLease>>
ControlTransportGate::acquire_stopped() {
  const std::lock_guard guard(state_->mutex);
  if (state_->running) {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
        {foundation::ErrorCode::conflict, "Stopped operation requires joined playback"});
  }
  if (state_->stopped_leases == std::numeric_limits<std::size_t>::max()) {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::failure(
        {foundation::ErrorCode::resource_exhausted, "Stopped lease count exhausted"});
  }
  auto lease = std::make_unique<Lease>(state_);
  ++state_->stopped_leases;
  return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
      std::move(lease));
}

void ControlTransportGate::PlaybackToken::finish_after_join() noexcept {
  if (!state_) {
    return;
  }
  {
    const std::lock_guard guard(state_->mutex);
    state_->running = false;
  }
  state_.reset();
}

} // namespace oscillate::application
