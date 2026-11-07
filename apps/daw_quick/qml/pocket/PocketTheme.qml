import QtQuick
import "../devicekit"

// Pocket design tokens: a precision low-end instrument. A deep charcoal
// case, graphite plates and recessed wells; the kick in warm amber, the bass
// in cool cyan, space cleared in a clear green and masking in red. Controls
// are lettered in warm white and lit amber.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "pocket"

    // The case: deep charcoal, a touch warmer at the top edge.
    caseTop: "#1d1c1b"
    caseColor: "#141414"
    caseBottom: "#0d0d0e"
    // Graphite plates and the recessed wells inside them.
    plate: "#1a1a1b"
    plateEdge: "#2c2b2a"
    plateHi: "#3b3936"
    well: "#0c0c0d"
    wellEdge: "#292826"
    seam: "#232221"
    grid: "#1f1f20"
    gridStrong: "#302f2d"

    // The two signals and the result.
    property color kick: "#ffb14e"
    property color kickSoft: "#c7782a"
    property color bass: "#4fd8e8"
    property color bassSoft: "#2a8fa0"
    property color cleared: "#5fe39a"
    property color masked: "#ff5a52"

    // Controls: amber arcs, a warm-white index.
    accent: "#ffb14e"
    accentSoft: "#b97a35"
    steel: "#8a5a26"
    glacier: "#f6efe4"
    property color glint: "#ffffff"

    ink: "#efe9e0"
    inkDim: "#a39a8e"
    inkFaint: "#8a8379"

    knobTop: "#3a3734"
    knobBottom: "#121212"
    track: "#262524"
    button: "#151515"
    buttonHi: "#1e1d1c"

    warning: "#f2b25c"
    live: "#5fe39a"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // ---- Panel layout ----------------------------------------------------------------

    // The case: its size, corners and where the gradient reaches caseColor.
    property int panelWidth: 1340
    property int panelHeight: 740
    property real caseRadius: 10
    property real caseMidStop: 0.35
    // Space between plates.
    property real plateGap: 10
    // A focused well's edge.
    property real focusStroke: 1.5
    // Dimmed controls whose setting does not apply.
    property real unusedOpacity: 0.45
    // Small spaced-caps captions and stat names.
    property real captionSpacing: 1.4
    property real statSpacing: 1.3
    property real keySpacing: 1.2

    // Header: the wordmark, its kick-to-bass underline and the subtitle.
    property real headerY: 10
    property real headerHeight: 54
    property real wordmarkY: 2
    property int wordmarkSize: 27
    property real wordmarkSpacing: 10
    // Underline and subtitle sit this far in from the wordmark's left edge.
    property real wordmarkInset: 2
    property real underlineGap: 1
    // How much shorter than the wordmark the underline runs.
    property real underlineTrim: 10
    property real underlineHeight: 2
    property real underlineRadius: 1
    property real subtitleGap: 7
    property real subtitleSpacing: 2.4

    // The preset browser and its display well.
    property real browserX: 230
    property real browserY: 5
    property real presetDisplayWidth: 560
    property real presetDisplayHeight: 42
    property real presetTextX: 14
    property real presetCaptionY: 5
    property real presetNameY: 16
    property real presetNameWidth: 250
    property real presetNameSpacing: 0.6
    property real presetDividerX: 276
    // The divider's gap from the well's top and bottom.
    property real presetDividerInset: 8
    property real readoutX: 288
    property real readoutValueY: 17
    // Room kept right of the readout for the chevron.
    property real readoutRight: 26
    property real chevronMargin: 10
    property real statusY: 14

    // Rows: the map and meters, the waveforms, the controls.
    property real rowAY: 76
    property real rowAHeight: 330
    property real rowBHeight: 132
    // The space-cleared plate beside the map.
    property real metersWidth: 330
    // Plots sit this far inside their plates, below the plate title.
    property real plotInset: 4
    property real mapTop: 26
    property real wavesTop: 24

    // The map's key, top right.
    property real legendMargin: 14
    property real legendY: 10
    property real legendSpacing: 14
    property real legendSwatchGap: 5
    property real legendSwatch: 9
    property real legendHeldWidth: 12
    property real legendLine: 2
    property real legendSwatchRadius: 1
    property real maskedKeyAlpha: 0.6

    // Space cleared: the headline, the overlap bars and the stat lines.
    property real clearedY: 34
    property int clearedSize: 54
    property real clearedLabelY: 100
    property real statInset: 18
    property real barsY: 126
    property real barGap: 10
    property real barRowHeight: 28
    property real barTrackY: 14
    property real barTrackHeight: 10
    property real barTrackRadius: 5
    property real barFillInset: 1
    property real overlapFillAlpha: 0.8
    property real statDividerY: 206
    property real statLinesY: 216
    property real statLineHeight: 15
    property real modeNoteY: 280

    // Control plates.
    property real controlY: 30
    property real knobRowX: 8
    property real knobRowY: 32
    property real modeWidth: 210
    property real duckWidth: 180
    property real duckKnobDiameter: 44
    property real duckNoteX: 96
    property real duckNoteY: 44
    property real duckNoteWidth: 74
    property real focusWidth: 180
    property real focusKnobDiameter: 40
    property real timingWidth: 360
    property real timingKnobX: 6
    property real timingKnobDiameter: 38
    property real timingControlsX: 170
    property real lookaheadCaptionY: 74
    property real lookaheadY: 88
    property real lookaheadHeight: 22
    property real subWidth: 156
    property real subKnobY: 72
    property real subKnobDiameter: 30
    property real outputKnobDiameter: 40
    // A knob cell: its width, default dial, and the room for its captions.
    property real cellWidth: 80
    property real cellDiameter: 38
    property real cellReadoutPad: 50
    property real cellLabelPad: 36

    // ---- Plots ------------------------------------------------------------------------

    // Gutters for the axis labels, shared so the map and waves line up.
    property real plotLeft: 40
    property real plotRight: 14
    // Axis labels' gap from the plot edge.
    property real axisLabelGap: 6
    property real timeLabelGap: 4

    // The masking map.
    property int mapWidth: 880
    property int mapHeight: 290
    property real mapPlotTop: 30
    property real mapPlotBottom: 24
    property real maskTopAlpha: 0.42
    property real maskBottomAlpha: 0.06
    property real kickGlowTopAlpha: 0.30
    property real kickGlowBottomAlpha: 0.02
    property real cutTopAlpha: 0.95
    property real cutBottomAlpha: 0.35
    // The bass after Pocket: a dashed line.
    property var afterDash: [4, 3]
    property real afterStroke: 1.2
    property real afterAlpha: 0.75
    property real curveStroke: 2.2
    property real heldAlpha: 0.55
    // The mono sub: hatching, a dashed edge and its label.
    property real hatchAlpha: 0.10
    property real hatchPitch: 8
    property real monoEdgeAlpha: 0.45
    property var monoDash: [2, 3]
    property real monoLabelX: 6
    property real monoLabelRise: 16
    property real monoLabelAlpha: 0.75
    // Outside the focus.
    property color focusDim: Qt.rgba(0, 0, 0, 0.46)
    // The hovered band's readout pill.
    property real bandReadoutEdge: 4
    property real bandReadoutY: 6
    property real bandReadoutPad: 16
    property real bandReadoutHeight: 20
    property real bandReadoutRadius: 10
    property real bandReadoutAlpha: 0.92
    // The focus handles: the hit strip, the line and the grip above the plot.
    property real handleWidth: 64
    property real handleLineAlpha: 0.6
    property real gripRise: 22
    property real gripPad: 14
    property real gripHeight: 17
    property real handleGrabWidth: 16

    // The waveforms.
    property int wavesWidth: 1200
    property int wavesHeight: 110
    property real wavesPlotTop: 6
    property real wavesPlotBottom: 18
    // The gap between the kick and bass lanes.
    property real laneGap: 3
    // Room between an envelope's peak and its lane edge.
    property real envelopeHeadroom: 2
    property real kickFillAlpha: 0.45
    property real bassOutAlpha: 0.58
    property real cutFillAlpha: 0.55
    property real bassInStroke: 1.2
    // The lane keys.
    property real waveKeyX: 4
    property real waveKeyGap: 3
    property real waveKeySwatch: 7
    property real waveKeyRadius: 1.5
    property int waveKeySize: typeMicro
    property real waveKeySpacing: 0.8

    // The control last touched, read back in the display.
    lastTouched: ""
}
