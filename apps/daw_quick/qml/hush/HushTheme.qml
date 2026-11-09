import QtQuick
import "../devicekit"

// Hush design tokens: a gate. A violet-black case and dusk plates; what the
// gate lets through, the thresholds and the chosen settings in lilac, what
// it holds back as a grey ghost, the key in mint.
KitTheme {
    prefix: "hush"

    caseColor: "#13111a"
    plate: "#1a1724"
    plateEdge: "#2a2638"
    plateHi: "#302b41"
    well: "#13111a"
    wellEdge: "#302b41"
    seam: "#2a2638"
    grid: "#252131"
    gridStrong: "#312c40"

    accent: "#b49cff"
    accentSoft: "#5f4f8f"
    steel: "#4a3f6e"
    glacier: "#f4f3f8"
    ink: "#e9e7f0"
    inkDim: "#a39cb8"
    inkFaint: "#968fab"
    accentInk: "#13111a"

    knobTop: "#2d283c"
    knobBottom: "#17141f"
    track: "#2d283c"
    button: "#1d1a27"
    buttonHi: "#262232"
    ledOff: "#2d283c"
    warning: "#f2b25c"
    live: "#7fe0c4"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The key (a routed sidechain, the key level, listen) in mint.
    property color key: "#7fe0c4"

    // The scrolling view.
    property color screen: "#100e16"
    // The input the gate holds back, behind what it lets through.
    property color held: "#3a3450"
    // The open threshold solid, the close threshold dashed.
    property real thresholdStroke: 1.5
    property real dashOn: 4
    property real dashOff: 4
    // The centre line the waveform mirrors about.
    property color centre: "#2a2638"
    // The gap between two slices' bars, in pixels.
    property real sliceGap: 1
    // Telemetry polls while playing (about 30 a second) and while idle.
    property int pollPlayingMs: 33
    property int pollIdleMs: 250
    // The state lamp in the view's title row.
    property real lampSize: 10

    // The dot used by HushSwitch's knob.
    property color dotFill: "#e9e7f0"

    // The panel's layout.
    property int panelWidth: 1160
    property int panelHeight: 640
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
    // The mark's gate: a line that rises, holds open and falls, as fractions
    // of its size.
    property var markPoints: [[0.2, 0.66], [0.36, 0.66], [0.36, 0.34], [0.64, 0.34], [0.64, 0.66], [0.8, 0.66]]
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    // Clear of the widest subtitle, so the browser stays put.
    headerBrowserX: 300
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
    // The keyed badge left of the status.
    property real keyedBadgeGap: 10
    property real keyedBadgePad: 16
    property real keyedBadgeHeight: 22
    property real keyedFillAlpha: 0.16
    property real keyedSpacing: 1

    // The body: the gate view on the left, the controls on the right.
    property real bodyY: 70
    property real viewWidth: 500
    property real plotHeight: 420
    // The view's scales: room for "−48" left of the plot.
    plotGutterLeft: 40
    property real metersGap: 12
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int meterValueSize: 15
    property int reductionValueSize: 22

    property real knobsHeight: 176
    property int knobDiameterLarge: 58
    property int knobDiameterSmall: 40
    property real knobHintGap: 4
    property real timingKnobsShare: 0.58
    property real segmentRowY: 34
    property real latencyGap: 10
    property real toggleRowHeight: 24
    property real toggleTrackWidth: 34
    property real toggleTrackHeight: 18
    property real toggleKnobInset: 2
    // The focus ring's gap outside the switch's track.
    property real toggleFocusGap: 3
    property real modeWidth: 250
    property real keySegmentWidth: 200

    segmentedEqualCells: true
    segmentedHeight: 30
    segmentedRadius: 8
    segmentedFrame: "#1d1a27"
    segmentedFrameEdge: "#302b41"
    segmentedDividers: true
    segmentedDivider: "#302b41"
    segmentedTextSize: typeLabel

    lastTouched: ""
}
