import QtQuick
import "../devicekit"

// Trim design tokens: a console channel strip. A cool graphite case,
// slate plates with recessed wells, ivory legends; the controls lit in
// cobalt sky; meters in the console's green, amber over -12 dBFS and red
// over -3; the stereo image in sky for the left and coral for the right.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "trim"

    // The case: graphite with a blue cast, a touch lighter at the top edge.
    caseTop: "#1d2128"
    caseColor: "#15181d"
    caseBottom: "#0e1014"
    // Where the case gradient reaches caseColor.
    property real caseMidStop: 0.35
    // Plates and the recessed wells inside them.
    plate: "#191d23"
    plateEdge: "#2a303a"
    plateHi: "#3b4350"
    well: "#0b0d11"
    wellEdge: "#262c36"
    seam: "#222730"

    // The stereo stage.
    property color stage: "#0d1016"
    property color stageLine: "#1e2530"
    property color stageStrong: "#2e3846"
    property color left: "#5cb8ff"
    property color right: "#ff8a6b"
    property color image: "#a9d8ff"
    // The field outside the speakers, shaded.
    property color stageOutside: Qt.rgba(stageStrong.r, stageStrong.g, stageStrong.b, 0.18)

    // Meters.
    property color meterLow: "#4fd18b"
    property color meterMid: "#f2c35c"
    property color meterHigh: "#ff5d55"
    property color meterOff: "#141820"
    property color peakHold: "#e9f1ff"
    // The foot of the meter's green zone.
    property color meterFoot: Qt.darker(meterLow, 1.6)

    // Controls: sky arcs, a pale index.
    accent: "#5cb8ff"
    accentSoft: "#2f6fa3"
    steel: "#2d5f8a"
    glacier: "#eef4fb"
    property color glint: "#ffffff"
    gridStrong: "#3a4555"

    ink: "#e9edf3"
    inkDim: "#98a2b1"
    inkFaint: "#7c8697"

    knobTop: "#454c58"
    knobBottom: "#111419"
    track: "#252b35"
    button: "#13161b"
    buttonHi: "#1c2027"

    // Monitoring and polarity states.
    property color solo: "#f2c35c"
    property color invert: "#ff6b5e"
    // An inverted polarity switch: a wash and an edge in the invert colour.
    property real invertFillAlpha: 0.14
    property real invertEdgeAlpha: 0.7

    warning: "#f2b25c"
    live: "#4fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // ---- Layout ------------------------------------------------------------------

    // The panel and its plates.
    property int panelWidth: 1240
    property int panelHeight: 740
    property real panelRadius: 10
    property real plateGap: 10
    // Controls and notes inside a plate, from its left edge.
    property real plateInset: 14
    // A centred note's inset either side.
    property real centredNoteInset: 16
    // A note at a plate's foot, from the bottom edge.
    property real footNoteRise: 34
    // Small caps captions over controls.
    property real captionSpacing: 1.4
    // Row A (level, meters, image); Row B fills the height below it.
    property real rowAY: 76
    property real rowAHeight: 400

    // The header: wordmark, tagline, preset browser, status.
    property real headerY: 10
    property real headerHeight: 54
    property real wordmarkY: 2
    property int wordmarkSize: 27
    property real wordmarkSpacing: 9
    // The underline and tagline sit in from the wordmark's left edge.
    property real wordmarkIndent: 2
    property real underlineGap: 1
    // The underline stops short of the wordmark's right edge.
    property real underlineTrim: 10
    property real underlineRadius: 1
    property real taglineGap: 7
    property real taglineSpacing: 2.4
    property real browserX: 250
    property real browserY: 5
    property int presetDisplayWidth: 540
    property int presetDisplayHeight: 42
    property real focusStroke: 1.5
    property real presetTextX: 14
    property real presetCaptionY: 5
    property real presetNameY: 16
    property int presetNameWidth: 250
    property real presetNameSpacing: 0.4
    property real presetDividerX: 276
    // The divider's gap from the display's top and bottom.
    property real presetDividerInset: 8
    property real readoutX: 288
    property real readoutValueY: 17
    // Room right of the readout for the chevron.
    property real readoutRight: 26
    property real readoutSpacing: 1.2
    property real presetChevronMargin: 10
    property real statusY: 14

    // The level plate: the gain knob, its value, the unity button.
    property int levelPlateWidth: 300
    property real gainKnobY: 46
    property int gainKnobDiameter: 150
    property real gainValueY: 228
    property int gainValueSize: 44
    property real gainCaptionY: 286
    property real unityY: 312
    property int unityWidth: 150
    property int unityHeight: 30
    // The unity button while the gain already is unity.
    property real unityIdleOpacity: 0.45
    property real unitySpacing: 1.6
    property real levelNoteY: 352

    // The meters plate: an IN and an OUT pair with the dB scale between.
    property int meterPlateWidth: 330
    property real meterLength: 250
    property real meterPairX: 26
    property real meterPairY: 34
    property real outPairX: 166
    property int meterPairWidth: 70
    // A pair's caption, peak readouts and L/R labels around its meters.
    property real meterPairChrome: 64
    property real peakReadoutY: 16
    property int peakReadoutWidth: 32
    // The meters within a pair, below its caption and readouts.
    property real pairMeterY: 34
    property int meterWidth: 22
    // The L/R labels' gap under the meters.
    property real meterLabelGap: 4
    property real meterScaleX: 108
    property int meterScaleWidth: 50
    property real scaleTickLength: 5
    // How far a scale label sits above its tick.
    property real scaleLabelLift: 6
    property real changeX: 258
    // Room right of the change readout.
    property real changeRight: 10
    property int changeValueSize: 20
    // The meter: inner padding of its well, its default size and its peak line.
    property real meterInset: 2
    property int meterDefaultWidth: 14
    property int meterDefaultLength: 200
    property real peakLineInset: 1
    property real peakLineHeight: 2
    // The meter's top (dBFS) and its zone boundaries.
    property real meterCeilDb: 6
    property real meterHotDb: -12
    property real meterOverDb: -3

    // The stereo image plate.
    property real stageWellY: 30
    property int stageWellHeight: 230
    property real stageWellRadius: 5
    property real stageNoteX: 10
    property real stageNoteY: 8
    property int stageNoteSize: typeMicro
    property real stageNoteSpacing: 1.2
    property real panRowY: 272
    property int panKnobDiameter: 50
    property real lawX: 112
    property real lawCaptionY: 276
    property real lawSegmentY: 292
    property int lawSegmentHeight: 28
    // Room right of the law column for the width knob.
    property real lawRight: 128
    property real lawNoteY: 328
    // The image drawing.
    property real speakerStroke: 1.5
    property real bandStroke: 14
    property real bandHaloStroke: 30
    property real bandHaloAlpha: 0.12

    // Row B: the stereo, monitor and repair plates.
    property int stereoPlateWidth: 420
    property int monitorPlateWidth: 400
    property real toggleRowY: 34
    property real secondToggleY: 86
    property int toggleHeight: 42
    property int stereoToggleWidth: 190
    property real swapToggleX: 216
    property real polarityCaptionY: 92
    property real polarityY: 106
    property int polarityWidth: 190
    property int polarityHeight: 42
    property int polarityGlyphSize: 20
    property real polarityTextX: 44
    property real polarityTextGap: 1
    property real polarityNameSpacing: 2
    property real polarityStateSpacing: 0.6
    property real stereoNoteY: 160
    property real soloCaptionY: 32
    property real soloSegmentY: 46
    property int soloSegmentHeight: 32
    property real soloNoteY: 92
    property real repairNoteY: 140

    // The control last touched, read back in the display.
    lastTouched: ""
}
