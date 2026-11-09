import QtQuick
import "../devicekit"

// Vista design tokens, "glacier": a precision mastering tool. Deep slate
// case, machined steel-blue plates, glacier-white lettering and a single
// ice-blue signal colour; amber only for warnings and the safety floor.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "vista"
    cardSheen: Qt.rgba(1, 1, 1, 0.05)

    // The case: deep slate, a touch lighter at the top edge.
    caseTop: "#17212c"
    caseColor: "#0f171f"
    caseBottom: "#0a1016"
    // Machined plates and the recessed wells inside them.
    plate: "#141e29"
    plateEdge: "#263649"
    plateHi: "#33475e"
    well: "#0a1118"
    wellEdge: "#22324a"
    seam: "#1b2938"
    grid: "#1d2b3b"
    gridStrong: "#2c3f55"

    // Signal: glacier white over steel blue.
    accent: "#9fd3ff"
    accentSoft: "#6aa8dc"
    steel: "#4f7fae"
    glacier: "#eaf5ff"
    property color glint: "#ffffff"
    // Correlation zones: healthy, wide, out of phase.
    property color positive: "#8fdcc2"
    property color wide: "#9fd3ff"
    property color negative: "#ff7a6b"

    ink: "#e9f1f8"
    inkDim: "#8ea3b8"
    inkFaint: "#7288a0"

    knobTop: "#2a3a4c"
    knobBottom: "#0d151d"
    track: "#1e2c3c"
    button: "#111a24"
    buttonHi: "#18242f"

    warning: "#f2b25c"
    live: "#8fdcc2"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel layout ----------------------------------------------------------------
    property int panelWidth: 1240
    property int panelHeight: 600
    property real panelRadius: 10
    property real panelMargin: 16
    // Between the cards of a row.
    property real panelGap: 10
    // Where the case gradient reaches its middle colour.
    property real caseMidStop: 0.35
    // Inner inset of a card's contents from its sides.
    property real cardInset: 14
    // A knob cell: its width and the room below the knob, with and without a readout.
    property int cellWidth: 80
    property int cellPad: 36
    property int cellReadoutPad: 50
    // Small spaced-caps captions over controls.
    property real captionSpacing: 1.4

    // The header (Kit.DeviceHeader): the wordmark over its fading underline
    // and subtitle, the preset browser and, once a control is touched, its
    // readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkInk: glacier
    wordmarkSize: 27
    wordmarkSpacing: 12
    // The underline and subtitle sit just in from the wordmark's edge.
    property real wordmarkIndent: 2
    property real wordmarkUnderlineGap: 1
    property color wordmarkFade: Qt.rgba(steel.r, steel.g, steel.b, 0.1)
    subtitleGap: 7
    subtitleSpacing: 2.6
    subtitleInk: inkDim
    headerBrowserX: 220
    presetDisplayWidth: 276
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
    readoutTextX: 14
    readoutNameY: 5
    readoutValueY: 17
    readoutSpacing: 1.2

    // The image row: vectorscope, bands, overall width.
    property real imageRowY: 76
    property real imageRowHeight: 340
    property int scopeCardWidth: 300
    property real scopeY: 24
    property int scopeSize: 244
    property real correlationCaptionY: 272
    property real correlationReadoutY: 270
    property real correlationY: 284
    property int correlationHeight: 46
    property int bandsCardWidth: 640
    property real bandCountCaptionY: 13
    property real bandCountY: 7
    property int bandCountWidth: 148
    property int bandCountHeight: 22
    property real bandsX: 6
    property real bandsY: 36
    // Clearance under the band strip.
    property real bandsFoot: 8
    property real widthKnobY: 28
    property int widthCellWidth: 140
    property int widthKnobDiameter: 64
    property real stageY: 156
    property real stageSideInset: 12
    property int stageCardHeight: 70
    // Inset of the readout list and the mono-sum promise in the image card.
    property real imageInset: 16
    property real readoutListY: 234
    property int readoutRowHeight: 16
    property real readoutRowSpacing: 1.3
    // The mono-sum promise pill, its bottom this far above the card's.
    property real promiseRise: 40
    property int promiseHeight: 26
    property real promiseSpacing: 1.8

    // The tool row: mono bass, stereoize, safe width, field, output.
    property int bassCardWidth: 196
    property int stereoizeCardWidth: 290
    property int safeCardWidth: 216
    property int fieldCardWidth: 360
    property real toggleX: 12
    property real toggleY: 32
    property real knobRowY: 34
    property real lowerKnobY: 64
    property int lowerKnobDiameter: 40
    property int segmentRowHeight: 24
    property real amountX: 8
    property int amountCellWidth: 96
    property int amountDiameter: 54
    property real modeX: 118
    property real modeCaptionY: 36
    property real modeY: 50
    property real haasY: 84
    property int haasDiameter: 34
    property real stereoizeNoteY: 90
    property real fieldRowX: 6
    property int fieldDiameter: 44
    property real fieldControlsX: 176
    property real invertCaptionY: 80
    property real invertY: 94
    property int outputDiameter: 48

    // ---- Vectorscope -------------------------------------------------------------------
    property int scopeDefaultSize: 240
    // Margin between the glass and the item's edge, room for the rim letters.
    property real scopeRim: 14
    property color glassCentre: "#111c27"
    // How much of the old trace each live frame erases.
    property color scopePersistenceFade: Qt.rgba(0, 0, 0, 0.30)
    property color scopeHalo: Qt.rgba(accent.r, accent.g, accent.b, 0.16)
    property color scopeCore: Qt.rgba(glacier.r, glacier.g, glacier.b, 0.85)
    property real scopeHaloDot: 4
    property real scopeCoreDot: 1.6
    property real scopePreviewDot: 1.8
    // The preview cloud's alpha: a floor plus a peak toward the centre.
    property real previewAlphaFloor: 0.10
    property real previewAlphaPeak: 0.25

    // ---- Correlation meter ---------------------------------------------------------------
    property int correlationDefaultWidth: 240
    property real correlationTrackY: 12
    property int correlationTrackHeight: 10
    property color correlationOutOfPhase: Qt.rgba(negative.r, negative.g, negative.b, 0.45)
    property color correlationWideFrom: Qt.rgba(wide.r, wide.g, wide.b, 0.18)
    property color correlationWideTo: Qt.rgba(wide.r, wide.g, wide.b, 0.20)
    property color correlationInPhase: Qt.rgba(positive.r, positive.g, positive.b, 0.40)
    property real correlationTickMajor: 6
    property real correlationTickMinor: 4
    // The floor line reaches this far past the track on each side.
    property real floorOverhang: 5
    // The input caret sits this far above the track.
    property real caretRise: 12
    property real outputBarWidth: 3
    property real outputBarOverhang: 3
    property real outputBarRadius: 1
    property real outputHaloWidth: 9
    property real outputHaloExtend: 4
    property color outputHalo: Qt.rgba(glacier.r, glacier.g, glacier.b, 0.16)

    // ---- Stage --------------------------------------------------------------------------
    property int stageDefaultWidth: 200
    property int stageDefaultHeight: 80
    property color imageWash: Qt.rgba(accent.r, accent.g, accent.b, 0.10)
    property color imageHalo: Qt.rgba(accent.r, accent.g, accent.b, 0.25)
    property real imageHaloWidth: 9
    property real imageLineWidth: 3
    property real speakerWidth: 8
    property real speakerHeight: 10
    property real listenerRadius: 3

    // ---- Band strip ---------------------------------------------------------------------
    property int bandsDefaultWidth: 600
    property int bandsDefaultHeight: 260
    // The plot frame's insets within the strip: room above for the band
    // names, and at the right.
    property real bandsPlotTop: 26
    property real bandsPlotRight: 12
    // The dashed line at 100% width.
    property var bandsUnityDash: [4, 3]
    property color monoShade: Qt.rgba(steel.r, steel.g, steel.b, 0.16)
    property color monoHatch: Qt.rgba(steel.r, steel.g, steel.b, 0.35)
    property real monoHatchPitch: 7
    property real monoLabelX: 5
    property real monoLabelRise: 16
    property real monoLabelSpacing: 1.5
    // The MONO label shows only when the shade is at least this wide.
    property real monoLabelMinWidth: 34
    // Alternate bands sit on a faintly lighter ground.
    property color bandAlternateFill: Qt.rgba(1, 1, 1, 0.018)
    property real bandColumnMin: 14
    property real bandColumnMax: 64
    property color bandWideTop: Qt.rgba(accent.r, accent.g, accent.b, 0.55)
    property color bandNarrowTop: Qt.rgba(steel.r, steel.g, steel.b, 0.55)
    property color bandColumnFoot: Qt.rgba(steel.r, steel.g, steel.b, 0.10)
    property real bandCapHeight: 2
    property color bandSettingEdge: Qt.rgba(glacier.r, glacier.g, glacier.b, 0.35)
    // The value and band name sit this far above their anchors.
    property real bandValueRise: 16
    property real bandNameRise: 16
    property real bandNameSpacing: 1.6
    // The live correlation chip at a band's foot.
    property real chipMinBandWidth: 40
    property real chipRise: 22
    property real chipPad: 12
    property int chipHeight: 16
    // Crossover handles.
    property int handleWidth: 56
    property real handleGrabWidth: minHitSize
    property real gripPad: 12
    property int gripHeight: 16
}
