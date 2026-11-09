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
    // Beats in the clip history: edge::history_beats, the telemetry's count.
    property int historyBeats: 16
    // Telemetry polls while playing (about 30 a second) and while idle.
    property int pollPlayingMs: 33
    property int pollIdleMs: 250
    // A beat with less clipping than this draws muted.
    property real historyLitDb: 1.0
    property color historyQuiet: "#3a4254"
    property real historyMinBar: 2
    // The gap between two beats' bars.
    property real historyGap: 6

    // The panel's layout.
    property int panelWidth: 1040
    property int panelHeight: 520
    property real panelRadius: 10
    property real cardGap: 12
    property real cardInset: 14
    property real cardContentY: 34
    property real captionSpacing: 1.2

    // The header (Kit.DeviceHeader): the mark beside the wordmark and its
    // subtitle, the preset browser, the readout once a control is touched,
    // then the compare and the status.
    property real headerY: 12
    headerHeight: 46
    property real markSize: 30
    property real markStroke: 2
    // The mark's flat-topped wave, as fractions of its size.
    property var markPoints: [[0.25, 0.68], [0.4, 0.36], [0.6, 0.36], [0.75, 0.68]]
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    // Clear of the widest subtitle (16× oversampled), so the browser stays put.
    headerBrowserX: 262
    presetDisplayWidth: 230
    presetDisplayHeight: 36
    presetDisplayRadius: radiusXl
    presetFill: button
    presetTextX: 12
    presetTagY: 3
    presetNameY: 15
    presetNameSize: typeLabel
    presetNameBold: false
    presetChevronInk: inkDim
    arrowHeight: 36
    readoutValueY: 16
    property real compareWidth: 110

    // The body: the transfer card on the left, the controls on the right.
    property real bodyY: 70
    property real transferWidth: 420
    // The transfer's frame; under it the clip history's frame, its bars
    // under the frame's chip row.
    property real plotHeight: 294
    property real historyFrameGap: 10
    property real historyBarsHeight: 44
    // The frames' scales: room for "−24 dBFS" left of the plot.
    plotGutterLeft: 50

    property real knobsHeight: 164
    property int knobDiameterLarge: 62
    property real knobHintGap: 4
    property real controlsRowHeight: 112
    property real togglesWidth: 260
    property real toggleRowHeight: 24
    property real toggleTrackWidth: 34
    property real toggleTrackHeight: 18
    property real toggleKnobInset: 2
    // The focus ring's gap outside the switch's track.
    property real toggleFocusGap: 3
    property real segmentRowY: 32
    property real latencyY: 72
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    // The ceiling's mark on the level bars, standing this far proud of them.
    property real ceilingMarkWidth: 2
    property real ceilingMarkOverhang: 3
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
