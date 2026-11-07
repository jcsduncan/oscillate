import QtQuick
import "../devicekit"

// Relay design tokens: a vintage tape-echo machine in cream enamel with
// olive panels and brass hardware. The accent follows the character:
// signal red for Digital, tape-era cyan for Tape, amber for Analog.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "relay"
    tipRadius: 3
    tipFill: screen
    tipEdge: brassLo
    tipInk: screenGlow
    menuMinWidth: 170
    menuFill: bodyTop
    menuEdge: seam
    menuHighlightAlpha: 0.25
    menuBoldHighlight: true
    knobWidthPad: 22
    knobMinWidth: 60
    knobHeightPad: 32
    knobInactiveOpacity: 0.4
    knobLongName: "group"
    knobCaptionGap: 0
    knobCaptionInset: 8
    knobLabelFont: ""
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 1.0
    knobLabelInk: label
    knobValueInk: brassHi
    segmentWidth: 150
    segmentHeight: 24
    segmentSpacing: 2
    segmentFocusMargin: 3
    segmentFocusRadius: 4
    segmentRadius: 2
    segmentRadiusVertical: 2
    segmentFillAcross: false
    segmentRule: "none"
    segmentLed: true
    segmentLedX: 5
    segmentFocusFill: Qt.rgba(0, 0, 0, 0.22)
    segmentLedSize: 6
    segmentLedOff: "#8f8568"
    segmentLedRim: true
    segmentPressDepth: 1
    segmentUpper: true
    segmentTextOffset: 4
    segmentLetterSpacing: 0.8
    dropdownEdge: brassLo
    dropdownTextSize: 12
    dropdownChevronInk: brass

    // Enamel body, top to bottom.
    property color bodyTop: "#f3ead3"
    property color body: "#ece1c4"
    property color bodyBottom: "#ddd0ae"
    seam: "#b9a77c"
    // Olive control plates.
    property color plateTop: "#5f6a3d"
    plate: "#525c33"
    plateEdge: "#3b4224"
    // Screen glass (the echo train).
    property color screen: "#1d2015"
    property color screenEdge: "#0e100a"
    property color screenGrid: "#2d3220"
    property color screenGlow: "#f1e2b0"
    // Ink on enamel and on olive.
    ink: "#2b2a21"
    inkDim: "#514c3c"
    inkFaint: "#6a634d"
    inkGround: body
    property color label: "#efe6cc"
    property color labelDim: "#d6d0b8"
    // Brass.
    property color brassHi: "#f0d58b"
    property color brass: "#c49a45"
    property color brassLo: "#7e5f22"
    // Bakelite knob caps.
    property color capTop: "#3a332a"
    property color capBottom: "#15120e"
    pointer: "#f5ecd2"

    readonly property var characterColors: ["#e0533a", "#53c3d0", "#f2b33d"]
    readonly property var characterNames: ["DIGITAL", "TAPE", "ANALOG"]
    readonly property var characterTaglines: ["32-BIT CLEAN · HEAD CROSSFADE",
                                              "WOW · FLUTTER · SATURATION",
                                              "BUCKET BRIGADE · CLOCK DARKENING"]
    accent: characterColors[0]
    // Left and right lanes on the screen.
    property color laneLeft: "#f1d48a"
    property color laneRight: "#9fd0c1"

    warning: "#c76a12"
    live: "#5c8f3a"
    property color vuRed: "#b8361f"

    mono: "DejaVu Sans Mono"
    serif: "DejaVu Serif"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel: its size, the enamel body's corners, edge and where its
    // gradient reaches the body colour.
    property int panelWidth: 1240
    property int panelHeight: 690
    property real bodyRadius: 12
    property real bodyMidStop: 0.55
    property real plateGap: 10
    // Small bold captions, tags and notes.
    property real captionSpacing: 1.4
    property real tagSpacing: 1.2
    property real noteSpacing: 0.8
    // Small print on the machine's badges and chips.
    property int finePrintSize: typeMicro
    property real finePrintSpacing: 1
    // Ink printed on brass, and the shadow edge round brass parts.
    property color brassInk: "#2a2318"
    property color brassShadowEdge: Qt.rgba(0, 0, 0, 0.4)

    // Olive plates: corners, edge, brass screws and the stencilled title.
    property real plateRadius: 7
    property real plateStroke: 1.5
    property real platePad: 18
    property real plateBodyY: 40
    property real plateNoteBottom: 28
    property real screwInset: 6
    property real screwSize: 6
    property real screwSlotLength: 4
    // Each screw's slot turns a little further than the last.
    property real screwSlotAngle: 35
    property real screwSlotTurn: 20
    property real plateTitleX: 18
    property real plateTitleY: 10
    property real plateTitleSpacing: 2.6
    property real plateNoteGap: 10
    property real plateNoteY: 11
    // A knob cell: its footprint around the dial.
    property real cellWidthPad: 26
    property real cellMinWidth: 66
    property real cellHeightPad: 34
    property real cellRowSpacing: 18

    // The header: wordmark, subtitle and model line.
    property real wordmarkInset: 4
    property real wordmarkY: 8
    property int wordmarkSize: 34
    property real subtitleGap: 12
    property real subtitleY: 18
    property real subtitleSpacing: 3
    property real modelLineY: 33
    property real modelLineSpacing: 2
    // The character switch.
    property real headerControlY: 14
    property real characterX: 430
    property real characterWidth: 330
    property real characterHeight: 36
    property real characterCaptionSpacing: 1.8
    property real characterSegmentGap: 12
    property real characterSegmentHeight: 30
    // The preset browser.
    property real presetArrowWidth: 26
    property real presetHeight: 32
    property real presetDisplayWidth: 250
    property real presetDisplayStroke: 1.5
    property real presetTextX: 10
    property real presetNameY: 3
    // Room kept right of the preset name for the chevron.
    property real presetNameRight: 30
    property real presetCategoryY: 18
    property color presetCategoryInk: "#8d8a6c"
    property real chevronMargin: 8
    // The brass trim under the header.
    property real trimY: 58
    property real trimHeight: 3
    property real trimMidStop: 0.5

    // The hero row: character bay, echo train, VU meter.
    property real heroY: 72
    property real heroHeight: 250
    property real bayWidth: 218
    property real vuWidth: 196

    // The control plates.
    property real plateRowY: 336
    property real plateRowHeight: 290
    property real timePlateWidth: 318
    property real feedbackPlateWidth: 400
    property real wowPlateWidth: 170
    property int heroDiameter: 70
    property int smallDialDiameter: 48
    property real timeHeroWidth: 136
    property real timeHeroHeight: 124
    property real divisionWidth: 120
    property real divisionHeight: 44
    property int divisionFontSize: 20
    property real syncX: 170
    property real syncY: 44
    property real syncWidth: 128
    property real offsetX: 196
    property real offsetY: 102
    property real routingY: 190
    property real routingHeight: 28
    property real feedbackGridX: 140
    property real feedbackColumnSpacing: 10
    property real outputGridX: 22
    property real outputColumnSpacing: 26

    // The brass nameplate along the bottom.
    property real nameplateBottom: 12
    property real nameplatePad: 14
    property real nameplateValueX: 300
    property real nameplateSpacing: 1.6
    property color nameplateInk: "#3a2c10"
    property color nameplateValueInk: "#2a1f08"

    // The character bay.
    property color bayTop: "#2d2a21"
    property color bayBottom: "#1b1914"
    property color bayCaptionInk: "#a39d82"
    property real bayTextX: 12
    property real bayNameY: 9
    property real bayNameSpacing: 2.4
    property real bayTaglineY: 24
    // Where the character's picture starts below the bay's title.
    property real bayStageTop: 40
    property real bayFootBottom: 12
    property real bayFootSpacing: 0.6
    // Tape: the reels, the tape path and the heads.
    property real reelSize: 74
    property real reelInset: 18
    property real reelY: 14
    property real reelRimInset: 2
    property real reelHubRadius: 14
    property real reelRingStep: 3
    property real reelFlangeInset: 6
    property real reelHubCapRadius: 9
    property real reelSpindleRadius: 3.5
    property color reelTape: "#3b2a1c"
    property color reelTapeRing: "#4e3826"
    property color reelFlange: Qt.rgba(0.85, 0.8, 0.66, 0.92)
    property color reelSpindle: "#2a2318"
    property color tapePath: "#5a3f28"
    // The tape's run past the heads: half its width, and its height above
    // the bay's foot (where the heads sit).
    property real tapeHeadSpan: 26
    property real headRowBottom: 30
    property real headWidth: 12
    property real headHeight: 14
    // Analog: the bucket-brigade chip, its pins and the clock lamp.
    property real chipOffset: -6
    property real chipWidth: 112
    property real chipHeight: 54
    property real chipPinX: 10
    property real chipPinPitch: 13
    property real chipPinWidth: 5
    property real chipPinLength: 7
    property real chipDotInset: 6
    property real chipDotSize: 7
    property real chipLabelOffset: -6
    property real chipSubOffset: 10
    property color chipBody: "#141311"
    property color chipEdge: "#3a3730"
    property color chipPin: "#b9b4a6"
    property color chipDot: "#2a2824"
    property color chipLabelInk: "#d8d2c0"
    property color chipSubInk: "#8f8a7c"
    property real clockRowBottom: 14
    property real clockLampSize: 7
    property real clockLampDim: 0.25
    property color clockInk: "#c9c2a6"
    // Digital: the DSP readout.
    property real readoutOffset: -4
    // How much narrower than the bay the readout is.
    property real readoutInset: 34
    property real readoutHeight: 62
    property real readoutValueOffset: -6
    property int readoutValueSize: 24
    property real readoutCaptionBottom: 5
    property color readoutBody: "#0c0d09"
    property color readoutEdge: "#33352a"
    property color readoutCaptionInk: "#6c6a58"

    // The echo train screen.
    property color glassTop: "#262a1b"
    property color glassBottom: "#15170f"
    property real echoPlotLeft: 34
    property real echoPlotRight: 14
    property real echoPlotTop: 30
    property real echoPlotBottom: 26
    property real echoOriginStroke: 1.5
    // Grid lines reach this far above the plot; labels sit this far up
    // from the bottom.
    property real gridOverhang: 6
    property real gridLabelBottom: 10
    property color gridLabelInk: "#6f7357"
    // Each lane's floor sits above its bottom edge, with headroom above
    // the tallest bar.
    property real laneFloorInset: 6
    property real laneHeadroom: 14
    property color dbGuideZero: "#343a26"
    property color dbGuide: "#2a2e1e"
    property real dbGuideDash: 2
    property real dbGuideGap: 4
    property color laneBaseline: "#3a4029"
    property real laneLabelX: 16
    property real laneLabelDrop: 4
    // Repeats lose their highs by blending toward a dark umber, never
    // further than highLossFloor of the lane colour. The umber is kept as
    // exact red, green and blue channels for the blend.
    readonly property var highLossUmber: [0.42, 0.22, 0.08]
    property real highLossFloor: 0.35
    property real dryWidth: 3
    property real bounceAlpha: 0.22
    property real bounceStroke: 1.2
    property real bounceLift: 18
    property color decayLine: Qt.rgba(1, 1, 1, 0.16)
    property real decayDash: 3
    // Repeats drawn this far past the plot's right edge still show.
    property real echoClipSlack: 4
    // A repeat: its glow, diffusion smear, bar, cap and glint.
    property real glowRise: 8
    property real glowWidth: 12
    property real glowAlpha: 0.18
    property real smearPad: 3
    property real smearTopAlpha: 0.45
    property real smearBaseAlpha: 0.12
    property real barWidth: 6
    property real barBaseAlpha: 0.45
    property real barCapRadius: 4
    property color barGlint: Qt.rgba(1, 1, 1, 0.55)
    property real glintOffset: 1
    property real glintRadius: 1.5
    // Readouts on the glass and the live lamp.
    property real echoReadoutX: 40
    property real echoReadoutY: 8
    property real echoReadoutSpacing: 18
    property real echoReadoutRight: 14
    property color echoCountInk: "#9b9a7a"
    property real liveLampY: 24
    property real liveLampSize: 6

    // The VU meter: the cream face, scale, needle and the duck lamps.
    property real vuInset: 6
    // The face's share of the meter's height; the duck strip has the rest.
    property real vuFaceRatio: 0.66
    property color vuFaceTop: "#f7eccb"
    property color vuFaceBottom: "#e9d9a8"
    property color vuFaceEdge: "#6b5320"
    property color vuCaptionInk: "#6b5320"
    property color vuNeedle: "#16130d"
    property color vuLampOff: "#b8aa80"
    property real vuRedZoneWidth: 5
    property real vuTickStroke: 1.2
    property real vuLabelDrop: 3
    property real vuNeedleStroke: 1.6
    property real pivotRadius: 12
    property real vuCaptionInset: 6
    property real vuCaptionTop: 4
    property real vuLampX: 7
    property real vuLampY: 6
    property real duckInset: 10
    property real duckGap: 8
    property real duckLampSpacing: 3
    property real duckLampHeight: 8
    property color duckInk: "#2b2318"
    property color duckLampOn: "#e7a53a"
    property color duckLampOff: "#5e4a22"

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        // Printed scale: eleven ticks just outside the arc.
        ctx.strokeStyle = Qt.rgba(knob.theme.label.r, knob.theme.label.g, knob.theme.label.b, 0.45)
        ctx.lineWidth = 1
        for (let i = 0; i <= 10; ++i) {
            const a = start + sweep * i / 10
            const inner = radius + 4.5
            const outer = radius + (i % 5 === 0 ? 8 : 6.5)
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(a) * inner, c + Math.sin(a) * inner)
            ctx.lineTo(c + Math.cos(a) * outer, c + Math.sin(a) * outer)
            ctx.stroke()
        }
        // Value arc.
        ctx.lineCap = "round"
        const from = knob.bipolar ? start + sweep / 2 : start
        ctx.lineWidth = 2.5
        ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.28)
        ctx.beginPath()
        ctx.arc(c, c, radius + 2, start, start + sweep)
        ctx.stroke()
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = knob.accent
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Knurled brass skirt.
        const skirt = radius - 0.5
        const brass = ctx.createLinearGradient(c - skirt, c - skirt, c + skirt, c + skirt)
        brass.addColorStop(0, knob.theme.brassHi)
        brass.addColorStop(0.55, knob.theme.brass)
        brass.addColorStop(1, knob.theme.brassLo)
        ctx.fillStyle = brass
        ctx.beginPath()
        ctx.arc(c, c, skirt, 0, Math.PI * 2)
        ctx.fill()
        ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.35)
        ctx.lineWidth = 0.8
        for (let k = 0; k < 36; ++k) {
            const a = Math.PI * 2 * k / 36
            ctx.beginPath()
            ctx.moveTo(c + Math.cos(a) * (skirt - 2.5), c + Math.sin(a) * (skirt - 2.5))
            ctx.lineTo(c + Math.cos(a) * skirt, c + Math.sin(a) * skirt)
            ctx.stroke()
        }
        // Bakelite cap.
        const cap = skirt * 0.74
        const fill = ctx.createRadialGradient(c - cap * 0.35, c - cap * 0.45, cap * 0.1, c, c, cap)
        fill.addColorStop(0, knob.theme.capTop)
        fill.addColorStop(1, knob.theme.capBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 1.8 : 1
        ctx.strokeStyle = knob.activeFocus ? knob.accent : "#0a0806"
        ctx.stroke()
        // Pointer: a cream line from the centre out across the skirt.
        ctx.strokeStyle = knob.theme.pointer
        ctx.lineWidth = 2.2
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * cap * 0.25, c + Math.sin(at) * cap * 0.25)
        ctx.lineTo(c + Math.cos(at) * (skirt - 1), c + Math.sin(at) * (skirt - 1))
        ctx.stroke()
    }

    // Segment keys: cream piano-key switches.
    function segmentFill(tile, stop) {
        if (stop === 0)
            return tile.on ? "#d9cda9" : bodyTop
        return tile.on ? "#c6b88f" : bodyBottom
    }
    function segmentEdge(tile) {
        return Qt.rgba(0, 0, 0, 0.45)
    }

    // Dropdown: a glowing screen readout.
    function dropdownFill(dropdown) {
        return screen
    }
    function dropdownInk(dropdown) {
        return screenGlow
    }
}
