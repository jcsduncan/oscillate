#pragma once

#include <memory>
#include <string>

#include <oscillate/foundation/error.hpp>

namespace oscillate::audio_io {

// Exclusive use of a sound card, negotiated with the sound server before the
// card's PCM is opened and given back after it is closed (ADR-0130).
// Control thread.
class DeviceReservation {
public:
  enum class Outcome : bool {
    reserved,  // this process owns the card's reservation until release()
    unmanaged, // no reservation service runs (no session bus): open directly
  };

  virtual ~DeviceReservation() = default;
  // A failure means the card is held and was not given up: do not open it.
  [[nodiscard]] virtual foundation::Result<Outcome> acquire(int card) = 0;
  // Gives back what acquire() took. Idempotent.
  virtual void release() noexcept = 0;
};

// The freedesktop device reservation protocol on the session bus, as JACK
// and PipeWire speak it: owns org.freedesktop.ReserveDevice1.Audio<card>,
// asking its current owner to give it up (RequestRelease) first. While it is
// held, a thread answers the owner-side calls and refuses requests to give
// the card back; release() drops the name, and the sound server takes the
// card again.
class DbusDeviceReservation final : public DeviceReservation {
public:
  // The priority it requests with: the highest, as a user's explicit choice.
  static constexpr int priority = 2'147'483'647;
  static constexpr int request_timeout_ms = 5'000;

  explicit DbusDeviceReservation(std::string application_name = "Oscillate");
  DbusDeviceReservation(const DbusDeviceReservation&) = delete;
  DbusDeviceReservation& operator=(const DbusDeviceReservation&) = delete;
  ~DbusDeviceReservation() override;

  [[nodiscard]] foundation::Result<Outcome> acquire(int card) override;
  void release() noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oscillate::audio_io
