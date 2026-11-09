import QtQuick
import "../devicekit"

// Press design tokens: a studio bus compressor. A warm graphite case,
// slate plates and a deep screen; the transfer curve and the controls lit
// in amber, the gain reduction in a hot orange that deepens with the
// squeeze, and the key in cyan when another track drives the detector.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "press"

    // The case: graphite with a warm cast, a touch lighter at the top edge.
    caseTop: "#22201c"
    caseColor: "#191714"
    caseBottom: "#11100e"
    plate: "#1d1b18"
    plateEdge: "#302c27"
    plateHi: "#433d35"
    well: "#0d0c0a"
    wellEdge: "#2a2622"
    seam: "#26231f"

    // The transfer screen.
    property color screen: "#0a0908"
    grid: "#1d1a16"
    gridStrong: "#2f2a23"
    property color unity: "#4a4237"
    property color knee: "#ffcf6b"
    property color operating: "#fff3d6"

    // Gain reduction, and the key.
    property color reduction: "#ff9a3c"
    property color reductionDeep: "#ff5a3c"
    property color key: "#6be3ff"

    accent: "#ffcf6b"
    accentSoft: "#a3803a"
    steel: "#7a5f2a"
    glacier: "#fbf6ee"
    ink: "#f1ece4"
    inkDim: "#a89f93"
    inkFaint: "#8c8376"

    knobTop: "#4a443c"
    knobBottom: "#13110f"
    track: "#2a2621"
    button: "#171512"
    buttonHi: "#201d19"
    ledOff: "#302c27"

    warning: "#f2b25c"
    live: "#7fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel: its size, the case's corners and where the case gradient
    // reaches its body colour.
    property int panelWidth: 1240
    property int panelHeight: 680
    property real caseRadius: 10
    property real caseMidStop: 0.35
    // The gap between plates.
    property real plateGap: 10
    // Small bold captions and tags.
    property real captionSpacing: 1.4
    property real tagSpacing: 1.2

    // The header (Kit.DeviceHeader): the wordmark over its underline and
    // subtitle, the preset browser and, once a control is touched, its
    // readout.
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
    readoutSpacing: tagSpacing
    readoutValueY: 17

    // The keyed tag left of the status badge: its gap, padding round the
    // text, tint and tracking.
    property real keyedBadgeGap: 10
    property real keyedBadgePad: 24
    property real keyedFillAlpha: 0.12
    property real keyedSpacing: 1

    // Plates: content inset, the y of the first control row, padding.
    property real cardInset: 10
    property real cardContentY: 30
    property real cardBodyY: 34
    property real cardPad: 14

    // Row A: transfer, reduction, levels, dynamics.
    property real rowAY: 76
    property real rowAHeight: 400
    property int transferWidth: 430
    property int reductionWidth: 140
    property int levelsWidth: 200

    // The transfer screen's plot area and drawing.
    property real transferRadius: 6
    property real transferPlotX: 40
    property real transferPlotY: 30
    property real transferPlotRight: 14
    property real transferPlotBottom: 24
    property real kneeFillAlpha: 0.08
    property real unityDash: 4
    property real thresholdLineAlpha: 0.5
    // The curve's glow: a wide faint stroke, a halo, then the line.
    property real curveGlowAlpha: 0.12
    property real curveGlowWidth: 8
    property real curveHaloAlpha: 0.28
    property real curveHaloWidth: 3.5
    property real curveWidth: 1.8
    property real operatingCrossAlpha: 0.25
    property real operatingHaloAlpha: 0.25
    property real operatingHaloRadius: 8
    property real operatingDotRadius: 4
    // The readouts across the screen's top right.
    property real screenReadoutRight: 14
    property real screenReadoutY: 8
    // The threshold handle: a diamond on the curve.
    property real handleSize: 13
    property real handleStroke: 1.5

    // The gain-reduction meter.
    property real reductionBarX: 30
    property real reductionBarHeight: 270
    property real reductionValueY: 316
    property int reductionValueSize: 26
    property real reductionCaptionY: 352
    property int reductionMeterWidth: 64
    property int reductionMeterHeight: 300
    property real reductionWellWidth: 24
    property real reductionFillInset: 2
    property real reductionScaleGap: 3
    property real reductionTickLength: 5
    property real reductionLabelX: 8
    property real reductionLabelY: -6

    // The level meters: an IN and an OUT pair of peak meters.
    property real meterHeight: 290
    property real meterPairWidth: 76
    // The pair's caption and peak readouts above the meters.
    property real meterPairHead: 54
    property real peakRowY: 14
    property real peakTextWidth: 34
    property real meterRowY: 32
    property real meterWidth: 18
    property real levelsNoteBottom: 26

    // The dynamics dials.
    property real thresholdKnobY: 40
    property int thresholdDiameter: 120
    property real dialRowY: 236
    property real dialRowSpacing: 40
    property int dialDiameter: 58
    property real dynamicsNoteBottom: 34

    // Row B: timing, detector, output.
    property int timingWidth: 380
    property real timingSpacing: 26
    property real timingNoteY: 8
    property int detectorWidth: 480
    property real toggleY: 38
    property int toggleWidth: 230
    property int toggleHeight: 42
    property real detectorKnobX: 262
    property real detectorNoteY: 92
    property real detectorNoteWidth: 236
    property real outputNoteX: 120
    property real outputNoteY: 42
}
