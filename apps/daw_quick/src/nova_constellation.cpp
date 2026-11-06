#include "nova_constellation.hpp"

#include "native_synth_device_controller.hpp"

#include "oscillate/native_dsp/synth/nova.hpp"

#include <QDir>
#include <QMetaObject>
#include <QSet>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <utility>

namespace oscillate::quick {
namespace {

namespace sound_map = application::sound_map;
namespace nova_morph = application::nova_morph;

constexpr std::string_view cache_directory = "/oscillate/constellation/";
constexpr std::string_view cache_extension = ".json";
constexpr std::string_view home_cache = "/.cache";

QString text(std::string_view view) {
  return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

} // namespace

NovaConstellation::NovaConstellation(NativeSynthDeviceController& controller, QString cache_file,
                                     QObject* parent)
    : QObject(parent), controller_(controller), engine_(native_dsp::synth::nova::engine()),
      cache_file_(std::move(cache_file)) {
  // A saved, renamed or deleted user preset changes the sky.
  connect(&controller_, &NativeSynthDeviceController::user_presets_changed, this, [this] {
    if (started_)
      rescan();
  });
}

NovaConstellation::~NovaConstellation() {
  library_worker_.request_stop();
  variation_worker_.request_stop();
}

QString NovaConstellation::default_cache_file() {
  QString cache = qEnvironmentVariable("XDG_CACHE_HOME");
  if (cache.isEmpty() || !QDir::isAbsolutePath(cache))
    cache = QDir::homePath() + text(home_cache);
  return cache + text(cache_directory) + text(native_dsp::synth::nova::engine().descriptor->id) +
         text(cache_extension);
}

QStringList NovaConstellation::locks() const {
  QStringList names;
  if (locks_.fx)
    names.append(text(lock_fx));
  if (locks_.envelopes)
    names.append(text(lock_envelopes));
  if (locks_.routes)
    names.append(text(lock_routes));
  return names;
}

void NovaConstellation::set_message(const QString& message) {
  if (message == message_)
    return;
  message_ = message;
  emit message_changed();
}

const NovaConstellation::Entry* NovaConstellation::find(const QString& key) const {
  for (const auto* list : {&entries_, &variations_}) {
    const auto found = std::ranges::find(*list, key, &Entry::key);
    if (found != list->end())
      return &*found;
  }
  return nullptr;
}

sound_map::Sound NovaConstellation::current_sound() const {
  return {controller_.current_values(), controller_.current_document()};
}

void NovaConstellation::start() {
  if (started_)
    return;
  started_ = true;
  rescan();
}

void NovaConstellation::rescan() {
  std::vector<Entry> fresh;
  const auto& presets = engine_.descriptor->presets;
  for (std::size_t i = 0; i < presets.size(); ++i) {
    auto sound = sound_map::factory_sound(engine_, i);
    if (!sound)
      continue;
    Entry entry;
    entry.name = text(presets[i].name);
    entry.key = text(factory_prefix) + entry.name;
    entry.category = text(presets[i].category);
    entry.preset = static_cast<int>(i);
    entry.sound = std::move(sound).value();
    entry.phrase = sound_map::register_for(presets[i].category);
    fresh.push_back(std::move(entry));
  }
  for (const auto& item : controller_.user_presets()) {
    const QString name = item.toMap().value(QStringLiteral("name")).toString();
    const auto file = controller_.user_preset_text(name);
    if (!file)
      continue;
    auto sound = sound_map::sound_from_preset_text(
        engine_, std::string_view(file->constData(), static_cast<std::size_t>(file->size())));
    if (!sound)
      continue; // Not a Nova preset this engine can read; it has no star.
    Entry entry;
    entry.name = name;
    entry.key = text(user_prefix) + name;
    entry.category = text(user_category);
    entry.user = true;
    entry.sound = std::move(sound).value();
    fresh.push_back(std::move(entry));
  }
  std::vector<std::size_t> pending;
  analysed_ = 0;
  for (std::size_t i = 0; i < fresh.size(); ++i) {
    auto& entry = fresh[i];
    const auto key = sound_map::sound_key(engine_, entry.sound, entry.phrase);
    entry.hash = key ? key.value() : 0;
    // What was measured before, by sound, is kept.
    const auto known = std::ranges::find_if(entries_, [&](const Entry& old) {
      return old.done && old.hash == entry.hash && old.phrase == entry.phrase;
    });
    if (key && known != entries_.end()) {
      entry.done = true;
      entry.features = known->features;
    } else if (!key) {
      entry.done = true; // Invalid for the engine: never a star.
    }
    if (entry.done)
      ++analysed_;
    else
      pending.push_back(i);
  }
  entries_ = std::move(fresh);
  total_ = static_cast<int>(entries_.size());
  ++generation_;
  place();
  emit progress_changed();
  if (!pending.empty())
    listen(std::move(pending));
}

void NovaConstellation::listen(std::vector<std::size_t> pending) {
  struct Work {
    std::size_t index = 0;
    std::uint64_t hash = 0;
    sound_map::Job job;
  };
  std::vector<Work> work;
  work.reserve(pending.size());
  for (const auto index : pending)
    work.push_back({index, entries_[index].hash, {entries_[index].sound, entries_[index].phrase}});
  std::vector<std::uint64_t> keys;
  keys.reserve(entries_.size());
  for (const auto& entry : entries_)
    keys.push_back(entry.hash);
  const std::filesystem::path file(cache_file_.toStdString());
  const auto generation = generation_;
  const auto* engine = &engine_;
  // Assigning stops and joins the previous listener first.
  library_worker_ = std::jthread([this, engine, work = std::move(work), keys = std::move(keys),
                                  file, generation](std::stop_token stop) {
    const auto post = [this, generation](std::size_t index,
                                         std::optional<sound_map::Features> features) {
      QMetaObject::invokeMethod(
          this, [this, generation, index, features] { landed(generation, index, features); },
          Qt::QueuedConnection);
    };
    auto cache = sound_map::FeatureCache::load(file);
    std::vector<sound_map::Job> jobs;
    std::vector<std::size_t> owners;
    for (std::size_t i = 0; i < work.size(); ++i) {
      if (const auto cached = cache.find(work[i].hash)) {
        post(work[i].index, cached);
        continue;
      }
      jobs.push_back(work[i].job);
      owners.push_back(i);
    }
    std::mutex guard;
    sound_map::analyse_all(
        *engine, jobs, sound_map::analysis_threads(),
        [&](std::size_t job, const foundation::Result<sound_map::Features>& result) {
          if (stop.stop_requested())
            return;
          const auto& item = work[owners[job]];
          std::optional<sound_map::Features> features;
          if (result) {
            features = result.value();
            const std::lock_guard lock(guard);
            cache.insert(item.hash, *features);
          }
          post(item.index, features);
        },
        [&stop] { return stop.stop_requested(); });
    // A finished listen forgets sounds the library no longer holds.
    if (!stop.stop_requested())
      cache.retain(keys);
    (void)cache.save();
  });
}

void NovaConstellation::landed(std::uint64_t generation, std::size_t index,
                               std::optional<sound_map::Features> features) {
  if (generation != generation_ || index >= entries_.size() || entries_[index].done)
    return;
  entries_[index].done = true;
  entries_[index].features = features;
  ++analysed_;
  place();
  emit progress_changed();
}

void NovaConstellation::landed_variation(std::uint64_t generation, std::size_t index,
                                         std::optional<sound_map::Features> features) {
  if (generation != variation_generation_ || index >= variations_.size() || variations_[index].done)
    return;
  variations_[index].done = true;
  variations_[index].features = features;
  if (++variations_landed_ == static_cast<int>(variations_.size()))
    mutating_ = false;
  place();
}

QVariantMap NovaConstellation::star(const Entry& entry) const {
  const auto at = sky_.place(*entry.features);
  return {{QStringLiteral("key"), entry.key},
          {QStringLiteral("name"), entry.name},
          {QStringLiteral("category"), entry.category},
          {QStringLiteral("user"), entry.user},
          {QStringLiteral("variation"), entry.key.startsWith(text(variation_prefix))},
          {QStringLiteral("x"), at.x},
          {QStringLiteral("y"), at.y},
          {QStringLiteral("movement"), at.movement},
          {QStringLiteral("width"), at.width}};
}

void NovaConstellation::place() {
  std::vector<sound_map::Features> library;
  for (const auto& entry : entries_)
    if (entry.features)
      library.push_back(*entry.features);
  sky_ = sound_map::Sky(library);
  stars_.clear();
  for (const auto& entry : entries_)
    if (entry.features && entry.features->audible)
      stars_.append(star(entry));
  variation_stars_.clear();
  for (const auto& entry : variations_)
    if (entry.features && entry.features->audible)
      variation_stars_.append(star(entry));
  emit stars_changed();
  emit variations_changed();
}

void NovaConstellation::begin_morph() {
  if (morphing_)
    return;
  base_ = current_sound();
  sent_document_ = base_.document;
  controller_.begin_state_gesture();
  morphing_ = controller_.state_gesture();
  if (!morphing_) {
    set_message(QStringLiteral("This synth cannot be edited now"));
    return;
  }
  emit morphing_changed();
}

bool NovaConstellation::morph(const QVariantList& points) {
  if (!controller_.editable()) {
    set_message(QStringLiteral("This synth cannot be edited now"));
    return false;
  }
  std::vector<const Entry*> chosen;
  std::vector<nova_morph::Part> parts;
  for (const auto& point : points) {
    const auto map = point.toMap();
    const auto* entry = find(map.value(QStringLiteral("key")).toString());
    if (entry == nullptr || parts.size() == nova_morph::max_parts)
      continue;
    chosen.push_back(entry);
    parts.push_back({&entry->sound, map.value(QStringLiteral("weight")).toDouble()});
  }
  if (parts.empty())
    return false;
  const bool single = !morphing_;
  auto result =
      nova_morph::morph(controller_.table(), parts, single ? current_sound() : base_, locks_);
  if (!result) {
    set_message(QString::fromStdString(result.error().message));
    return false;
  }
  set_message({});
  const auto* lead = chosen[nova_morph::heaviest(parts)];
  category_ = lead->category;

  // The readout, heaviest first.
  double total = 0.0;
  for (const auto& part : parts)
    total += part.weight;
  std::vector<std::size_t> order(parts.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::ranges::stable_sort(
      order, [&](std::size_t a, std::size_t b) { return parts[a].weight > parts[b].weight; });
  blend_.clear();
  for (const auto i : order)
    blend_.append(QVariantMap{{QStringLiteral("key"), chosen[i]->key},
                              {QStringLiteral("name"), chosen[i]->name},
                              {QStringLiteral("category"), chosen[i]->category},
                              {QStringLiteral("weight"), parts[i].weight / total}});

  if (single) {
    controller_.apply_state(result.value().values, std::move(result.value().document));
  } else {
    // The document goes only when it changes (a new heaviest star, or a
    // lock); the values glide on the live path.
    const bool changed = result.value().document != sent_document_;
    if (changed)
      sent_document_ = std::move(result.value().document);
    controller_.update_state(result.value().values, changed ? &sent_document_ : nullptr);
  }
  if (!selected_.isEmpty()) {
    selected_.clear();
    emit selected_changed();
  }
  emit blend_changed();
  return true;
}

void NovaConstellation::end_morph() {
  if (!morphing_)
    return;
  controller_.end_state_gesture();
  morphing_ = false;
  emit morphing_changed();
}

void NovaConstellation::set_lock(const QString& name, bool locked) {
  bool* flag = nullptr;
  if (name == text(lock_fx))
    flag = &locks_.fx;
  else if (name == text(lock_envelopes))
    flag = &locks_.envelopes;
  else if (name == text(lock_routes))
    flag = &locks_.routes;
  if (flag == nullptr || *flag == locked)
    return;
  *flag = locked;
  emit locks_changed();
}

bool NovaConstellation::is_locked(const QString& name) const { return locks().contains(name); }

void NovaConstellation::mutate() {
  const auto base = current_sound();
  QString category = category_;
  if (const int preset = controller_.preset_index(); preset >= 0)
    category = text(engine_.descriptor->presets[static_cast<std::size_t>(preset)].category);
  if (category.isEmpty())
    category = text(user_category);
  const auto phrase = sound_map::register_for(category.toStdString());
  // Same sound and press, same variations; each press scatters anew.
  const auto key = sound_map::sound_key(engine_, base, phrase);
  const std::uint64_t seed = (key ? key.value() : 0) + ++mutations_;
  auto sounds = nova_morph::mutate(controller_.table(), base, seed, locks_);
  variations_.clear();
  std::vector<sound_map::Job> jobs;
  for (std::size_t i = 0; i < sounds.size(); ++i) {
    Entry entry;
    const auto number = QString::number(static_cast<qulonglong>(i + 1));
    entry.key = text(variation_prefix) + number;
    entry.name = text(variation_name) + number;
    entry.category = category;
    entry.phrase = phrase;
    entry.sound = std::move(sounds[i]);
    jobs.push_back({entry.sound, phrase});
    variations_.push_back(std::move(entry));
  }
  variations_landed_ = 0;
  mutating_ = !variations_.empty();
  const auto generation = ++variation_generation_;
  place();
  const auto* engine = &engine_;
  // Assigning stops and joins the previous scatter first.
  variation_worker_ =
      std::jthread([this, engine, jobs = std::move(jobs), generation](std::stop_token stop) {
        sound_map::analyse_all(
            *engine, jobs, sound_map::analysis_threads(),
            [&](std::size_t index, const foundation::Result<sound_map::Features>& result) {
              if (stop.stop_requested())
                return;
              std::optional<sound_map::Features> features;
              if (result)
                features = result.value();
              QMetaObject::invokeMethod(
                  this,
                  [this, generation, index, features] {
                    landed_variation(generation, index, features);
                  },
                  Qt::QueuedConnection);
            },
            [&stop] { return stop.stop_requested(); });
      });
}

bool NovaConstellation::apply_star(const QString& key) {
  const auto* entry = find(key);
  if (entry == nullptr)
    return false;
  end_morph();
  // Copied: loading may rescan, which replaces the entries.
  const int preset = entry->preset;
  const bool user = entry->user;
  const QString name = entry->name;
  const QString category = entry->category;
  const auto sound = entry->sound;
  bool applied = true;
  if (preset >= 0) {
    controller_.apply_preset(preset);
    applied = controller_.preset_index() == preset;
  } else if (user) {
    applied = controller_.apply_user_preset(name);
  } else {
    controller_.apply_state(sound.values, sound.document);
  }
  if (!applied) {
    set_message(QStringLiteral("Cannot load ") + name);
    return false;
  }
  set_message({});
  category_ = category;
  selected_ = key;
  blend_.clear();
  emit selected_changed();
  emit blend_changed();
  return true;
}

QString NovaConstellation::next_blend_name() const {
  QSet<QString> taken;
  for (const auto& item : controller_.user_presets())
    taken.insert(item.toMap().value(QStringLiteral("name")).toString());
  for (qulonglong n = 1;; ++n) {
    const QString name = text(blend_name) + QString::number(n);
    if (!taken.contains(name))
      return name;
  }
}

QString NovaConstellation::keep(const QString& name) {
  const QString chosen = name.trimmed().isEmpty() ? next_blend_name() : name.trimmed();
  end_morph();
  // Saving announces the new preset; the sky then listens to it.
  if (!controller_.save_user_preset(chosen)) {
    set_message(QStringLiteral("Cannot save ") + chosen);
    return {};
  }
  set_message({});
  selected_ = text(user_prefix) + chosen;
  emit selected_changed();
  return chosen;
}

} // namespace oscillate::quick
