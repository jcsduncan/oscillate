import QtQuick
import "../devicekit"

// Edge design tokens: a clipper. A blue-black case and slate plates, the
// curve, the clipping and the chosen settings in a hot orange, the output's
// true peak in teal.
KitTheme {
    prefix: "edge"

    caseColor: "#10131a"
    plate: "#171c26"
    plateEdge: "#262c38"
    plateHi: "#2c3343"
    well: "#10131a"
    wellEdge: "#2c3343"
    seam: "#262c38"
    grid: "#232937"
    gridStrong: "#2e3546"

    accent: "#f08a5d"
    accentSoft: "#8a4f37"
    steel: "#6a3c2a"
    glacier: "#f4f5f8"
    ink: "#e7e9ee"
    inkDim: "#9aa3b4"
    inkFaint: "#8f97a8"
    accentInk: "#10131a"

    knobTop: "#2a3141"
    knobBottom: "#151922"
    track: "#2a3141"
    button: "#1b202b"
    buttonHi: "#232a37"
    ledOff: "#2a3141"
    warning: "#f2b25c"
    live: "#5fc7c0"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The transfer display.
    property color screen: "#10131a"
    // The line the curve leaves (output = input), dashed.
    property color unity: "#3a4254"
    property color dotFill: "#e7e9ee"
    property real curveStroke: 2.5
    property real dotRadius: 5
    property real dotStroke: 2
    property real dashOn: 4
    property real dashOff: 4
    property real unityDashOn: 2
    // A beat with less clipping than this draws muted.
    property real historyLitDb: 1.0
    property color historyQuiet: "#3a4254"
    property real historyMinBar: 2
    property real historyGap: 6

    // The panel's layout.
    property int panelWidth: 1040
    property int panelHeight: 520
    property real panelRadius: 10
    property real cardGap: 12
    property real cardInset: 14
    property real cardContentY: 34
    property real captionSpacing: 1.2

    // The header: the mark, the wordmark and its subtitle, then the status,
    // the preset and the compare.
    property real headerY: 12
    property real headerHeight: 46
    property real markSize: 30
    property real markStroke: 2
    // The mark's flat-topped wave, as fractions of its size.
    property var markPoints: [[0.25, 0.68], [0.4, 0.36], [0.6, 0.36], [0.75, 0.68]]
    property real markGap: 12
    property int wordmarkSize: 17
    property real wordmarkSpacing: 2.2
    property real subtitleSpacing: 1
    property real presetWidth: 230
    property real presetHeight: 32
    property real presetTextX: 12
    property real presetArrowGap: 4
    property real headerSpacing: 10
    property real compareWidth: 110

    // The body: the transfer card on the left, the controls on the right.
    property real bodyY: 70
    property real transferWidth: 420
    property real plotHeight: 300
    property real plotLeft: 34
    property real plotRight: 10
    property real plotTop: 10
    property real plotBottom: 22
    property real plotLabelLift: 6
    property real historyHeight: 34
    property real historyY: 342
    property real historyCaptionGap: 6

    property real knobsHeight: 164
    property int knobDiameterLarge: 62
    property real knobHintGap: 4
    property real controlsRowHeight: 112
    property real togglesWidth: 260
    property real toggleRowHeight: 24
    property real toggleTrackWidth: 34
    property real toggleTrackHeight: 18
    property real toggleKnobInset: 2
    property real segmentRowY: 32
    property real latencyY: 72
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int meterValueSize: 15
    property int clippedValueSize: 22

    segmentedEqualCells: true
    segmentedHeight: 30
    segmentedRadius: 8
    segmentedFrame: "#1b202b"
    segmentedFrameEdge: "#2c3343"
    segmentedDividers: true
    segmentedDivider: "#2c3343"
    segmentedTextSize: typeLabel

    lastTouched: ""
}
