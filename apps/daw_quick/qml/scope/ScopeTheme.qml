import QtQuick
import "../devicekit"

// Scope design tokens: a phosphor oscilloscope. A near-black case with a
// green cast, a deep bottle-green screen behind glass, a fine graticule;
// the track traced in phosphor green and the sidechain in cyan, the write
// beam bright white-green; alignment in the same two inks with amber for
// a warning and red for an inverted polarity.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "scope"

    // The case: black with a green cast, a touch lighter at the top edge.
    caseTop: "#1a1f1c"
    caseColor: "#121614"
    caseBottom: "#0b0e0c"
    // Plates and the recessed wells inside them.
    plate: "#161b18"
    plateEdge: "#283029"
    plateHi: "#38423a"
    well: "#090c0a"
    wellEdge: "#243027"
    seam: "#202822"

    // The screen: bottle-green glass, its graticule and the beat grid.
    property color screen: "#04100a"
    property color screenEdge: "#1c3a26"
    grid: "#123522"
    gridStrong: "#1f5536"
    property color bar: "#3a8f5c"
    property color graticuleInk: "#3f7a55"

    // The two traces and the beam.
    property color trace: "#6dff9a"
    property color traceGlow: "#2fbf62"
    property color key: "#45d6ff"
    property color keyGlow: "#1f87b3"
    property color beam: "#dcffe6"

    // Alignment.
    property color aligned: "#6dff9a"
    property color near: "#f2c35c"
    property color off: "#ff6257"

    // Controls: phosphor arcs, a pale index.
    accent: "#6dff9a"
    accentSoft: "#2f9a58"
    steel: "#23704a"
    glacier: "#e9f7ee"
    property color glint: "#ffffff"

    ink: "#e8f2eb"
    inkDim: "#93a69a"
    inkFaint: "#73897b"

    knobTop: "#394139"
    knobBottom: "#101411"
    track: "#222b24"
    button: "#121713"
    buttonHi: "#1b221d"

    warning: "#f2b25c"
    live: "#6dff9a"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel: its size, the case's corners and where the case gradient
    // reaches its body colour.
    property int panelWidth: 1240
    property int panelHeight: 760
    property real caseRadius: 10
    property real caseMidStop: 0.35
    // The gap between plates.
    property real plateGap: 10
    // Small bold captions and tags.
    property real captionSpacing: 1.4
    property real tagSpacing: 1.2

    // The header (Kit.DeviceHeader): the wordmark with its two-ink
    // underline over the subtitle, the preset browser and, once a control
    // is touched, its readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkSize: 27
    wordmarkSpacing: 9
    wordmarkInk: glacier
    // The underline and subtitle sit this far in from the wordmark's left.
    property real wordmarkInset: 2
    property real underlineGap: 1
    // How much shorter than the wordmark the underline runs.
    property real underlineTrim: 10
    subtitleGap: 7
    subtitleSpacing: 2.4
    subtitleInk: inkDim
    headerBrowserX: 250
    presetDisplayWidth: 300
    presetDisplayHeight: 42
    presetDisplayRadius: radiusLg
    presetTextX: 14
    presetTagY: 5
    presetTagSpacing: captionSpacing
    presetNameY: 16
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: inkDim
    readoutWidth: 240
    readoutNameY: 5
    readoutSpacing: tagSpacing
    readoutValueY: 17

    // Plates: content inset, caption and first control rows, padding.
    property real cardInset: 10
    property real cardContentY: 30
    property real cardCaptionY: 32
    property real cardBodyY: 34
    property real cardControlY: 46
    property real cardPad: 14

    // The sweep plate and its legend.
    property real rowAY: 76
    property real rowAHeight: 450
    property int sweepWidth: 846
    property real legendY: 9
    property real legendHiddenOpacity: 0.35
    property real swatchLength: 16

    // The phosphor screen: glass, padding round the plot, graticule.
    // The glass is lifted at the top and shaded at the bottom.
    property real glassLift: 1.35
    property real glassMidStop: 0.5
    property real glassShade: 1.2
    property real screenPadX: 14
    // The left padding also holds the amplitude scale.
    property real screenPadLeft: plotGutterLeft + screenPadX
    property real screenPadTop: 12
    property real screenPadBottom: 26
    property real barLineAlpha: 0.55
    // The centre line's ticks: their length and how far above the line
    // they start.
    property real centreTickLength: 5
    property real centreTickRise: 2
    // The screen's scales (Kit.PlotFrame), in the graticule's ink.
    plotTickGap: 6
    plotTickSize: typeSmall
    plotTickInk: graticuleInk
    property real noSignalSpacing: 4
    // The traces: brightness by age (a floor plus a share for freshness),
    // the glow fill and stroke, and the line; the sidechain is dimmer.
    property real traceFreshFloor: 0.4
    property real traceFreshGain: 0.6
    property real traceFillAlpha: 0.30
    property real traceGlowWidth: 3
    property real traceGlowAlpha: 0.22
    property real traceLineWidth: 1.2
    property real traceLineAlpha: 0.95
    property real keyTraceAlpha: 0.85
    // The beam: the erased gap ahead of it, its bloom and its line.
    property real eraseGap: 1
    property real eraseWidth: 18
    property real eraseAlpha: 0.95
    property real bloomWidth: 10
    property real bloomMidStop: 0.5
    property real bloomAlpha: 0.22
    property real beamAlpha: 0.8

    // The alignment plate.
    property real alignPad: 16
    property real needleY: 44
    property real needleHeight: 172
    property real delayY: 218
    property int delaySize: 46
    property real samplesY: 276
    property real samplesSpacing: 0.6
    property real alignRuleY: 302
    // Polarity.
    property real polarityCaptionY: 316
    property real polarityY: 330
    property real polarityHeight: 34
    property real invertedFillAlpha: 0.12
    property real polarityEdgeAlpha: 0.6
    property real polarityIdleEdgeAlpha: 0.3
    property real polaritySignX: 12
    property int polaritySignSize: 22
    property real polarityTextX: 40
    property real polaritySpacing: 2
    property real polarityHintRight: 12
    // Match: the confidence bar, its guess mark and scale.
    property real matchCaptionY: 380
    property real matchValueY: 377
    property real confidenceY: 396
    property real confidenceHeight: 10
    property real confidenceMarkOverhang: 3
    property real confidenceScaleY: 414
    property int confidenceScaleSize: typeMicro
    // The alignment needle's dial.
    // Room below the hub, and beside and above the arc.
    property real needleHubBottom: 24
    property real needleSideRoom: 36
    property real needleTopRoom: 26
    property real zoneWidth: 6
    property real zoneAlpha: 0.35
    property real majorTickLength: 8
    property real minorTickLength: 5
    property real tickGap: 1
    property real tickLabelRadius: 17
    property real tickLabelDrop: 3
    property real sideLabelDrop: 16
    property real needleGlowWidth: 6
    property real needleGlowAlpha: 0.18
    // The needle stops this far short of the arc.
    property real needleShort: 4
    property real hubRadius: 6
    property real hubDotRadius: 2.5

    // The hint bar.
    property real hintHeight: 42
    property real hintEdgeAlpha: 0.45
    property real hintLampX: 14
    property real hintLampSize: 10
    property real hintHaloSize: 24
    property real hintHaloAlpha: 0.16
    property real hintTextX: 38
    property real hintTextRight: 14

    // The control plates.
    property int windowWidth: 560
    property int displayWidth: 330
    property int syncToggleWidth: 150
    property int syncToggleHeight: 42
    property int toggleRowHeight: 40
    property real windowColumnX: 180
    property int lengthHeight: 30
    property int timeDiameter: 40
    property real timeNoteX: 270
    property real timeNoteY: 52
    property real lengthNoteY: 86
    property int segmentRowHeight: 26
    property real displayToggleY: 84
    property real bandCaptionY: 84
    property real bandY: 98
}
