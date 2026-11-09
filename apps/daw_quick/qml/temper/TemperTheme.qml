import QtQuick
import "../devicekit"

// Temper design tokens: a mixing console's resonance screen. A blue-black
// case and a deep screen; what Temper removes glows copper to amber, where
// it listens is ice blue, the sidechain it makes room for is violet, and
// each dynamic band keeps its own colour on its node, curve and chip.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "temper"
    segmentWidth: 260
    segmentHeight: 26
    segmentSpacing: 3
    segmentRadius: 4
    segmentLetterSpacing: 0.8
    switchRadius: -1
    switchHeight: 18

    // The case.
    caseTop: "#161b22"
    caseColor: "#10141a"
    caseBottom: "#0b0e13"
    plate: "#151a21"
    plateEdge: "#262f3a"
    plateHi: "#33404f"
    well: "#0b0e13"
    wellEdge: "#222b36"
    seam: "#1e2630"

    // The screen.
    property color screen: "#080a0e"
    property color screenGlow: "#0d131b"
    grid: "#141b24"
    gridStrong: "#222c38"
    property color zero: "#3a4757"

    // What is removed: copper where it is gentle, amber where it is deep.
    accent: "#ff8a3d"
    accentSoft: "#8c3412"
    steel: "#5a4030"
    property color cut: "#ff8a3d"
    property color cutHot: "#ffd27a"
    // Where it listens.
    property color listen: "#9ad7ff"
    // The key it makes room for.
    property color side: "#b892ff"
    // The live spectra: in as a soft fill, out as a line.
    property color spectrumIn: "#3a4757"
    property color spectrumOut: "#e6ebf1"
    // The heatmap's ramp: no cut, then copper, then amber-white.
    property color heatCold: "#0b0e13"
    property color heatWarm: "#8c3412"
    property color heatHot: "#ffd68c"
    property var bands: ["#4fd1c5", "#ef6fa8", "#a3e05f", "#6fa8ff",
                         "#f3d35a", "#ff9f6b", "#c7a0ff", "#7fe0e0"]

    glacier: "#eef3f8"
    ink: "#e6ebf1"
    inkDim: "#8b97a6"
    inkFaint: "#778595"

    knobTop: "#3d4552"
    knobBottom: "#0f1217"
    track: "#222b36"
    button: "#12161c"
    buttonHi: "#1a2028"
    ledOff: "#2a313b"

    warning: "#f2b25c"
    live: "#4fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel geometry ----------------------------------------------------------
    property int panelWidth: 1360
    property int panelHeight: 800
    property real caseRadius: 10
    // Where caseColor sits in the case gradient.
    property real caseMidStop: 0.35
    // Between the display, the inspector and the cards.
    property real cardGap: 10
    // Tracking for small bold capitals (captions, button labels).
    property real capsSpacing: 1.4
    // Tracking for the readout's parameter name and the display's hint.
    property real capsTightSpacing: 1.2

    // The header (Kit.DeviceHeader): the ember lamp and wordmark over the
    // subtitle, the preset browser, the readout and the status badge, with
    // Soft/Hard, the phase, its latency and Delta in the second row.
    property real headerY: 10
    headerHeight: 46
    headerBrowserX: 196
    property real emberY: 12
    property real emberSize: 10
    wordmarkInk: glacier
    wordmarkSize: 25
    wordmarkSpacing: 8
    subtitleGap: 3
    subtitleInk: inkDim
    subtitleSpacing: 2.4
    presetDisplayWidth: 210
    presetDisplayRadius: radiusLg
    presetTagY: 5
    presetTagSpacing: capsSpacing
    presetNameY: 16
    presetNameSize: typeSubtitle
    presetNameBold: false
    presetChevronMargin: 9
    presetChevronInk: inkDim
    readoutWidth: 200
    readoutRadius: radiusLg
    readoutNameY: 5
    readoutSpacing: capsTightSpacing
    readoutValueY: 17
    readoutValueSize: typeEmphasis
    property real modeWidth: 120
    property real phaseWidth: 270
    property real chipWidth: 76
    property real chipHeight: 26
    property real buttonRadius: 5

    // The display card and the screen inside it.
    property real displayHeight: 380
    property real cardInset: 10
    property real cardBodyY: 28
    property real plotLeft: 40
    property real plotTop: 26
    property real plotRight: 16
    property real plotBottom: 28
    property real heatHeight: 92
    // The plot's frame: its scales as the screen drew them.
    plotTickInk: inkFaint
    plotTickSize: typeCaption
    plotChipFill: screen
    // Where the screen's glow fades into the screen colour.
    property real screenGlowEnd: 0.7
    property real displayCaptionSpacing: 1.6
    property real spectrumInAlpha: 0.55
    property real spectrumOutAlpha: 0.8
    property real spectrumOutStroke: 1.2
    property var sideDash: [4, 4]
    property real sideStroke: 1.3
    // The cut's fill: faint at 0 dB, hot at the floor.
    property real cutFillTopAlpha: 0.15
    property real cutFillDeepAlpha: 0.8
    property real cutStroke: 1.4
    property var bandRangeDash: [3, 3]
    property real bandRangeAlpha: 0.4
    property real bandRangeChosenAlpha: 0.7
    property real bandGainAlpha: 0.2
    property real bandGainChosenAlpha: 0.3
    property real listenGlowAlpha: 0.15
    property real listenGlowStroke: 7
    // Sensitivity node diamonds and band handles.
    property real nodeHitSize: minHitSize
    property real nodeDiamond: 11
    property real nodeDiamondPressed: 13
    property real nodeStroke: 1.6
    property real handleSize: 17
    property real handleChosenSize: 20
    property real handleStroke: 1.5
    property real offBandOpacity: 0.45

    // The band inspector.
    property real inspectorHeight: 112
    property real inspectorX: 14
    property real inspectorRowY: 40
    property real inspectorCaptionY: 28
    property real inspectorGroupGap: 18
    property real inspectorKnobY: 24
    property real inspectorNowY: 30
    property real bandChipSize: 26
    property real kindWidth: 276
    property int bandKnobDiameter: 30
    // The band's On switch, centred on the card title's line.
    property real inspectorSwitchY: 7
    // From the inspector's right edge.
    property real removeRight: 104
    property real removeWidth: 88
    property real removeHeight: 28

    // The cards.
    property real cardKnobsY: 34
    property real cardLowerY: 66
    // A card's note sits this far above its bottom.
    property real cardNoteBottom: 30
    // A note beside a knob sits this far below the knob's top.
    property real cardNoteDrop: 8
    property real suppressWidth: 310
    property real timeWidth: 170
    property real splitWidth: 236
    property real stereoWidth: 210
    property real unmaskWidth: 200
    property real stereoSegmentY: 32
    property real toggleY: 30
    property real toggleWidth: 92
    property real toggleHeight: 28
    property real keyMeterX: 112
    property real keyMeterY: 41
    property real keyMeterHeight: 6
}
