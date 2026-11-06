#include "insert_chain_view.hpp"

#include "device_strip_catalog.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "saved_processing.hpp"

#include <QString>
#include <QVariantList>

#include <algorithm>
#include <ranges>
#include <variant>

namespace oscillate::quick {
namespace {

[[nodiscard]] QString native_insert_name(project::NativeInsertKind kind) {
  switch (kind) {
  case project::NativeInsertKind::convolution:
    return QStringLiteral("Convolution reverb (unavailable)");
  case project::NativeInsertKind::effect:
    return QStringLiteral("Built-in effect");
  }
  return QStringLiteral("Unknown built-in effect");
}

} // namespace

QVariantMap insert_chain_view(const project::SignalPathProjection& path,
                              const std::vector<project::EffectInsert>& saved_effects,
                              const project::Track* track) {
  QVariantList effects;
  for (const auto& effect : path.effects) {
    const auto* native = std::get_if<project::NativeInsertKind>(&effect.identity);
    QString name = native ? native_insert_name(*native) : QStringLiteral("Missing VST3 identity");
    const auto* built_in = native && *native == project::NativeInsertKind::effect
                               ? native_dsp::effects::find_engine(effect.native_engine)
                               : nullptr;
    if (native && *native == project::NativeInsertKind::effect)
      name = built_in ? QString::fromStdString(std::string(built_in->descriptor->name))
                      : QStringLiteral("%1 (unavailable)")
                            .arg(QString::fromStdString(effect.native_engine));
    QString plugin_id;
    if (const auto* plugin_id_value = std::get_if<project::PluginInstanceId>(&effect.identity)) {
      plugin_id = QString::fromStdString(plugin_id_value->to_string());
      if (track) {
        if (const auto* plugin = detail::find_vst3(*track, *plugin_id_value)) {
          name = QString::fromStdString(plugin->name);
          if (name.isEmpty())
            name = QStringLiteral("Unnamed VST3 effect");
        }
      }
    }
    QVariantMap view{{"slotId", QString::fromStdString(effect.slot.to_string())},
                     {"pluginId", plugin_id},
                     {"name", name},
                     {"type", native ? QStringLiteral("BUILT-IN") : QStringLiteral("VST3 EFFECT")},
                     {"missing", effect.missing_plugin},
                     {"enabled", effect.enabled}};
    const auto saved = std::ranges::find(saved_effects, effect.slot, &project::EffectInsert::id);
    if (saved != saved_effects.end()) {
      if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&saved->processor)) {
        view.insert(QStringLiteral("classId"), QString::fromStdString(plugin->class_id));
        view.insert(QStringLiteral("vendor"), QString::fromStdString(plugin->vendor));
        view.insert(QStringLiteral("version"), QString::fromStdString(plugin->version));
        view.insert(QStringLiteral("bundle"), QString::fromStdString(plugin->bundle_path));
        view.insert(QStringLiteral("sha256"), QString::fromStdString(plugin->binary_sha256));
        view.insert(QStringLiteral("stateBytes"), static_cast<int>(plugin->component_state.size()));
        view.insert(QStringLiteral("parameterCount"), static_cast<int>(plugin->parameters.size()));
      }
      if (const auto* patch = std::get_if<project::NativeEffectPatch>(&saved->processor)) {
        // ADR-0043: one projection for every built-in effect engine.
        view.insert(QStringLiteral("nativeKind"), QStringLiteral("effect"));
        view.insert(QStringLiteral("engine"), QString::fromStdString(patch->engine));
        view.insert(QStringLiteral("engineName"), name);
        view.insert(QStringLiteral("engineAvailable"), built_in != nullptr);
        view.insert(QStringLiteral("readsSidechain"), built_in != nullptr && built_in->sidechain);
        view.insert(QStringLiteral("generation"), static_cast<qulonglong>(patch->generation));
        view.insert(QStringLiteral("changedParameters"),
                    static_cast<int>(patch->parameters.size()));
        const auto& style = device_strip_style(patch->engine);
        view.insert(
            QStringLiteral("accent"),
            QString::fromUtf8(style.accent.data(), static_cast<qsizetype>(style.accent.size())));
        view.insert(
            QStringLiteral("kindLabel"),
            QString::fromUtf8(style.kind.data(), static_cast<qsizetype>(style.kind.size())));
      }
    }
    effects.push_back(view);
  }

  QString source_name;
  QString source_status;
  QString source_engine;
  if (const auto* native = std::get_if<project::ProjectedNativeInstrumentSource>(&path.source)) {
    const auto* engine = native_dsp::synth::find_engine(native->engine);
    source_name = engine ? QString::fromStdString(std::string(engine->descriptor->name))
                         : QString::fromStdString(native->engine);
    if (engine)
      source_engine = QString::fromStdString(native->engine);
    if (!engine)
      source_status = QStringLiteral("Built-in synth unavailable");
  } else if (const auto* vst3 = std::get_if<project::ProjectedVst3InstrumentSource>(&path.source)) {
    source_name = QStringLiteral("VST3 instrument");
    if (track) {
      const auto* plugin = detail::find_vst3(*track, vst3->plugin);
      if (plugin && !plugin->name.empty())
        source_name = QString::fromStdString(plugin->name);
    }
    if (vst3->missing)
      source_status = QStringLiteral("Missing plugin; source unavailable");
  } else if (track && track->kind == project::TrackKind::instrument) {
    source_name = QStringLiteral("No instrument source");
  }
  QVariantMap source_plugin;
  if (track) {
    if (const auto* plugin = detail::vst3_instrument_source(*track)) {
      source_plugin = {{"pluginId", QString::fromStdString(plugin->id.to_string())},
                       {"slotId", QString()},
                       {"name", QString::fromStdString(plugin->name)},
                       {"vendor", QString::fromStdString(plugin->vendor)},
                       {"version", QString::fromStdString(plugin->version)},
                       {"classId", QString::fromStdString(plugin->class_id)},
                       {"bundle", QString::fromStdString(plugin->bundle_path)},
                       {"sha256", QString::fromStdString(plugin->binary_sha256)},
                       {"missing", plugin->missing},
                       {"stateBytes", static_cast<int>(plugin->component_state.size())},
                       {"parameterCount", static_cast<int>(plugin->parameters.size())}};
    }
  }
  const auto& source_style = device_strip_style(source_engine.toStdString());
  return QVariantMap{
      {"effects", effects},
      {"sourceName", source_name},
      {"sourceEngine", source_engine},
      {"sourceAccent", QString::fromUtf8(source_style.accent.data(),
                                         static_cast<qsizetype>(source_style.accent.size()))},
      {"sourceKind", QString::fromUtf8(source_style.kind.data(),
                                       static_cast<qsizetype>(source_style.kind.size()))},
      {"sourceStatus", source_status},
      {"sourcePlugin", source_plugin},
      {"status", QStringLiteral("Saved")}};
}

} // namespace oscillate::quick
