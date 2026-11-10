import QtQuick
import "../devicekit"

// Lens design tokens: a precision optical instrument. A deep near-black blue
// case, inky glass plates and darker wells; the primary curve a luminous
// cyan over a filled gradient, the second curve a warm coral, the sidechain
// a dashed violet and the difference gold. Grids are fine and cool, the
// lettering crisp and white.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "lens"
    cardSheen: Qt.rgba(0.6, 0.8, 1, 0.05)

    // The case: night blue, a breath lighter at the top edge.
    caseTop: "#111829"
    caseColor: "#0a0f1b"
    caseBottom: "#060911"
    // Where the gradient reaches caseColor.
    property real caseMidStop: 0.3
    // Glass plates and the recessed wells inside them.
    plate: "#0d1424"
    plateEdge: "#1d2943"
    plateHi: "#2c3b5c"
    well: "#04070e"
    wellEdge: "#1a2540"
    seam: "#141d33"
    grid: "#111a2e"
    gridStrong: "#1f2d4c"
    property color gridText: "#5d6d8f"

    // The curves.
    property color primary: "#7fe3ff"
    property color primaryDeep: "#1e7fd0"
    property color secondary: "#ff9a76"
    property color hold: "#e9f6ff"
    property color overlay: "#b9a3ff"
    property color difference: "#ffd166"
    property color resonance: "#ff6f91"
    // The spectrum's well glows faintly toward the top.
    property color wellGlow: "#070c18"
    // Frost over a frozen picture.
    property color frostFill: Qt.rgba(0.62, 0.85, 1, 0.05)
    property color frostEdge: Qt.rgba(0.62, 0.85, 1, 0.45)
    // The cursor readout's glass.
    property color cursorReadoutFill: Qt.rgba(0.02, 0.04, 0.08, 0.86)

    // Controls: cyan arcs, a white index.
    accent: "#7fe3ff"
    accentSoft: "#3f8fb8"
    steel: "#2a6ea0"
    glacier: "#f2f8ff"
    property color glint: "#ffffff"

    ink: "#e8f0ff"
    inkDim: "#94a3c4"
    inkFaint: "#7180a0"

    knobTop: "#2a3654"
    knobBottom: "#070b15"
    track: "#18223a"
    button: "#0a101d"
    buttonHi: "#121b2f"

    property color freeze: "#9fd8ff"
    warning: "#f2b25c"
    live: "#5fe3c0"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The curves' strokes and washes.
    property real overlayAlpha: 0.85
    property real overlayStroke: 1.4
    property real primaryFillTopAlpha: 0.42
    property real primaryFillMidStop: 0.55
    property real primaryFillMidAlpha: 0.2
    property real primaryFillBottomAlpha: 0.02
    property real primaryGlowAlpha: 0.18
    property real primaryGlowWidth: 6
    property real primaryStroke: 1.7
    property real secondaryGlowAlpha: 0.2
    property real secondaryGlowWidth: 5
    property real secondaryStroke: 1.5
    property real holdAlpha: 0.55
    // The difference's zero line, dashed, and its curve.
    property real differenceAxisAlpha: 0.35
    property real differenceDashPeriod: 6
    property real differenceDashLength: 3
    property real differenceStroke: 1.6
    // A resonance marker: a small down-pointing triangle above the curve,
    // its note lettered above that.
    property real resonanceLift: 6
    property real resonanceMarkHalfWidth: 4
    property real resonanceMarkHeight: 7
    property real resonanceLabelLift: 9
    property real crosshairAlpha: 0.35
    property real crosshairDotRadius: 3.5

    // The spectrum plot: the dB scale left, margins right (wider when the
    // difference scale shows) and the frequency labels below.
    property int spectrumWidth: 960
    property int spectrumHeight: 340
    property real spectrumScaleWidth: 46
    property real spectrumTop: 14
    property real spectrumRightPad: 16
    property real differenceScaleWidth: 44
    property real spectrumAxisHeight: 26
    property real cursorReadoutPad: 18
    property real cursorReadoutHeight: 22

    // The spectrogram: columns this wide, each run overlapping the next by
    // half a pixel so no seam shows, and its night-to-white map as
    // [position, [r, g, b]] stops with a gamma on the level.
    property int spectrogramHeight: 110
    property real spectrogramColumnWidth: 3
    property real spectrogramOverlap: 0.5
    property var spectrogramStops: [[0, [4, 7, 14]], [0.3, [8, 22, 58]], [0.52, [20, 74, 150]],
                                    [0.7, [44, 156, 230]], [0.84, [128, 226, 255]], [1, [255, 255, 255]]]
    property real spectrogramGamma: 1.35

    // The panel's layout.
    property int panelWidth: 1380
    property int panelHeight: 946
    property real panelRadius: 10
    property real cardGap: 10
    property real cardInset: 14
    // Where a card's first content sits below its title.
    property real cardContentY: 30
    property real toggleY: 32
    property real noteBottom: 26
    property real captionSpacing: 1.4
    property real captionTightSpacing: 1.2
    property real subcaptionSpacing: 0.6
    // Spaced caps for banners and toggle labels.
    property real bannerSpacing: 2
    // A control with nothing to act on.
    property real idleOpacity: 0.45

    // The header (Kit.DeviceHeader): the wordmark over its spectrum sweep
    // and subtitle, the preset browser and, once a control is touched, its
    // readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkSize: 27
    wordmarkSpacing: 12
    wordmarkInk: glacier
    // How much shorter than the wordmark its sweep is, and where the sweep
    // reaches the primary.
    property real wordmarkRuleTrim: 12
    property real wordmarkSweepStop: 0.6
    subtitleGap: 7
    subtitleSpacing: 2.4
    subtitleInk: inkDim
    headerBrowserX: 230
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
    readoutWidth: 260
    readoutSpacing: captionTightSpacing
    readoutValueY: 17

    // The spectrum card's toolbar: the key and the drawing buttons.
    property real toolbarY: 7
    property real toolbarSpacing: 14
    property real toolbarDividerHeight: 18
    property real keyGap: 5
    property real keySwatchWidth: 11
    // At least the kit's least hit size.
    property real buttonHeight: minHitSize
    property real buttonSpacing: 3
    property real buttonOnFillAlpha: 0.16
    property real buttonOnEdgeAlpha: 0.6
    property real ceilingTextWidth: 54

    // Row A: the spectrum and the resonance table.
    property real rowAY: 76
    property real rowAHeight: 340
    property real sideWidth: 310
    property real spectrumY: 30
    property real tableHeaderY: 32
    property real noteColumnWidth: 84
    property real frequencyColumnWidth: 96
    property real tableX: 10
    property real tableY: 48
    property real tableSpacing: 1
    property real tableRowHeight: 30
    property real strongestWashAlpha: 0.08
    property real rankWidth: 16
    property real noteX: 22
    property real noteNameY: 3
    property real noteCentsY: 17
    property real frequencyX: 92
    property real levelX: 186
    property real levelBarX: 248
    property real levelBarWidth: 34
    property real levelBarHeight: 4

    // Row B: the spectrogram and the reading.
    property real rowBHeight: 132
    property real gramY: 28
    property real colourScaleHeight: 8
    property real colourScaleLabelY: 41
    property real readingLinesY: 60
    property real readingLineHeight: 14

    // Row C: the controls.
    property real analysisPlateWidth: 372
    property real averagingPlateWidth: 280
    property real displayPlateWidth: 270
    property real referencePlateWidth: 190
    property real segmentY: 43
    property real secondCaptionY: 78
    property real secondSegmentY: 91
    property real segmentRowHeight: 24
    property real holdToggleY: 82
    property real holdToggleWidth: 150
    property real decayKnobX: 176
    property real decayKnobY: 74
    property int decayKnobDiameter: 30
    property real differenceToggleY: 78
    property real differenceToggleHeight: 36
    property real differenceLedX: 11
    property real differenceLedSize: 9
    property real differenceTextX: 30
    property real freezeToggleHeight: 52
    property real freezeNoteY: 94
    property real rowCHeight: 162

    // Row D: match EQ and tonal balance (ADR-0120).
    property real matchPlateWidth: 800
    property real matchPlotWidth: 420
    property real matchControlsX: 446
    property real matchKnobY: 74
    property int matchKnobDiameter: 30
    property real matchButtonsY: 150
    property real matchRangeDb: 12
    property color matchOutsideFill: Qt.rgba(0.02, 0.03, 0.06, 0.6)
    property real matchBandDotRadius: 6
    property real matchBandDotTextDrop: 3
    property real tonalPickerY: 30
    property real tonalPickerNameWidth: 220
    property real tonalPlotY: 62
    property real tonalFooterBottom: 26
    property real tonalRangeDb: 9
    property real tonalRangeAlpha: 0.18
    property real tonalDotRadius: 3

    // The control last touched, read back in the display.
    lastTouched: ""
}
