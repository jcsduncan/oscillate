import QtQuick
import "../devicekit"

// Tune design tokens: a precision strobe tuner. A black case and black
// plates, warm amber light for everything that reads (the note, the needle,
// the strobe), a cooler green when in tune, and red when far out.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "tune"
    cardSheen: Qt.rgba(1, 0.9, 0.7, 0.05)

    // The case: true black, faintly warm at the top edge.
    caseTop: "#1b1814"
    caseColor: "#110f0d"
    caseBottom: "#0a0908"
    // Where the case's gradient reaches caseColor.
    property real caseMidStop: 0.35
    property real caseRadius: 10
    property int panelWidth: 1240
    property int panelHeight: 760
    // Plates and the recessed wells inside them.
    plate: "#15130f"
    plateEdge: "#2c2720"
    plateHi: "#433b30"
    well: "#070605"
    wellEdge: "#2a241c"
    seam: "#221e18"
    grid: "#231d15"
    gridStrong: "#3b3122"

    // The amber light, its glow, and the strobe band.
    property color amber: "#ffb340"
    property color amberHot: "#ffd27a"
    property color amberDim: "#8a5a1a"
    property color amberGlow: "#3a2508"
    property color strobeDark: "#140d04"

    // How far out: in tune, close, far.
    property color inTune: "#9fe870"
    property color near: "#ffb340"
    property color off: "#ff5f45"

    // Controls: amber arcs, a pale index.
    accent: "#ffb340"
    accentSoft: "#a8661c"
    steel: "#7a4c14"
    glacier: "#fbf1e2"
    property color glint: "#ffffff"

    ink: "#f5ece0"
    inkDim: "#a89a88"
    inkFaint: "#8b7d69"

    knobTop: "#3e3830"
    knobBottom: "#100e0b"
    track: "#2a251e"
    button: "#13110e"
    buttonHi: "#1d1a15"

    warning: "#f2b25c"
    live: "#9fe870"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // How far out a reading is: within inTuneCents it reads in tune (green),
    // within nearCents close (amber), beyond that far (red).
    property real inTuneCents: 3
    property real nearCents: 15

    // Type with a particular job.
    // Small spaced-caps captions and plate labels.
    property real captionSpacing: 1.4
    // The CENTS unit beside a large cents reading.
    property real unitSpacing: 1.6
    property real adviceSpacing: 0.8
    property real waitingSpacing: 3
    property real tagSpacing: 1.2
    // The big readings: note letters, cents and the reference.
    property int bigNoteSize: 160
    property int noteSize: 176
    property int settledNoteSize: 128
    property int centsReadoutSize: 40
    property int settledCentsSize: 34
    property int referenceReadoutSize: 30

    // Plates: content inset from the plate's edge.
    property real plateInset: 14
    property real plateGap: 10
    // The header (Kit.DeviceHeader): the wordmark with its amber underline
    // over the tagline (the panel's own brand), the preset browser and,
    // once a control is touched, its readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkSize: 27
    wordmarkSpacing: 9
    wordmarkInk: glacier
    // The underline: its height, its gap below the word and how much
    // shorter than the word it is; the tagline sits subtitleGap below the word.
    property real underlineHeight: 2
    property real underlineGap: 1
    property real underlineTrim: 10
    subtitleGap: 7
    subtitleSpacing: 2.4
    subtitleInk: inkDim
    headerBrowserX: 250
    presetDisplayWidth: 290
    presetDisplayHeight: 42
    presetDisplayRadius: radiusLg
    presetTextX: plateInset
    presetTagY: 5
    presetTagSpacing: captionSpacing
    presetNameY: 16
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: inkDim
    readoutWidth: 240
    readoutTextX: plateInset
    readoutNameY: 5
    readoutValueY: 17
    readoutSpacing: 1.2

    // Light: the soft glow behind lit marks and its tint.
    property real glowStroke: 7
    property real glowAlpha: 0.18
    property real noteGlowAlpha: 0.13
    property real noteGlowScale: 1.04
    // The cents needle.
    property real centsWindowStroke: 8
    property real centsWindowAlpha: 0.35
    property real needleStroke: 2.5
    // The hint bar: its edge in the tone, a lamp with a halo.
    property real hintEdgeAlpha: 0.45
    property int hintLampSize: 10
    property int hintHaloSize: 24
    property real hintHaloAlpha: 0.16
    // The gauges.
    property int gaugeBarHeight: 8
    property real gaugeValueLift: 3

    // The strobe: bar ends fade to strobeEdgeAlpha; rows dim with their
    // harmonic and the band dims with the signal's clarity.
    property real strobeEdgeAlpha: 0.55
    property real strobeBarRadius: 1
    property real strobeFloorOpacity: 0.35
    property real strobeRowFade: 0.18
    property real strobeIdleOpacity: 0.12
    property int strobeMarkWidth: 2
    property int strobeMarkHeight: 6
    // The strobe glass: a sheen at the top clearing to a shade at the foot.
    property color glassSheen: Qt.rgba(1, 1, 1, 0.05)
    property color glassClear: Qt.rgba(1, 1, 1, 0)
    property color glassShade: Qt.rgba(0, 0, 0, 0.25)
    property real glassSheenStop: 0.4

    // The history and trajectory screens.
    property real viewWellRadius: 5
    property real sharpLineAlpha: 0.5
    property int historyDotSize: 4
    property real historyFloorOpacity: 0.35
    property real nowInset: 10
    property real settledLineAlpha: 0.55
    property int tagHeight: 16
    property real tagLift: 3
    property real traceDotRadius: 1.8
}
