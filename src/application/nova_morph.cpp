// The Constellation's morph and mutate rules for Nova (ADR-0077).
#include "oscillate/application/nova_morph.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace oscillate::application::nova_morph {
namespace {
using foundation::ErrorCode;
using native_dsp::ParameterDescriptor;
using sound_map::Sound;
template <typename T> using Result = foundation::Result<T>;

// Mutation leaves these alone: the level, tuning, the performance controls
// and the plumbing (voicing, polyphony, quality, Kick Lock).
constexpr std::string_view master_id = "nova.master";
constexpr std::array<std::string_view, 3> fixed_prefixes{"nova.global.", performance_prefix,
                                                         "nova.lock."};
constexpr std::array<std::string_view, 3> fixed_suffixes{".fine", ".pitch", ".return"};

// splitmix64: a small seeded generator whose sequence is the same on every
// platform, unlike the standard distributions.
class SplitMix {
public:
  explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ += golden;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return z ^ (z >> 31U);
  }
  // -1..1, from the top 53 bits.
  double bipolar() noexcept {
    constexpr double unit = 0x1.0p-53;
    return 2.0 * static_cast<double>(next() >> 11U) * unit - 1.0;
  }
  static constexpr std::uint64_t golden = 0x9e37'79b9'7f4a'7c15ULL;

private:
  std::uint64_t state_;
};

[[nodiscard]] bool starts(std::string_view id, std::string_view prefix) noexcept {
  return id.starts_with(prefix);
}

// "nova.fx.<uid>.<p>": the effect slot uid; nullopt for the rack's own
// parameters (split, oversampling, buses).
[[nodiscard]] std::optional<std::uint32_t> fx_slot_uid(std::string_view id) noexcept {
  if (!starts(id, fx_prefix))
    return std::nullopt;
  const auto rest = id.substr(fx_prefix.size());
  std::uint32_t uid = 0;
  const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), uid);
  if (parsed.ec != std::errc{} || parsed.ptr == rest.data() ||
      parsed.ptr == rest.data() + rest.size() || *parsed.ptr != '.')
    return std::nullopt;
  return uid;
}

[[nodiscard]] const std::string* module_at(const project::NativeSynthDocument& document,
                                           std::uint32_t uid) noexcept {
  for (const auto& chain : document.chains)
    for (const auto& slot : chain.modules)
      if (slot.uid == uid)
        return &slot.module;
  return nullptr;
}

[[nodiscard]] bool is_route_amount(std::string_view id) noexcept {
  return starts(id, route_prefix) && id.ends_with(route_amount_suffix);
}

[[nodiscard]] bool is_fx_media(const project::NativeMediaSlot& slot) noexcept {
  return slot.id.starts_with(fx_media_prefix) && slot.id.ends_with(fx_media_suffix);
}

[[nodiscard]] std::optional<std::size_t>
route_amount_index(const native_dsp::EngineDescriptor& table, std::uint32_t slot) {
  return native_dsp::find_parameter(table, std::string(route_prefix) + std::to_string(slot) +
                                               std::string(route_amount_suffix));
}

// A sound's amount for the route from `source` to `destination`, 0 when it
// has none.
[[nodiscard]] double route_amount(const native_dsp::EngineDescriptor& table, const Sound& sound,
                                  const project::NativeModRoute& like) {
  const auto found = std::ranges::find_if(sound.document.routes, [&](const auto& route) {
    return route.source == like.source && route.destination == like.destination;
  });
  if (found == sound.document.routes.end())
    return 0.0;
  const auto index = route_amount_index(table, found->slot);
  return index ? sound.values[*index] : 0.0;
}

} // namespace

std::size_t heaviest(std::span<const Part> parts) noexcept {
  std::size_t best = 0;
  for (std::size_t i = 1; i < parts.size(); ++i)
    if (parts[i].weight > parts[best].weight)
      best = i;
  return best;
}

Result<Sound> morph(const native_dsp::EngineDescriptor& table, std::span<const Part> parts,
                    const Sound& current, Locks locks) {
  const auto size = table.parameters.size();
  if (parts.empty() || parts.size() > max_parts)
    return Result<Sound>::failure(
        {ErrorCode::invalid_argument, "Morph blends one to three sounds"});
  double total = 0.0;
  for (const auto& part : parts) {
    if (part.sound == nullptr || part.sound->values.size() != size || !std::isfinite(part.weight) ||
        part.weight < 0.0)
      return Result<Sound>::failure({ErrorCode::invalid_argument, "A morph part is not usable"});
    total += part.weight;
  }
  if (!(total > 0.0) || current.values.size() != size)
    return Result<Sound>::failure({ErrorCode::invalid_argument, "The morph has no weight"});

  const auto& lead = *parts[heaviest(parts)].sound;
  Sound result = lead;
  for (std::size_t i = 0; i < size; ++i) {
    const ParameterDescriptor& parameter = table.parameters[i];
    const std::string_view id = parameter.id;
    if ((locks.fx && starts(id, fx_prefix)) || (locks.envelopes && starts(id, envelope_prefix)) ||
        (locks.routes && is_route_amount(id)) || starts(id, performance_prefix)) {
      result.values[i] = current.values[i];
      continue;
    }
    // Route amounts follow the routes, below; switches and choices the
    // heaviest.
    if (is_route_amount(id) || native_dsp::is_discrete(parameter))
      continue;
    // An effect slot's settings mean something only for its module.
    const std::string* module = nullptr;
    const auto uid = fx_slot_uid(id);
    if (uid) {
      module = module_at(lead.document, *uid);
      if (module == nullptr)
        continue;
    }
    double weight = 0.0;
    double position = 0.0;
    for (const auto& part : parts) {
      if (uid && module != nullptr) {
        const auto* theirs = module_at(part.sound->document, *uid);
        if (theirs == nullptr || *theirs != *module)
          continue;
      }
      weight += part.weight;
      position += part.weight * native_dsp::to_normalised(parameter, part.sound->values[i]);
    }
    if (weight > 0.0)
      result.values[i] = native_dsp::sanitise(
          parameter, native_dsp::from_normalised(parameter, position / weight));
  }

  if (locks.routes) {
    result.document.routes = current.document.routes;
  } else {
    for (const auto& route : result.document.routes) {
      const auto index = route_amount_index(table, route.slot);
      if (!index)
        continue;
      double amount = 0.0;
      for (const auto& part : parts)
        amount += part.weight * route_amount(table, *part.sound, route);
      result.values[*index] = native_dsp::sanitise(table.parameters[*index], amount / total);
    }
  }
  if (locks.fx) {
    result.document.chains = current.document.chains;
    std::erase_if(result.document.media, is_fx_media);
    for (const auto& slot : current.document.media)
      if (is_fx_media(slot))
        result.document.media.push_back(slot);
  }
  return Result<Sound>::success(std::move(result));
}

bool mutable_parameter(const ParameterDescriptor& parameter, Locks locks) noexcept {
  const std::string_view id = parameter.id;
  if (native_dsp::is_discrete(parameter) || id == master_id)
    return false;
  if (std::ranges::any_of(fixed_prefixes, [&](std::string_view p) { return starts(id, p); }) ||
      std::ranges::any_of(fixed_suffixes, [&](std::string_view s) { return id.ends_with(s); }))
    return false;
  return !(locks.fx && starts(id, fx_prefix)) &&
         !(locks.envelopes && starts(id, envelope_prefix)) &&
         !(locks.routes && is_route_amount(id));
}

std::vector<Sound> mutate(const native_dsp::EngineDescriptor& table, const Sound& base,
                          std::uint64_t seed, Locks locks, std::size_t count) {
  std::vector<Sound> variations;
  if (base.values.size() != table.parameters.size())
    return variations;
  variations.reserve(count);
  for (std::size_t v = 0; v < count; ++v) {
    SplitMix random(seed + SplitMix::golden * (v + 1));
    Sound variation = base;
    for (std::size_t i = 0; i < table.parameters.size(); ++i) {
      const auto& parameter = table.parameters[i];
      if (!mutable_parameter(parameter, locks))
        continue;
      const double position = std::clamp(native_dsp::to_normalised(parameter, base.values[i]) +
                                             mutation_spread * random.bipolar(),
                                         0.0, 1.0);
      variation.values[i] =
          native_dsp::sanitise(parameter, native_dsp::from_normalised(parameter, position));
    }
    variations.push_back(std::move(variation));
  }
  return variations;
}

} // namespace oscillate::application::nova_morph
