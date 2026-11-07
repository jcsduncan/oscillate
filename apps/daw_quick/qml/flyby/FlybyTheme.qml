import QtQuick
import "../devicekit"

// Flyby design tokens: a surveyor's chart of the space round your head at
// night. Slate case and a deep chart; the path and the source glow amber,
// you (the listener) are mint, the near ear's level reads in sky blue.
KitTheme {
    prefix: "flyby"
    segmentHeight: 24
    segmentSpacing: 3
    segmentRadius: 5
    segmentLetterSpacing: 0.8
    switchRadius: -1
    switchHeight: 18

    // The case.
    caseTop: "#141c1f"
    caseColor: "#10171a"
    caseBottom: "#0b1113"
    plate: "#141c1f"
    plateEdge: "#24323a"
    plateHi: "#33454f"
    well: "#0a1012"
    wellEdge: "#24323a"
    seam: "#1c272c"

    // The chart.
    property color chart: "#0a1012"
    grid: "#1c282e"
    gridStrong: "#24323a"

    // The path and the source.
    accent: "#f2a541"
    accentSoft: "#5a3c14"
    steel: "#3a4a52"
    property color path: "#f2a541"
    property color wave: Qt.rgba(242 / 255, 165 / 255, 65 / 255, 0.22)
    property color handleFill: "#0a1012"
    // You.
    property color listener: "#6fd3b8"
    property color listenerFill: "#1a2428"
    property color sky: "#79b7e6"
    property color rose: "#f07a7a"

    glacier: "#eef4f1"
    ink: "#e4ece9"
    inkDim: "#93a6a3"
    inkFaint: "#708986"

    knobTop: "#33454f"
    knobBottom: "#0e1416"
    track: "#1c282e"
    button: "#121a1d"
    buttonHi: "#1a2428"
    ledOff: "#24323a"

    warning: "#f2a541"
    live: "#6fd3b8"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"
    property string display: "DejaVu Sans Condensed"

    lastTouched: ""

    // ---- Panel layout ----------------------------------------------------------
    property int panelWidth: 1280
    property int panelHeight: 760
    property real caseRadius: 10
    // Where the case gradient reaches caseColor.
    property real caseMidStop: 0.4
    // Between the chart and the cards, and between cards.
    property real panelGap: 10
    // The column of cards beside the chart.
    property real sideWidth: 392

    // ---- Header ------------------------------------------------------------
    property real headerTop: 10
    property real headerHeight: 52
    property int wordmarkSize: 27
    property real wordmarkSpacing: 5
    property real taglineSpacing: 2.2
    // The preset browser.
    property real browserX: 300
    property real browserY: 5
    property real presetWidth: 360
    property real presetHeight: 42
    // Caption rows (top) and value rows inside the preset display.
    property real presetCaptionY: 5
    property real presetNameY: 16
    property real presetNameWidth: 170
    property real presetCaptionSpacing: 1.4
    property real presetDividerX: 192
    property real presetDividerInset: 8
    property real readoutX: 202
    property real readoutValueY: 17
    // Room right of the readout for the chevron.
    property real readoutRight: 24
    property real readoutSpacing: 1.2
    property real chevronMargin: 9
    // The in/out peak meters.
    property real meterTop: 4
    property real meterWidth: 8
    property real meterHeight: 44

    // ---- The chart's live readings ------------------------------------------
    property real readingWidth: 104
    property real readingHeight: 38
    property real readingRadius: 7
    property real readingInset: 9
    property real readingCaptionY: 5
    property real readingValueY: 17
    property real readingSpacing: 1.3
    property color readingFill: Qt.rgba(chart.r, chart.g, chart.b, 0.88)

    // ---- Cards -------------------------------------------------------------
    property real pathCardHeight: 146
    property real motionCardHeight: 150
    property real physicsCardHeight: 112
    property real roomCardWidth: 150
    // The first control row under a card's title, and the knob row below it.
    property real cardBodyY: 30
    property real cardKnobRowY: 60
    // Notes and controls beside a card's knobs.
    property real cardSideX: 160
    property real pathNoteY: 68
    property real motionSideY: 64
    // A note at a card's foot: its top above the card's bottom edge.
    property real cardFootNote: 34
    property real pathSegmentWidth: 150
    property real directionX: 172

    // ---- The chart ------------------------------------------------------------
    property real pathWidth: 2
    property color handleArm: Qt.rgba(ink.r, ink.g, ink.b, 0.25)
    property color closestRing: Qt.rgba(listener.r, listener.g, listener.b, 0.45)
    property color sourceLine: Qt.rgba(path.r, path.g, path.b, 0.35)
    property var closestDash: [2, 4]
    property var sourceDash: [3, 4]
    // The legend and the scale in the chart's top corners.
    property real chartLabelTop: 10
    property real legendLineHeight: 1.3
    // You: the head, and the ears' least brightness when quiet.
    property real headSize: 22
    property real headStroke: 2
    property real earQuietOpacity: 0.3
    // The source.
    property real sourceSize: 16
    property real sourceCore: 6
    // The path's points to drag: ends, handles, and the reach round them.
    property real pointEndSize: 12
    property real pointHandleSize: 10
    property real pointStroke: 1.5
    property real pointGrabMargin: 7
}
