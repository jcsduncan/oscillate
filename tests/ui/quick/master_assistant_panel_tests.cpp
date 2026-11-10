#include "master_assistant_controller.hpp"
#include "panel_fit_checks.hpp"
#include "project_bridge.hpp"

#include "oscillate/application/project_session.hpp"
#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/fake_output.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// The master assistant's panel (ADR-0124) on a real bridge: Analyse fills
// the measurements and the proposal and every text fits; Accept puts the
// kept devices on the master as one undo step and measures the master
// again; a device left out stays off the master; Reject leaves the project
// as it was; the A/B plays through the preview output and switches sides.
namespace {
namespace application = oscillate::application;
namespace project = oscillate::project;
namespace quick = oscillate::quick;
namespace panel_fit = oscillate::test::panel_fit;

using oscillate::test::require;

constexpr int window_width = 1600;
constexpr int window_height = 860;
constexpr std::uint32_t rate = 48'000;
constexpr double rate_hz = 48'000.0;
constexpr double seconds = 6.0;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr std::int16_t full_scale = 32'767;
constexpr std::uint64_t seed = 0x6d61'7374'6572'0124ULL;
// The A/B's blocks pumped before and after switching sides.
constexpr int pump_blocks = 8;

void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  put16(bytes, static_cast<std::uint16_t>(value));
  put16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

// Kicks on the beat over a quiet, wide noise bed: a mix that wants a master.
std::filesystem::path write_mix(const std::filesystem::path& path) {
  constexpr std::uint32_t header_bytes = 36;
  constexpr std::uint32_t format_bytes = 16;
  constexpr std::uint16_t pcm = 1;
  constexpr std::uint16_t stereo = 2;
  constexpr std::uint16_t frame_bytes = 4;
  constexpr std::uint16_t bits = 16;
  constexpr double beat = 0.5;
  constexpr double kick_hz = 55.0;
  constexpr double decay = 0.12;
  constexpr double kick_level = 0.5;
  constexpr double bed_level = 0.05;
  const auto frames = static_cast<std::uint32_t>(seconds * rate_hz);
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  put32(bytes, header_bytes + frames * frame_bytes);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(bytes, format_bytes);
  put16(bytes, pcm);
  put16(bytes, stereo);
  put32(bytes, rate);
  put32(bytes, rate * frame_bytes);
  put16(bytes, frame_bytes);
  put16(bytes, bits);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  put32(bytes, frames * frame_bytes);
  oscillate::foundation::SplitMix64 random(seed);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const double since = std::fmod(t, beat);
    const double kick = kick_level * std::exp(-since / decay) * std::sin(two_pi * kick_hz * since);
    for (int channel = 0; channel < 2; ++channel) {
      const double x = kick + bed_level * (2.0 * random.unit() - 1.0);
      put16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                       std::lround(std::clamp(x, -1.0, 1.0) * full_scale))));
    }
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "mix fixture written");
  return path;
}

template <typename T> T take(oscillate::foundation::Result<T> result, const char* what) {
  require(result.has_value(), what);
  return std::move(result).value();
}

struct Song {
  QTemporaryDir directory;
  std::filesystem::path root;
  std::unique_ptr<quick::ProjectBridge> bridge;
  // The bridge owns it while the project is open.
  application::ProjectSession* session = nullptr;
  quick::MasterAssistantController* controller = nullptr;
};

// The project in `folder`/song, with the mix imported.
std::unique_ptr<application::ProjectSession> make_session(const std::filesystem::path& folder) {
  auto session = take(application::ProjectSession::create(folder / "song"), "project");
  const auto wav = write_mix(folder / "mix.wav");
  const int fd = ::open(wav.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "fixture opens");
  auto track = session->imports().import_wav_audio(fd);
  (void)::close(fd);
  (void)take(std::move(track), "the mix imports");
  require(session->saving().save().has_value(), "saved");
  return session;
}

std::unique_ptr<Song> make_song() {
  auto song = std::make_unique<Song>();
  require(song->directory.isValid(), "fixture directory");
  const std::filesystem::path folder(song->directory.path().toStdString());
  song->root = folder / "song";
  song->bridge = std::make_unique<quick::ProjectBridge>(folder.string());
  auto session = make_session(folder);
  song->session = session.get();
  song->bridge->set_project(std::move(session), song->root);
  song->controller =
      qobject_cast<quick::MasterAssistantController*>(song->bridge->master_assistant());
  require(song->controller != nullptr, "the bridge has the master assistant");
  return song;
}

// Analyse to the end: a proposal.
void analyse(quick::MasterAssistantController& controller) {
  controller.set_loudness(QStringLiteral("club"));
  controller.set_character(QStringLiteral("Punchy"));
  controller.analyse();
  require(controller.wait(), "the analysis ran");
  require(controller.phase() == QStringLiteral("proposed"),
          "a proposal: " + controller.status().toStdString());
}

QQuickItem* find_named(QQuickItem* parent, const QString& name) {
  if (parent->objectName() == name)
    return parent;
  for (auto* child : parent->childItems()) {
    if (auto* found = find_named(child, name))
      return found;
  }
  return nullptr;
}

QQuickItem* item(QQuickItem* root, const QString& name) {
  auto* found = find_named(root, name);
  require(found != nullptr, "panel has " + name.toStdString());
  return found;
}

struct Panel {
  QQmlEngine engine;
  std::unique_ptr<QObject> object;
  QQuickWindow window;
  QQuickItem* root = nullptr;
};

std::unique_ptr<Panel> load(QObject* controller) {
  auto panel = std::make_unique<Panel>();
  QQmlComponent component(&panel->engine, QUrl(QStringLiteral("qrc:/MasterAssistantView.qml")));
  panel->object.reset(component.createWithInitialProperties(
      {{QStringLiteral("controller"), QVariant::fromValue(controller)}}));
  require(panel->object != nullptr,
          ("the assistant view did not load: " + component.errorString()).toStdString());
  panel->root = qobject_cast<QQuickItem*>(panel->object.get());
  require(panel->root != nullptr, "the view is an item");
  panel->window.resize(window_width, window_height);
  panel->root->setParentItem(panel->window.contentItem());
  panel->root->setSize(QSizeF(window_width, window_height));
  panel->window.show();
  require(QTest::qWaitForWindowExposed(&panel->window), "window exposed");
  QCoreApplication::processEvents();
  return panel;
}

void click(Panel& panel, QQuickItem* target) {
  const auto centre = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0));
  QTest::mouseClick(&panel.window, Qt::LeftButton, {}, centre.toPoint());
  QCoreApplication::processEvents();
}

std::vector<std::string> master_engines(const application::ProjectSession& session) {
  std::vector<std::string> out;
  for (const auto& insert : session.snapshot().master_effects)
    if (const auto* patch = std::get_if<project::NativeEffectPatch>(&insert.processor))
      out.push_back(patch->engine);
  return out;
}

// ---- Cases ---------------------------------------------------------------------------

// Analyse fills the readouts, the tonal view and the proposal, each device
// with its line; every text fits.
void analyse_fills_the_panel_and_it_fits() {
  auto song = make_song();
  auto panel = load(song->controller);
  item(panel->root, QStringLiteral("assistantGenre:all"));
  item(panel->root, QStringLiteral("assistantLoudness:club"));
  item(panel->root, QStringLiteral("assistantCharacter:Punchy"));
  panel_fit::require_text_fits(panel->root, "master assistant, empty");
  analyse(*song->controller);
  QCoreApplication::processEvents();
  for (const auto* id : {"integrated", "shortTerm", "momentary", "truePeak", "crest"})
    item(panel->root, QStringLiteral("assistantReadout:") + QString::fromLatin1(id));
  item(panel->root, QStringLiteral("assistantTonal"));
  const auto devices = song->controller->devices();
  require(!devices.isEmpty(), "devices proposed");
  for (const auto& device : devices) {
    const auto map = device.toMap();
    item(panel->root, QStringLiteral("assistantDevice:") + map.value("engine").toString());
    require(!map.value("reason").toString().isEmpty(), "every device says why");
  }
  item(panel->root, QStringLiteral("assistantDevice:ceiling"));
  require(!song->controller->prediction().isEmpty(), "the outcome is predicted");
  panel_fit::require_text_fits(panel->root, "master assistant, proposed");
}

// Accept is one undo step that puts the chain on the master, then the
// master is measured as it renders; one undo takes the chain off.
void accept_is_one_undo_step_and_verifies() {
  auto song = make_song();
  auto panel = load(song->controller);
  analyse(*song->controller);
  QCoreApplication::processEvents();
  auto* session = song->session;
  const auto steps = session->history().undo_steps().size();
  click(*panel, item(panel->root, QStringLiteral("assistantAccept")));
  require(session->history().undo_steps().size() == steps + 1, "one undo step");
  const auto engines = master_engines(*session);
  require(std::ranges::find(engines, "ceiling") != engines.end(), "Ceiling is on the master");
  require(song->controller->wait(), "the master is measured again");
  require(song->controller->phase() == QStringLiteral("verified"), "verified");
  require(!song->controller->verification().isEmpty(), "the verification reads");
  require(song->controller->verified_on_target(),
          "on target: " + song->controller->verification().join(' ').toStdString());
  QCoreApplication::processEvents();
  item(panel->root, QStringLiteral("assistantVerifiedCaption"));
  song->bridge->undo();
  QCoreApplication::processEvents();
  require(session->snapshot().master_effects.empty(), "one undo takes it all off");
}

// A device left out stays off the master; Reject leaves the project as it was.
void leaving_out_and_rejecting() {
  auto song = make_song();
  auto panel = load(song->controller);
  analyse(*song->controller);
  QCoreApplication::processEvents();
  auto* session = song->session;
  const auto revision = session->snapshot().revision;
  click(*panel, item(panel->root, QStringLiteral("assistantReject")));
  require(session->snapshot().revision == revision, "Reject changes nothing");
  require(song->controller->phase() == QStringLiteral("idle"), "the proposal is gone");

  analyse(*song->controller);
  const auto devices = song->controller->devices();
  int ceiling = -1;
  for (const auto& device : devices)
    if (device.toMap().value("engine").toString() == QStringLiteral("ceiling"))
      ceiling = device.toMap().value("index").toInt();
  require(ceiling >= 0, "a Ceiling to leave out");
  song->controller->setDeviceAccepted(ceiling, false);
  song->controller->accept();
  const auto engines = master_engines(*session);
  require(std::ranges::find(engines, "ceiling") == engines.end(), "the Ceiling left out");
  require(engines.size() + 1 == static_cast<std::size_t>(devices.size()), "the rest accepted");
  (void)song->controller->wait();
}

// The A/B plays through the preview output and switches sides at once.
void the_ab_plays_through_the_preview() {
  auto song = make_song();
  analyse(*song->controller);
  auto device = std::make_shared<oscillate::test::FakePlaybackDevice>();
  // The bridge's preview device is the system's; drive the A/B from a
  // controller of its own with a fake one.
  quick::MasterAssistantController::Hooks hooks;
  hooks.session = [&] { return song->session; };
  hooks.preview = [&]() -> std::optional<quick::MasterAssistantController::Preview> {
    return quick::MasterAssistantController::Preview{
        oscillate::test::fake_output_factory(device), std::nullopt,
        oscillate::test::FakePlaybackDevice::pump_frames};
  };
  quick::MasterAssistantController ab(std::move(hooks));
  analyse(ab);
  ab.startAudition();
  require(ab.auditioning() && ab.player() != nullptr, "the A/B plays");
  for (int block = 0; block < pump_blocks; ++block)
    device->pump();
  require(device->peak > 0.0F, "it is heard");
  require(!ab.hearing_proposal(), "the current master first");
  ab.hearProposal(true);
  for (int block = 0; block < pump_blocks; ++block)
    device->pump();
  require(ab.hearing_proposal() && ab.player()->proposal_chosen(), "then the proposal");
  ab.stopAudition();
  require(!ab.auditioning(), "stopped");
  // Leaving the view silences it.
  auto panel = load(&ab);
  ab.startAudition();
  require(ab.auditioning(), "playing in the view");
  panel.reset();
  require(!ab.auditioning(), "closing the view stops the A/B");
}

// A goal changed while Analyse runs: its proposal, for the old goals, is
// dropped rather than offered.
void a_goal_changed_mid_run_drops_the_result() {
  auto song = make_song();
  song->controller->set_loudness(QStringLiteral("streaming"));
  song->controller->analyse();
  require(song->controller->busy(), "running");
  song->controller->set_loudness(QStringLiteral("loud"));
  (void)song->controller->wait();
  require(song->controller->phase() == QStringLiteral("idle"), "nothing proposed");
  require(song->controller->devices().isEmpty(), "no devices offered");
}

void screenshot_if_requested() {
  const char* directory = std::getenv("MASTER_ASSISTANT_PANEL_SCREENSHOT_DIR");
  if (directory == nullptr || *directory == '\0')
    return;
  auto song = make_song();
  auto panel = load(song->controller);
  analyse(*song->controller);
  QCoreApplication::processEvents();
  panel_fit::require_text_fits(panel->root, "master assistant screenshot");
  QDir().mkpath(QString::fromLocal8Bit(directory));
  const QString path = QString::fromLocal8Bit(directory) + QStringLiteral("/master-assistant.png");
  require(panel->window.grabWindow().save(path, "PNG"), "screenshot saved");
  std::cout << "wrote " << path.toStdString() << '\n';
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  return oscillate::test::run_cases(
      argc, argv, "master assistant panel contract passed",
      {{"analyse_fills_the_panel_and_it_fits", analyse_fills_the_panel_and_it_fits},
       {"accept_is_one_undo_step_and_verifies", accept_is_one_undo_step_and_verifies},
       {"leaving_out_and_rejecting", leaving_out_and_rejecting},
       {"the_ab_plays_through_the_preview", the_ab_plays_through_the_preview},
       {"a_goal_changed_mid_run_drops_the_result", a_goal_changed_mid_run_drops_the_result},
       {"screenshot_if_requested", screenshot_if_requested}});
}
