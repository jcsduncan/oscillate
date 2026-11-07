#pragma once

#include "oscillate/project/insert_projection.hpp"
#include "oscillate/project/model.hpp"

#include <QVariantMap>

#include <cstdint>
#include <vector>

namespace oscillate::quick {

[[nodiscard]] QVariantMap insert_chain_view(const project::SignalPathProjection& path,
                                            const std::vector<project::EffectInsert>& saved_effects,
                                            const project::Track* track);

} // namespace oscillate::quick
