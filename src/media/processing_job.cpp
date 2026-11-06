#include <oscillate/media/processing_job.hpp>

#include <limits>
#include <utility>

namespace oscillate::media {

foundation::Result<JobTicket> CompletionGate::issue(CacheKey key) {
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return foundation::Result<JobTicket>::failure(
        {foundation::ErrorCode::resource_exhausted, "processing generation exhausted"});
  }
  cancel();
  ++generation_;
  auto state = std::make_shared<std::atomic<JobTicket::State>>(JobTicket::State::active);
  current_ = JobTicket(key, generation_, std::move(state));
  return foundation::Result<JobTicket>::success(*current_);
}

void CompletionGate::cancel() noexcept {
  if (current_) {
    current_->state_->store(JobTicket::State::cancelled, std::memory_order_release);
    current_.reset();
  }
}

bool CompletionGate::claim_install(const JobTicket& ticket,
                                   const CacheKey& current_inputs) noexcept {
  if (!current_ || ticket.generation_ != generation_ || ticket.state_ != current_->state_ ||
      ticket.key_ != current_->key_ || ticket.key_ != current_inputs || ticket.cancelled()) {
    return false;
  }
  current_->state_->store(JobTicket::State::claimed, std::memory_order_release);
  current_.reset();
  return true;
}

} // namespace oscillate::media
