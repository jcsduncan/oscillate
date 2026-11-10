#include "oscillate/render_graph/rebalance.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace oscillate::render_graph {

native_dsp::RoleTone role_tone(const project::RoleBalance& balance) noexcept {
  return {balance.gain_db, balance.low_db, balance.low_hz,  balance.mid_db,
          balance.mid_hz,  balance.mid_q,  balance.high_db, balance.high_hz};
}

const native_dsp::RoleTarget& RebalanceStages::target(project::MixRole role) const {
  const auto index = static_cast<std::size_t>(role);
  return targets_[index < project::mix_role_count ? index : project::mix_role_count];
}

foundation::Result<void> RebalanceStages::update(const project::Snapshot& snapshot) {
  using R = foundation::Result<void>;
  const bool rate_changed = snapshot.sample_rate != sample_rate_;
  std::array<std::optional<native_dsp::RoleTone>, project::mix_role_count> tones;
  const project::RoleBalance neutral_balance{};
  std::vector<native_dsp::RoleTarget> targets;
  targets.reserve(project::mix_role_count + 1);
  std::array<bool, project::mix_role_count> changed{};
  for (std::size_t role = 0; role < project::mix_role_count; ++role) {
    tones[role] =
        role_tone(snapshot.rebalance.enabled ? snapshot.rebalance.roles[role] : neutral_balance);
    changed[role] = rate_changed || tones[role] != tones_[role];
    if (!changed[role]) {
      targets.push_back(targets_[role]);
      continue;
    }
    auto made = native_dsp::role_target(*tones[role], snapshot.sample_rate);
    if (!made)
      return R::failure(made.error());
    targets.push_back(std::move(made).value());
  }
  if (rate_changed) {
    auto neutral = native_dsp::neutral_target(snapshot.sample_rate);
    if (!neutral)
      return R::failure(neutral.error());
    targets.push_back(std::move(neutral).value());
    // A stage runs at one rate: a new rate makes new stages.
    entries_.clear();
  } else {
    targets.push_back(targets_.back());
  }
  sample_rate_ = snapshot.sample_rate;
  tones_ = tones;
  targets_ = std::move(targets);
  roles_ = project::mix_roles(snapshot);

  std::map<project::TrackId, Entry> kept;
  for (const auto& role : roles_) {
    const auto found = entries_.find(role.track);
    if (found == entries_.end())
      continue;
    auto entry = std::move(found->second);
    const auto index = static_cast<std::size_t>(role.stage);
    const bool retuned = index < project::mix_role_count && changed[index];
    if (entry.runtime && (entry.stage != role.stage || retuned))
      entry.runtime->offer(target(role.stage));
    entry.stage = role.stage;
    kept.emplace(role.track, std::move(entry));
  }
  entries_ = std::move(kept);
  return R::success();
}

std::shared_ptr<native_dsp::RoleStage> RebalanceStages::stage(project::TrackId track) {
  if (const auto found = entries_.find(track); found != entries_.end())
    return found->second.runtime;
  const auto role = std::ranges::find(roles_, track, &project::TrackRole::track);
  if (role == roles_.end())
    return nullptr;
  auto runtime = std::make_shared<native_dsp::RoleStage>(target(role->stage));
  entries_.emplace(track, Entry{role->stage, runtime});
  return runtime;
}

std::uint64_t RebalanceStages::tail_frames(project::TrackId track) const {
  const auto role = std::ranges::find(roles_, track, &project::TrackRole::track);
  if (role == roles_.end())
    return 0;
  const auto& made = target(role->stage);
  if (made.flat_eq)
    return 0;
  const auto tail = native_dsp::estimate_eq_tail_frames(made.eq);
  return tail ? tail.value() : 0;
}

} // namespace oscillate::render_graph
