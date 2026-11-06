#include "plugin_automation_controller.hpp"
#include "saved_processing.hpp"

#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace oscillate::quick {
namespace {

// Rows are a bounded page of the filtered set; the view reports the total and
// match counts so a large plugin is never silently truncated.
constexpr qsizetype max_projected_parameters = 64;
constexpr qsizetype max_projected_lanes = 64;
constexpr qsizetype max_parameter_filter_length = 128;
constexpr std::size_t max_verified_plugins = 256;

[[nodiscard]] const project::AutomationLane* parameter_lane(const project::Track& track,
                                                            project::PluginInstanceId plugin,
                                                            std::uint32_t parameter) {
  const auto lane = std::ranges::find_if(track.automation, [&](const auto& candidate) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&candidate.target);
    return target != nullptr && target->plugin == plugin && target->parameter_id == parameter;
  });
  return lane == track.automation.end() ? nullptr : &*lane;
}

[[nodiscard]] const project::EffectInsert* effect_slot(const project::Track& track,
                                                       project::PluginInstanceId plugin) {
  const auto slot = std::ranges::find_if(track.effects, [plugin](const auto& candidate) {
    const auto* saved = detail::vst3_effect(candidate);
    return saved != nullptr && saved->id == plugin;
  });
  return slot == track.effects.end() ? nullptr : &*slot;
}

[[nodiscard]] QString
parameter_name(const std::unordered_map<std::uint32_t, const VerifiedPluginParameter*>& verified,
               std::uint32_t id) {
  const auto found = verified.find(id);
  if (found != verified.end() && !found->second->name.isEmpty())
    return found->second->name;
  return QStringLiteral("Parameter %1").arg(id);
}

// Verified host order first, then saved references and lane targets that the
// host did not report, each ID once.
[[nodiscard]] std::vector<std::uint32_t>
parameter_ids(const project::Track& track, const project::Vst3PluginInstance& plugin,
              const std::vector<VerifiedPluginParameter>* verified) {
  std::vector<std::uint32_t> ids;
  std::unordered_set<std::uint32_t> seen;
  const auto add = [&](std::uint32_t id) {
    if (seen.insert(id).second)
      ids.push_back(id);
  };
  if (verified != nullptr) {
    for (const auto& item : *verified)
      add(item.id);
  }
  for (const auto& reference : plugin.parameters)
    add(reference.id);
  for (const auto& lane : track.automation) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
    if (target != nullptr && target->plugin == plugin.id)
      add(target->parameter_id);
  }
  return ids;
}

[[nodiscard]] QString result_status(qsizetype shown, qsizetype matches, qsizetype total,
                                    const QString& filter) {
  if (total == 0)
    return QStringLiteral("This plugin reports no parameters");
  if (filter.isEmpty()) {
    if (shown == total)
      return QStringLiteral("%1 parameters").arg(total);
    return QStringLiteral("Showing first %1 of %2 parameters. Search by name or ID to find the "
                          "rest")
        .arg(shown)
        .arg(total);
  }
  if (matches == 0)
    return QStringLiteral("No parameter name or ID matches “%1”").arg(filter);
  if (shown == matches)
    return QStringLiteral("%1 of %2 parameters match “%3”").arg(matches).arg(total).arg(filter);
  return QStringLiteral("Showing first %1 of %2 matches for “%3”. Refine the search")
      .arg(shown)
      .arg(matches)
      .arg(filter);
}

} // namespace

PluginAutomationController::PluginAutomationController(Session session, CanEdit can_edit,
                                                       Report report, Commit commit,
                                                       LiveRevision live_revision,
                                                       LaneView lane_view, QObject* parent)
    : QObject(parent), session_(std::move(session)), can_edit_(std::move(can_edit)),
      report_(std::move(report)), commit_(std::move(commit)),
      live_revision_(std::move(live_revision)), lane_view_(std::move(lane_view)) {
  view_ = project_view();
}

void PluginAutomationController::select_plugin(project::TrackId track,
                                               project::PluginInstanceId plugin) {
  selection_ = Selection{track, plugin, std::nullopt};
  filter_.clear();
  refresh();
}

void PluginAutomationController::clear_selection() {
  selection_.reset();
  filter_.clear();
  refresh();
}

void PluginAutomationController::publish_verified_parameters(
    project::PluginInstanceId plugin, std::vector<VerifiedPluginParameter> parameters) {
  if (verified_.size() >= max_verified_plugins && !verified_.contains(plugin))
    verified_.clear();
  verified_.insert_or_assign(plugin, std::move(parameters));
  refresh();
}

void PluginAutomationController::reset_project() {
  selection_.reset();
  verified_.clear();
  filter_.clear();
  refresh();
}

void PluginAutomationController::refresh() {
  auto next = project_view();
  if (next == view_)
    return;
  view_ = std::move(next);
  emit changed();
}

void PluginAutomationController::set_parameter_filter(const QString& filter) {
  auto next = filter.trimmed().left(max_parameter_filter_length);
  if (next == filter_)
    return;
  filter_ = std::move(next);
  refresh();
}

std::optional<PluginAutomationController::PluginContext>
PluginAutomationController::plugin_context() const {
  auto* current = session_();
  if (current == nullptr || !selection_)
    return std::nullopt;
  const auto& snapshot = current->snapshot();
  const auto track = std::ranges::find(snapshot.tracks, selection_->track, &project::Track::id);
  if (track == snapshot.tracks.end())
    return std::nullopt;
  const auto* source = detail::vst3_instrument_source(*track);
  const bool is_source = source != nullptr && source->id == selection_->plugin &&
                         track->kind == project::TrackKind::instrument;
  const auto* slot = is_source ? nullptr : effect_slot(*track, selection_->plugin);
  const auto* plugin = is_source ? source : slot ? detail::vst3_effect(*slot) : nullptr;
  if (plugin == nullptr)
    return std::nullopt;
  PluginContext context{.track = &*track,
                        .plugin = plugin,
                        .host_order = nullptr,
                        .verified = {},
                        .is_source = is_source,
                        .bypassed = slot != nullptr && !slot->enabled};
  if (const auto found = verified_.find(plugin->id); found != verified_.end()) {
    context.host_order = &found->second;
    context.verified.reserve(found->second.size());
    for (const auto& item : found->second)
      context.verified.emplace(item.id, &item);
  }
  return context;
}

QVariantMap PluginAutomationController::describe(const PluginContext& context,
                                                 std::uint32_t id) const {
  const auto& plugin = *context.plugin;
  const auto reference =
      std::ranges::find(plugin.parameters, id, &project::Vst3ParameterReference::id);
  const bool has_baseline = reference != plugin.parameters.end();
  const auto metadata = context.verified.find(id);
  const bool known = metadata != context.verified.end();
  const bool automatable = known && metadata->second->continuous_automatable;
  const auto* lane = parameter_lane(*context.track, plugin.id, id);
  QString note;
  if (!has_baseline)
    note = QStringLiteral("Save a value first; it becomes the lane baseline");
  else if (known && !automatable)
    note = QStringLiteral("Not a continuous automatable parameter");
  else if (context.bypassed)
    note = QStringLiteral("Enable this effect to add automation");
  else if (!known)
    note = QStringLiteral("Unverified saved reference · ID %1").arg(id);
  return QVariantMap{
      {"id", id},
      {"name", parameter_name(context.verified, id)},
      {"verified", known},
      {"automatable", automatable},
      {"hasBaseline", has_baseline},
      {"baseline", has_baseline ? QVariant(reference->normalized_value) : QVariant()},
      {"lanePresent", lane != nullptr},
      {"pointCount", lane ? static_cast<int>(lane->points.size()) : 0},
      {"canAutomate",
       lane != nullptr || (has_baseline && !context.bypassed && (!known || automatable))},
      {"note", note}};
}

QVariantMap PluginAutomationController::project_view() const {
  QVariantMap result{{"active", false}, {"parameterSelected", false}};
  const auto context = plugin_context();
  if (!context)
    return result;
  const auto& snapshot = session_()->snapshot();
  const auto& track = *context->track;
  const auto& plugin = *context->plugin;
  const auto ids = parameter_ids(track, plugin, context->host_order);

  QVariantList lanes;
  int lane_count = 0;
  for (const auto& lane : track.automation) {
    const auto* target = std::get_if<project::Vst3ParameterTarget>(&lane.target);
    if (target == nullptr || target->plugin != plugin.id)
      continue;
    ++lane_count;
    if (lanes.size() < max_projected_lanes)
      lanes.push_back(QVariantMap{{"parameterId", target->parameter_id},
                                  {"name", parameter_name(context->verified, target->parameter_id)},
                                  {"laneId", QString::fromStdString(lane.id.to_string())},
                                  {"pointCount", static_cast<int>(lane.points.size())},
                                  {"selected", selection_->parameter == target->parameter_id}});
  }

  // An exact ID match leads; other matches keep host order.
  std::vector<std::uint32_t> matches;
  bool ok = false;
  const auto exact = filter_.toUInt(&ok);
  for (const auto id : ids) {
    if (filter_.isEmpty() || QString::number(id).startsWith(filter_) ||
        parameter_name(context->verified, id).contains(filter_, Qt::CaseInsensitive)) {
      if (ok && id == exact)
        matches.insert(matches.begin(), id);
      else
        matches.push_back(id);
    }
  }
  QVariantList parameters;
  for (const auto id : matches) {
    if (parameters.size() == max_projected_parameters)
      break;
    parameters.push_back(describe(*context, id));
  }
  const auto total = static_cast<qsizetype>(ids.size());
  const auto match_count = static_cast<qsizetype>(matches.size());

  result.insert({{"active", true},
                 {"trackId", QString::fromStdString(track.id.to_string())},
                 {"pluginId", QString::fromStdString(plugin.id.to_string())},
                 {"pluginName", QString::fromStdString(plugin.name)},
                 {"role", context->is_source ? QStringLiteral("source") : QStringLiteral("effect")},
                 {"missing", plugin.missing},
                 {"bypassed", context->bypassed},
                 {"verified", context->host_order != nullptr},
                 {"parameters", parameters},
                 {"parameterCount", static_cast<int>(total)},
                 {"filter", filter_},
                 {"matchCount", static_cast<int>(match_count)},
                 {"resultsLimited", parameters.size() < match_count},
                 {"resultStatus", result_status(parameters.size(), match_count, total, filter_)},
                 {"lanes", lanes},
                 {"laneCount", lane_count}});
  if (!selection_->parameter)
    return result;

  const auto parameter = *selection_->parameter;
  auto selected = describe(*context, parameter);
  const auto* lane = parameter_lane(track, plugin.id, parameter);
  QVariantMap lane_view{{"present", false},
                        {"laneId", QString()},
                        {"timebase", QStringLiteral("Musical")},
                        {"pointCount", 0},
                        {"points", QVariantList{}}};
  if (lane != nullptr)
    lane_view.insert(lane_view_(snapshot, *lane));

  QString state;
  QString status;
  const auto revision = snapshot.revision;
  if (lane == nullptr) {
    state = QStringLiteral("none");
    status = QStringLiteral("No saved lane. Add a point to create one from the saved baseline.");
  } else if (plugin.missing) {
    state = QStringLiteral("missing");
    status = QStringLiteral("Saved lane is not rendered: the plugin is missing. Relink it to "
                            "use this automation.");
  } else if (context->bypassed) {
    state = QStringLiteral("bypassed");
    status = QStringLiteral("Saved lane is not rendered while this effect is bypassed.");
  } else if (!selected.value(QStringLiteral("verified")).toBool()) {
    state = QStringLiteral("unverified");
    status = context->is_source
                 ? QStringLiteral("Saved lane; this instrument parameter is not verified "
                                  "here, so it is not shown as active automation.")
                 : QStringLiteral("Saved lane; scan the plugin and open its editor to "
                                  "verify this parameter before relying on it.");
  } else if (!selected.value(QStringLiteral("automatable")).toBool()) {
    state = QStringLiteral("unsupported");
    status = QStringLiteral("Saved lane is not rendered: the plugin reports this parameter as "
                            "stepped or not automatable.");
  } else if (const auto live = live_revision_(); !live) {
    state = QStringLiteral("stopped");
    status = QStringLiteral("Saved in revision %1; applies when playback starts.").arg(revision);
  } else if (*live == revision) {
    state = QStringLiteral("active");
    status = QStringLiteral("Active in live audio at revision %1.").arg(revision);
  } else {
    state = QStringLiteral("pending");
    status = QStringLiteral("Saved in revision %1 but not active: live audio still plays "
                            "revision %2. Stop and Play to apply.")
                 .arg(revision)
                 .arg(*live);
  }
  result.insert({{"parameterSelected", true},
                 {"parameter", selected},
                 {"lane", lane_view},
                 {"renderState", state},
                 {"renderStatus", status}});
  return result;
}

application::ProjectSession* PluginAutomationController::editable_session() {
  if (auto* project = session_(); project != nullptr && can_edit_())
    return project;
  report_(QStringLiteral("Finish the current project operation before editing automation"));
  return nullptr;
}

void PluginAutomationController::failure(const QString& context, const foundation::Error& error) {
  report_(QStringLiteral("%1: %2").arg(context, QString::fromStdString(error.message)));
}

void PluginAutomationController::accepted(const QString& action) {
  const auto revision = session_()->snapshot().revision;
  commit_();
  refresh();
  report_(live_revision_()
              ? QStringLiteral("%1 · revision %2 saved; Stop and Play to apply it to live audio")
                    .arg(action)
                    .arg(revision)
              : QStringLiteral("%1 · revision %2").arg(action).arg(revision));
}

std::optional<PluginAutomationController::LaneTarget>
PluginAutomationController::lane_target() const {
  if (!selection_ || !selection_->parameter)
    return std::nullopt;
  return LaneTarget{selection_->track, selection_->plugin, *selection_->parameter};
}

void PluginAutomationController::select_parameter(quint32 parameter_id) {
  if (!selection_)
    return;
  selection_->parameter = parameter_id;
  refresh();
}

void PluginAutomationController::show_parameters() {
  if (!selection_)
    return;
  selection_->parameter.reset();
  refresh();
}

void PluginAutomationController::automate(quint32 parameter_id) {
  if (!selection_) {
    report_(QStringLiteral("Open a VST3 plugin before automating a parameter"));
    return;
  }
  const auto plugin = *selection_;
  // Look up the full plugin set, not the filtered page, so a parameter that the
  // current search hides is still described by its stable ID.
  const auto context = plugin_context();
  const auto ids = context ? parameter_ids(*context->track, *context->plugin, context->host_order)
                           : std::vector<std::uint32_t>{};
  if (std::ranges::find(ids, parameter_id) == ids.end()) {
    report_(QStringLiteral("Parameter %1 is not available on this plugin").arg(parameter_id));
    return;
  }
  const auto description = describe(*context, parameter_id);
  if (description.value(QStringLiteral("lanePresent")).toBool()) {
    select_parameter(parameter_id);
    return;
  }
  if (!description.value(QStringLiteral("canAutomate")).toBool()) {
    report_(QStringLiteral("%1: %2").arg(description.value(QStringLiteral("name")).toString(),
                                         description.value(QStringLiteral("note")).toString()));
    return;
  }
  auto* current = editable_session();
  if (current == nullptr)
    return;
  const auto result = current->parameter_automation().set_vst3_parameter_automation_point(
      plugin.track, plugin.plugin, parameter_id, 0,
      description.value(QStringLiteral("baseline")).toDouble());
  if (!result)
    return failure(QStringLiteral("Plugin automation"), result.error());
  selection_ = Selection{plugin.track, plugin.plugin, parameter_id};
  accepted(QStringLiteral("Plugin automation lane created from the saved baseline"));
}

void PluginAutomationController::set_point(int zero_based_bar, double normalized_value) {
  const auto target = lane_target();
  if (!target) {
    report_(QStringLiteral("Select a plugin parameter to automate"));
    return;
  }
  if (zero_based_bar < 0 || zero_based_bar >= 65'536 || !std::isfinite(normalized_value) ||
      normalized_value < 0.0 || normalized_value > 1.0) {
    report_(QStringLiteral("Plugin point needs a bar from 1 to 65536 and a value from 0 to 1"));
    return;
  }
  auto* current = editable_session();
  if (current == nullptr)
    return;
  const auto result = current->parameter_automation().set_vst3_parameter_automation_point(
      target->track, target->plugin, target->parameter, zero_based_bar, normalized_value);
  if (!result)
    return failure(QStringLiteral("Plugin automation"), result.error());
  accepted(QStringLiteral("Plugin automation point saved"));
}

void PluginAutomationController::set_value(const QString& point_id, double normalized_value) {
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  const auto target = lane_target();
  if (!target || !point || !std::isfinite(normalized_value) || normalized_value < 0.0 ||
      normalized_value > 1.0) {
    report_(QStringLiteral("Select a plugin point and a value from 0 to 1"));
    return;
  }
  auto* current = editable_session();
  if (current == nullptr)
    return;
  const auto result = current->parameter_automation().set_vst3_parameter_automation_value(
      target->track, target->plugin, target->parameter, point.value(), normalized_value);
  if (!result)
    return failure(QStringLiteral("Plugin automation"), result.error());
  accepted(QStringLiteral("Plugin automation value changed"));
}

void PluginAutomationController::remove_point(const QString& point_id) {
  const auto point = project::AutomationPointId::parse(point_id.toStdString());
  const auto target = lane_target();
  if (!target || !point) {
    report_(QStringLiteral("Select a plugin point to remove"));
    return;
  }
  auto* current = editable_session();
  if (current == nullptr)
    return;
  const auto result = current->parameter_automation().remove_vst3_parameter_automation_point(
      target->track, target->plugin, target->parameter, point.value());
  if (!result)
    return failure(QStringLiteral("Plugin automation"), result.error());
  accepted(QStringLiteral("Plugin automation point removed"));
}

void PluginAutomationController::remove_lane() {
  const auto target = lane_target();
  if (!target) {
    report_(QStringLiteral("Select a plugin lane to remove"));
    return;
  }
  auto* current = editable_session();
  if (current == nullptr)
    return;
  const auto result = current->parameter_automation().remove_vst3_parameter_automation_lane(
      target->track, target->plugin, target->parameter);
  if (!result)
    return failure(QStringLiteral("Plugin automation"), result.error());
  accepted(QStringLiteral("Plugin automation lane removed"));
}

} // namespace oscillate::quick
