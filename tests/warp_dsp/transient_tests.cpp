#include "fixtures.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/analysis.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace warp = oscillate::warp_dsp;
namespace analysis = oscillate::warp_dsp::analysis;

using oscillate::test::require;

struct Score {
  double precision = 0.0;
  double recall = 0.0;
  double mean_error_ms = 0.0;
  double worst_error_ms = 0.0;
};

// Greedy one-to-one matching within `tolerance` seconds.
Score score(const std::vector<analysis::Transient>& found, const std::vector<std::uint64_t>& truth,
            std::uint32_t rate, double tolerance) {
  std::vector<bool> used(found.size(), false);
  std::size_t matched = 0;
  double total = 0.0;
  double worst = 0.0;
  for (const auto onset : truth) {
    std::size_t best = found.size();
    double best_error = tolerance * rate;
    for (std::size_t index = 0; index < found.size(); ++index) {
      const double error =
          std::fabs(static_cast<double>(found[index].frame) - static_cast<double>(onset));
      if (!used[index] && error <= best_error) {
        best = index;
        best_error = error;
      }
    }
    if (best == found.size() && std::getenv("WARP_TRANSIENT_DEBUG") != nullptr)
      std::printf("  missed onset at %.4f s\n", static_cast<double>(onset) / rate);
    if (best != found.size()) {
      used[best] = true;
      ++matched;
      total += best_error;
      worst = std::max(worst, best_error);
    }
  }
  if (std::getenv("WARP_TRANSIENT_DEBUG") != nullptr) {
    for (std::size_t index = 0; index < found.size(); ++index) {
      if (!used[index])
        std::printf("  extra transient at %.4f s strength %.3f\n",
                    static_cast<double>(found[index].frame) / rate, found[index].strength);
    }
  }
  Score result;
  const auto as_double = [](std::size_t value) { return static_cast<double>(value); };
  result.precision = found.empty() ? 1.0 : as_double(matched) / as_double(found.size());
  result.recall = truth.empty() ? 1.0 : as_double(matched) / as_double(truth.size());
  result.mean_error_ms = matched == 0 ? 0.0 : 1000.0 * total / as_double(matched) / rate;
  result.worst_error_ms = 1000.0 * worst / rate;
  return result;
}

Score evaluate(const char* name, const warp_fixtures::Stereo& signal, std::uint32_t rate) {
  warp::MemorySourceView view(signal.left, signal.right);
  const auto found = analysis::detect_transients(view, {rate, 1.0F, 0.030});
  REQUIRE_OK(found, "transient detection failed");
  const auto& transients = found.value();
  for (std::size_t index = 1; index < transients.size(); ++index)
    REQUIRE_GT(transients[index].frame, transients[index - 1].frame,
               "transients not strictly increasing");
  const auto result = score(transients, signal.onsets, rate, 0.010);
  std::printf("%-14s %6u Hz  found %4zu truth %4zu  P %.3f R %.3f  mean %.3f ms  worst %.3f ms\n",
              name, rate, transients.size(), signal.onsets.size(), result.precision, result.recall,
              result.mean_error_ms, result.worst_error_ms);
  return result;
}

void detects_onsets_accurately() {
  for (const std::uint32_t rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    auto clicks = warp_fixtures::click_train(rate, 6.0, 0.25);
    warp_fixtures::add_noise_floor(clicks, 1e-4, 1);
    const auto click_score = evaluate("clicks", clicks, rate);
    REQUIRE_GE(click_score.precision, 0.99, "click onsets missed or misplaced");
    REQUIRE_GE(click_score.recall, 0.99, "click onsets missed or misplaced");
    REQUIRE_LE(click_score.worst_error_ms, 0.1, "click onsets missed or misplaced");

    auto drums = warp_fixtures::drum_hits(rate, 8.0, 0.18, 2);
    warp_fixtures::add_noise_floor(drums, 1e-4, 3);
    const auto drum_score = evaluate("drums", drums, rate);
    REQUIRE_GE(drum_score.precision, 0.97, "drum onsets missed or misplaced");
    REQUIRE_GE(drum_score.recall, 0.97, "drum onsets missed or misplaced");
    REQUIRE_LE(drum_score.mean_error_ms, 1.0, "drum onsets missed or misplaced");

    auto plucked = warp_fixtures::plucks(rate, 10.0, 0.21, 4);
    warp_fixtures::add_noise_floor(plucked, 1e-4, 5);
    const auto pluck_score = evaluate("legato plucks", plucked, rate);
    REQUIRE_GE(pluck_score.precision, 0.95, "pluck onsets missed or misplaced");
    REQUIRE_GE(pluck_score.recall, 0.95, "pluck onsets missed or misplaced");
    REQUIRE_LE(pluck_score.mean_error_ms, 2.0, "pluck onsets missed or misplaced");
  }
}

void ignores_vibrato_and_silence() {
  const auto pad = warp_fixtures::vibrato_pad(48'000, 8.0);
  warp::MemorySourceView view(pad.left, pad.right);
  const auto found = analysis::detect_transients(view, {48'000, 1.0F, 0.030});
  REQUIRE_OK(found, "pad analysis failed");
  std::printf("vibrato pad    found %zu\n", found.value().size());
  REQUIRE_LE(found.value().size(), 1, "vibrato pad produced false transients");
  const auto quiet = warp_fixtures::silence(std::size_t{48'000} * 3);
  warp::MemorySourceView silent(quiet.left, quiet.right);
  const auto none = analysis::detect_transients(silent, {48'000, 1.0F, 0.030});
  REQUIRE_OK(none, "silence produced transients");
  REQUIRE_TRUE(none.value().empty(), "silence produced transients");
}

void validates_and_cancels() {
  const auto signal = warp_fixtures::click_train(48'000, 2.0, 0.25);
  warp::MemorySourceView view(signal.left, signal.right);
  REQUIRE_TRUE(!analysis::detect_transients(view, {1'000, 1.0F, 0.03}), "invalid options accepted");
  REQUIRE_TRUE(!analysis::detect_transients(view, {48'000, 0.0F, 0.03}),
               "invalid options accepted");
  REQUIRE_TRUE(!analysis::detect_transients(view, {48'000, 1.0F, -1.0}),
               "invalid options accepted");
  std::atomic<bool> cancelled{true};
  require(!analysis::detect_transients(view, {48'000, 1.0F, 0.03}, &cancelled),
          "cancelled analysis completed");
}

} // namespace

int main() {
  try {
    detects_onsets_accurately();
    ignores_vibrato_and_silence();
    validates_and_cancels();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
