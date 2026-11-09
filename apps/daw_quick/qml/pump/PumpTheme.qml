import QtQuick
import "../devicekit"

// Pump design tokens: the house effect plate in deep navy. The volume shape
// is coral, the kick it is drawn against slate, time (the playhead, the
// cycle) amber; the shape as actually heard, after depth and anti-click, a
// paler coral.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "pump"
    segmentHeight: 26
    segmentSpacing: 0
    segmentRadius: 6
    segmentLetterSpacing: 0.6
    knobLabelBold: true
    knobLabelSpacing: 1.4
    knobLabelSize: 9

    // The plate: navy, lit from the top left.
    caseTop: "#131f3a"
    caseColor: "#0c1324"
    caseBottom: "#0a1020"
    plate: "#101a2d"
    plateEdge: "#1e2b44"
    plateHi: "#2a3a58"
    well: "#070d1a"
    wellEdge: "#2b3c5c"
    seam: "#1a2640"
    grid: "#132036"
    gridStrong: "#22324e"
    cardRadius: 11
    cardTitleInk: "#e6ecf5"
    cardTitleSize: 10
    cardTitleSpacing: 2

    // The shape, the kick and time.
    property color shape: "#ff7a5c"
    property color shapeSoft: Qt.rgba(1, 122 / 255, 92 / 255, 0.14)
    property color shapeOn: "#2a1b22"
    property color heard: "#ffb199"
    property color kick: "#6f86b0"
    property color time: "#f5b14c"
    property color pointFill: "#0b1224"

    // The shape editor. A followed curve (a slot or band without its own)
    // is drawn faint and dashed; the volume as heard is a fine dashed line.
    curveRadius: 9
    curveEdge: plateEdge
    curveLine: shape
    curveLineWidth: 2.2
    curvePointWidth: 1.6
    curvePointFill: pointFill
    curveBendTravel: 90
    property real followAlpha: 0.45
    property var followDash: [6, 4]
    property real heardAlpha: 0.75
    property var heardDash: [4, 3]

    accent: "#ff7a5c"
    accentSoft: "#a3503d"
    steel: "#7a3a2c"
    segmentAccent: "#ff7a5c"
    knobAccent: "#ff7a5c"

    glacier: "#ffe1d8"
    ink: "#e6ecf5"
    inkDim: "#8d9ab0"
    inkFaint: "#77849c"

    knobTop: "#1a2742"
    knobBottom: "#0b1324"
    track: "#1c2944"
    button: "#070d1a"
    buttonHi: "#0f1830"
    ledOff: "#1c2944"

    warning: "#f5b14c"
    live: "#4fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel geometry ----------------------------------------------------------
    property int panelWidth: 1280
    property int panelHeight: 720
    property real caseRadius: 14
    // Where caseColor sits in the case gradient.
    property real caseMidStop: 0.55
    // Between the cards.
    property real cardGap: 10
    // Inside a card: the inset of its plots and grids, and the top of its body.
    property real cardInset: 10
    property real cardBodyY: 30
    // Captions and headings.
    property real captionSpacing: 0.6
    property real headingSpacing: 2
    // Tracking for small bold capitals (the preset category, the name field).
    property real capsSpacing: 1.4
    // Wrapped notes, and the trigger pages' looser ones.
    property real noteLineHeight: 1.25
    property real modeNoteLineHeight: 1.3
    // A well cut into a card: the library's tiles, the trigger's source.
    property real wellRadius: 7

    // A pill button: its height, its padding round the label, its tracking,
    // and its opacity disabled and when it only follows.
    property real pillHeight: minHitSize
    property real pillPad: 16
    property real pillSpacing: 0.6
    property real pillDisabledOpacity: 0.5
    property real pillDimOpacity: 0.55
    // The accent edge a focused control of the panel's own draws (pills,
    // grid tiles, library tiles, pattern steps).
    property real focusEdgeWidth: strokeBold

    // The header (Kit.DeviceHeader): the wordmark over its subtitle and rule,
    // the preset browser and, once a control is touched or the curve
    // edited, its readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkSize: 30
    wordmarkSpacing: 12
    wordmarkInk: "#ffd5c9"
    subtitleGap: 1
    subtitleSpacing: 2
    subtitleInk: inkDim
    property real ruleGap: 3
    headerBrowserX: 214
    presetDisplayWidth: 340
    presetDisplayHeight: 44
    presetDisplayRadius: 9
    presetTextX: 14
    presetTagY: 6
    presetTagSpacing: capsSpacing
    presetNameY: 18
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: inkDim
    readoutWidth: 260
    readoutSpacing: 1.2
    readoutValueY: 19
    readoutValueInk: shape
    readoutValueSize: typeLabel
    // The name field typed in place of the preset display: its input's top,
    // and the room it leaves on the right for its hint.
    property real nameInputY: 19
    property real nameFieldRight: 120

    // The body: the side column (library, slots), the editor, the trigger,
    // and the lower row (band, output).
    property real bodyY: 68
    property real sideWidth: 200
    property real triggerWidth: 250
    property real lowerHeight: 150
    property real libraryHeight: 352

    // The shape library's tiles and their thumbnails.
    property real shapeTileHeight: 46
    property real thumbHeight: 26
    // The thumbnail's line stays this far inside its box.
    property real thumbInset: 2
    property real thumbStroke: 1.6
    property real shapeNameY: 31

    // The slots card: Play, Morph, the pattern and its steps.
    property real playCaptionY: 38
    property real playDropdownX: 52
    property real playDropdownY: 32
    property real morphY: 64
    property real patternX: 98
    property real patternCaptionY: 68
    property real patternStepperY: 82
    property real stepGridY: 156
    property real stepGap: 5
    property real stepHeight: 34
    property real stepNowStroke: 1.5
    property real stepUnusedOpacity: 0.35
    property real stepNumberX: 5
    property real stepNumberY: 3
    // The step's slot letter sits this far below its centre.
    property real stepLetterDrop: 3
    property real slotNoteY: 236

    // The shape card: the tab row (slots, bands, follow, view and grid), the
    // tool row, the editor and the tool help under it.
    property real tabRowY: 6
    property real tabGap: 3
    property real slotTabsX: 70
    property real slotTabWidth: 34
    property real bandTabsX: 222
    property real bandTabWidth: 46
    // The follow pill, past the band tabs.
    property real resetBandX: 374
    property real gridTileWidth: 36
    property real toolRowY: 34
    // Between groups of tool pills.
    property real toolGroupGap: 8
    property real curveY: 64
    // Under the editor: the tool help.
    property real curveFoot: 24
    // The tool help sits this far above the card's bottom.
    property real toolHelpBottom: 19

    // The trigger card: mode, source, rate and feel, then the mode's page,
    // then Depth and Nudge.
    property real sourceY: 62
    property real sourceHeight: 26
    property real sourceTextX: 10
    property real sourceOffOpacity: 0.5
    property real rateHeadingY: 96
    property real rateY: 110
    property real feelY: 138
    property real compactSegmentHeight: 24
    property real smallSegmentHeight: 22
    property real modePageY: 172
    property real modePageHeight: 210
    // A page's note beside its knob.
    property real modeNoteX: 92
    property real swingNoteY: 14
    property int smallKnobDiameter: 36
    property int largeKnobDiameter: 44
    property real detectY: 82
    property real detectHeight: 8
    property real detectMinWidth: 2
    property real detectCaptionY: 92
    property real lookaheadCaptionY: 110
    property real lookaheadX: 120
    property real lookaheadY: 106
    property real lengthCaptionY: 138
    property real lengthX: 76
    property real lengthY: 134
    property real listenY: 164
    property real listenHeight: 30
    property real noteStepperY: 18
    property real noteStepperWidth: 64
    property real noteHighX: 82
    property real learnX: 154
    property real lastNoteY: 46
    property real velocityY: 64
    property real velocityNoteY: 72
    property real midiLengthCaptionY: 166
    property real midiLengthY: 162
    property real depthRowY: 388
    property real depthRowGap: 22

    // The band card.
    property real bandCardX: 14
    property real bandColumnWidth: 270
    property real stereoY: 60
    property real bandNoteY: 92
    property real bandKnobsX: 300
    property real bandKnobsY: 26
    property real bandKnobGap: 20
    property int bandKnobDiameter: 42

    // The output card: gain reduction, then Mix and Output.
    property real reductionCaptionY: 11
    property real reductionY: 28
    property real reductionGap: 3
    property real reductionLabelWidth: 28
    property int reductionLabelSize: typeMicro
    property real reductionHeight: 10
    property real reductionBandHeight: 6
    property real outputKnobsY: 58
    property real outputKnobGap: 18
    property int outputKnobDiameter: 38

    // The shape editor: the plot inside the well, the audio behind it, the
    // scales, the playhead's dot, the rubber band and the key.
    property real curvePlotSide: 14
    property real curvePlotTop: 30
    property real curvePlotBottom: 24
    property real ioInputAlpha: 0.4
    property real ioOutputAlpha: 0.55
    property real kickAlpha: 0.55
    // The output drawn faint over the kick.
    property real kickOutputAlpha: 0.22
    // The cycle's length label sits this far above the editor's bottom.
    property real footLabelBottom: 8
    property real lengthLabelRight: 46
    property real playheadDot: 8
    property real rubberFillAlpha: 0.08
    property real rubberEdgeAlpha: 0.6
    property real keyInset: 10
    property real keyGap: 14
    property real keySwatchGap: 5
}
