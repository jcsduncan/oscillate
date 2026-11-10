#include "oscillate/application/master_assistant.hpp"

#include "oscillate/application/mix_check.hpp"
#include "oscillate/export/offline_master.hpp"
#include "oscillate/media/prefetch.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/project/mix_roles.hpp"
#include "oscillate/project/model.hpp"
#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <map>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>

namespace oscillate::application::master_assistant {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
namespace dsp = native_dsp::dsp;

static_assert(core::role_count == project::mix_role_count,
              "the assistant's roles are the rebalance's");

constexpr std::size_t channels = 2;
constexpr std::size_t read_block_frames = 65'536;
constexpr std::string_view cancelled_message = "The master assistant was cancelled";
// Progress shares: the render, then planning.
constexpr double render_share = 0.6;
constexpr double rerender_share = 0.3;

// Offline renders take their lease from a gate of their own (as Mix Check's
// do): they never touch the live graph, so playback may start meanwhile.
class DetachedGate final : public project::TransportGate {
public:
  class Lease final : public project::StoppedTransportLease {};
  foundation::Result<std::unique_ptr<project::StoppedTransportLease>> acquire_stopped() override {
    return foundation::Result<std::unique_ptr<project::StoppedTransportLease>>::success(
        std::make_unique<Lease>());
  }
};

// A file descriptor closed with its owner.
class Descriptor {
public:
  explicit Descriptor(int fd) noexcept : fd_(fd) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() {
    if (fd_ >= 0)
      (void)::close(fd_);
  }
  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

// A programme kept in an unnamed file (O_TMPFILE) in the project's cache
// directory: it has no name to leave behind and goes with its descriptor.
class TempFileStore final : public core::Store {
public:
  static Result<std::unique_ptr<core::Store>> open(int directory, std::uint32_t rate) {
    const int fd = ::openat(directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0)
      return Result<std::unique_ptr<core::Store>>::failure(
          {ErrorCode::io_failure, "Cannot keep the master assistant's audio in the cache"});
    return Result<std::unique_ptr<core::Store>>::success(
        std::unique_ptr<core::Store>(new TempFileStore(fd, rate)));
  }
  [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return rate_; }
  [[nodiscard]] std::uint64_t frames() const noexcept override { return frames_; }
  [[nodiscard]] Result<std::size_t> read(std::uint64_t at, std::span<float> left,
                                         std::span<float> right) override {
    if (at >= frames_)
      return Result<std::size_t>::success(0);
    const auto n = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min(left.size(), right.size()), frames_ - at));
    buffer_.resize(n * channels);
    if (!transfer(false, at, n))
      return Result<std::size_t>::failure({ErrorCode::io_failure, "Cannot read the cached audio"});
    for (std::size_t i = 0; i < n; ++i) {
      left[i] = buffer_[channels * i];
      right[i] = buffer_[channels * i + 1];
    }
    return Result<std::size_t>::success(n);
  }
  [[nodiscard]] Result<void> append(std::span<const float> left,
                                    std::span<const float> right) override {
    const auto n = std::min(left.size(), right.size());
    buffer_.resize(n * channels);
    for (std::size_t i = 0; i < n; ++i) {
      buffer_[channels * i] = left[i];
      buffer_[channels * i + 1] = right[i];
    }
    if (!transfer(true, frames_, n))
      return Result<void>::failure({ErrorCode::io_failure, "Cannot cache the rendered audio"});
    frames_ += n;
    return Result<void>::success();
  }

private:
  TempFileStore(int fd, std::uint32_t rate) noexcept : fd_(fd), rate_(rate) {}
  bool transfer(bool write, std::uint64_t frame, std::size_t frames) {
    auto* bytes = reinterpret_cast<char*>(buffer_.data());
    std::size_t left = frames * channels * sizeof(float);
    auto offset = static_cast<off_t>(frame * channels * sizeof(float));
    while (left > 0) {
      const auto done = write ? ::pwrite(fd_.get(), bytes, left, offset)
                              : ::pread(fd_.get(), bytes, left, offset);
      if (done < 0 && errno == EINTR)
        continue;
      if (done <= 0)
        return false;
      bytes += done;
      offset += done;
      left -= static_cast<std::size_t>(done);
    }
    return true;
  }

  Descriptor fd_;
  std::uint32_t rate_;
  std::uint64_t frames_ = 0;
  std::vector<float> buffer_;
};

// Stores in the project's cache directory, or in memory without one.
core::StoreFactory stores_for(const persistence::ProjectStore& store) {
  auto directory = store.open_cache_directory();
  std::shared_ptr<Descriptor> held =
      directory ? std::make_shared<Descriptor>(directory.value()) : nullptr;
  return [held](std::uint32_t rate) -> Result<std::unique_ptr<core::Store>> {
    if (held) {
      auto file = TempFileStore::open(held->get(), rate);
      if (file)
        return file;
    }
    return Result<std::unique_ptr<core::Store>>::success(std::make_unique<core::MemoryStore>(rate));
  };
}

// The snapshot as the master chain hears it: no chain, the level at unity.
project::Snapshot before_chain(const project::Snapshot& snapshot) {
  auto out = snapshot;
  out.master_effects.clear();
  out.master_level = 1.0;
  return out;
}

Result<void> render(const project::Snapshot& snapshot, const persistence::ProjectStore& reader,
                    const exporting::RenderTaps& taps) {
  DetachedGate gate;
  auto authority = exporting::StoppedExportAuthority::acquire(snapshot, gate);
  if (!authority)
    return Result<void>::failure(authority.error());
  return exporting::render_master_taps(snapshot, reader, std::move(authority).value(), taps);
}

// The part of a block from `first` that falls in [from, to).
struct Slice {
  std::size_t offset = 0;
  std::size_t frames = 0;
};
Slice slice(std::uint64_t first, std::size_t frames, Range within) {
  const auto begin = std::max(first, within.first);
  const auto end = std::min(first + frames, within.last);
  if (begin >= end)
    return {};
  return {static_cast<std::size_t>(begin - first), static_cast<std::size_t>(end - begin)};
}

// The roles' mid power per region, from their tracks' outputs.
class RoleMeter {
public:
  RoleMeter(const project::Snapshot& snapshot, std::uint32_t rate) : rate_(rate) {
    for (const auto& role : project::mix_roles(snapshot))
      if (role.stage != project::MixRole::none)
        role_of_.emplace(role.track, static_cast<std::size_t>(role.stage));
  }
  void push(std::span<const exporting::TrackTap> tracks, Slice part) {
    if (part.frames == 0)
      return;
    for (auto& sum : sums_)
      sum.assign(part.frames, 0.0F);
    std::array<bool, core::role_count> heard{};
    for (const auto& tap : tracks) {
      const auto found = role_of_.find(tap.track);
      if (found == role_of_.end())
        continue;
      auto& sum = sums_[found->second];
      heard[found->second] = true;
      for (std::size_t i = 0; i < part.frames; ++i)
        sum[i] += half * (tap.left[part.offset + i] + tap.right[part.offset + i]);
    }
    for (std::size_t k = 0; k < core::role_count; ++k) {
      if (!heard[k])
        continue;
      if (!meters_[k])
        meters_[k].emplace(rate_);
      meters_[k].value().push(sums_[k]);
    }
  }
  [[nodiscard]] core::RolePowers powers() const {
    core::RolePowers out;
    for (std::size_t k = 0; k < core::role_count; ++k)
      if (meters_[k])
        out[k] = meters_[k].value().powers();
    return out;
  }

private:
  static constexpr float half = 0.5F;
  std::uint32_t rate_;
  std::map<project::TrackId, std::size_t> role_of_;
  std::array<std::vector<float>, core::role_count> sums_;
  std::array<std::optional<core::RegionMeter>, core::role_count> meters_;
};

// Renders `snapshot` and keeps the master over the range: into a store and
// an analysis, and (with `roles`) the roles' shares.
struct Rendered {
  core::Analysis mix;
  core::RolePowers roles;
  std::shared_ptr<core::Store> programme;
};
Result<Rendered> render_programme(const project::Snapshot& snapshot,
                                  const persistence::ProjectStore& reader,
                                  std::optional<Range> range, bool with_roles,
                                  const Progress& progress, double from, double share) {
  using R = Result<Rendered>;
  const auto rate = snapshot.sample_rate;
  const auto limit = static_cast<std::uint64_t>(max_programme_seconds * rate);
  const std::uint64_t first = range ? range->first : 0;
  const std::uint64_t last = range ? range->last : std::numeric_limits<std::uint64_t>::max();
  if (range && (range->last <= range->first || range->last - range->first > limit))
    return R::failure({ErrorCode::invalid_argument,
                       "Choose a region of up to 30 minutes for the master assistant"});
  auto made = stores_for(reader)(rate);
  if (!made)
    return R::failure(made.error());
  std::shared_ptr<core::Store> programme = std::move(made).value();
  core::Analyser analyser(rate);
  std::optional<RoleMeter> roles;
  if (with_roles)
    roles.emplace(snapshot, rate);
  std::optional<foundation::Error> failure;
  bool too_long = false;
  bool cancelled = false;
  bool range_done = false;
  exporting::RenderTaps taps;
  taps.trim_silent_tail = true;
  if (with_roles)
    taps.tracks = [&](std::span<const exporting::TrackTap> tracks, std::uint64_t at) {
      if (!tracks.empty())
        roles->push(tracks, slice(at, tracks.front().left.size(), Range{first, last}));
    };
  taps.master = [&](std::span<const float> left, std::span<const float> right, std::uint64_t at) {
    const auto part = slice(at, left.size(), Range{first, last});
    if (part.frames == 0 || failure)
      return;
    const auto l = left.subspan(part.offset, part.frames);
    const auto r = right.subspan(part.offset, part.frames);
    analyser.push(l, r);
    auto appended = programme->append(l, r);
    if (!appended)
      failure = appended.error();
    too_long = too_long || programme->frames() > limit;
  };
  taps.progress = [&](std::uint64_t made_frames, std::uint64_t total) {
    if (failure || too_long)
      return false;
    // Past the range there is nothing more to measure.
    if (made_frames >= last) {
      range_done = true;
      return false;
    }
    const double done =
        total == 0 ? 0.0 : static_cast<double>(made_frames) / static_cast<double>(total);
    cancelled = progress && !progress(from + share * done, "Rendering the mix");
    return !cancelled;
  };
  auto rendered = render(snapshot, reader, taps);
  if (failure)
    return R::failure(*failure);
  if (too_long)
    return R::failure({ErrorCode::invalid_argument,
                       "The song is longer than 30 minutes: choose a region for the assistant"});
  if (cancelled)
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  // A render stopped at the range's end is complete for the assistant.
  if (!rendered && !range_done)
    return R::failure(rendered.error());
  Rendered out;
  out.mix = std::move(analyser).finish();
  if (roles)
    out.roles = roles->powers();
  out.programme = std::move(programme);
  return R::success(std::move(out));
}

Result<project::NativeEffectPatch> patch_for(const native_dsp::effects::EffectEngine& engine,
                                             const core::Device& device,
                                             const project::NativeEffectPatch* base) {
  std::vector<double> values;
  project::NativeSynthDocument document;
  if (base != nullptr) {
    auto resolved = native_inserts::resolve_native_effect(*base);
    if (!resolved)
      return Result<project::NativeEffectPatch>::failure(resolved.error());
    values = std::move(resolved.value().values);
    document = base->document;
  } else {
    auto fresh = native_inserts::native_effect_patch_for_preset(engine, std::nullopt);
    if (!fresh)
      return Result<project::NativeEffectPatch>::failure(fresh.error());
    values = native_dsp::default_values(*engine.descriptor);
    document = fresh.value().document;
  }
  for (const auto& [id, value] : device.values) {
    const auto index = native_dsp::find_parameter(*engine.descriptor, id);
    if (!index)
      return Result<project::NativeEffectPatch>::failure(
          {ErrorCode::not_found, "No parameter " + id + " on " + device.engine});
    values[*index] = native_dsp::sanitise(engine.descriptor->parameters[*index], value);
  }
  auto patch = native_inserts::make_native_effect_patch(engine, values, std::move(document));
  if (patch && base != nullptr)
    patch.value().link = base->link;
  return patch;
}

// The master chain a snapshot plays, as the offline chain runs it: its
// enabled built-in effects (anything else cannot be simulated and is left
// out).
Result<std::vector<core::ChainDevice>> chain_of(const project::Snapshot& snapshot,
                                                std::optional<core::Knob>& limiter,
                                                std::optional<core::Knob>& trim) {
  std::vector<core::ChainDevice> chain;
  for (const auto& insert : snapshot.master_effects) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor);
    if (!insert.enabled || patch == nullptr)
      continue;
    auto resolved = native_inserts::resolve_native_effect(*patch);
    if (!resolved)
      return Result<std::vector<core::ChainDevice>>::failure(resolved.error());
    const auto& table = *resolved.value().engine->descriptor;
    if (!limiter && patch->engine == "ceiling")
      limiter = core::Knob{chain.size(), native_dsp::find_parameter(table, "ceiling.gain").value()};
    if (!trim && patch->engine == "trim")
      trim = core::Knob{chain.size(), native_dsp::find_parameter(table, "trim.gain").value()};
    chain.push_back({resolved.value().engine, std::move(resolved.value().values),
                     patch->document.empty() ? std::nullopt : std::optional(patch->document)});
  }
  return Result<std::vector<core::ChainDevice>>::success(std::move(chain));
}

Result<project::Snapshot> planned_snapshot(const project::Snapshot& snapshot,
                                           const core::Proposal& proposal) {
  auto ops = operations(snapshot, proposal, {});
  if (!ops)
    return Result<project::Snapshot>::failure(ops.error());
  DetachedGate gate;
  auto model = project::ProjectModel::open(snapshot, gate);
  if (!model)
    return Result<project::Snapshot>::failure(model.error());
  auto applied = model.value().apply(ops.value());
  if (!applied)
    return Result<project::Snapshot>::failure(applied.error());
  return Result<project::Snapshot>::success(*model.value().snapshot());
}

// The current master over the audition window: the programme itself when
// the master has no chain and unity level, else a render of it up to the
// window's end.
Result<core::Capture> current_window(const Measured& measured, core::Window window) {
  const auto& snapshot = *measured.pinned->snapshot;
  core::Capture out;
  if (snapshot.master_effects.empty() && snapshot.master_level == 1.0) {
    out.left.resize(window.frames);
    out.right.resize(window.frames);
    auto got = measured.programme->read(window.first, out.left, out.right);
    if (!got)
      return Result<core::Capture>::failure(got.error());
    out.left.resize(got.value());
    out.right.resize(got.value());
    return Result<core::Capture>::success(std::move(out));
  }
  const auto from = measured.first_frame + window.first;
  const auto to = from + window.frames;
  exporting::RenderTaps taps;
  taps.trim_silent_tail = true;
  taps.master = [&](std::span<const float> left, std::span<const float> right, std::uint64_t at) {
    const auto part = slice(at, left.size(), Range{from, to});
    out.left.insert(out.left.end(), left.begin() + static_cast<std::ptrdiff_t>(part.offset),
                    left.begin() + static_cast<std::ptrdiff_t>(part.offset + part.frames));
    out.right.insert(out.right.end(), right.begin() + static_cast<std::ptrdiff_t>(part.offset),
                     right.begin() + static_cast<std::ptrdiff_t>(part.offset + part.frames));
  };
  taps.progress = [&](std::uint64_t made, std::uint64_t) { return made < to; };
  auto rendered = render(snapshot, measured.pinned->reader, taps);
  if (!rendered && out.left.empty())
    return Result<core::Capture>::failure(rendered.error());
  return Result<core::Capture>::success(std::move(out));
}

double window_lufs(const core::Capture& capture, std::uint32_t rate) {
  return core::analyse(capture.left, capture.right, rate).lufs();
}

std::size_t engine_rank(const core::Proposal& proposal, std::string_view engine) {
  for (std::size_t i = 0; i < proposal.devices.size(); ++i)
    if (proposal.devices[i].engine == engine)
      return i;
  return proposal.devices.size();
}

} // namespace

bool selected(const std::vector<bool>& flags, std::size_t index) noexcept {
  return flags.empty() || (index < flags.size() && flags[index]);
}

Result<Measured> measure(PinnedSnapshot pinned, std::optional<Range> range,
                         const Progress& progress) {
  using R = Result<Measured>;
  if (!pinned.snapshot)
    return R::failure({ErrorCode::invalid_argument, "No project to measure"});
  const auto snapshot = before_chain(*pinned.snapshot);
  auto rendered =
      render_programme(snapshot, pinned.reader, range, true, progress, 0.0, render_share);
  if (!rendered)
    return R::failure(rendered.error());
  if (!rendered.value().mix.integrated_lufs)
    return R::failure({ErrorCode::invalid_argument,
                       "The mix is silent there: nothing for the assistant to measure"});
  Measured out;
  out.range = range;
  out.first_frame = range ? range->first : 0;
  out.mix = std::move(rendered.value().mix);
  out.roles = rendered.value().roles;
  out.programme = std::move(rendered.value().programme);
  out.pinned = std::make_shared<const PinnedSnapshot>(std::move(pinned));
  if (progress && !progress(1.0, "Measured"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  return R::success(std::move(out));
}

Result<core::Analysis> measure_reference(const std::filesystem::path& path,
                                         const std::function<bool()>& cancelled) {
  using R = Result<core::Analysis>;
  std::vector<float> left(read_block_frames);
  std::vector<float> right(read_block_frames);
  std::optional<core::Analyser> analyser;
  const auto run = [&](auto& reader, std::uint32_t rate, std::uint64_t frames,
                       auto convert) -> Result<void> {
    if (rate == 0 || static_cast<double>(frames) > reference_max_seconds * rate)
      return Result<void>::failure(
          {ErrorCode::invalid_argument, "The reference is too long or unreadable"});
    analyser.emplace(rate);
    std::uint64_t at = 0;
    while (at < frames) {
      if (cancelled && cancelled())
        return Result<void>::failure({ErrorCode::cancelled, "Reference cancelled"});
      auto got = convert(reader, at);
      if (!got)
        return Result<void>::failure(got.error());
      if (got.value() == 0)
        break;
      analyser->push(std::span<const float>(left).first(got.value()),
                     std::span<const float>(right).first(got.value()));
      at += got.value();
    }
    return Result<void>::success();
  };
  if (auto wav = media::WavReader::open(path)) {
    std::vector<media::StereoFrame> frames(read_block_frames);
    auto done = run(wav.value(), wav.value().sample_rate(), wav.value().frame_count(),
                    [&](media::WavReader& reader, std::uint64_t at) -> Result<std::size_t> {
                      auto got = reader.read_at(at, frames);
                      if (got)
                        for (std::size_t i = 0; i < got.value(); ++i) {
                          left[i] = frames[i].left;
                          right[i] = frames[i].right;
                        }
                      return got;
                    });
    if (!done)
      return R::failure(done.error());
  } else {
#if OSCILLATE_HAVE_CODEC
    auto encoded = codec::EncodedReader::open(path);
    if (!encoded)
      return R::failure(encoded.error());
    std::vector<codec::StereoFrame> frames(read_block_frames);
    const auto info = encoded.value().info();
    auto done = run(encoded.value(), info.sample_rate, info.frames,
                    [&](codec::EncodedReader& reader, std::uint64_t at) -> Result<std::size_t> {
                      auto got = reader.read_at(at, frames);
                      if (got)
                        for (std::size_t i = 0; i < got.value(); ++i) {
                          left[i] = frames[i].left;
                          right[i] = frames[i].right;
                        }
                      return got;
                    });
    if (!done)
      return R::failure(done.error());
#else
    return R::failure(wav.error());
#endif
  }
  if (!analyser)
    return R::failure({ErrorCode::invalid_argument, "The reference is unreadable"});
  auto analysis = std::move(analyser.value()).finish();
  if (!analysis.integrated_lufs)
    return R::failure({ErrorCode::invalid_argument, "The reference is silent or too short"});
  return R::success(std::move(analysis));
}

Result<Proposed> propose(const Measured& measured, const core::Reference& reference,
                         core::Goals goals, const Progress& progress) {
  using R = Result<Proposed>;
  const auto& snapshot = *measured.pinned->snapshot;
  const auto& reader = measured.pinned->reader;
  goals.after_chain_db = dsp::gain_to_db(snapshot.master_level, core::floor_db);
  bool stopped = false;
  double fraction = 0.0;
  std::string stage;
  const auto step = [&](double at, const std::string& text) {
    fraction = at;
    stage = text;
    stopped = stopped || (progress && !progress(at, text));
    return !stopped;
  };
  // Between steps the planner asks again at the same place.
  const core::Cancelled cancelled = [&] { return !step(fraction, stage); };
  if (!step(0.0, "Planning"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  const auto window = core::loudest_window(measured.mix, audition_seconds);
  const auto stores = stores_for(reader);

  // Nudges first, then the chain on the nudged mix.
  auto first = core::derive(measured.mix, reference, goals, measured.roles);
  const core::Analysis* mix = &measured.mix;
  std::shared_ptr<core::Store> programme = measured.programme;
  Rendered nudged;
  if (!first.nudges.empty()) {
    auto with = before_chain(snapshot);
    auto ops = operations(snapshot, first, {std::vector<bool>(first.devices.size(), false), {}});
    if (!ops)
      return R::failure(ops.error());
    for (const auto& op : ops.value())
      if (const auto* rebalance = std::get_if<project::SetMasterRebalance>(&op))
        with.rebalance = rebalance->rebalance;
    auto again =
        render_programme(with, reader, measured.range, false, progress, 0.0, rerender_share);
    if (!again)
      return R::failure(again.error());
    nudged = std::move(again).value();
    mix = &nudged.mix;
    programme = nudged.programme;
  }
  if (!step(rerender_share, "Learning the limiter's gain"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  auto planned = core::plan(*programme, *mix, reference, goals, stores, {}, window, cancelled);
  if (!planned)
    return R::failure(planned.error());
  auto proposal = std::move(planned.value().proposal);
  proposal.nudges = first.nudges;
  proposal.mix_lufs = measured.mix.lufs();
  proposal.mix_true_peak_dbtp = measured.mix.true_peak_dbtp;

  // The proposal's chain as the project will play it: the planned master,
  // with any devices it keeps. When that is not the chain just learned, the
  // gain is learned again on it.
  auto plan_snapshot = planned_snapshot(snapshot, proposal);
  if (!plan_snapshot)
    return R::failure(plan_snapshot.error());
  std::optional<core::Knob> limiter;
  std::optional<core::Knob> trim;
  auto chain = chain_of(plan_snapshot.value(), limiter, trim);
  if (!chain)
    return R::failure(chain.error());
  if (!limiter)
    return R::failure({ErrorCode::internal, "The planned master has no limiter"});
  auto learned = std::move(planned.value().learned);
  const bool same_chain =
      plan_snapshot.value().master_effects.size() == proposal.devices.size() &&
      std::ranges::equal(chain.value(), proposal.devices,
                         [](const core::ChainDevice& link, const core::Device& device) {
                           return link.engine->descriptor->id == device.engine;
                         });
  if (!same_chain) {
    core::GainSearch search;
    search.limiter = *limiter;
    search.trim = trim;
    search.target_lufs = goals.lufs;
    search.output_gain = snapshot.master_level;
    if (!step(rerender_share + (1.0 - rerender_share) / 2.0, "Checking the planned master"))
      return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
    auto relearned =
        core::learn_gain(*programme, std::move(chain).value(), search, stores, window, cancelled);
    if (!relearned)
      return R::failure(relearned.error());
    learned = std::move(relearned).value();
    core::apply_learned(proposal, learned);
  }

  Proposed out;
  out.proposal = std::move(proposal);
  out.learned = std::move(learned);
  out.before = core::outcome(measured.mix, reference);
  out.predicted = core::outcome(out.learned.output, reference);
  out.window = window;
  out.proposed = out.learned.window;
  auto current = current_window(measured, window);
  if (!current)
    return R::failure(current.error());
  out.current = std::move(current).value();
  out.current_lufs = window_lufs(out.current, snapshot.sample_rate);
  out.proposed_lufs = window_lufs(out.proposed, snapshot.sample_rate);
  if (!step(1.0, "Proposed"))
    return R::failure({ErrorCode::cancelled, std::string(cancelled_message)});
  return R::success(std::move(out));
}

Result<std::vector<project::Operation>> operations(const project::Snapshot& snapshot,
                                                   const core::Proposal& proposal,
                                                   const Selection& selection) {
  using R = Result<std::vector<project::Operation>>;
  const project::SignalPathId master = project::MasterSignalPath{};
  std::vector<project::Operation> out;
  // The chain as the operations leave it: each entry's engine ("" for
  // anything not built in) and whether it was claimed by a device.
  struct Entry {
    std::string engine;
    const project::EffectInsert* insert = nullptr;
    bool claimed = false;
  };
  std::vector<Entry> chain;
  for (const auto& insert : snapshot.master_effects) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor);
    chain.push_back({patch != nullptr ? patch->engine : std::string(), &insert, false});
  }
  for (std::size_t d = 0; d < proposal.devices.size(); ++d) {
    if (!selected(selection.devices, d))
      continue;
    const auto& device = proposal.devices[d];
    const auto* engine = native_dsp::effects::find_engine(device.engine);
    if (engine == nullptr)
      return R::failure({ErrorCode::not_found, "No built-in effect " + device.engine});
    const auto existing = std::ranges::find_if(chain, [&](const Entry& e) {
      return e.insert != nullptr && !e.claimed && e.engine == device.engine;
    });
    if (existing != chain.end()) {
      existing->claimed = true;
      const auto& base = std::get<project::NativeEffectPatch>(existing->insert->processor);
      auto patch = patch_for(*engine, device, &base);
      if (!patch)
        return R::failure(patch.error());
      if (!(patch.value().parameters == base.parameters))
        out.emplace_back(project::SetNativeEffectSettings{master, existing->insert->id,
                                                          std::move(patch).value()});
      if (!existing->insert->enabled)
        out.emplace_back(project::SetEffectInsertEnabled{master, existing->insert->id, true});
      continue;
    }
    auto patch = patch_for(*engine, device, nullptr);
    if (!patch)
      return R::failure(patch.error());
    // Before the first device the proposal places after this one.
    const auto rank = engine_rank(proposal, device.engine);
    const auto later = std::ranges::find_if(chain, [&](const Entry& e) {
      return !e.engine.empty() && engine_rank(proposal, e.engine) < proposal.devices.size() &&
             engine_rank(proposal, e.engine) > rank;
    });
    const auto index = static_cast<std::size_t>(later - chain.begin());
    if (chain.size() >= project::max_effect_slots_per_path)
      return R::failure({ErrorCode::resource_exhausted, "The master's chain is full"});
    out.emplace_back(project::AddNativeEffect{master, index, std::move(patch).value()});
    chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(index),
                 Entry{device.engine, nullptr, true});
  }
  bool nudged = false;
  auto rebalance = snapshot.rebalance;
  if (!rebalance.enabled)
    rebalance = project::MasterRebalance{};
  for (std::size_t n = 0; n < proposal.nudges.size(); ++n) {
    if (!selected(selection.nudges, n))
      continue;
    const auto& nudge = proposal.nudges[n];
    if (nudge.role >= project::mix_role_count)
      return R::failure({ErrorCode::invalid_argument, "No such role"});
    auto& role = rebalance.roles[nudge.role];
    role.gain_db = std::clamp(role.gain_db + nudge.gain_db, -project::rebalance_gain_limit_db,
                              project::rebalance_gain_limit_db);
    nudged = true;
  }
  if (nudged) {
    rebalance.enabled = true;
    out.emplace_back(project::SetMasterRebalance{rebalance});
  }
  return R::success(std::move(out));
}

Result<void> accept(ProjectSession& session, const core::Proposal& proposal,
                    const Selection& selection, const project::Snapshot& measured) {
  if (!mix_check::same_mix(session.snapshot(), measured))
    return Result<void>::failure(
        {ErrorCode::conflict, "The mix changed since it was measured: analyse it again"});
  auto ops = operations(session.snapshot(), proposal, selection);
  if (!ops)
    return Result<void>::failure(ops.error());
  if (ops.value().empty())
    return Result<void>::failure({ErrorCode::invalid_argument, "Nothing to accept"});
  return session.effects().apply_master_operations(std::move(ops).value());
}

Result<core::Analysis> verify(const PinnedSnapshot& pinned, std::optional<Range> range,
                              const Progress& progress) {
  if (!pinned.snapshot)
    return Result<core::Analysis>::failure({ErrorCode::invalid_argument, "No project to verify"});
  auto rendered =
      render_programme(*pinned.snapshot, pinned.reader, range, false, progress, 0.0, 1.0);
  if (!rendered)
    return Result<core::Analysis>::failure(rendered.error());
  return Result<core::Analysis>::success(std::move(rendered.value().mix));
}

// ---- Audition ------------------------------------------------------------------------

Audition::Audition(const core::Capture& current, double current_gain_db,
                   const core::Capture& proposed, double proposed_gain_db,
                   std::uint32_t sample_rate)
    : frames_(std::min(current.left.size(), proposed.left.size())),
      step_(1.0 / std::max(1.0, crossfade_seconds * static_cast<double>(sample_rate))) {
  const auto a = static_cast<float>(dsp::db_to_gain(current_gain_db));
  const auto b = static_cast<float>(dsp::db_to_gain(proposed_gain_db));
  const auto n = static_cast<std::size_t>(frames_);
  current_left_.resize(n);
  current_right_.resize(n);
  proposed_left_.resize(n);
  proposed_right_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    current_left_[i] = current.left[i] * a;
    current_right_[i] = current.right[i] * a;
    proposed_left_[i] = proposed.left[i] * b;
    proposed_right_[i] = proposed.right[i] * b;
  }
}

void Audition::choose_proposal(bool proposal) noexcept {
  proposal_.store(proposal, std::memory_order_release);
}
bool Audition::proposal_chosen() const noexcept {
  return proposal_.load(std::memory_order_acquire);
}
std::uint64_t Audition::position() const noexcept {
  return position_.load(std::memory_order_acquire);
}

void Audition::render(std::span<engine::StereoFrame> out) noexcept {
  if (frames_ == 0) {
    std::ranges::fill(out, engine::StereoFrame{0.0F, 0.0F});
    return;
  }
  const double wanted = proposal_.load(std::memory_order_acquire) ? 1.0 : 0.0;
  for (auto& frame : out) {
    blend_ = blend_ < wanted ? std::min(wanted, blend_ + step_) : std::max(wanted, blend_ - step_);
    const auto b = static_cast<float>(blend_);
    const auto a = 1.0F - b;
    const auto i = static_cast<std::size_t>(at_);
    frame.left = a * current_left_[i] + b * proposed_left_[i];
    frame.right = a * current_right_[i] + b * proposed_right_[i];
    if (++at_ == frames_)
      at_ = 0;
  }
  position_.store(at_, std::memory_order_release);
}

bool Audition::render_source(void* context, std::span<engine::StereoFrame> out,
                             engine::BlockContext) noexcept {
  static_cast<Audition*>(context)->render(out);
  return true;
}

} // namespace oscillate::application::master_assistant
