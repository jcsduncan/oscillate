#include "document.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace oscillate::native_dsp::synth::nova {
namespace {

using foundation::ErrorCode;
using foundation::Result;

Result<void> invalid(std::string message) {
  return Result<void>::failure({ErrorCode::invalid_argument, "Nova: " + std::move(message)});
}

// "prefix<n>" with n in [first, last], else nullopt.
[[nodiscard]] std::optional<std::size_t> numbered(std::string_view id, std::string_view prefix,
                                                  std::size_t first, std::size_t last) {
  if (!id.starts_with(prefix))
    return std::nullopt;
  const auto digits = id.substr(prefix.size());
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (error != std::errc{} || end != digits.data() + digits.size() || value < first || value > last)
    return std::nullopt;
  return value;
}

[[nodiscard]] std::optional<std::size_t> oscillator_slot(std::string_view id) {
  static constexpr std::array<std::string_view, oscillator_count> ids{"osc.a", "osc.b", "osc.c"};
  for (std::size_t o = 0; o < ids.size(); ++o)
    if (ids[o] == id)
      return o;
  return std::nullopt;
}

// warp.<o>.<w>: oscillator a..c, slot 1..2.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> remap_curve(std::string_view id) {
  if (id.size() != 8 || !id.starts_with("warp.") || id[6] != '.')
    return std::nullopt;
  if (id[5] < 'a' || id[5] > 'c' || id[7] < '1' || id[7] > '2')
    return std::nullopt;
  return std::pair{static_cast<std::size_t>(id[5] - 'a'), static_cast<std::size_t>(id[7] - '1')};
}

[[nodiscard]] bool known(std::span<const std::string_view> names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::string media_key(const project::NativeMediaSlot& slot) {
  if (const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media))
    return "builtin:" + builtin->name;
  return std::get<project::ManagedAudioReference>(slot.media).sha256 + ":" +
         std::to_string(slot.frame_length);
}

[[nodiscard]] const project::NativeMediaSlot*
find_media(const project::NativeSynthDocument& document, std::string_view id) {
  const auto found = std::ranges::find(document.media, id, &project::NativeMediaSlot::id);
  return found == document.media.end() ? nullptr : &*found;
}

[[nodiscard]] std::shared_ptr<const SynthMediaAsset> find_asset(const SynthMedia& media,
                                                                std::string_view slot) {
  const auto found = std::ranges::find(media, slot, &SynthMediaEntry::slot);
  return found == media.end() ? nullptr : found->asset;
}

[[nodiscard]] std::vector<float> mono(const SynthMediaAsset& asset) {
  std::vector<float> out(asset.frames.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = 0.5F * (asset.frames[i].left + asset.frames[i].right);
  return out;
}

} // namespace

Result<void> validate_document(const project::NativeSynthDocument& document) {
  const auto& table = descriptor();
  for (const auto& route : document.routes) {
    if (!source_from_id(route.source))
      return invalid("unknown modulation source '" + route.source + "'");
    if (!route.aux.empty() && !source_from_id(route.aux))
      return invalid("unknown modulation aux '" + route.aux + "'");
    const auto destination = find_parameter(table, route.destination);
    if (!destination || !table.parameters[*destination].automatable)
      return invalid("'" + route.destination + "' cannot be modulated");
  }
  for (const auto& curve : document.curves) {
    if (!numbered(curve.id, "lfo.", 1, lfo_count) && !remap_curve(curve.id))
      return invalid("unknown curve '" + curve.id + "'");
  }
  for (const auto& chain : document.chains) {
    if (chain.id != "fx.main" && chain.id != "fx.bus.1" && chain.id != "fx.bus.2")
      return invalid("unknown effect chain '" + chain.id + "'");
    if (chain.modules.size() > fx_max_modules_per_chain)
      return invalid("too many effects in '" + chain.id + "'");
    for (const auto& slot : chain.modules) {
      if (slot.uid < 1 || slot.uid > fx_slot_uids)
        return invalid("effect slot " + std::to_string(slot.uid) + " is out of range");
      if (!fx_kind_from_id(slot.module))
        return invalid("unknown effect '" + slot.module + "'");
    }
  }
  for (const auto& sequence : document.sequences) {
    if (!numbered(sequence.id, "clip.", 1, sequencer_clips))
      return invalid("unknown sequence '" + sequence.id + "'");
  }
  for (const auto& slot : document.media) {
    const auto* builtin = std::get_if<project::NativeBuiltinMedia>(&slot.media);
    if (oscillator_slot(slot.id)) {
      if (builtin && !known(builtin_table_names(), builtin->name))
        return invalid("unknown built-in table '" + builtin->name + "'");
    } else if (slot.id == "noise") {
      if (builtin && !known(builtin_noise_names(), builtin->name))
        return invalid("unknown built-in noise '" + builtin->name + "'");
    } else if (slot.id.starts_with("fx.") && slot.id.ends_with(".ir")) {
      const auto uid = numbered(slot.id.substr(0, slot.id.size() - 3), "fx.", 1, fx_slot_uids);
      if (!uid || builtin)
        return invalid("invalid impulse slot '" + slot.id + "'");
    } else {
      return invalid("unknown media slot '" + slot.id + "'");
    }
  }
  for (const auto& label : document.labels) {
    if (!numbered(label.id, "macro.", 1, macro_count) && label.id != arp_pattern_label &&
        label.id != arp_groove_label)
      return invalid("unknown label '" + label.id + "'");
  }
  return validate_analogue_lanes(document.voice_lanes);
}

Result<std::shared_ptr<const DocumentState>>
prepare_document(const project::NativeSynthDocument& document, const SynthMedia& media,
                 double sample_rate, const DocumentState* previous_state) {
  using R = Result<std::shared_ptr<const DocumentState>>;
  if (auto valid = validate_document(document); !valid)
    return R::failure(valid.error());
  const auto* previous = static_cast<const NovaState*>(previous_state);
  const auto& table = descriptor();
  auto state = std::make_shared<NovaState>();

  for (const auto& route : document.routes) {
    CompiledRoute compiled;
    compiled.slot = static_cast<std::uint16_t>(route.slot);
    // validate_document above found every route's destination and sources.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    compiled.destination = static_cast<std::uint16_t>(*find_parameter(table, route.destination));
    compiled.source = static_cast<std::uint8_t>(*source_from_id(route.source));
    compiled.aux =
        route.aux.empty() ? aux_none : static_cast<std::uint8_t>(*source_from_id(route.aux));
    // NOLINTEND(bugprone-unchecked-optional-access)
    compiled.bipolar = route.bipolar;
    compiled.targets_amount = route.destination.starts_with("nova.mod.");
    compiled.tension = static_cast<float>(route.tension);
    compiled.scale = static_cast<float>(route.scale);
    state->routes.push_back(compiled);
  }

  state->lfo_shapes.fill(default_lfo_shape());
  for (const auto& curve : document.curves) {
    if (const auto lfo = numbered(curve.id, "lfo.", 1, lfo_count)) {
      state->lfo_shapes[*lfo - 1] = sample_curve(curve.points);
    } else if (const auto remap = remap_curve(curve.id)) {
      state->remap_storage.push_back(std::make_unique<CurveShape>(sample_curve(curve.points)));
      state->remap[remap->first][remap->second] = state->remap_storage.back().get();
    }
  }

  static constexpr std::array<std::string_view, oscillator_count> osc_slots{"osc.a", "osc.b",
                                                                            "osc.c"};
  for (std::size_t o = 0; o < oscillator_count; ++o) {
    const auto* slot = find_media(document, osc_slots[o]);
    const std::string key = slot ? media_key(*slot) : "builtin:basic_shapes";
    state->table_keys[o] = key;
    if (previous && previous->table_keys[o] == key) {
      state->tables[o] = previous->tables[o];
      state->samples[o] = previous->samples[o];
      continue;
    }
    if (!slot || std::holds_alternative<project::NativeBuiltinMedia>(slot->media)) {
      const auto name = slot ? std::get<project::NativeBuiltinMedia>(slot->media).name
                             : std::string("basic_shapes");
      state->tables[o] = builtin_table(name);
      if (!state->tables[o])
        return R::failure({ErrorCode::not_found, "Nova: built-in table '" + name + "' is missing"});
      continue;
    }
    auto asset = find_asset(media, slot->id);
    if (!asset)
      return R::failure(
          {ErrorCode::unavailable, "Nova: media slot '" + slot->id + "' is not loaded"});
    state->samples[o] = asset;
    if (slot->frame_length > 0) {
      auto built = build_wavetable(mono(*asset), slot->frame_length, slot->id);
      if (!built)
        return R::failure(built.error());
      state->tables[o] = std::move(built).value();
    }
  }

  {
    const auto* slot = find_media(document, "noise");
    const std::string key = slot ? media_key(*slot) : "builtin:white";
    state->noise_key = key;
    if (previous && previous->noise_key == key) {
      state->noise = previous->noise;
    } else if (!slot || std::holds_alternative<project::NativeBuiltinMedia>(slot->media)) {
      const auto name =
          slot ? std::get<project::NativeBuiltinMedia>(slot->media).name : std::string("white");
      state->noise = builtin_noise(name);
      if (!state->noise)
        return R::failure({ErrorCode::not_found, "Nova: built-in noise '" + name + "' is missing"});
    } else {
      auto asset = find_asset(media, "noise");
      if (!asset)
        return R::failure({ErrorCode::unavailable, "Nova: the noise slot is not loaded"});
      auto built = build_noise(*asset, "noise");
      if (!built)
        return R::failure(built.error());
      state->noise = std::move(built).value();
    }
  }

  auto fx = build_fx_layout(document, sample_rate, control_interval_frames, media,
                            previous ? previous->fx.get() : nullptr);
  if (!fx)
    return R::failure(fx.error());
  state->fx = std::move(fx).value();
  state->clips = compile_sequencer_clips(document);
  state->analogue = compile_analogue_edits(document.voice_lanes);
  return R::success(std::move(state));
}

} // namespace oscillate::native_dsp::synth::nova
