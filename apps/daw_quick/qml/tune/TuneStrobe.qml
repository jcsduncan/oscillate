pragma ComponentBehavior: Bound
import QtQuick

// The strobe band: three rows of amber bars drifting in proportion to the
// cents error, right when sharp and left when flat, the lower rows at twice
// and four times the speed (the second and fourth harmonics, as on a
// mechanical strobe). In tune they stand still and turn green.
Item {
    id: strobe
    required property TuneTheme theme
    property real cents: 0
    property bool live: false
    property real clarity: 1
    // Pixels a second per cent at the top row.
    property real speed: 3
    property real offset: 0
    readonly property real period: 40
    // Three rows: the fundamental, the second and the fourth harmonic.
    readonly property int rows: 3
    // Bars past the visible ones, so a row's wrap is never seen.
    readonly property int spareBars: 2
    // The drift's frame interval, and its longest step after a stall (s).
    readonly property int frameInterval: 16
    readonly property real maxStep: 0.1
    // The centre marks stand this far past the glass's edges.
    readonly property real markOverhang: 1
    readonly property bool steady: Math.abs(cents) < 1
    readonly property color ink: steady ? theme.inTune : theme.amber

    objectName: "tuneStrobe"
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: strobe.theme.radiusLg
        color: strobe.theme.strobeDark
        border.color: strobe.theme.wellEdge
    }

    Timer {
        interval: strobe.frameInterval
        repeat: true
        running: strobe.live && strobe.visible
        property real last: 0
        onRunningChanged: last = 0
        onTriggered: {
            const now = Date.now()
            const dt = last > 0 ? Math.min(strobe.maxStep, (now - last) / 1000) : 0
            last = now
            // Unwrapped: each row takes its own phase from it.
            strobe.offset += strobe.cents * strobe.speed * dt
        }
    }

    Column {
        id: band
        x: strobe.theme.spaceXxs
        y: strobe.theme.spaceXxs
        width: strobe.width - 2 * strobe.theme.spaceXxs
        height: strobe.height - 2 * strobe.theme.spaceXxs
        spacing: strobe.theme.spaceXxs
        Repeater {
            model: strobe.rows
            delegate: Item {
                id: row
                required property int index
                readonly property real p: strobe.period / Math.pow(2, index)
                readonly property real shift: ((strobe.offset * Math.pow(2, index)) % p + p) % p
                width: parent.width
                height: (band.height - (strobe.rows - 1) * band.spacing) / strobe.rows
                clip: true
                Repeater {
                    model: Math.ceil(row.width / row.p) + strobe.spareBars
                    delegate: Rectangle {
                        required property int index
                        x: (index - 1) * row.p + row.shift
                        width: row.p / 2
                        height: row.height
                        radius: strobe.theme.strobeBarRadius
                        opacity: strobe.live ? (strobe.theme.strobeFloorOpacity + (1 - strobe.theme.strobeFloorOpacity) * Math.max(0, Math.min(1, strobe.clarity))) * (1 - row.index * strobe.theme.strobeRowFade) : strobe.theme.strobeIdleOpacity
                        gradient: Gradient {
                            GradientStop { position: 0; color: Qt.rgba(strobe.ink.r, strobe.ink.g, strobe.ink.b, strobe.theme.strobeEdgeAlpha) }
                            GradientStop { position: 0.5; color: strobe.ink }
                            GradientStop { position: 1; color: Qt.rgba(strobe.ink.r, strobe.ink.g, strobe.ink.b, strobe.theme.strobeEdgeAlpha) }
                        }
                    }
                }
            }
        }
    }
    // The glass's sheen and the centre mark.
    Rectangle {
        anchors.fill: parent
        radius: strobe.theme.radiusLg
        color: "transparent"
        border.color: strobe.theme.wellEdge
        gradient: Gradient {
            GradientStop { position: 0; color: strobe.theme.glassSheen }
            GradientStop { position: strobe.theme.glassSheenStop; color: strobe.theme.glassClear }
            GradientStop { position: 1; color: strobe.theme.glassShade }
        }
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: -strobe.markOverhang
        width: strobe.theme.strobeMarkWidth
        height: strobe.theme.strobeMarkHeight
        color: strobe.theme.glacier
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height - height + strobe.markOverhang
        width: strobe.theme.strobeMarkWidth
        height: strobe.theme.strobeMarkHeight
        color: strobe.theme.glacier
    }
}
