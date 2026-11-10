import QtQuick
import "../devicekit"

// Gild design tokens: a multiband exciter. A warm umber case and smoked
// plates; what Gild adds and the chosen settings in leaf gold; the
// programme as a grey ghost under it. Each band its own colour (gold,
// coral, teal, lilac).
KitTheme {
    prefix: "gild"

    caseColor: "#13110d"
    plate: "#1b1813"
    plateEdge: "#2e2920"
    plateHi: "#3a3428"
    well: "#13110d"
    wellEdge: "#3a3428"
    seam: "#2e2920"
    grid: "#24201a"
    gridStrong: "#322c23"

    accent: "#f5c451"
    accentSoft: "#8a6c2a"
    steel: "#5c4a24"
    glacier: "#f7f3ea"
    ink: "#ede8dd"
    inkDim: "#aaa292"
    inkFaint: "#9a9282"
    accentInk: "#13110d"

    knobTop: "#2f2a21"
    knobBottom: "#17140f"
    track: "#2f2a21"
    button: "#1e1b15"
    buttonHi: "#28241c"
    ledOff: "#2f2a21"
    warning: "#f2b25c"
    live: "#7fe0c4"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The bands' colours, low to high; the output.
    property var bandColours: ["#f5c451", "#ff8a6b", "#5fd3c4", "#c49bff"]
    property color output: "#ede8dd"

    // The spectrum view.
    property color screen: "#0f0d0a"
    // The programme behind what Gild adds.
    property color held: "#36312a"
    property color heldEdge: "#4d463b"
    // What Gild adds: a gold wash under its outline.
    property real addedWashAlpha: 0.35
    // The bands' shading behind the spectra, and their crossovers.
    property real bandWashAlpha: 0.06
    property real crossoverStroke: 1
    property real dashOn: 4
    property real dashOff: 4
    property real traceStroke: 2
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
    // The mark: a sine and, over it, its third harmonic, as fractions of
    // its size.
    property real markWaveInset: 0.2
    property real markWaveHeight: 0.18
    property real markHarmonicHeight: 0.08
    property int markHarmonic: 3
    property int markPoints: 24
    property real markGap: 12
    wordmarkSize: 17
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 2.2
    wordmarkInk: glacier
    subtitleSize: typeSmall
    subtitleSpacing: 1
    subtitleInk: accent
    // Clear of the widest subtitle, so the browser stays put.
    headerBrowserX: 360
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
    property real plotHeight: 520
    // The view's level scale: room for "-96" left of the plot.
    plotGutterLeft: 40
    property real metersGap: 12
    property real meterBarHeight: 4
    property real meterBarRadius: 2
    property int meterValueSize: 15

    property real bandsHeight: 112
    property real harmonicsHeight: 352
    property int knobDiameterMedium: 48
    property int knobDiameterSmall: 40
    property real bandSwatch: 8
    property real bandCountWidth: 180
    // Room between the crossover knobs for their long names.
    property real crossoverGap: 40
    property real segmentRowY: 34
    property real latencyGap: 8
    property real qualityWidth: 300
    property real curveGap: 10
    property real knobRowGap: 4

    segmentedEqualCells: true
    segmentedHeight: 30
    segmentedRadius: 8
    segmentedFrame: "#1e1b15"
    segmentedFrameEdge: "#3a3428"
    segmentedDividers: true
    segmentedDivider: "#3a3428"
    segmentedTextSize: typeLabel
    dropdownWidth: 110

    lastTouched: ""
}
