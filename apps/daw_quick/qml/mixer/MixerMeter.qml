import QtQuick
import "../shell"
import "MixerDb.js" as Db

// One channel's peak meter on the strip's decibel scale (MixerDb.js), with
// `inset` left at each end as the fader and scale leave it, so a reading
// sits level with the scale's mark. Hidden fill when there is no reading
// (`valid` false), never a fabricated level.
Item {
    id: meter
    property real peak: 0
    // The held peak: a line that waits, then falls (C++ ballistics).
    property real hold: 0
    property bool valid: false
    property bool clipped: false
    // Room left above and below the scale (the fader cap's half-height).
    property real inset: 0
    // A held peak this high (a scale position) shows in the clip colour.
    readonly property real holdHotAbove: 0.9
    // The meter's look.
    readonly property int meterWidth: 7
    readonly property color wellColor: Theme.mixerMeterWellColor
    readonly property color overColor: Theme.mixerMeterOverColor
    readonly property color hotColor: Theme.mixerMeterHotColor
    readonly property color safeColor: Theme.mixerMeterSafeColor
    readonly property color floorColor: Theme.mixerMeterFloorColor
    // Where the gradient turns from over to hot, and from hot to safe.
    readonly property real hotStop: 0.12
    readonly property real safeStop: 0.35
    readonly property color holdColor: Theme.mixerMeterHoldColor
    readonly property real holdOpacity: 0.85
    readonly property real clipHeight: 3
    readonly property real clipRadius: 1
    implicitWidth: meterWidth
    function position(level) { return Db.positionOf(level) }
    readonly property real fill: valid ? position(peak) : 0
    readonly property real held: valid ? position(hold) : 0
    Accessible.role: Accessible.Indicator
    Accessible.name: valid ? (peak <= 0 ? "Silent" : Db.toDb(peak).toFixed(1) + " dB peak")
                           : "No reading"
    Item {
        id: column
        objectName: "mixerMeterColumn"
        y: meter.inset
        width: parent.width
        height: parent.height - 2 * meter.inset
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSm
            color: meter.wellColor
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: parent.height * meter.fill
            radius: Theme.radiusSm
            gradient: Gradient {
                GradientStop { position: 0; color: meter.overColor }
                GradientStop { position: meter.hotStop; color: meter.hotColor }
                GradientStop { position: meter.safeStop; color: meter.safeColor }
                GradientStop { position: 1; color: meter.floorColor }
            }
        }
        Rectangle {
            visible: meter.held > 0
            anchors.left: parent.left
            anchors.right: parent.right
            y: parent.height * (1 - meter.held)
            height: Theme.strokeBold
            color: meter.held > meter.holdHotAbove ? meter.overColor : meter.holdColor
            opacity: meter.holdOpacity
        }
        Rectangle {
            visible: meter.clipped
            anchors.left: parent.left
            anchors.right: parent.right
            height: meter.clipHeight
            radius: meter.clipRadius
            color: meter.overColor
        }
    }
}
