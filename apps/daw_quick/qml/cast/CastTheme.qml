import QtQuick
import "../devicekit"

// Cast design tokens: a convolution reverb. A charcoal case with warm
// stone plates; the impulse response, the chosen IR and the settings in
// amber, its energy decay in a pale sea-glass, the wet signal in the same.
KitTheme {
    prefix: "cast"

    caseColor: "#141312"
    plate: "#1c1a18"
    plateEdge: "#2d2a26"
    plateHi: "#36322d"
    well: "#141312"
    wellEdge: "#36322d"
    seam: "#2d2a26"
    grid: "#272420"
    gridStrong: "#34302a"

    accent: "#f0b86e"
    accentSoft: "#8c6a3f"
    steel: "#5b4b37"
    glacier: "#f6f2ec"
    ink: "#ece6dd"
    inkDim: "#b0a697"
    inkFaint: "#9a907f"
    accentInk: "#141312"

    knobTop: "#302c27"
    knobBottom: "#181614"
    track: "#302c27"
    button: "#201d1a"
    buttonHi: "#2a2622"
    ledOff: "#302c27"
    warning: "#f2b25c"
    live: "#8fd6c8"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The energy decay curve and the wet meters, in sea-glass.
    property color decay: "#8fd6c8"
    // The response view.
    property color screen: "#11100f"
    property real envelopeFillAlpha: 0.28
    property real envelopeStroke: 1
    property real decayStroke: 1.5
    // The pre-delay's gap before the IR, shaded.
    property color predelayShade: "#1f1c19"
    // Telemetry polls while playing (about 30 a second) and while idle.
    property int pollPlayingMs: 33
    property int pollIdleMs: 250

    // The IR list.
    property real listRowHeight: 26
    property real listSectionHeight: 22
    property real listRowRadius: 4
    property color listRowHover: "#262320"
    property real listTextX: 10
    property real listMarkWidth: 3
    property real dropHintY: 6
    property real browserButtonsHeight: 70

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
    // The mark: an impulse and its decaying echoes, as fractions of its
    // size (x, top of each line).
    property var markLines: [[0.22, 0.2], [0.36, 0.42], [0.48, 0.56], [0.6, 0.66], [0.72, 0.74], [0.82, 0.8]]
    property real markBaseline: 0.82
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    property real subtitleGap: 2
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

    // The body: the IR browser, its response and its shape on top; space,
    // tone, ducking and the mix below.
    property real bodyY: 70
    property real rowAHeight: 340
    property real browserWidth: 270
    property real responseWidth: 560
    plotGutterLeft: 40
    property real readoutsHeight: 46
    property int readoutValueSize: 15
    property real meterHeight: 34
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int knobDiameterLarge: 54
    property int knobDiameterSmall: 42
    property real knobHintGap: 4
    property real shapeRowGap: 8
    property real switchRowGap: 10
    property real spaceWidth: 210
    property real toneWidth: 310
    property real duckWidth: 370
    property real keySegmentWidth: 150

    segmentedEqualCells: true
    segmentedHeight: 28
    segmentedRadius: 8
    segmentedFrame: "#201d1a"
    segmentedFrameEdge: "#36322d"
    segmentedDividers: true
    segmentedDivider: "#36322d"
    segmentedTextSize: typeLabel

    lastTouched: ""
}
