import QtQuick
import "../devicekit"

// Strike design tokens: a transient shaper. A graphite-olive case and
// slate plates; the lift, the chosen settings and the first band in a
// struck lime, the cut in sky; the input as a grey ghost under what Strike
// lets out in near-white. Each band its own colour (lime, sky, amber,
// violet).
KitTheme {
    prefix: "strike"

    caseColor: "#111410"
    plate: "#181c17"
    plateEdge: "#283026"
    plateHi: "#323b2f"
    well: "#111410"
    wellEdge: "#323b2f"
    seam: "#283026"
    grid: "#222820"
    gridStrong: "#2e362b"

    accent: "#d4f55b"
    accentSoft: "#6f8a2e"
    steel: "#4c5c2a"
    glacier: "#f5f7f1"
    ink: "#e9ede4"
    inkDim: "#a2aa98"
    inkFaint: "#949c8a"
    accentInk: "#111410"

    knobTop: "#2c3329"
    knobBottom: "#151914"
    track: "#2c3329"
    button: "#1c211b"
    buttonHi: "#252b23"
    ledOff: "#2c3329"
    warning: "#f2b25c"
    live: "#7fe0c4"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The bands' colours, low to high; the cut and the output.
    property var bandColours: ["#d4f55b", "#5fd3ff", "#ffb547", "#c49bff"]
    property color cut: "#5fd3ff"
    property color output: "#e9ede4"
    // What Strike lets out: a faint wash over the input's ghost.
    property real outputWashAlpha: 0.18

    // The scrolling view.
    property color screen: "#0e110d"
    // The input behind what Strike lets out.
    property color held: "#2f362c"
    // The 0 dB line the gains move from, and the ceiling when Clip is on.
    property color centre: "#3a4336"
    property real traceStroke: 2
    property real ceilingStroke: 1
    property real dashOn: 4
    property real dashOff: 4
    // The gap between two slices' bars, in pixels.
    property real sliceGap: 1
    // Telemetry polls while playing (about 30 a second) and while idle.
    property int pollPlayingMs: 33
    property int pollIdleMs: 250

    // The panel's layout.
    property int panelWidth: 1240
    property int panelHeight: 700
    property real panelRadius: 10
    property real cardGap: 12
    property real cardInset: 14
    property real cardContentY: 34
    property real captionSpacing: 1.2

    // The header: the mark beside the wordmark and its subtitle, the preset
    // browser, the readout once a control is touched, then the status.
    property real headerY: 12
    headerHeight: 46
    property real markSize: 30
    property real markStroke: 2
    // The mark: a hit's envelope, a sharp rise and a falling tail, as
    // fractions of its size.
    property var markPoints: [[0.18, 0.68], [0.34, 0.68], [0.42, 0.24], [0.52, 0.52], [0.66, 0.62], [0.82, 0.68]]
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    // Clear of the widest subtitle, so the browser stays put.
    headerBrowserX: 320
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

    // The body: the view on the left, the controls on the right.
    property real bodyY: 70
    property real viewWidth: 600
    property real plotHeight: 470
    // The view's gain scale: room for "+18 dB" left of the plot.
    plotGutterLeft: 46
    property real metersGap: 12
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int meterValueSize: 15

    property real bandsHeight: 112
    property real shapeHeight: 214
    property int knobDiameterLarge: 58
    property int knobDiameterMedium: 48
    property int knobDiameterSmall: 40
    property real bandSwatch: 8
    property real bandCountWidth: 180
    // Room between the crossover knobs for their long names.
    property real crossoverGap: 40
    property real segmentRowY: 34
    property real latencyGap: 8
    property real timingWidth: 300

    segmentedEqualCells: true
    segmentedHeight: 30
    segmentedRadius: 8
    segmentedFrame: "#1c211b"
    segmentedFrameEdge: "#323b2f"
    segmentedDividers: true
    segmentedDivider: "#323b2f"
    segmentedTextSize: typeLabel

    lastTouched: ""
}
