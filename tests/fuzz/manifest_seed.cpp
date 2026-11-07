#include "private.hpp"

#include "oscillate/foundation/id.hpp"
#include "oscillate/timeline/timing.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

int main() try {
  using namespace oscillate;
  const auto id = project::ProjectId::from_bits({0, 1});
  const auto tempo = timeline::TempoMap::create({{{0}, 120.0, timeline::TempoShape::step}}, 1);
  const auto meter = timeline::MeterMap::create({{{0}, 4, 4}}, 1);
  if (!id || !tempo || !meter)
    throw std::runtime_error("cannot build manifest seed");
  project::Snapshot snapshot{
      id.value(), 1, 48'000, std::move(tempo).value(), std::move(meter).value(), {}};
  const auto text = persistence::detail::encode_manifest(
      snapshot, std::string(32, 'a'), std::nullopt, persistence::SaveKind::explicit_save);
  if (!persistence::detail::decode_manifest(text))
    throw std::runtime_error("generated manifest seed was not accepted");
  std::cout << text;
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
