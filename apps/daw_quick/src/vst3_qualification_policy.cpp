#include "vst3_qualification_policy.hpp"

namespace oscillate::quick {

Vst3LiveQualification verified_vst3_live_qualification() {
  Vst3LiveQualification result;
  // Development admission from docs/plugins/surge-xt-live-latency-qualification.md.
  // The separate whole-graph allocation gate is red; this is not a release
  // compatibility certification or an admission for another Surge control.
  for (const auto block_frames : {64U, 128U}) {
    result.parameters.push_back({"b8584398f314819241ca7287117d5559ed838d37ef08faab9847f27ba2993cec",
                                 "abcdef019182faeb566d624153675854", 1945359057U, 48'000U,
                                 block_frames});
  }
  return result;
}

} // namespace oscillate::quick
