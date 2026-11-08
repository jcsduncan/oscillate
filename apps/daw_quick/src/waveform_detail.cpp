#include "waveform_detail.hpp"

#include "oscillate/media/audio_reader.hpp"
#include "oscillate/persistence/store.hpp"

#include <QVariantMap>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <utility>

namespace oscillate::quick {
namespace {

constexpr std::size_t maximum_columns = 4096;
constexpr std::size_t read_frames = 16'384;
// Open readers the worker keeps (each holds a descriptor and decode state);
// past this the cache starts again, so a long session cannot exhaust them.
constexpr std::size_t max_open_readers = 32;

} // namespace

QVariantList detail_columns(const WaveformDetail& detail, std::span<const std::uint64_t> edges) {
  QVariantList output;
  if (edges.size() < 2 || edges.size() - 1 > maximum_columns || !std::ranges::is_sorted(edges) ||
      !detail.covers(edges.front(), std::max(edges.back(), edges.front() + 1)))
    return output;
  const auto columns = edges.size() - 1;
  output.reserve(static_cast<qsizetype>(columns));
  for (std::size_t index = 0; index < columns; ++index) {
    const auto begin = std::min(edges[index], detail.end_frame() - 1);
    const auto end = std::min(std::max(edges[index + 1], begin + 1), detail.end_frame());
    float minimum = 1.0F;
    float maximum = -1.0F;
    for (auto frame = begin; frame < end; ++frame) {
      const auto& sample = detail.frames[frame - detail.first_frame];
      minimum = std::min({minimum, sample.left, sample.right});
      maximum = std::max({maximum, sample.left, sample.right});
    }
    output.push_back(QVariantMap{{"min", minimum}, {"max", maximum}});
  }
  return output;
}

WaveformDetailWorker::WaveformDetailWorker() {
  thread_ = std::jthread([shared = shared_](const std::stop_token& stopped) {
    std::filesystem::path root;
    std::optional<persistence::ProjectStore> store;
    std::unordered_map<std::string, media::AudioReader> readers;
    std::array<media::StereoFrame, read_frames> scratch{};
    while (!stopped.stop_requested()) {
      std::optional<Request> next_request;
      std::string id;
      {
        std::unique_lock lock(shared->mutex);
        if (!shared->changed.wait(lock, stopped, [&] { return !shared->pending.empty(); }))
          break;
        auto next = shared->pending.begin();
        id = next->first;
        next_request.emplace(std::move(next->second));
        shared->pending.erase(next);
      }
      const auto& wanted = *next_request;
      // A source that cannot be read answers with no frames, so the
      // controller stops asking and keeps the summary's view.
      const auto unreadable = [&] {
        std::lock_guard lock(shared->mutex);
        shared->ready.push_back({wanted.generation, id, {}});
      };
      if (wanted.root != root || !store) {
        readers.clear();
        store.reset();
        root = wanted.root;
        auto opened = persistence::ProjectStore::open_reader(root);
        if (!opened) {
          unreadable();
          continue;
        }
        store.emplace(std::move(opened.value()));
      }
      auto reader = readers.find(id);
      if (reader == readers.end()) {
        if (readers.size() >= max_open_readers)
          readers.clear();
        auto descriptor = store->open_verified_source(wanted.source, stopped);
        if (!descriptor) {
          unreadable();
          continue;
        }
        auto opened = media::AudioReader::open_fd(descriptor.value(), wanted.source);
        (void)::close(descriptor.value());
        if (!opened) {
          unreadable();
          continue;
        }
        reader = readers.emplace(id, std::move(opened.value())).first;
      }
      const auto end =
          std::min({wanted.end, reader->second.frame_count(), wanted.begin + max_frames});
      if (end <= wanted.begin) {
        unreadable();
        continue;
      }
      WaveformDetail detail{.first_frame = wanted.begin, .frames = {}};
      detail.frames.reserve(end - wanted.begin);
      bool superseded = false;
      while (detail.end_frame() < end && !stopped.stop_requested()) {
        const auto count = std::min<std::uint64_t>(read_frames, end - detail.end_frame());
        auto got = reader->second.read_at(detail.end_frame(), std::span(scratch).first(count));
        if (!got || got.value() == 0)
          break;
        detail.frames.insert(detail.frames.end(), scratch.begin(),
                             scratch.begin() + static_cast<std::ptrdiff_t>(got.value()));
        // A newer stretch of this source replaces this one: read that instead.
        std::lock_guard lock(shared->mutex);
        if (shared->pending.contains(id)) {
          superseded = true;
          break;
        }
      }
      if (superseded)
        continue;
      std::lock_guard lock(shared->mutex);
      shared->ready.push_back({wanted.generation, id, std::move(detail)});
    }
  });
}

WaveformDetailWorker::~WaveformDetailWorker() {
  thread_.request_stop();
  shared_->changed.notify_all();
  // Like the summary worker, a slow decoder read may finish after the window
  // closes; the worker owns everything it touches.
  thread_.detach();
}

void WaveformDetailWorker::request(std::filesystem::path root, project::SourceRecord source,
                                   std::uint64_t begin, std::uint64_t end,
                                   std::uint64_t generation) {
  {
    std::lock_guard lock(shared_->mutex);
    auto id = source.id.to_string();
    shared_->pending.insert_or_assign(
        std::move(id), Request{std::move(root), std::move(source), begin, end, generation});
  }
  shared_->changed.notify_one();
}

std::vector<WaveformDetailWorker::Outcome> WaveformDetailWorker::collect() {
  std::lock_guard lock(shared_->mutex);
  std::vector<Outcome> output;
  output.swap(shared_->ready);
  return output;
}

} // namespace oscillate::quick
