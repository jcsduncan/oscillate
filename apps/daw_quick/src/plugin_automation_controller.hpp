#pragma once

#include "oscillate/application/project_session.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oscillate::quick {

// Parameter metadata observed from a verified, loaded plugin host. Only a
// continuous, automatable parameter can render as live automation.
struct VerifiedPluginParameter {
  std::uint32_t id{0};
  QString name;
  bool continuous_automatable{false};
};

// UI-thread commands and projection for track-owned VST3 parameter lanes on
// the plugin selected in the VST3 editor. The bridge owns project lifecycle,
// publishes each accepted revision and reports the live graph revision.
class PluginAutomationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap view READ view NOTIFY changed)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  // nullopt while stopped; otherwise the revision the live graph renders
  // (0 while the first graph is still being prepared).
  using LiveRevision = std::function<std::optional<std::uint64_t>()>;
  using LaneView =
      std::function<QVariantMap(const project::Snapshot&, const project::AutomationLane&)>;

  PluginAutomationController(Session session, CanEdit can_edit, Report report, Commit commit,
                             LiveRevision live_revision, LaneView lane_view,
                             QObject* parent = nullptr);

  [[nodiscard]] QVariantMap view() const { return view_; }

  // Bridge-side lifecycle. Verified metadata survives editor close so lanes
  // keep their names; a project switch clears it.
  void select_plugin(project::TrackId track, project::PluginInstanceId plugin);
  void clear_selection();
  void publish_verified_parameters(project::PluginInstanceId plugin,
                                   std::vector<VerifiedPluginParameter> parameters);
  void reset_project();
  void refresh();

  // Name substring or decimal ID prefix; an exact ID match lists first. The
  // view projects a bounded page of matches and reports the overflow.
  Q_INVOKABLE void set_parameter_filter(const QString& filter);

  // Creates the lane from the saved baseline at bar 1, or selects it.
  Q_INVOKABLE void automate(quint32 parameter_id);
  Q_INVOKABLE void select_parameter(quint32 parameter_id);
  Q_INVOKABLE void show_parameters();
  Q_INVOKABLE void set_point(int zero_based_bar, double normalized_value);
  Q_INVOKABLE void set_value(const QString& point_id, double normalized_value);
  Q_INVOKABLE void remove_point(const QString& point_id);
  Q_INVOKABLE void remove_lane();

signals:
  void changed();

private:
  struct Selection {
    project::TrackId track;
    project::PluginInstanceId plugin;
    std::optional<std::uint32_t> parameter;
  };
  // Saved plugin and verified metadata for the current selection.
  struct PluginContext {
    const project::Track* track{nullptr};
    const project::Vst3PluginInstance* plugin{nullptr};
    // Host order and an ID index into the same published metadata.
    const std::vector<VerifiedPluginParameter>* host_order{nullptr};
    std::unordered_map<std::uint32_t, const VerifiedPluginParameter*> verified;
    bool is_source{false};
    bool bypassed{false};
  };
  struct LaneTarget {
    project::TrackId track;
    project::PluginInstanceId plugin;
    std::uint32_t parameter;
  };

  [[nodiscard]] std::optional<LaneTarget> lane_target() const;
  [[nodiscard]] application::ProjectSession* editable_session();
  void failure(const QString& context, const foundation::Error& error);
  void accepted(const QString& action);
  [[nodiscard]] std::optional<PluginContext> plugin_context() const;
  [[nodiscard]] QVariantMap describe(const PluginContext& context, std::uint32_t id) const;
  [[nodiscard]] QVariantMap project_view() const;

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  LiveRevision live_revision_;
  LaneView lane_view_;
  std::optional<Selection> selection_;
  QString filter_;
  std::map<project::PluginInstanceId, std::vector<VerifiedPluginParameter>> verified_;
  QVariantMap view_;
};

} // namespace oscillate::quick
