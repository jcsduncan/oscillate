import QtQuick
import "../devicekit"

// Orbit design tokens: a deep indigo night sky with violet glass plates.
// Each mode has its own luminous accent; the panel binds `accent` to the
// current mode's.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "orbit"
    tipRadius: 3
    knobDiameter: 34
    knobDialPad: 12
    knobWidthPad: 18
    knobMinWidth: 56
    knobHeightPad: 28
    knobLongName: "group"
    knobLabelFont: ""
    knobLabelSize: 9
    knobLabelBold: true
    knobLabelSpacing: 0.9
    segmentWidth: 120
    segmentHeight: 22
    segmentSpacing: 2
    segmentFocusMargin: 2
    segmentFocusRadius: 4
    segmentRadius: 3
    segmentRadiusVertical: 3
    segmentFillAcross: false
    segmentRule: "none"
    segmentLed: true
    segmentLedX: 4
    dropdownHeight: 22
    dropdownTextX: 7
    dropdownTextRight: 22
    dropdownChevronMargin: 6
    arrowWidth: 26
    arrowHeight: 40
    arrowRadius: 7
    arrowPrevious: "◀"
    arrowNext: "▶"
    arrowGlyphSize: 10
    arrowInk: accent

    property color bodyTop: "#141032"
    property color body: "#0d0a24"
    property color bodyBottom: "#070517"
    seam: "#2a2458"
    plate: "#120e2c"
    plateEdge: "#2b2560"
    well: "#080619"
    wellEdge: "#241f4d"
    grid: "#221d48"
    ink: "#eeecff"
    inkDim: "#9e98d2"
    inkFaint: "#5f5a98"
    property color sky: "#06041a"
    property color nebulaA: "#3b1d7a"
    property color nebulaB: "#123a6a"

    readonly property var modeColors: ["#b9a2ff", "#ffc56e", "#ff90d9", "#7fd8ff",
                                       "#6dffd3", "#ffa07c", "#93a9ff"]
    readonly property var modeNames: ["CHORUS", "ENSEMBLE", "DIMENSION", "FLANGER",
                                      "PHASER", "VIBRATO", "TREMOLO"]
    readonly property var modeTaglines: ["1–4 VOICE CHORUS", "THREE-PHASE STRING ENSEMBLE",
                                         "ANTI-PHASE DIMENSION", "FEEDBACK FLANGER",
                                         "2–12 STAGE PHASER", "PITCH VIBRATO",
                                         "TREMOLO · AUTO-PAN"]
    // The current mode's accent and the right channel's companion tint.
    accent: modeColors[0]
    readonly property color companion: Qt.lighter(accent, 1.35)

    knobRing: "#030212"
    knobTop: "#3a3478"
    knobBottom: "#100d2b"
    pointer: "#f7f5ff"
    button: "#151137"
    buttonHi: "#1e1948"
    ledOff: "#2b2656"

    warning: "#ffb24a"
    live: "#62e6c3"

    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel: a night-sky card.
    property int panelWidth: 1180
    property int panelHeight: 708
    property real panelRadius: 10
    // Where the body's gradient passes through its middle colour.
    property real bodyStop: 0.55
    // White-hot centres: the cores, bodies and markers' lit spots.
    property color glint: "#ffffff"

    // Glass plates: a violet glass top over the plate, a dot in the accent
    // before the title and an optional note after it.
    property real plateRadius: 9
    property color plateGlass: Qt.rgba(0.11, 0.09, 0.26, 0.92)
    property real plateDotX: 12
    property real plateDotY: 14
    property real plateDotSize: 4
    property real plateTitleX: 22
    property real plateTitleY: 9
    property real plateTitleSpacing: 2.4
    property real plateNoteGap: 10
    property real plateNoteY: 10
    property real plateNoteSpacing: 1.2
    // A plate's contents: left inset, knob row, lower row, and the body
    // below the title.
    property real plateContentX: 14
    property real plateKnobsY: 34
    property real plateLowerY: 124
    property real plateBodyY: 30
    // Small captions over the sky and in the plates.
    property real captionSpacing: 1.3

    // A knob cell: the knob and room for its captions.
    property int cellDiameter: 46
    property real cellWidthPad: 26
    property real cellMinWidth: 74
    property real cellHeightPad: 34

    // Header: the orbit mark, the wordmark and its tagline.
    property real headerY: 12
    property real headerHeight: 52
    property real markSize: 44
    property real markRingWidth: 40
    property real markRingHeight: 16
    property real markRingAlpha: 0.75
    property real markRingStroke: 1.2
    // The mark's orbit tilt, in degrees.
    property real markRingTilt: -18
    property real markCoreSize: 12
    property real markMoonX: 36
    property real markMoonY: 13
    property real markMoonSize: 6
    property int wordmarkSize: 29
    property real wordmarkSpacing: 9
    property real taglineIndent: 2
    property real taglineGap: 1
    property real taglineSpacing: 1.7
    property real statusY: 12

    // The preset display: category and name on the left, the last-touched
    // readout right of a divider.
    property real browserX: 320
    property real browserY: 4
    property real presetDisplayWidth: 470
    property real presetDisplayHeight: 40
    property real presetDisplayRadius: 7
    property real focusStroke: 1.5
    property real presetTextX: 12
    property real presetCaptionY: 4
    property real presetCategorySpacing: 1.2
    property real presetNameY: 15
    property real presetNameWidth: 210
    property real presetDividerX: 232
    property real presetDividerInset: 7
    property real presetReadoutX: 242
    // Room kept right of the readout for the chevron.
    property real presetReadoutRight: 24
    property real readoutSpacing: 1
    property real presetValueY: 16
    property real presetChevronMargin: 9

    // Mode tabs, each with a tiny orbit glyph.
    property real tabsY: 76
    property real tabHeight: 42
    property real tabOnAlpha: 0.13
    property real tabOnStroke: 1.5
    property real tabGlyphX: 12
    property real tabGlyphWidth: 20
    property real tabGlyphHeight: 12
    property real tabGlyphCore: 5
    property real tabGlyphMoonX: 15
    property real tabGlyphMoonY: -1
    property real tabGlyphMoonSize: 4
    property real tabTextOffset: 10
    property real tabSpacing: 1.8

    // The hero: the orrery's sky and the sweep plate beside it.
    property real heroY: 130
    property real heroHeight: 336
    property real skyWidth: 700
    property real skyRadius: 10
    // The readouts' inset from the sky's edges.
    property real skyInsetX: 18
    property real skyInsetY: 14
    property real sweepReadoutGap: 14
    property int readoutSize: 20
    property real liveLampGap: 7
    property real liveLampSize: 7
    property real sweepInset: 6
    property real sweepCaptionX: 24
    property real sweepCaptionY: 132

    // The control plates: Motion, Voice and Output.
    property real controlsY: 478
    property real controlsHeight: 214
    property real motionWidth: 486
    property real motionChoicesX: 270
    property real motionChoicesY: 36
    property real motionChoiceWidth: 196
    property real motionFootY: 190
    // The division while the rate runs free.
    property real unsyncedOpacity: 0.4
    property real voiceWidth: 410
    property real voiceChoiceWidth: 240
    property real voiceNoteX: 18
    property real voiceNoteY: 44
    property real voiceNoteLineHeight: 1.25
    property real outputKnobsX: 10
    property int outputCellDiameter: 34

    // The orrery: sky, nebulae, stars, the orbit, its bodies and the core.
    property color skyTop: "#0b0828"
    property real nebulaAAlpha: 0.5
    property real nebulaBAlpha: 0.45
    // A haze of the accent behind the core.
    property real hazeAlpha: 0.07
    property color star: Qt.rgba(0.9, 0.9, 1.0, 1)
    property real starSize: 0.8
    property real brightStarSize: 1.6
    property real starAlphaMin: 0.15
    property real starAlphaRange: 0.55
    property real outerGuideAlpha: 0.18
    property real innerGuideAlpha: 0.14
    property real orbitGlowAlpha: 0.12
    property real orbitGlowWidth: 7
    property real orbitLineAlpha: 0.55
    property real orbitLineWidth: 1.3
    property real stereoArcAlpha: 0.5
    property real stereoArcWidth: 1.4
    property real stereoDash: 3
    property real stereoDashGap: 4
    property real stereoCaptionAlpha: 0.9
    // The stereo caption's baseline below its point on the ring.
    property real stereoCaptionDrop: 4
    property real trailAlpha: 0.42
    property real trailDotSize: 1.2
    property real trailDotGrow: 1.6
    // A body's radius, grown as it nears the viewer.
    property real bodyRadius: 6.5
    property real bodyNearGrow: 3.0
    property real bodyGlowAlpha: 0.55
    // A body's L/R label, right of and above it.
    property real bodyLabelX: 12
    property real bodyLabelRise: 10
    property real coreRadius: 13
    property real coreAlpha: 0.9
    property real coreGlowRadius: 64
    property real coreGlowAlpha: 0.5
    // Where the core's glow falls off to its knee alpha.
    property real coreGlowKnee: 0.35
    property real coreGlowKneeAlpha: 0.16

    // The sweep scope: the range track's band and needles, the LFO traces
    // and the markers on them.
    property real sweepPad: 18
    property real bandHalf: 5
    property real bandMinWidth: 2
    property real bandEdgeAlpha: 0.25
    property real bandPeakAlpha: 0.6
    property real needleHalf: 12
    property real needleWidth: 2
    property real needleGlowWidth: 6
    property real needleGlowAlpha: 0.25
    property real traceWidth: 1.6
    property real traceAlpha: 0.95
    property real companionTraceWidth: 1.4
    property real companionTraceAlpha: 0.85
    property real traceGlowWidth: 6
    property real traceGlowAlpha: 0.18
    property real markerGlowRadius: 10
    property real markerGlowAlpha: 0.7
    property real markerDotRadius: 2.6

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        const a = knob.accent
        ctx.lineCap = "round"
        // The orbit: a faint full track.
        ctx.lineWidth = 2
        ctx.strokeStyle = knob.theme.grid
        ctx.beginPath()
        ctx.arc(c, c, radius + 2, start, start + sweep)
        ctx.stroke()
        // Value arc with a wide soft glow.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, 0.16)
            ctx.lineWidth = 8
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
            ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, 0.9)
            ctx.lineWidth = 2.2
            ctx.beginPath()
            ctx.arc(c, c, radius + 2, Math.min(from, at), Math.max(from, at))
            ctx.stroke()
        }
        // Glass body, lit from the upper left.
        const body = radius - 3
        const fill = ctx.createRadialGradient(c - body * 0.35, c - body * 0.45, body * 0.1,
                                              c, c, body)
        fill.addColorStop(0, knob.theme.knobTop)
        fill.addColorStop(1, knob.theme.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, body, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 1.6 : 1
        ctx.strokeStyle = knob.activeFocus ? a : knob.theme.knobRing
        ctx.stroke()
        // Pointer.
        const p = knob.theme.pointer
        ctx.strokeStyle = Qt.rgba(p.r, p.g, p.b, 0.85)
        ctx.lineWidth = 1.6
        ctx.beginPath()
        ctx.moveTo(c + Math.cos(at) * body * 0.3, c + Math.sin(at) * body * 0.3)
        ctx.lineTo(c + Math.cos(at) * (body - 3), c + Math.sin(at) * (body - 3))
        ctx.stroke()
        // The moon on the ring.
        const mx = c + Math.cos(at) * (radius + 2)
        const my = c + Math.sin(at) * (radius + 2)
        const glow = ctx.createRadialGradient(mx, my, 0, mx, my, 6)
        glow.addColorStop(0, Qt.rgba(a.r, a.g, a.b, 0.6))
        glow.addColorStop(1, Qt.rgba(a.r, a.g, a.b, 0))
        ctx.fillStyle = glow
        ctx.beginPath()
        ctx.arc(mx, my, 6, 0, Math.PI * 2)
        ctx.fill()
        ctx.fillStyle = p
        ctx.beginPath()
        ctx.arc(mx, my, 2.3, 0, Math.PI * 2)
        ctx.fill()
    }

    // Segment keys.
    function segmentFill(tile, stop) {
        return stop === 0 && !tile.pressed ? buttonHi : button
    }
    function segmentEdge(tile) {
        return tile.on ? Qt.rgba(tile.accent.r, tile.accent.g, tile.accent.b, 0.7) : knobRing
    }
}
