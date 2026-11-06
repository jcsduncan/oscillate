#include "oscillate/application/routed_source_selection.hpp"

#include <stdexcept>
#include <utility>

namespace oscillate::application {

struct RoutedSourceSelection::Descriptor {
  RoutedSourceBinding active;
  RoutedSourceBinding candidate;
};

RoutedSourceSelection::CallbackLease::CallbackLease(RoutedSourceSelection& owner,
                                                    const Descriptor* descriptor) noexcept
    : owner_(&owner), descriptor_(descriptor) {}

RoutedSourceSelection::CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), descriptor_(other.descriptor_) {}

RoutedSourceSelection::CallbackLease::~CallbackLease() {
  if (owner_ != nullptr)
    owner_->callback_epoch_.fetch_add(1, std::memory_order_seq_cst);
}

void* RoutedSourceSelection::CallbackLease::source_for(std::uint64_t revision) const noexcept {
  if (descriptor_ == nullptr || revision == 0)
    return nullptr;
  if (descriptor_->candidate.revision == revision)
    return descriptor_->candidate.source.get();
  if (descriptor_->active.revision == revision)
    return descriptor_->active.source.get();
  return nullptr;
}

void* RoutedSourceSelection::CallbackLease::source_for_graph_revision(
    std::uint64_t revision) const noexcept {
  if (descriptor_ == nullptr || revision < descriptor_->active.revision)
    return nullptr;
  if (descriptor_->candidate.revision == revision)
    return descriptor_->candidate.source.get();
  if (descriptor_->candidate.revision != 0 && revision > descriptor_->candidate.revision)
    return nullptr;
  return descriptor_->active.source.get();
}

RoutedSourceSelection::Retired::Retired(std::unique_ptr<Descriptor> descriptor) noexcept
    : descriptor_(std::move(descriptor)) {}
RoutedSourceSelection::Retired::Retired(Retired&&) noexcept = default;
RoutedSourceSelection::Retired&
RoutedSourceSelection::Retired::operator=(Retired&&) noexcept = default;
RoutedSourceSelection::Retired::~Retired() = default;

RoutedSourceSelection::RoutedSourceSelection(RoutedSourceBinding initial) {
  if (initial.revision == 0 || !initial.source)
    throw std::invalid_argument("routed source selection needs a revision and source");
  current_ = std::make_unique<Descriptor>(Descriptor{std::move(initial), {}});
  published_.store(current_.get(), std::memory_order_seq_cst);
}

RoutedSourceSelection::~RoutedSourceSelection() = default;

bool RoutedSourceSelection::replace(std::unique_ptr<Descriptor> next) noexcept {
  if (retired_ || !next)
    return false;
  retired_ = std::move(current_);
  current_ = std::move(next);
  published_.store(current_.get(), std::memory_order_seq_cst);
  const auto epoch = callback_epoch_.load(std::memory_order_seq_cst);
  retire_after_epoch_ = (epoch & 1U) != 0 ? epoch + 1 : epoch;
  return true;
}

bool RoutedSourceSelection::stage_candidate(RoutedSourceBinding candidate) {
  if (retired_ || current_->candidate.revision != 0 ||
      candidate.revision <= current_->active.revision || !candidate.source)
    return false;
  auto next = std::make_unique<Descriptor>(Descriptor{current_->active, std::move(candidate)});
  return replace(std::move(next));
}

bool RoutedSourceSelection::promote_candidate(std::uint64_t revision) {
  if (retired_ || revision == 0 || current_->candidate.revision != revision)
    return false;
  auto next = std::make_unique<Descriptor>(Descriptor{current_->candidate, {}});
  return replace(std::move(next));
}

bool RoutedSourceSelection::cancel_candidate(std::uint64_t revision) {
  if (retired_ || revision == 0 || current_->candidate.revision != revision)
    return false;
  auto next = std::make_unique<Descriptor>(Descriptor{current_->active, {}});
  return replace(std::move(next));
}

RoutedSourceSelection::CallbackLease RoutedSourceSelection::enter_callback() noexcept {
  callback_epoch_.fetch_add(1, std::memory_order_seq_cst);
  return CallbackLease(*this, published_.load(std::memory_order_seq_cst));
}

std::optional<RoutedSourceSelection::Retired>
RoutedSourceSelection::take_retired_if_quiescent() noexcept {
  if (!retired_ || callback_epoch_.load(std::memory_order_seq_cst) < retire_after_epoch_)
    return std::nullopt;
  return Retired(std::move(retired_));
}

} // namespace oscillate::application
