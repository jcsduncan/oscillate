#include "anvil_panel_model.hpp"

#include "native_synth_document_model.hpp"

#include "oscillate/native_dsp/synth/anvil.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace oscillate::quick {

QVariantList anvil_peaks(const std::vector<float>& samples, std::size_t first, std::size_t count,
                         int columns) {
  QVariantList peaks;
  if (columns <= 0)
    return peaks;
  peaks.reserve(columns);
  const auto end = std::min(samples.size(), first + count);
  for (int column = 0; column < columns; ++column) {
    const auto from =
        first + count * static_cast<std::size_t>(column) / static_cast<std::size_t>(columns);
    const auto to = std::max(from + 1, first + count * static_cast<std::size_t>(column + 1) /
                                                   static_cast<std::size_t>(columns));
    float peak = 0.0F;
    for (auto i = from; i < to && i < end; ++i)
      peak = std::max(peak, std::abs(samples[i]));
    peaks.push_back(static_cast<double>(std::min(peak, 1.0F)));
  }
  return peaks;
}

AnvilPanelModel::AnvilPanelModel(NativeSynthDeviceController& controller, MediaLoader load_media,
                                 std::uint32_t sample_rate, QObject* parent)
    : QObject(parent), controller_(controller), load_media_(std::move(load_media)),
      sample_rate_(sample_rate) {
  // A drag edits many times a frame; one render serves them all.
  timer_.setSingleShot(true);
  timer_.setInterval(16);
  connect(&timer_, &QTimer::timeout, this, &AnvilPanelModel::start_render);
  connect(&controller_, &NativeSynthDeviceController::values_changed, this,
          &AnvilPanelModel::schedule);
  if (auto* document = controller_.document_model()) {
    connect(document, &NativeSynthDocumentModel::curves_changed, this, &AnvilPanelModel::schedule);
    connect(document, &NativeSynthDocumentModel::media_changed, this, &AnvilPanelModel::schedule);
  }
  render();
  worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

AnvilPanelModel::~AnvilPanelModel() {
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void AnvilPanelModel::set_tempo(double bpm) {
  if (!std::isfinite(bpm) || bpm <= 0.0 || bpm == tempo_)
    return;
  tempo_ = bpm;
  schedule();
}

void AnvilPanelModel::schedule() {
  if (!timer_.isActive())
    timer_.start();
}

AnvilPanelModel::Job AnvilPanelModel::take_job() {
  timer_.stop();
  const auto* document = controller_.document_model();
  const project::NativeSynthDocument empty;
  const auto& doc = document != nullptr ? document->document() : empty;
  if (doc.media != media_slots_) {
    media_slots_ = doc.media;
    media_ = load_media_ && !doc.media.empty() ? load_media_(doc) : nullptr;
  }
  return Job{controller_.current_values(), doc, media_, sample_rate_, tempo_, ++serial_};
}

void AnvilPanelModel::render() {
  const auto job = take_job();
  apply(native_dsp::synth::anvil::render_preview(std::span<const double>(job.values), job.document,
                                                 job.media.get(), job.sample_rate, job.tempo),
        job.serial);
}

void AnvilPanelModel::start_render() {
  {
    const std::scoped_lock lock(mutex_);
    pending_ = take_job();
  }
  wake_.notify_one();
}

void AnvilPanelModel::run(const std::stop_token& stop) {
  while (true) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      if (!wake_.wait(lock, stop, [this] { return pending_.has_value(); }))
        return;
      job = std::move(*pending_);
      pending_.reset();
    }
    auto preview =
        native_dsp::synth::anvil::render_preview(std::span<const double>(job.values), job.document,
                                                 job.media.get(), job.sample_rate, job.tempo);
    // Posted events die with the model, so a late result is never applied.
    QMetaObject::invokeMethod(
        this, [this, preview = std::move(preview), serial = job.serial] { apply(preview, serial); },
        Qt::QueuedConnection);
  }
}

void AnvilPanelModel::apply(const native_dsp::synth::anvil::Preview& preview,
                            std::uint64_t serial) {
  // Only the newest edit's render is shown.
  if (serial != serial_)
    return;
  const auto total = preview.master.size();
  const auto kick_frames = std::min(preview.kick_frames, total);
  kick_ = anvil_peaks(preview.master, 0, kick_frames, kick_columns);
  beat_ = anvil_peaks(preview.master, 0, total, beat_columns);
  rumble_ = anvil_peaks(preview.rumble, 0, std::min(total, preview.rumble.size()), beat_columns);
  kick_fraction_ = total > 0 ? static_cast<double>(kick_frames) / static_cast<double>(total) : 1.0;
  kick_seconds_ = static_cast<double>(kick_frames) / static_cast<double>(sample_rate_);
  tail_hz_ = preview.tail_hz;
  peak_db_ = preview.peak_db;
  ++revision_;
  emit changed();
}

} // namespace oscillate::quick
