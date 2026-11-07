import QtQuick
import "../devicekit"

// Ember design tokens: a charcoal panel between walnut cheeks, cream legends
// and warm amber accents, like a well-kept 1980s poly.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "ember"
    accent: amber
    menuFill: raised
    menuEdge: line
    menuHighlightAlpha: 0.18
    menuHeaderInk: dim
    menuInk: cream
    knobDialPad: 10
    knobWidthPad: 16
    knobMinWidth: 54
    knobHeightPad: 30
    knobDisabledOpacity: 0.5
    knobInactiveOpacity: 0.42
    knobLongName: "none"
    knobTip: false
    knobCaptionInset: 3
    knobLabelUpper: false
    knobLabelFont: ""
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 0.4
    knobLabelInk: dim
    knobValueFont: ""
    knobValueSize: 9
    knobValueBold: true
    knobNextNoteMark: true
    knobNextNoteInk: dim
    knobNextNoteSize: 5
    knobNextNoteInset: 6
    dropdownWidth: 92
    dropdownHeight: 22
    dropdownRadius: 2
    dropdownEdge: line
    dropdownSpeaksGroup: false
    dropdownTextX: 7
    dropdownTextRight: 22
    dropdownFont: ""
    dropdownTextSize: 10
    dropdownChevronMargin: 6
    dropdownChevronInk: dim
    arrowHeight: 28
    arrowRadius: 3
    arrowInk: cream
    arrowGlyphSize: 18
    statusHeight: 22
    statusPad: 28
    statusLampX: 9
    statusLampSize: 7
    statusTextX: 20
    statusEdgeAlpha: 0.5
    statusPendingInk: dim
    viewRadius: 3
    viewEdge: engraving
    viewGrid: line
    viewMarker: cream
    viewLabelInk: dim

    property color canvas: "#1a1917"
    property color panel: "#24221f"
    property color raised: "#2f2c28"
    well: "#141311"
    property color line: "#3d3934"
    property color engraving: "#0f0e0c"
    property color text: "#efe6d2"
    property color dim: "#b3a891"
    property color faint: "#948a77"
    ink: text
    inkDim: dim
    inkFaint: faint
    inkGround: panel
    warning: "#f0b45a"
    live: "#9fd58a"

    property color amber: "#f29b38"
    property color ember: "#e8603a"
    property color gold: "#e6c271"
    property color cream: "#efe6d2"
    property color teal: "#7cc4b4"

    property color woodDark: "#2b1a10"
    property color woodLight: "#5a3620"
    property color woodGrain: "#3b2314"

    property color faderCap: "#e9e1cf"
    property color faderCapLine: "#2a2520"
    // Sliders as on a classic poly's front panel: scales beside the slot
    // (the left one marking each step of a short stepped range), the label
    // under it and the value in a bubble over the cap while touched.
    faderWidth: 34
    faderHeight: 132
    faderTip: false
    faderBottom: 30
    faderAreaBottom: 26
    faderTrackEdge: engraving
    faderLitWidth: 4
    faderLitRadius: radiusXs
    faderLitOpacity: 0.75
    faderGripInk: faderCapLine
    faderTicks: 10
    faderStepScale: true
    faderTickInk: faint
    faderFoot: "label"
    faderLabelInk: dim
    faderBubble: true
    faderBubbleFill: engraving
    faderBubbleInk: cream
    property real faderCapLighten: 1.05
    property real faderCapDarken: 1.22
    function faderCapFill(fader, stop) {
        return stop === 0 ? Qt.lighter(faderCap, faderCapLighten) : Qt.darker(faderCap, faderCapDarken)
    }
    function faderCapEdge(fader) {
        return fader.activeFocus ? fader.accent : faderCapLine
    }
    property color knobBody: "#2b2825"
    property color knobSkirt: "#171513"

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Scale dots around the skirt.
        ctx.fillStyle = knob.theme.faint
        for (let i = 0; i <= 10; ++i) {
            const a = start + sweep * i / 10
            ctx.beginPath()
            ctx.arc(c + Math.cos(a) * (radius + 3), c + Math.sin(a) * (radius + 3),
                    i % 5 === 0 ? 1.4 : 0.9, 0, Math.PI * 2)
            ctx.fill()
        }
        // Lit arc.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = knob.accent
            ctx.lineWidth = 2
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(c, c, radius - 1, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Skirt and cap.
        ctx.fillStyle = knob.theme.knobSkirt
        ctx.beginPath()
        ctx.arc(c, c, radius - 3, 0, Math.PI * 2)
        ctx.fill()
        const body = radius - 7
        const fill = ctx.createRadialGradient(c - body * 0.35, c - body * 0.4, 1, c, c, body)
        fill.addColorStop(0, Qt.lighter(knob.theme.knobBody, 1.9))
        fill.addColorStop(1, knob.theme.knobBody)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = 1
        ctx.strokeStyle = knob.activeFocus ? knob.accent : knob.theme.engraving
        ctx.stroke()
        // Cream pointer line.
        ctx.strokeStyle = knob.theme.cream
        ctx.lineWidth = 2.2
        ctx.lineCap = "round"
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * 0.25, c + Math.sin(at) * body * 0.25)
        ctx.lineTo(c + Math.cos(at) * (radius - 4), c + Math.sin(at) * (radius - 4))
        ctx.stroke()
    }

    // Dropdown and switch: engraved panel buttons.
    function dropdownInk(dropdown) {
        return cream
    }
    function switchFill(toggle, stop) {
        return stop === 0 ? (toggle.pressed ? well : Qt.lighter(raised, 1.15)) : raised
    }
    function switchEdge(toggle) {
        return toggle.activeFocus ? toggle.accent : engraving
    }
    function switchLedEdge(toggle) {
        return toggle.on ? Qt.lighter(toggle.accent, 1.3) : engraving
    }
    function switchInk(toggle) {
        return toggle.on ? cream : dim
    }

    // Panel buttons (the kit's SegmentedControl as EmberButtons): a row of
    // engraved latching keys with LEDs, ringed in the accent when focused.
    segmentedHeight: 26
    segmentedCellWidth: 38
    segmentedEqualCells: true
    segmentedSpacing: spaceXxs
    segmentedRadius: radiusSm
    segmentedCellRadius: radiusXs
    segmentedFrame: "transparent"
    segmentedFrameEdge: "transparent"
    segmentedGradient: true
    segmentedLeds: true
    // A key's face lightens toward its top.
    readonly property real keyFaceLift: 1.15
    function segmentedFill(cell, stop) {
        return stop === 0 ? (cell.pressed ? well : Qt.lighter(raised, keyFaceLift)) : raised
    }
    function segmentedEdge(cell) {
        return engraving
    }
    function segmentedLedEdge(cell) {
        return cell.on ? Qt.lighter(cell.accent, segmentedLedLift) : engraving
    }
    function segmentedInk(cell) {
        return cell.on ? cream : dim
    }
    function segmentedFont(cell) {
        return Qt.font({ pixelSize: typeCaption, bold: true })
    }

    // Header: raised preset arrows and a toned status pill.
    function arrowFill(arrow, stop) {
        return stop === 0 ? Qt.lighter(raised, 1.15) : raised
    }
    function arrowEdge(arrow) {
        return arrow.activeFocus ? amber : engraving
    }
    function statusFill(tone) {
        return Qt.rgba(tone.r, tone.g, tone.b, 0.12)
    }
    function statusInk(tone) {
        return tone
    }

    // ---- Panel layout ----------------------------------------------------

    property int panelWidth: 1240
    property real cheekWidth: 26
    property real panelMargin: 10
    property real headerHeight: 50
    property real row1Height: 296
    property real row2Height: 318

    // Walnut cheeks.
    property color woodMid: Qt.darker(woodLight, 1.15)
    property color woodEdge: Qt.rgba(0, 0, 0, 0.45)
    property int grainLines: 7
    property real grainInset: 3
    property real grainMargin: 4
    property real grainStep: 12
    property real grainAlpha: 0.35
    property real grainAlphaStep: 0.08
    // Grain lines cycle through this many alpha steps.
    property int grainAlphaCycle: 3

    // Header: flame, wordmark, preset display and status.
    property real flameWidth: 26
    property real flameHeight: 32
    property real wordmarkGap: 10
    property real wordmarkTop: 6
    property int wordmarkSize: 22
    property real wordmarkSpacing: 6
    property real taglineSpacing: 2
    property real presetBoxWidth: 310
    property real presetBoxHeight: 28
    property real presetCategorySpacing: 1.2
    // Room the category and EDITED legends leave beside the preset name.
    property real presetNameReserve: 150
    property real presetEditedRight: 22
    property real noteHintGap: 5
    property real noteHintMarkSize: 5
    property real noteHintMarkRotation: 45

    // Sections: an engraved title strip over the body.
    property real sectionStripHeight: 24
    property real sectionStripRadius: 5
    property real sectionUnderlineOpacity: 0.85
    property real sectionTitleX: 10
    property real sectionTitleSpacing: 2
    property real sectionPad: 8
    property real sectionFoot: 6
    // Buttons and switches in a title strip.
    property real stripControlHeight: 18
    property real captionSpacing: 1
    property real buttonRowHeight: 22
    // The control row under a section's top button row.
    property real belowButtonsY: 32
    // The control row under a small caption.
    property real belowCaptionY: 14
    property real displayWellHeight: 96

    // DCO.
    property real dcoWidth: 210
    property real dcoRangeWidth: 112
    property real dcoFaderWidth: 32
    property real tallFaderHeight: 222
    property real dcoPwmCaptionY: 238
    property real dcoPwmButtonsX: 30
    property real footRowY: 232

    // VCO B and Poly-Mod.
    property real vcoWidth: 168
    property real wideFaderWidth: 38
    property real vcoFaderHeight: 194
    property real polyModWidth: 142
    property real polyFaderHeight: 210
    property real polyTargetCaptionX: 84
    property real polyTargetX: 80
    property real polyTargetWidth: 46
    property real polyNoteY: 236

    // Filter.
    property real filterWidth: 330
    property real filterTypeWidth: 128
    property real filterScopeHeight: 70
    property real filterFaderY: 80
    property real filterFaderWidth: 36
    property real filterFaderHeight: 172
    property real filterDividerHeight: 150

    // Envelope.
    property real envWidth: 132
    property real envViewHeight: 48
    property real envFaderY: 58
    property real envFaderWidth: 29
    property real envFaderHeight: 196

    // LFO.
    property real lfoWidth: 212
    property real lfoViewY: 30
    property real lfoKnobY: 140
    property real lfoKnobPitch: 66
    property real lfoNoteY: 232
    property real lfoStroke: 1.6
    // A synced rate or delay time: a division dropdown with its caption.
    property real divisionDropdownY: 12
    property real divisionCaptionY: 49

    // Voice and performance wheels.
    property real voiceWidth: 262
    property real voiceModeWidth: 168
    property real voiceChordX: 174
    property real perfCaptionY: 124
    property real perfWheelsY: 142
    property real wheelSpacing: 22
    property real wheelHeight: 124
    property real voiceNoteX: 150
    property real voiceNoteY: 146
    property real wheelWidth: 24
    property real wheelImplicitHeight: 104
    property real wheelLabelSpace: 18
    property real wheelLabelOverhang: 8
    property int wheelRibs: 13
    property real wheelRibInset: 3
    property real wheelRibTop: 2
    property real wheelRibBottom: 4
    property real wheelRibHeight: 2
    property real wheelRibMinOpacity: 0.35
    property real wheelThumbInset: 2
    property real wheelThumbHeight: 8

    // Circuit.
    property real circuitWidth: 170
    property real humRowY: 84
    property real circuitViewY: 170
    property real circuitNoteY: 272
    property real cardIdleAlpha: 0.3
    property real cardBarWidth: 6
    property real cardTickWidth: 12
    property real cardTickHeight: 2
    // Room under the plot for the card numbers.
    property real cardNumberRow: 12
    property real cardNumberInset: 2
    property real cardPlotHeadroom: 2

    // Effects.
    property real effectsWidth: 404
    property real fxChorusX: 76
    property real fxChorusY: 18
    property real fxChorusWidth: 200
    property real fxChorusHeight: 24
    property real fxHissX: 290
    property real fxDividerY: 106
    property real fxDelayCaptionY: 122
    property real fxSwitchY: 118
    property real fxSyncX: 70
    property real fxPingPongX: 128
    property real fxKnobY: 146
    property real fxTapeDividerX: 196
    property real fxTapeDividerY: 120
    property real fxTapeDividerHeight: 120
    property real fxTapeX: 210
    property real fxNoteY: 256

    // Output and maker's badge.
    property real outKnobDiameter: 44
    property real lampSize: 10
    property color lampRim: Qt.lighter(ember, 1.4)
    property real badgeVoiceSpacing: 1.5
    property real badgeWidth: 62
    property real badgeHeight: 20
    property real badgeNameSpacing: 2.5
}
