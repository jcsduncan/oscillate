#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/effects/chop.hpp"
#include "oscillate/native_dsp/effects/contour.hpp"
#include "oscillate/native_dsp/effects/deck.hpp"
#include "oscillate/native_dsp/effects/dust.hpp"
#include "oscillate/native_dsp/effects/edge.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/native_dsp/effects/flyby.hpp"
#include "oscillate/native_dsp/effects/forge.hpp"
#include "oscillate/native_dsp/effects/lens.hpp"
#include "oscillate/native_dsp/effects/loudness.hpp"
#include "oscillate/native_dsp/effects/orbit.hpp"
#include "oscillate/native_dsp/effects/pocket.hpp"
#include "oscillate/native_dsp/effects/press.hpp"
#include "oscillate/native_dsp/effects/pump.hpp"
#include "oscillate/native_dsp/effects/relay.hpp"
#include "oscillate/native_dsp/effects/scope.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/effects/signal.hpp"
#include "oscillate/native_dsp/effects/strata.hpp"
#include "oscillate/native_dsp/effects/temper.hpp"
#include "oscillate/native_dsp/effects/trim.hpp"
#include "oscillate/native_dsp/effects/tune.hpp"
#include "oscillate/native_dsp/effects/vista.hpp"

#include <array>

namespace oscillate::native_dsp::effects {
namespace {
// The classic processors' engines (ADR-0047) come last.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): engines are built from constant tables.
const std::array<const EffectEngine*, 24> registry{
    &contour::engine(),  &forge::engine(), &orbit::engine(),  &relay::engine(),
    &expanse::engine(),  &vista::engine(), &deck::engine(),   &pocket::engine(),
    &pump::engine(),     &chop::engine(),  &temper::engine(), &strata::engine(),
    &edge::engine(),     &flyby::engine(), &dust::engine(),   &lens::engine(),
    &loudness::engine(), &scope::engine(), &tune::engine(),   &trim::engine(),
    &signal::engine(),   &shape::engine(), &press::engine(),  &ceiling::engine()};
} // namespace

std::span<const EffectEngine* const> engines() noexcept { return registry; }

const EffectEngine* find_engine(std::string_view id) noexcept {
  for (const EffectEngine* engine : registry) {
    if (engine->descriptor->id == id) {
      return engine;
    }
  }
  return nullptr;
}

} // namespace oscillate::native_dsp::effects
