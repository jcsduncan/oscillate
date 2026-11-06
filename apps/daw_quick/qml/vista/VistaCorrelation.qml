pragma ComponentBehavior: Bound
import QtQuick

// The correlation meter, -1 to +1: out of phase on the left (coral), wide
// in the middle (ice), mono-safe on the right (sea green). A steel caret
// marks the input, a glacier bar the output; with Safe Width on, an amber
// line marks the floor the output may not cross.
Item {
    id: meter
    required property VistaTheme theme
    property real inValue: 1
    property real outValue: 1
    property bool live: false
    property bool floorOn: false
    property real floor: 0
    // Where the wide (ice) zone of the track's gradient peaks.
    readonly property real wideFrom: 0.45
    readonly property real wideTo: 0.6

    objectName: "vistaCorrelation"
    implicitWidth: theme.correlationDefaultWidth
    implicitHeight: theme.correlationHeight

    function xOf(rho) {
        return track.x + (Math.max(-1, Math.min(1, rho)) + 1) / 2 * track.width
    }

    Rectangle {
        id: track
        x: meter.theme.cardInset
        y: meter.theme.correlationTrackY
        width: meter.width - 2 * meter.theme.cardInset
        height: meter.theme.correlationTrackHeight
        radius: meter.theme.radiusXs
        border.color: meter.theme.wellEdge
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: meter.theme.correlationOutOfPhase }
            GradientStop { position: meter.wideFrom; color: meter.theme.correlationWideFrom }
            GradientStop { position: meter.wideTo; color: meter.theme.correlationWideTo }
            GradientStop { position: 1.0; color: meter.theme.correlationInPhase }
        }
    }
    // Ticks and figures at -1, -0.5, 0, +0.5, +1.
    Repeater {
        model: [-1, -0.5, 0, 0.5, 1]
        delegate: Item {
            id: tick
            required property real modelData
            Rectangle {
                x: meter.xOf(tick.modelData) - meter.theme.hairline / 2
                y: track.y + track.height
                width: meter.theme.hairline
                height: tick.modelData === 0 ? meter.theme.correlationTickMajor : meter.theme.correlationTickMinor
                color: meter.theme.gridStrong
            }
            Text {
                x: meter.xOf(tick.modelData) - width / 2
                y: track.y + track.height + meter.theme.correlationTickMajor
                text: tick.modelData > 0 ? "+" + tick.modelData : String(tick.modelData).replace("-", "−")
                color: meter.theme.inkFaint
                font.family: meter.theme.mono
                font.pixelSize: meter.theme.typeMicro
            }
        }
    }
    // The floor.
    Rectangle {
        objectName: "vistaCorrelationFloor"
        visible: meter.floorOn
        x: meter.xOf(meter.floor) - meter.theme.strokeBold / 2
        y: track.y - meter.theme.floorOverhang
        width: meter.theme.strokeBold
        height: track.height + 2 * meter.theme.floorOverhang
        color: meter.theme.warning
    }
    // Input: a steel caret above the track.
    Text {
        visible: meter.live
        x: meter.xOf(meter.inValue) - width / 2
        y: track.y - meter.theme.caretRise
        text: "▼"
        color: meter.theme.accentSoft
        font.pixelSize: meter.theme.typeCaption
    }
    // Output: a bright bar through the track.
    Rectangle {
        visible: meter.live
        x: meter.xOf(meter.outValue) - meter.theme.outputBarWidth / 2
        y: track.y - meter.theme.outputBarOverhang
        width: meter.theme.outputBarWidth
        height: track.height + 2 * meter.theme.outputBarOverhang
        radius: meter.theme.outputBarRadius
        color: meter.theme.glacier
        Rectangle {
            anchors.centerIn: parent
            width: meter.theme.outputHaloWidth
            height: parent.height + meter.theme.outputHaloExtend
            radius: meter.theme.radiusSm
            color: meter.theme.outputHalo
        }
    }
}
