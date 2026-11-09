import QtQuick
import "../devicekit"

// Strata design tokens: a multiband dynamics processor. A cool graphite case
// and a deep screen; each band its own colour (teal, amber, coral, violet,
// sky, lime), the output spectrum in near-white
// over the input's faint trace, and the key in cyan.
KitTheme {
    prefix: "strata"

    caseTop: "#161b22"
    caseColor: "#10141a"
    caseBottom: "#0b0e13"
    plate: "#141a21"
    plateEdge: "#222b36"
    plateHi: "#33404f"
    well: "#0b0e13"
    wellEdge: "#222b36"
    seam: "#1b222b"

    property color screen: "#080a0e"
    grid: "#161d26"
    gridStrong: "#25303d"
    property color inputTrace: "#3b4a5c"
    property color outputTrace: "#e8eef6"
    property color unity: "#34414f"
    property color key: "#6be3ff"
    property var bandColours: ["#4fd1c5", "#ffb547", "#ff7a6b", "#b892ff", "#6cb8ff", "#b6e36b"]

    accent: "#ff9f5a"
    accentSoft: "#a3653a"
    steel: "#5c4632"
    glacier: "#f4f7fb"
    ink: "#e6edf5"
    inkDim: "#94a1b2"
    inkFaint: "#768597"

    knobTop: "#3a4350"
    knobBottom: "#10141a"
    track: "#1c232c"
    button: "#11161c"
    buttonHi: "#1a2028"
    ledOff: "#28303a"

    warning: "#f2b25c"
    live: "#7fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    lastTouched: ""

    // The panel: its size, the case's corners and where the case gradient
    // reaches its body colour.
    property int panelWidth: 1320
    property int panelHeight: 800
    property real caseRadius: 10
    property real caseMidStop: 0.35
    // The gap between cards.
    property real plateGap: 10
    // Small bold captions and tags.
    property real captionSpacing: 1.4
    property real tagSpacing: 1.2

    // A text button (Kick-aware low): padding round the label, its tint
    // when lit, and how faint it reads when the device is not editable.
    property real buttonPad: 20
    property real buttonHeight: 26
    property real buttonRadius: 5
    property real buttonLitAlpha: 0.2
    property real buttonDisabledOpacity: 0.5
    // The accent edge a focused control of the panel's own draws.
    property real focusEdgeWidth: strokeBold

    // The header (Kit.DeviceHeader): the wordmark over its band-coloured
    // underline and subtitle, the preset browser, the readout once a control
    // is touched; the mode row under it.
    property real headerY: 10
    headerHeight: 50
    wordmarkSize: 27
    wordmarkSpacing: 9
    wordmarkInk: glacier
    // The underline and subtitle sit this far in from the wordmark's left.
    property real wordmarkInset: 2
    property real underlineGap: 1
    // How much shorter than the wordmark the underline runs.
    property real underlineTrim: 20
    subtitleGap: 7
    subtitleSpacing: 2.4
    subtitleInk: inkDim
    headerBrowserX: 230
    presetDisplayWidth: 300
    presetDisplayHeight: 40
    presetDisplayRadius: radiusLg
    presetTextX: 12
    presetTagY: 5
    presetTagSpacing: captionSpacing
    presetNameY: 16
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: inkDim
    readoutValueY: 16

    // The mode row: the mode and phase segments.
    property int modeSegmentWidth: 130
    property int phaseSegmentWidth: 170

    // The keyed badge at the end of the mode row.
    property real keyedBadgePad: 24
    property real keyedFillAlpha: 0.12

    // The band strip: a tab per band above the spectrum.
    property real stripInset: 10
    // The strip's gap below the header.
    property real stripGap: 6
    property real stripHeight: 30
    property real tabY: 2
    property real tabPad: 16
    property real tabHeight: 26
    property real tabChosenAlpha: 0.18
    property real tabDotSize: 7
    property real tabDotRadius: 4
    property real tabLabelSpacing: 1
    // The solo, mute, bypass and remove flags.
    property real flagSize: minHitSize

    // The spectrum card.
    // The spectrum's gap below the strip.
    property real spectrumGap: 2
    property real spectrumHeight: 296
    property real spectrumInset: 10
    // The strip below the spectrum for the note and the band count.
    property real spectrumFoot: 34
    property real spectrumNoteX: 12
    property real spectrumNoteBottom: 20
    property real bandsRowBottom: 22
    property int bandsSegmentWidth: 150
    property int bandsSegmentHeight: 18

    // The spectrum's drawing: band tints, traces and gain fills.
    property real bandTintAlpha: 0.04
    property real bandTintSelectedAlpha: 0.09
    property real inputTraceWidth: 1.2
    property real outputTraceWidth: 1.6
    property real gainFillAlpha: 0.35
    // A band's dashed threshold line and the strip that grabs it; the line
    // stops clear of the crossovers' grab strips.
    property real thresholdLineInset: minHitSize / 2
    property real thresholdHitHeight: minHitSize
    property real thresholdDash: 6
    property real thresholdDashGap: 4
    property real thresholdDashOpacity: 0.8
    // A crossover handle: its grab strip, line, knob and label.
    property real crossoverHitWidth: minHitSize
    property real crossoverLineOpacity: 0.7
    property real crossoverKnobY: 6
    property real crossoverKnobSize: 12
    property real crossoverLabelBottom: 16

    // Cards along the bottom: content padding and where it starts.
    property real cardPad: 14
    property real cardContentY: 30
    property int transferCardWidth: 330
    property int bandCardWidth: 480
    property int ottCardWidth: 220
    // The band card's knob grid and its process and key rows.
    property real bandKnobsY: 32
    property real knobColumnSpacing: 22
    property int bandKnobDiameter: 46
    property real bandRowsGap: 10
    property real bandRowCaptionWidth: 56
    property int processSegmentWidth: 300
    property int keySegmentWidth: 240
    // The OTT card.
    property int ottDepthDiameter: 84
    property real ottRowY: 170
    property int smallKnobDiameter: 42
    // The global card.
    property real globalColumnSpacing: 26

    // The transfer view: unity line, curve, the live dot and the handles.
    property real unityDash: 4
    property real transferCurveWidth: 2.2
    property real levelDotRadius: 4.5
    property real handleSize: 14
    // How far past a handle's edge it can still be grabbed.
    property real handleHitSlop: 4

    function band(index) {
        return bandColours[index % bandColours.length]
    }
}
