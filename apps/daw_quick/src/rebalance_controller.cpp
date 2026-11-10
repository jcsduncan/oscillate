#include "rebalance_controller.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/application/role_listening.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/rebalance.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/render_graph/rebalance.hpp"
#include "track_routing_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace oscillate::quick {
namespace {

using project::MixRole;
using project::RoleBalance;

// How a parameter's knob position maps to its value: dB linearly,
// frequencies and Q on a log scale.
enum class Scale : std::uint8_t { linear, log };

struct Field {
  std::string_view id;
  std::string_view name;
  std::string_view group;
  double RoleBalance::* member;
  double minimum;
  double maximum;
  double fallback;
  Scale scale;
  std::string_view unit;
};

constexpr double limit_db = project::rebalance_gain_limit_db;
const std::array<Field, 8> fields{{
    {"gain", "Gain", "Role", &RoleBalance::gain_db, -limit_db, limit_db, 0.0, Scale::linear, "dB"},
    {"low", "Gain", "Low", &RoleBalance::low_db, -limit_db, limit_db, 0.0, Scale::linear, "dB"},
    {"lowHz", "Freq", "Low", &RoleBalance::low_hz, project::rebalance_low_min_hz,
     project::rebalance_low_max_hz, project::rebalance_low_default_hz, Scale::log, "Hz"},
    {"mid", "Gain", "Mid", &RoleBalance::mid_db, -limit_db, limit_db, 0.0, Scale::linear, "dB"},
    {"midHz", "Freq", "Mid", &RoleBalance::mid_hz, project::rebalance_mid_min_hz,
     project::rebalance_mid_max_hz, project::rebalance_mid_default_hz, Scale::log, "Hz"},
    {"midQ", "Q", "Mid", &RoleBalance::mid_q, project::rebalance_mid_min_q,
     project::rebalance_mid_max_q, project::rebalance_mid_default_q, Scale::log, ""},
    {"high", "Gain", "High", &RoleBalance::high_db, -limit_db, limit_db, 0.0, Scale::linear, "dB"},
    {"highHz", "Freq", "High", &RoleBalance::high_hz, project::rebalance_high_min_hz,
     project::rebalance_high_max_hz, project::rebalance_high_default_hz, Scale::log, "Hz"},
}};

// An arrow key or wheel notch: in dB on a dB control, as a knob position
// on a log one; Shift for the fine step.
constexpr double db_step = 0.5;
constexpr double db_fine_step = 0.1;
constexpr double position_step = 0.02;
constexpr double position_fine_step = 0.005;
// Frequencies from this up read in kHz, with one decimal below the next.
constexpr double kilo = 1'000.0;
constexpr double whole_khz_from = 10'000.0;
constexpr int q_decimals = 2;

const Field* find_field(const QString& id) {
  const auto key = id.toStdString();
  const auto found = std::ranges::find(fields, std::string_view(key), &Field::id);
  return found == fields.end() ? nullptr : &*found;
}

double clamp_to(const Field& field, double value) {
  return std::clamp(value, field.minimum, field.maximum);
}

double position_of(const Field& field, double value) {
  if (field.scale == Scale::log)
    return std::log(clamp_to(field, value) / field.minimum) /
           std::log(field.maximum / field.minimum);
  return (clamp_to(field, value) - field.minimum) / (field.maximum - field.minimum);
}

double value_at(const Field& field, double position) {
  const double p = std::clamp(position, 0.0, 1.0);
  if (field.scale == Scale::log)
    return field.minimum * std::pow(field.maximum / field.minimum, p);
  return field.minimum + p * (field.maximum - field.minimum);
}

QString signed_number(double value, int decimals) {
  // A typographic minus; a plus on gains over zero.
  auto text = QString::number(std::abs(value), 'f', decimals);
  if (value > 0.0)
    return QStringLiteral("+") + text;
  if (value < 0.0 && text != QString::number(0.0, 'f', decimals))
    return QStringLiteral("−") + text;
  return text;
}

QString format(const Field& field, double value) {
  if (field.unit == "dB")
    return signed_number(value, 1) + QStringLiteral(" dB");
  if (field.unit == "Hz") {
    if (value >= whole_khz_from)
      return QString::number(value / kilo, 'f', 0) + QStringLiteral(" kHz");
    if (value >= kilo)
      return QString::number(value / kilo, 'f', 1) + QStringLiteral(" kHz");
    return QString::number(value, 'f', 0) + QStringLiteral(" Hz");
  }
  return QString::number(value, 'f', q_decimals);
}

constexpr std::array<MixRole, project::mix_role_count> balanced_roles{
    MixRole::vocals, MixRole::drums, MixRole::bass, MixRole::music};

// Each role's id and name, in MixRole order (none last).
constexpr std::array<std::pair<std::string_view, std::string_view>, project::mix_role_count + 1>
    role_texts{{{"vocals", "Vocals"},
                {"drums", "Drums"},
                {"bass", "Bass"},
                {"music", "Music"},
                {"none", "None"}}};

QString qstring(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::size_t role_index(MixRole role) {
  return std::min(static_cast<std::size_t>(role), project::mix_role_count);
}
QString role_id(MixRole role) { return qstring(role_texts[role_index(role)].first); }
QString role_name(MixRole role) { return qstring(role_texts[role_index(role)].second); }

std::optional<MixRole> parse_role(const QString& id) {
  for (const auto role :
       {MixRole::vocals, MixRole::drums, MixRole::bass, MixRole::music, MixRole::none}) {
    if (role_id(role) == id)
      return role;
  }
  return std::nullopt;
}

// The role menu's Auto, as QML names it.
QString auto_choice() { return QStringLiteral("auto"); }

QString track_kind(project::TrackKind kind) {
  return kind == project::TrackKind::instrument   ? QStringLiteral("instrument")
         : kind == project::TrackKind::group_bus  ? QStringLiteral("group")
         : kind == project::TrackKind::return_bus ? QStringLiteral("return")
                                                  : QStringLiteral("audio");
}

// Why a track plays its role, in a word: the instrument's name when its
// device decides it.
QString reason_text(const project::Track& track, project::RoleReason reason) {
  switch (reason) {
  case project::RoleReason::chosen:
    return QStringLiteral("chosen");
  case project::RoleReason::group:
    return QStringLiteral("group");
  case project::RoleReason::device: {
    const auto* patch = std::get_if<project::NativeInstrumentPatch>(&track.instrument_source);
    const auto* engine = patch ? native_dsp::synth::find_engine(patch->engine) : nullptr;
    if (engine && engine->descriptor)
      return qstring(engine->descriptor->name);
    return patch ? QString::fromStdString(patch->engine) : QStringLiteral("instrument");
  }
  case project::RoleReason::name:
    return QStringLiteral("name");
  case project::RoleReason::heard:
    return QStringLiteral("heard");
  case project::RoleReason::notes:
    return QStringLiteral("notes");
  case project::RoleReason::summed:
    return QStringLiteral("its tracks");
  case project::RoleReason::fallback:
    break;
  }
  return QStringLiteral("default");
}

constexpr int colour_digits = 6;
constexpr int hexadecimal = 16;
QString colour_text(const std::optional<std::uint32_t>& colour) {
  return colour ? QStringLiteral("#%1").arg(*colour, colour_digits, hexadecimal, QLatin1Char('0'))
                : QString();
}

// The EQ's response, as the stage runs it at the project's rate.
QVariantList response(const RoleBalance& balance, std::uint32_t sample_rate) {
  QVariantList curve;
  const auto target = native_dsp::role_target(render_graph::role_tone(balance), sample_rate);
  if (!target)
    return curve;
  curve.reserve(RebalanceController::curve_points);
  const double span = RebalanceController::curve_to_hz / RebalanceController::curve_from_hz;
  for (int point = 0; point < RebalanceController::curve_points; ++point) {
    const double hz =
        RebalanceController::curve_from_hz *
        std::pow(span, static_cast<double>(point) /
                           static_cast<double>(RebalanceController::curve_points - 1));
    curve.push_back(native_dsp::eq_magnitude_db(target.value().eq, hz).value_or(0.0));
  }
  return curve;
}

} // namespace

std::optional<MeterReading> combine_meters(const std::vector<MeterReading>& readings) noexcept {
  if (readings.empty())
    return std::nullopt;
  MeterReading combined;
  double left_power = 0.0;
  double right_power = 0.0;
  for (const auto& reading : readings) {
    combined.left_peak = std::max(combined.left_peak, reading.left_peak);
    combined.right_peak = std::max(combined.right_peak, reading.right_peak);
    left_power += static_cast<double>(reading.left_rms) * reading.left_rms;
    right_power += static_cast<double>(reading.right_rms) * reading.right_rms;
    combined.clipped = combined.clipped || reading.clipped;
  }
  combined.left_rms = static_cast<float>(std::sqrt(left_power));
  combined.right_rms = static_cast<float>(std::sqrt(right_power));
  return combined;
}

// ---- One role ---------------------------------------------------------------

RebalanceRole::RebalanceRole(RebalanceController& owner, MixRole role, QObject* parent)
    : QObject(parent), owner_(owner), role_(role) {}

QString RebalanceRole::id() const { return role_id(role_); }
QString RebalanceRole::name() const { return role_name(role_); }
QString RebalanceRole::meter_id() const {
  return qstring(RebalanceController::meter_prefix) + id();
}
bool RebalanceRole::editable() const { return owner_.can_edit_now(); }
bool RebalanceRole::neutral() const { return project::neutral(owner_.balance(role_)); }

QVariantMap RebalanceRole::parameter(const QString& id) const {
  const auto* field = find_field(id);
  if (!field)
    return {};
  return {{QStringLiteral("name"), qstring(field->name)},
          {QStringLiteral("group"), qstring(field->group)},
          {QStringLiteral("minimum"), field->minimum},
          {QStringLiteral("maximum"), field->maximum},
          {QStringLiteral("default"), field->fallback},
          {QStringLiteral("unit"), qstring(field->unit)},
          {QStringLiteral("bipolar"), field->scale == Scale::linear}};
}

double RebalanceRole::value(const QString& id) const {
  const auto* field = find_field(id);
  return field ? owner_.balance(role_).*(field->member) : 0.0;
}

double RebalanceRole::normalised(const QString& id) const {
  const auto* field = find_field(id);
  return field ? position_of(*field, owner_.balance(role_).*(field->member)) : 0.0;
}

QString RebalanceRole::display(const QString& id) const {
  const auto* field = find_field(id);
  return field ? format(*field, owner_.balance(role_).*(field->member)) : QString();
}

void RebalanceRole::begin_gesture(const QString& /*id*/) { gesture_ = owner_.next_gesture(); }

void RebalanceRole::set_normalised(const QString& id, double position) {
  if (const auto* field = find_field(id))
    set_value(id, value_at(*field, position));
}

void RebalanceRole::set_value(const QString& id, double value) {
  const auto* field = find_field(id);
  if (!field || !std::isfinite(value))
    return;
  auto balance = owner_.balance(role_);
  balance.*(field->member) = clamp_to(*field, value);
  owner_.edit_role(role_, balance, gesture_);
}

void RebalanceRole::end_gesture(const QString& /*id*/) { gesture_.reset(); }
void RebalanceRole::cancel_gesture(const QString& /*id*/) { gesture_.reset(); }

void RebalanceRole::reset_to_default(const QString& id) {
  if (const auto* field = find_field(id))
    set_value(id, field->fallback);
}

void RebalanceRole::step(const QString& id, int direction, bool fine) {
  const auto* field = find_field(id);
  if (!field)
    return;
  const double sign = direction < 0 ? -1.0 : 1.0;
  const double current = owner_.balance(role_).*(field->member);
  if (field->scale == Scale::linear)
    set_value(id, current + sign * (fine ? db_fine_step : db_step));
  else
    set_normalised(id, position_of(*field, current) +
                           sign * (fine ? position_fine_step : position_step));
}

void RebalanceRole::reset() {
  if (owner_.balance(role_) != RoleBalance{})
    owner_.edit_role(role_, RoleBalance{}, std::nullopt);
}

void RebalanceRole::update(QStringList track_names, QVariantList curve) {
  track_names_ = std::move(track_names);
  curve_ = std::move(curve);
  ++revision_;
  emit changed();
}

// ---- The panel --------------------------------------------------------------

RebalanceController::RebalanceController(Hooks hooks, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), timer_(this) {
  for (std::size_t index = 0; index < roles_.size(); ++index)
    roles_[index] = new RebalanceRole(*this, balanced_roles[index], this);
  timer_.setInterval(poll_ms);
  connect(&timer_, &QTimer::timeout, this, &RebalanceController::poll);
}

RebalanceController::~RebalanceController() {
  if (cancel_)
    cancel_->store(true, std::memory_order_release);
}

bool RebalanceController::enabled() const {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  return session != nullptr && session->snapshot().rebalance.enabled;
}

void RebalanceController::set_enabled(bool on) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr || session->snapshot().rebalance.enabled == on)
    return;
  auto next = session->snapshot().rebalance;
  next.enabled = on;
  edit(
      [&](application::ProjectSession& current) {
        return current.tracks().set_master_rebalance(next);
      },
      QStringLiteral("Rebalance"));
}

QList<QObject*> RebalanceController::roles() const {
  QList<QObject*> list;
  for (auto* role : roles_)
    list.push_back(role);
  return list;
}

QVariantList RebalanceController::choices() const {
  QVariantList list{QVariantMap{{QStringLiteral("id"), auto_choice()},
                                {QStringLiteral("name"), QStringLiteral("Auto")}}};
  for (const auto role :
       {MixRole::vocals, MixRole::drums, MixRole::bass, MixRole::music, MixRole::none})
    list.push_back(QVariantMap{{QStringLiteral("id"), role_id(role)},
                               {QStringLiteral("name"), role_name(role)}});
  return list;
}

double RebalanceController::range_db() const noexcept { return limit_db; }

RoleBalance RebalanceController::balance(MixRole role) const {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto index = static_cast<std::size_t>(role);
  if (session == nullptr || index >= project::mix_role_count)
    return {};
  return session->snapshot().rebalance.roles[index];
}

bool RebalanceController::can_edit_now() const {
  return hooks_.session && hooks_.session() != nullptr;
}

std::uint64_t RebalanceController::next_gesture() noexcept {
  const auto gesture = next_gesture_++;
  if (next_gesture_ == 0)
    next_gesture_ = 1;
  return gesture;
}

bool RebalanceController::edit(
    const std::function<foundation::Result<void>(application::ProjectSession&)>& apply,
    const QString& what) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr) {
    set_status(QStringLiteral("Open a project first"));
    return false;
  }
  if (hooks_.can_edit && !hooks_.can_edit()) {
    set_status(what + QStringLiteral(": not now, another edit is running"));
    return false;
  }
  const auto applied = apply(*session);
  if (!applied) {
    set_status(what + QStringLiteral(": ") + QString::fromStdString(applied.error().message));
    return false;
  }
  if (hooks_.commit)
    hooks_.commit();
  return true;
}

void RebalanceController::edit_role(MixRole role, const RoleBalance& balance,
                                    std::optional<std::uint64_t> gesture) {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  const auto index = static_cast<std::size_t>(role);
  if (session == nullptr || index >= project::mix_role_count)
    return;
  auto next = session->snapshot().rebalance;
  // Moving a role turns the rebalance on: an edit is meant to be heard.
  if (next.roles[index] == balance && next.enabled)
    return;
  next.roles[index] = balance;
  next.enabled = true;
  edit(
      [&](application::ProjectSession& current) {
        return current.tracks().set_master_rebalance(next, gesture);
      },
      QStringLiteral("Rebalance"));
}

// QML passes a row's track id and the menu's role key, both strings.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RebalanceController::setTrackRole(const QString& track_id, const QString& role) {
  const auto track = project::TrackId::parse(track_id.toStdString());
  if (!track) {
    set_status(QStringLiteral("Choose a track"));
    return;
  }
  std::optional<MixRole> chosen;
  if (role != auto_choice()) {
    chosen = parse_role(role);
    if (!chosen) {
      set_status(QStringLiteral("Choose a role"));
      return;
    }
  }
  edit(
      [&](application::ProjectSession& current) {
        return current.tracks().set_track_mix_role(track.value(), chosen);
      },
      QStringLiteral("Track role"));
}

std::optional<MeterReading> RebalanceController::role_meter(const QString& id,
                                                            const TrackMeter& track) const {
  const auto prefix = qstring(meter_prefix);
  if (!id.startsWith(prefix) || !track)
    return std::nullopt;
  const auto role = parse_role(id.mid(prefix.size()));
  const auto index = role ? static_cast<std::size_t>(*role) : project::mix_role_count;
  if (index >= project::mix_role_count)
    return std::nullopt;
  std::vector<MeterReading> readings;
  for (const auto& member : stage_tracks_[index]) {
    if (const auto reading = track(member))
      readings.push_back(*reading);
  }
  return combine_meters(readings);
}

void RebalanceController::project_changed() {
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr) {
    for (auto& tracks : stage_tracks_)
      tracks.clear();
    for (auto* role : roles_)
      role->update({}, {});
    set_tracks({});
    emit changed();
    return;
  }
  const auto& snapshot = session->snapshot();
  const auto roles = project::mix_roles(snapshot);
  const TrackRoutingView view(snapshot);
  std::array<QStringList, project::mix_role_count> names;
  for (auto& tracks : stage_tracks_)
    tracks.clear();
  // Rows in the track list's order: each group with its members under it.
  std::vector<std::pair<int, QVariantMap>> rows;
  for (std::size_t index = 0; index < snapshot.tracks.size() && index < roles.size(); ++index) {
    const auto& track = snapshot.tracks[index];
    const auto& role = roles[index];
    const auto name = track.name.empty() ? view.name(track.id) : QString::fromStdString(track.name);
    if (const auto stage = static_cast<std::size_t>(role.stage); stage < project::mix_role_count)
      stage_tracks_[stage].push_back(track.id);
    const bool bus =
        track.kind == project::TrackKind::group_bus || track.kind == project::TrackKind::return_bus;
    if (const auto played = static_cast<std::size_t>(role.role);
        !bus && played < project::mix_role_count)
      names[played].push_back(name);
    const auto& nesting = view.nesting(track.id);
    rows.emplace_back(
        nesting.display_index,
        QVariantMap{{QStringLiteral("id"), QString::fromStdString(track.id.to_string())},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("color"), colour_text(track.color)},
                    // The saved order, which picks the palette colour of an uncoloured track.
                    {QStringLiteral("order"), static_cast<int>(index)},
                    {QStringLiteral("kind"), track_kind(track.kind)},
                    {QStringLiteral("nested"), nesting.bus.has_value()},
                    {QStringLiteral("role"), role_id(role.role)},
                    {QStringLiteral("roleName"), role_name(role.role)},
                    {QStringLiteral("reason"), reason_text(track, role.reason)},
                    {QStringLiteral("chosen"), track.mix_role.has_value()}});
  }
  for (std::size_t index = 0; index < roles_.size(); ++index)
    roles_[index]->update(names[index],
                          response(snapshot.rebalance.roles[index], snapshot.sample_rate));
  std::ranges::stable_sort(rows, {}, &std::pair<int, QVariantMap>::first);
  QVariantList list;
  for (const auto& entry : rows)
    list.push_back(entry.second);
  set_tracks(std::move(list));
  emit changed();
}

void RebalanceController::set_tracks(QVariantList rows) {
  // The list rebuilds its rows only when a track or its role changed, not
  // on every step of a drag.
  if (rows == tracks_)
    return;
  tracks_ = std::move(rows);
  emit tracksChanged();
}

// ---- Listening --------------------------------------------------------------

void RebalanceController::listen() {
  if (job_.running())
    return;
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr) {
    set_status(QStringLiteral("Open a project first"));
    return;
  }
  const auto& tracks = session->snapshot().tracks;
  const auto audible = std::ranges::count_if(tracks, [](const project::Track& track) {
    return track.kind == project::TrackKind::audio && !track.frozen && !track.clips.empty();
  });
  if (audible == 0) {
    set_status(QStringLiteral("No audio tracks to listen to"));
    return;
  }
  auto pinned = session->exports().pin_snapshot();
  if (!pinned) {
    set_status(QStringLiteral("Cannot listen: ") + QString::fromStdString(pinned.error().message));
    return;
  }
  cancel_ = std::make_shared<std::atomic<bool>>(false);
  listened_ = pinned.value().snapshot;
  listened_session_ = session->session_generation();
  const auto refused = job_.start(
      listened_session_,
      [pinned = std::move(pinned).value(), cancel = cancel_] {
        return application::listen_for_roles(pinned.reader, *pinned.snapshot, [cancel] {
          return cancel->load(std::memory_order_acquire);
        });
      },
      [](const std::string& what) {
        return Heard::failure({foundation::ErrorCode::internal, what});
      });
  if (refused) {
    set_status(QStringLiteral("Cannot listen: ") + QString::fromStdString(*refused));
    return;
  }
  timer_.start();
  set_status(audible == 1 ? QStringLiteral("Listening to 1 audio track…")
                          : QStringLiteral("Listening to %1 audio tracks…").arg(audible));
  emit changed();
}

void RebalanceController::cancelListening() {
  if (cancel_)
    cancel_->store(true, std::memory_order_release);
}

void RebalanceController::poll() {
  if (job_.finished())
    finish();
}

bool RebalanceController::wait_for_listening() {
  job_.wait();
  finish();
  return !job_.running();
}

void RebalanceController::finish() {
  auto finished = job_.take();
  timer_.stop();
  const bool cancelled = cancel_ && cancel_->load(std::memory_order_acquire);
  const auto listened = std::exchange(listened_, {});
  emit changed();
  if (!finished) {
    set_status(QStringLiteral("Listening failed"));
    return;
  }
  if (cancelled) {
    set_status(QStringLiteral("Listening cancelled"));
    return;
  }
  auto* session = hooks_.session ? hooks_.session() : nullptr;
  if (session == nullptr || session->session_generation() != finished->generation || !listened) {
    set_status(QStringLiteral("Listening stopped: the project changed"));
    return;
  }
  if (!finished->result) {
    set_status(QStringLiteral("Listening failed: ") +
               QString::fromStdString(finished->result.error().message));
    return;
  }
  // Only what still holds: a track removed, or whose clips changed while
  // listening, keeps its role.
  std::vector<project::Operation> heard;
  QStringList described;
  const auto& now = session->snapshot();
  const TrackRoutingView view(now);
  for (const auto& operation : finished->result.value()) {
    const auto* role = std::get_if<project::SetTrackHeardRole>(&operation);
    if (role == nullptr)
      continue;
    const auto current = std::ranges::find(now.tracks, role->id, &project::Track::id);
    const auto before = std::ranges::find(listened->tracks, role->id, &project::Track::id);
    if (current == now.tracks.end() || before == listened->tracks.end() ||
        current->clips != before->clips)
      continue;
    heard.push_back(operation);
    const auto name =
        current->name.empty() ? view.name(current->id) : QString::fromStdString(current->name);
    described.push_back(name + QStringLiteral(" ") +
                        role_name(role->role.value_or(MixRole::none)).toLower());
  }
  if (heard.empty()) {
    set_status(QStringLiteral("Listened: every role holds"));
    return;
  }
  if (edit(
          [&](application::ProjectSession& current) {
            return current.tracks().apply_heard_roles(heard);
          },
          QStringLiteral("Listen")))
    set_status(QStringLiteral("Heard ") + described.join(QStringLiteral(", ")));
}

void RebalanceController::set_status(QString text) {
  if (status_.set(std::move(text)))
    emit changed();
}

} // namespace oscillate::quick
