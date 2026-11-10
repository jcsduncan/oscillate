#include "device_panels.hpp"

#include <algorithm>
#include <array>

namespace oscillate::quick {
namespace {

// The inputs a sidechain-keyed effect names its key with.
constexpr std::string_view keyed_inputs = "keyed keyName";

constexpr std::array panels{
    // ---- Instruments --------------------------------------------------------
    DevicePanel{"prism", "qrc:/prism/PrismPanel.qml", 1240, 768, ""},
    DevicePanel{"ember", "qrc:/ember/EmberPanel.qml", 1240, 700, ""},
    DevicePanel{"pulse", "qrc:/pulse/PulsePanel.qml", 1240, 700, ""},
    DevicePanel{"halo", "qrc:/halo/HaloPanel.qml", 1280, 816, ""},
    DevicePanel{"torque", "qrc:/torque/TorquePanel.qml", 1280, 770, ""},
    DevicePanel{"drift", "qrc:/drift/DriftPanel.qml", 1280, 806, ""},
    DevicePanel{"concert", "qrc:/concert/ConcertPanel.qml", 1280, 656, ""},
    DevicePanel{"fret", "qrc:/fret/FretPanel.qml", 1280, 820, ""},
    DevicePanel{"wren", "qrc:/wren/WrenPanel.qml", 1280, 952, ""},
    DevicePanel{"bow", "qrc:/bow/BowPanel.qml", 1240, 760, ""},
    DevicePanel{"stick", "qrc:/stick/StickPanel.qml", 1240, 760, ""},
    // Nova names its own designed and minimum sizes.
    DevicePanel{"nova", "qrc:/nova/NovaPanel.qml", 0, 0, "zoom keyBase grooves", true},
    DevicePanel{"anvil", "qrc:/anvil/AnvilPanel.qml", 1440, 800, "anvil songScale"},
    DevicePanel{"crate", "qrc:/crate/CratePanel.qml", 1440, 680, "playheadBar"},
    DevicePanel{"knock", "qrc:/knock/KnockPanel.qml", 1400, 940,
                "knock playheadBar selectedClipIds"},
    // ---- Effects ------------------------------------------------------------
    DevicePanel{"contour", "qrc:/contour/ContourPanel.qml", 1180, 620, ""},
    DevicePanel{"forge", "qrc:/forge/ForgePanel.qml", 1120, 566, ""},
    DevicePanel{"orbit", "qrc:/orbit/OrbitPanel.qml", 1180, 708, ""},
    DevicePanel{"relay", "qrc:/relay/RelayPanel.qml", 1240, 690, "relay"},
    DevicePanel{"expanse", "qrc:/expanse/ExpansePanel.qml", 1280, 690, ""},
    DevicePanel{"cast", "qrc:/cast/CastPanel.qml", 1240, 700, keyed_inputs},
    DevicePanel{"vista", "qrc:/vista/VistaPanel.qml", 1240, 600, ""},
    DevicePanel{"deck", "qrc:/deck/DeckPanel.qml", 1240, 740, ""},
    DevicePanel{"pocket", "qrc:/pocket/PocketPanel.qml", 1340, 740, ""},
    DevicePanel{"pump", "qrc:/pump/PumpPanel.qml", 1280, 720, keyed_inputs},
    DevicePanel{"chop", "qrc:/chop/ChopPanel.qml", 1240, 760, ""},
    DevicePanel{"lens", "qrc:/lens/LensPanel.qml", 1380, 946, ""},
    DevicePanel{"loudness", "qrc:/loudness/LoudnessPanel.qml", 1240, 760, ""},
    DevicePanel{"scope", "qrc:/scope/ScopePanel.qml", 1240, 760, "trackName keyName sampleRate"},
    DevicePanel{"tune", "qrc:/tune/TunePanel.qml", 1240, 760, ""},
    DevicePanel{"trim", "qrc:/trim/TrimPanel.qml", 1240, 740, ""},
    DevicePanel{"signal", "qrc:/signal/SignalPanel.qml", 1240, 740, ""},
    DevicePanel{"shape", "qrc:/shape/ShapePanel.qml", 1240, 720, ""},
    DevicePanel{"press", "qrc:/press/PressPanel.qml", 1240, 680, keyed_inputs},
    DevicePanel{"ceiling", "qrc:/ceiling/CeilingPanel.qml", 1240, 640, ""},
    DevicePanel{"edge", "qrc:/edge/EdgePanel.qml", 1040, 520, ""},
    DevicePanel{"hush", "qrc:/hush/HushPanel.qml", 1160, 640, keyed_inputs},
    DevicePanel{"strike", "qrc:/strike/StrikePanel.qml", 1240, 700, ""},
    DevicePanel{"keel", "qrc:/keel/KeelPanel.qml", 1280, 760, keyed_inputs},
    DevicePanel{"gild", "qrc:/gild/GildPanel.qml", 1280, 760, ""},
    DevicePanel{"temper", "qrc:/temper/TemperPanel.qml", 1360, 800, ""},
    DevicePanel{"strata", "qrc:/strata/StrataPanel.qml", 1320, 800, keyed_inputs},
    DevicePanel{"flyby", "qrc:/flyby/FlybyPanel.qml", 1280, 760, keyed_inputs},
    DevicePanel{"dust", "qrc:/dust/DustPanel.qml", 1360, 714, keyed_inputs},
};

} // namespace

std::span<const DevicePanel> device_panels() noexcept { return panels; }

const DevicePanel* device_panel(std::string_view id) noexcept {
  const auto found = std::ranges::find(panels, id, &DevicePanel::id);
  return found == panels.end() ? nullptr : &*found;
}

} // namespace oscillate::quick
