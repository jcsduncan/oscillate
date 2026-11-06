import QtQuick
import "../devicekit"

// Dust design tokens: a smoky listening room after midnight. A brown-black
// case like an old cabinet's lacquer, dusty amber for the record and what
// it plays, a desaturated teal for the air (rain, wind, the duck). Each
// layer keeps its own muted tint, on its card and its meter alike.
KitTheme {
    prefix: "dust"
    knobDiameter: 28
    knobMinWidth: 52
    knobWidthPad: 22
    knobHeightPad: 30
    knobLabelBold: true
    knobLabelSpacing: 1.1
    // A layer's level fader: a slim groove lit from the bottom, a cap the
    // fader's width, the value (less its "dB") under it. A press jumps to
    // the pointer; Shift at the press drags finely from where it is.
    faderWidth: 26
    faderHeight: 160
    faderSpeaksGroup: true
    faderTop: 0
    faderBottom: 14
    faderAreaBottom: 14
    faderJump: "always"
    faderLitFromCentre: false
    faderTrackFull: false
    faderTrackFocusEdge: wellEdge
    faderCapWidth: 0
    faderCapInset: 2
    faderCapHeight: 10
    faderGripHeight: 1
    faderFoot: "value"
    segmentHeight: 22
    segmentSpacing: 3
    segmentRadius: 4
    segmentLetterSpacing: 0.4
    switchRadius: -1
    switchHeight: 20
    switchTextSize: 10
    switchLetterSpacing: 1.4
    cardRadius: 8
    cardTitleSpacing: 2.6

    // The cabinet.
    caseTop: "#1a1512"
    caseColor: "#14100d"
    caseBottom: "#0e0b09"
    plate: "#1d1814"
    plateEdge: "#2f2721"
    plateHi: "#463a30"
    well: "#0b0907"
    wellEdge: "#2f2721"
    seam: "#251f1a"
    grid: "#221c17"
    gridStrong: "#30281f"

    // Dusty amber (the record, the bed) and the air's teal (the duck).
    accent: "#c8a36a"
    accentSoft: "#5b4526"
    steel: "#3c3127"
    knobAccent: accent
    segmentAccent: accent
    property color amber: "#c8a36a"
    property color amberHot: "#e6c48a"
    property color teal: "#6f9c98"
    property color tealDeep: "#2c4644"
    // The record itself.
    property color vinyl: "#0a0807"
    property color vinylGroove: "#1b1714"
    property color vinylSheen: Qt.rgba(1, 0.92, 0.8, 0.07)
    property color label: "#8a5a2e"
    property color labelInk: "#f0dcb8"
    property color spark: "#ffe2a8"
    // A layer that is off: its card and meter at this opacity.
    property real offOpacity: 0.42

    // Each layer's tint, keyed by its id in dust.<layer>.*.
    readonly property var layerTint: ({
        vinyl: "#c8a36a", tape: "#b98458", hum: "#cdb15e", radio: "#93ab92",
        rain: "#6f9c98", wind: "#8ea3a8", room: "#a8977f", machine: "#9b8c73"
    })

    glacier: "#f4e9d8"
    ink: "#ebe0cf"
    inkDim: "#a3958a"
    inkFaint: "#6b5f55"

    knobTop: "#3a3029"
    knobBottom: "#120e0b"
    knobRing: "#2f2721"
    pointer: "#f4e9d8"
    track: "#2a221c"
    button: "#17130f"
    buttonHi: "#211b16"
    ledOff: "#2f2721"

    warning: "#e0925a"
    live: "#8fb39a"
    meterSafe: "#8fb39a"
    meterHot: "#d9b46a"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"
    property string display: "DejaVu Serif Condensed"

    // ---- The case and its layout ----------------------------------------------------
    property int panelWidth: 1360
    property real caseRadius: 10
    // Where the case's middle colour sits down its gradient.
    property real caseMidStop: 0.45
    property real sectionGap: 10
    property real headerHeight: 52
    property real topRowHeight: 196
    property real shelfHeight: 268
    property real footRowHeight: 136
    // The wrapped notes along a card's foot.
    property int noteSize: typeCaption
    // A note's baseline room above its card's foot.
    property real noteRise: 22

    // ---- The header ------------------------------------------------------------------
    property int wordmarkSize: 28
    property real wordmarkSpacing: 9
    property real taglineSpacing: 2.2
    // Small bold upper-case captions (the preset's, OUT), the readout's
    // parameter name, and the tags under the record and over the seed.
    property real captionSpacing: 1.4
    property real readoutSpacing: 1.2
    property real tagSpacing: 1.6
    // The preset display: its place in the header, its size and the columns
    // inside it (caption and name, a divider, then the last-touched readout).
    property real presetX: 330
    property real presetTop: 5
    property real presetWidth: 380
    property real presetHeight: 42
    property real presetCaptionTop: 5
    property real presetNameTop: 16
    property real presetNameWidth: 180
    property real presetDividerX: 202
    property real presetReadoutX: 212
    property real presetValueTop: 17
    property real presetChevronInset: 9
    // The output's peak meters.
    property real outMeterWidth: 8
    property real outMeterHeight: 44

    // ---- The record and the bed cards ------------------------------------------------
    property real recordCardWidth: 470
    property real recordTop: 24
    // The bed meter inside its card: in from the sides, under the title,
    // and clear of the foot.
    property real bedInset: 14
    property real bedTop: 30

    // ---- A layer's card ----------------------------------------------------------------
    property real layerHeaderHeight: 34
    property real layerFaderWidth: 24
    property real layerGlyphTop: 11
    property real layerGlyphSize: 14
    property real layerSwitchX: 22
    // An on layer's card edge, its tint at this alpha.
    property real layerEdgeAlpha: 0.45

    // ---- The foot cards ------------------------------------------------------------------
    property real footKnobDiameter: 36
    property real footKnobsTop: 28
    property real ageCardWidth: 400
    property real ageKnobSpacing: 18
    property real duckCardWidth: 470
    property real duckSourceTop: 32
    property real duckSourceWidth: 210
    property real duckKeyTop: 62
    property real duckKnobsX: 232
    property real seedInset: 14
    property real seedTop: 34
    property real seedStepperWidth: 74
    property real diceWidth: 30
    property real diceHeight: 22
    property real diceDisabledOpacity: 0.5

    // ---- A layer's glyph ------------------------------------------------------------------
    property real glyphSize: 16
    property real glyphStroke: 1.2

    // ---- The bed meter -----------------------------------------------------------------------
    property real bedMeterWidth: 440
    property real bedMeterHeight: 200
    property int meterLabelSize: 7
    property real meterLabelSpacing: 0.8
    // A layer's bar: its well's widest, and the room it leaves in its slot.
    property real bedWellWidth: 18
    property real bedWellRoom: 12
    property real duckWellWidth: 14
    property real meterGlyphSize: 12
    property real meterTitleTop: 18
    property real duckValueTop: 16
    // A bar's gradient: its tint lightened at the top, darkened at the foot.
    property real meterTintLighten: 1.25
    property real meterTintDarken: 1.6

    // ---- The record -------------------------------------------------------------------------
    property real recordWidth: 360
    property real recordHeight: 200
    // The record's opacity with the vinyl layer off.
    property real discOffOpacity: 0.6
    property real scratchWidth: 0.7
    property real deepScratchWidth: 1.4
    // A scratch's ink alpha: a faint base, deeper with wear.
    property real scratchAlpha: 0.10
    property real scratchWearAlpha: 0.25
    property real speckAlpha: 0.35
    property real labelRingWidth: 0.8
    // The label's title and side line: their smallest type.
    property int labelTitleMinSize: 7
    property int labelSideMinSize: 6
    property real spindleRadius: 3
    // The tonearm, its headshell, and the pivot's boss.
    property real armWidth: 4
    property real headshellWidth: 7
    property real pivotRadius: 8
    property real sparkLeadSize: 4
    property real sparkSize: 2
    // The speed and state beside the record.
    property real recordInfoGap: 22
    property int recordSpeedSize: typeHeading

    lastTouched: ""
}
