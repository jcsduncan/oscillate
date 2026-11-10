import QtQuick
import "../devicekit"

// Keel design tokens: adaptive tonal balance. A deep spruce case and
// slate plates; the target, the chosen settings and the curve in sea-glass
// mint; what Keel lifts in amber and what it takes down in sky; the tracked
// balance as a grey ghost under the balance Keel lets out in near-white.
KitTheme {
    prefix: "keel"

    caseColor: "#0f1513"
    plate: "#16201d"
    plateEdge: "#24312c"
    plateHi: "#2f3e38"
    well: "#0f1513"
    wellEdge: "#2f3e38"
    seam: "#24312c"
    grid: "#1d2925"
    gridStrong: "#2a3833"

    accent: "#8fe3b0"
    accentSoft: "#3f7a59"
    steel: "#2e5a46"
    glacier: "#f2f7f4"
    ink: "#e6eee9"
    inkDim: "#a0aea6"
    inkFaint: "#8f9e96"
    accentInk: "#0f1513"

    knobTop: "#293631"
    knobBottom: "#121a17"
    track: "#293631"
    button: "#1a2421"
    buttonHi: "#23302b"
    ledOff: "#293631"
    warning: "#f2b25c"
    live: "#7fe0c4"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // What Keel lifts and takes down; the balance it lets out.
    property color boost: "#ffb547"
    property color cut: "#5fd3ff"
    property color output: "#e6eee9"
    // The correction's bars, faint under the lines.
    property real correctionAlpha: 0.35
    // The tracked balance behind what Keel lets out.
    property color held: "#3a4a43"
    property real heldAlpha: 0.55

    // The views.
    property color screen: "#0c1210"
    // The 0 dB line the correction moves from.
    property color centre: "#34443d"
    property real traceStroke: 2
    property real targetStroke: 2
    property real dashOn: 5
    property real dashOff: 4
    // The drawn curve's handles: their radius, and the chosen one's ring.
    property real handleRadius: 4
    property real handleRing: 7
    // The history's colour at full scale (dB) and the least alpha drawn.
    property real historyFullDb: 6
    property real historyFloorAlpha: 0.04
    // Telemetry polls while playing (about 30 a second) and while idle.
    property int pollPlayingMs: 33
    property int pollIdleMs: 250

    // The panel's layout.
    property int panelWidth: 1280
    property int panelHeight: 760
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
    // The mark: a keel's line, a level waterline over a deep fin, as
    // fractions of its size.
    property var markPoints: [[0.2, 0.4], [0.8, 0.4], [0.62, 0.52], [0.54, 0.76], [0.46, 0.76], [0.38, 0.52], [0.2, 0.4]]
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    // Clear of the widest subtitle, so the browser stays put.
    headerBrowserX: 340
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

    // The body: the views on the left, the controls on the right.
    property real bodyY: 70
    property real viewWidth: 740
    property real balanceHeight: 390
    property real historyHeight: 150
    // The views' scales: room for "+18 dB" and "10 kHz" left of the plots.
    plotGutterLeft: 50
    property real metersGap: 12
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int meterValueSize: 15

    property real balanceCardHeight: 132
    property real targetCardHeight: 214
    property real regionsCardHeight: 132
    property int knobDiameterLarge: 54
    property int knobDiameterMedium: 46
    property int knobDiameterSmall: 40
    property real targetChoiceWidth: 250
    property real genreNameWidth: 180
    property real noteGap: 8

    segmentedEqualCells: true
    segmentedHeight: 30
    segmentedRadius: 8
    segmentedFrame: "#1a2421"
    segmentedFrameEdge: "#2f3e38"
    segmentedDividers: true
    segmentedDivider: "#2f3e38"
    segmentedTextSize: typeLabel

    lastTouched: ""
}
