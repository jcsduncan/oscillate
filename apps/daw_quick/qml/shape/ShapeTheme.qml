import QtQuick
import "../devicekit"

// Shape design tokens: a mastering console's EQ section. A cool graphite
// case, slate plates and a deep blue-black screen with a fine graticule;
// the summed curve in ice cyan, and each band in its own colour, the same
// on its handle, its curve and its tile.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "shape"
    segmentWidth: 300
    segmentHeight: 26
    segmentSpacing: 3
    segmentRadius: 4
    segmentLetterSpacing: 0.8
    switchRadius: -1
    switchHeight: 18

    // The case: graphite with a blue cast, a touch lighter at the top edge.
    caseTop: "#1c2127"
    caseColor: "#14181d"
    caseBottom: "#0d1014"
    plate: "#181c22"
    plateEdge: "#29303a"
    plateHi: "#39424f"
    well: "#0a0d11"
    wellEdge: "#232a34"
    seam: "#212730"

    // The screen.
    property color screen: "#070a0e"
    property color screenGlow: "#0b1720"
    // The screen's darkest corner, where its glow fades out.
    property color screenDeep: "#040608"
    grid: "#121b24"
    gridStrong: "#1f2f3c"
    property color zero: "#2c5266"

    // The summed curve and the eight bands, low to high.
    accent: "#7ee0ff"
    accentSoft: "#2f7f99"
    steel: "#2a6a80"
    property color curve: "#bff1ff"
    // The live spectra behind the curves: the signal in, a soft fill; out, a
    // line.
    property color spectrumIn: "#3a6f8a"
    property color spectrumOut: "#8fd4ef"
    property var bands: ["#ff6b8b", "#ff9d5c", "#ffd36b", "#9be36b",
                         "#5fe0bf", "#6bc8ff", "#9b8cff", "#e48cff"]

    glacier: "#eef6fb"
    ink: "#e8eef4"
    inkDim: "#94a1b0"
    inkFaint: "#788698"

    knobTop: "#434b57"
    knobBottom: "#101318"
    track: "#232a34"
    button: "#13171c"
    buttonHi: "#1b2027"
    ledOff: "#2a3039"

    warning: "#f2b25c"
    live: "#4fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel geometry ----------------------------------------------------------
    property int panelWidth: 1240
    property int panelHeight: 720
    property real caseRadius: 10
    // Where caseColor sits in the case gradient.
    property real caseMidStop: 0.35
    // Between the cards.
    property real cardGap: 10
    // Tracking for small bold capitals (captions, the preset category).
    property real capsSpacing: 1.4
    // Tracking for the readout's parameter name and the screen's hint.
    property real capsTightSpacing: 1.2

    // The header (Kit.DeviceHeader): the wordmark over its band-colour rule
    // and subtitle, the preset browser and, once a control is touched, its
    // readout.
    property real headerY: 10
    headerHeight: 54
    wordmarkSize: 27
    wordmarkSpacing: 9
    wordmarkInk: glacier
    // The band-colour rule under the wordmark: its gap below the wordmark and
    // how much shorter than the wordmark it runs.
    property real bandRuleGap: 1
    property real bandRuleTrim: 10
    subtitleGap: 7
    subtitleSpacing: 2.4
    subtitleInk: inkDim
    headerBrowserX: 250
    presetDisplayWidth: 300
    presetDisplayHeight: 42
    presetDisplayRadius: radiusLg
    presetTextX: 14
    presetTagY: 5
    presetTagSpacing: capsSpacing
    presetNameY: 16
    presetNameBold: false
    presetChevronMargin: 10
    presetChevronInk: inkDim
    readoutWidth: 240
    readoutSpacing: capsTightSpacing
    readoutValueY: 17

    // The cards: the top row (response, levels) and the bottom row (bands,
    // the selected band).
    property real rowAY: 76
    property real rowAHeight: 400
    property real cardInset: 10
    property real cardBodyY: 30
    // Where text and rows start inside a card, and their top below its title.
    property real cardTextX: 14
    property real cardRowY: 32

    // The levels card and its IN and OUT meter pairs.
    property real meterCardWidth: 210
    property real meterHeight: 290
    property real meterPairWidth: 76
    // A pair's caption and peak readouts above its meters.
    property real meterPairChrome: 54
    property real meterPairY: 34
    property real meterPeakY: 14
    property real meterPeakWidth: 34
    property real meterBarY: 32
    property real meterBarWidth: 18
    // The scale note sits this far above the card's bottom.
    property real meterNoteBottom: 26

    // The band tiles.
    property real bandsCardWidth: 600
    property real tileRadius: 5
    property real tileChosenAlpha: 0.12
    property real tileChosenEdgeAlpha: 0.8
    property real tileChosenStroke: 1.3
    property real tileLedHeight: 3
    property real tileNumberY: 14
    property int tileNumberSize: 22
    property real tileKindY: 46
    property real tileFreqY: 62
    property real tileGainY: 78

    // The selected band's card; a band that is off shows an OFF chip.
    property real offChipPad: 5
    property real offChipHeight: 16
    property real offChipRadius: 3
    property real kindSegmentY: 44
    property real bandKnobsY: 82
    property real bandKnobGap: 22
    property int bandKnobDiameter: 54
    // A band knob's laid-out width.
    property real bandKnobWidth: 80
    // The notes column's room from the card's right edge.
    property real bandNotesRight: 30

    // The response screen.
    property real plotLeft: 40
    property real plotTop: 28
    property real plotRight: 16
    property real plotBottom: 24
    // Where the screen's glow fades into the screen colour.
    property real screenGlowEnd: 0.6
    property real captionX: 12
    property real captionY: 9
    property real captionSpacing: 1.6
    // The hint's inset from the screen's right edge.
    property real hintRight: 16
    // The response screen's scales (Kit.PlotFrame).
    plotTickGap: 5
    plotTickSize: typeCaption
    property real spectrumInAlpha: 0.35
    property real spectrumOutAlpha: 0.7
    property real spectrumOutStroke: 1.2
    // Each band's wash and line, faint unless it is the selected band.
    property real bandFillAlpha: 0.09
    property real bandFillChosenAlpha: 0.20
    property real bandLineAlpha: 0.5
    property real bandLineChosenAlpha: 0.9
    property real bandLineChosenStroke: 1.4
    // The summed curve: a wide glow, a tighter glow, then the line.
    property real curveGlowWideAlpha: 0.10
    property real curveGlowWideStroke: 8
    property real curveGlowTightAlpha: 0.24
    property real curveGlowTightStroke: 3.5
    property real curveStroke: 1.8
    // The band handles.
    property real handleSize: 17
    property real handleChosenSize: 20
    property real handleAlpha: 0.2
    property real handleChosenAlpha: 0.35
    property real handleStroke: 1.5
    // The handle's grab area reaches this far past its edge.
    property real handleGrabOutset: 4
}
