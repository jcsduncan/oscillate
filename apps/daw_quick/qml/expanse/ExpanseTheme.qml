import QtQuick
import "../devicekit"

// Expanse design tokens, "night sky": a midnight case under aurora greens
// and violets, lettered in starlight. Calm and spacious.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "expanse"
    accent: aurora
    steel: auroraViolet
    arrowRadius: 8

    // The sky: deep midnight, lightening a touch toward the horizon.
    property color skyTop: "#0b1233"
    property color sky: "#070c24"
    property color skyBottom: "#04071a"
    property color star: "#f2f0ff"
    // Glass plates over the sky.
    plate: "#0a1130"
    plateEdge: "#1b2858"
    well: "#050920"
    wellEdge: "#1f2d60"
    seam: "#16214a"

    // The aurora: green at the base of the curtain, violet at its crown.
    property color aurora: "#52f0b0"
    property color auroraTeal: "#2fd0c8"
    property color auroraViolet: "#9d7cff"
    property color auroraMagenta: "#d77cf4"
    property color ice: "#c4ecff"

    ink: "#e9eeff"
    inkDim: "#97a5cc"
    inkFaint: "#5c6a94"

    knobTop: "#1b2552"
    knobBottom: "#060a1c"
    track: "#18234c"
    button: "#0a1131"
    buttonHi: "#111a42"

    warning: "#f0a646"
    live: "#52f0b0"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel's frame: its size, rounded case and where the sky gradient
    // reaches its body colour.
    property int panelWidth: 1280
    property int panelHeight: 690
    property real caseRadius: 12
    property real skyMidStop: 0.45
    // Between the plates.
    property real plateGap: 10
    // A focused or lit control's edge.
    property real focusStroke: 1.5

    // The night behind the plates: stars of varied size and brightness and
    // two faint aurora ribbons, each a slow swell with a finer ripple.
    property real starMinRadius: 0.35
    property real starRadiusRange: 0.9
    property real starMinAlpha: 0.08
    property real starAlphaRange: 0.32
    property real ribbonY: 34
    property real ribbonAlpha: 0.10
    property real ribbonVioletY: 52
    property real ribbonVioletAlpha: 0.08
    property real ribbonPeakStop: 0.55
    property real ribbonWidth: 16
    property real ribbonStep: 16
    property real ribbonSwell: 10
    property real ribbonSwellLength: 140
    property real ribbonRipple: 6
    property real ribbonRippleLength: 47

    // Glass plates: translucent over the sky, a starlit sheen along the top
    // edge, titles in widely spaced caps beside an optional note.
    property color glassFill: Qt.rgba(plate.r, plate.g, plate.b, 0.82)
    property color sheen: Qt.rgba(ice.r, ice.g, ice.b, 0.20)
    property color sheenClear: Qt.rgba(1, 1, 1, 0)
    property real plateTitleSpacing: 3.2
    property real plateNoteY: 11
    property real noteSpacing: 1.2
    // The title band above a plate's contents.
    property real plateHeaderHeight: 28
    property real plateInset: 10

    // A knob cell: its width and the height it adds below the knob, with
    // and without a readout.
    property real cellWidth: 80
    property real cellHeightPad: 36
    property real cellReadoutHeightPad: 50

    // The header: the wordmark, its aurora rule and the subtitle.
    property real headerY: 10
    property real headerHeight: 54
    property int wordmarkSize: 27
    property real wordmarkSpacing: 11
    property real wordmarkRuleGap: 1
    property real wordmarkRuleHeight: 2
    // How much shorter than the wordmark the rule runs.
    property real wordmarkRuleInset: 12
    property real wordmarkRuleMidStop: 0.55
    property color wordmarkRuleFade: Qt.rgba(auroraViolet.r, auroraViolet.g, auroraViolet.b, 0.1)
    property real subtitleGap: 7
    property real subtitleSpacing: 2.6
    property real statusY: 14

    // The preset display between the arrows, and the readout inside it.
    property real presetX: 252
    property real presetY: 5
    property real presetWidth: 560
    property real presetHeight: 42
    property color presetFill: Qt.rgba(well.r, well.g, well.b, 0.92)
    property real presetTextX: 14
    property real presetCaptionY: 5
    property real presetCaptionSpacing: 1.4
    property real presetNameY: 16
    property real presetNameWidth: 250
    property real presetNameSpacing: 0.6
    property real presetDividerX: 276
    property real presetDividerInset: 8
    property real readoutX: 288
    property real readoutValueY: 17
    property real readoutRight: 26
    property real readoutSpacing: 1.2
    property real chevronMargin: 10

    // The plate grid: three rows and the widths of the fixed plates.
    property real rowAY: 76
    property real rowAHeight: 300
    property real rowBHeight: 140
    property real rowCHeight: 138
    property real spaceWidth: 200
    property real landscapeWidth: 800
    property real dampingWidth: 340
    // A plate of two knobs (diffusion, motion, tone).
    property real pairPlateWidth: 180
    property real levelsWidth: 262
    property real lightWidth: 534

    // The space list and the landscape inside their plates.
    property real spaceListY: 32
    property real landscapeY: 26

    // Dimensions: Size and Decay large, Pre-Delay small, Freeze below.
    property real dimensionsKnobY: 30
    property int bigKnobDiameter: 64
    property real bigCellWidth: 108
    property real predelayY: 148
    property int smallKnobDiameter: 34
    property real toggleMargin: 14

    // Shimmer: the amount knob, then the interval and its note.
    property real shimmerCellY: 34
    property int shimmerKnobDiameter: 50
    property real shimmerCellWidth: 96
    property real shimmerTextX: 128
    property real intervalCaptionY: 42
    property real intervalCaptionSpacing: 1.4
    property real intervalY: 58
    property real intervalHeight: 30
    property real shimmerNoteY: 98

    // Sky light: the level meters.
    property real meterColumnY: 36
    property int meterWidth: 300
    property int meterHeight: 22
    property real meterLabelWidth: 44
    // Room right of the bar for the value.
    property real meterValueRoom: 56
    property real meterValueWidth: 48
    property real meterBarHeight: 6
    // The smallest lit length, so a quiet level still shows.
    property real meterMinFill: 6
    // The bar's fill starts at this fraction of its tone.
    property real meterFadeAlpha: 0.35
    property color meterTick: Qt.rgba(inkFaint.r, inkFaint.g, inkFaint.b, 0.35)

    // Small bold captions (meter labels, readout bands, the landscape state).
    property real captionSpacing: 1.6

    // The Freeze pill: frosted ice when on, with a halo of frost.
    property int toggleWidth: 200
    property int toggleHeight: 50
    property color toggleOnStart: Qt.rgba(ice.r, ice.g, ice.b, 0.30)
    property color toggleOnEnd: Qt.rgba(auroraViolet.r, auroraViolet.g, auroraViolet.b, 0.18)
    property real frostHaloSpread: 5
    property real frostHaloWidth: 4
    property color frostHalo: Qt.rgba(ice.r, ice.g, ice.b, 0.12)
    property real toggleGlyphX: 18
    property int toggleGlyphSize: 22
    property real toggleLabelSpacing: 3.5
    property real toggleCaptionSpacing: 1

    // The decay landscape: its dark field, the curtains, the guide lines,
    // the frost when frozen and the live glow.
    property int landscapeDefaultWidth: 760
    property int landscapeDefaultHeight: 250
    property color fieldTop: "#060a22"
    property color fieldBottom: "#03051a"
    property color fieldGrid: Qt.rgba(inkFaint.r, inkFaint.g, inkFaint.b, 0.16)
    property real curtainAlpha: 0.9
    property real shimmerCurtainAlpha: 0.95
    // The dimmest the curtains go however low the late level.
    property real curtainLevelFloor: 0.25
    property color predelayShade: Qt.rgba(0, 0, 0.02, 0.35)
    property color predelayEdge: Qt.rgba(star.r, star.g, star.b, 0.35)
    property var predelayDash: [2, 3]
    property color crossoverLine: Qt.rgba(ice.r, ice.g, ice.b, 0.28)
    property var crossoverDash: [4, 4]
    // The -20 and -40 dB lines, and the RT60 line itself.
    property real isoLineAlpha: 0.14
    property real rtLineAlpha: 0.85
    property real rtLineWidth: 1.6
    // How far the modulation ripples the lines, per unit depth.
    property real rtWobble: 3
    // Shimmer's copy of the RT60 line: brighter with more shimmer.
    property real shimmerLineAlpha: 0.45
    property real shimmerLineAlphaRange: 0.5
    property real shimmerLineWidth: 1.4
    property var shimmerDash: [5, 4]
    property color frostStart: Qt.rgba(ice.r, ice.g, ice.b, 0.02)
    property color frostEnd: Qt.rgba(ice.r, ice.g, ice.b, 0.20)
    property color glowTop: Qt.rgba(auroraViolet.r, auroraViolet.g, auroraViolet.b, 0.16)
    property real glowMidStop: 0.55
    property color glowMid: Qt.rgba(aurora.r, aurora.g, aurora.b, 0.10)
    property color glowBottom: Qt.rgba(aurora.r, aurora.g, aurora.b, 0.22)

    // The landscape's axes: labels clear of the field, time ticks below it.
    property real axisLabelGap: 7
    property color timeTick: Qt.rgba(inkFaint.r, inkFaint.g, inkFaint.b, 0.6)
    property real timeTickLength: 4
    property real timeLabelInset: 14
    property int maxTimeTicks: 7

    // The early-reflections strip: its dark bed, the late onset line and
    // the taps as stars rising from its floor.
    property color stripFill: Qt.rgba(0.01, 0.02, 0.08, 0.9)
    property color stripPredelayShade: Qt.rgba(0, 0, 0, 0.35)
    property color stripOnset: Qt.rgba(aurora.r, aurora.g, aurora.b, 0.55)
    property real stripOnsetInset: 3
    property int stripLabelSize: 7
    property real stripTextY: 3
    property real stripNoteY: 9
    property real tapBase: 4
    property real tapMinLength: 4
    // The strip height a full-strength tap leaves clear.
    property real tapLengthInset: 10
    property real tapWidth: 1.3
    property real tapAlpha: 0.85
    property real tapHaloRadius: 4
    property real tapHaloAlpha: 0.9
    // Early level sets how bright the taps are, from this floor.
    property real tapPresenceFloor: 0.3
    property color tapFade: Qt.rgba(ice.r, ice.g, ice.b, 0)
    property color starFade: Qt.rgba(star.r, star.g, star.b, 0)

    // The readouts along the landscape's top.
    property real readoutRowSpacing: 18
    property real readoutDot: 7

    // The knob face.
    function paintKnob(ctx, knob, size) {
        const c = size / 2
        const radius = knob.diameter / 2
        const start = Math.PI * 0.75
        const sweep = Math.PI * 1.5
        const at = start + sweep * knob.position
        const t = knob.theme
        // The track: a faint ring of night.
        ctx.lineCap = "round"
        ctx.lineWidth = 3
        ctx.strokeStyle = t.track
        ctx.beginPath()
        ctx.arc(c, c, radius, start, start + sweep)
        ctx.stroke()
        // The value: an aurora arc, glowing.
        const from = knob.bipolar ? start + sweep / 2 : start
        if (Math.abs(at - from) > 0.001) {
            const lo = Math.min(from, at)
            const hi = Math.max(from, at)
            const g = ctx.createLinearGradient(c - radius, c + radius, c + radius, c - radius)
            g.addColorStop(0, t.aurora)
            g.addColorStop(0.5, t.auroraTeal)
            g.addColorStop(1, t.auroraViolet)
            ctx.strokeStyle = g
            ctx.globalAlpha = 0.22
            ctx.lineWidth = 8
            ctx.beginPath()
            ctx.arc(c, c, radius, lo, hi)
            ctx.stroke()
            ctx.globalAlpha = 1
            ctx.lineWidth = 3
            ctx.beginPath()
            ctx.arc(c, c, radius, lo, hi)
            ctx.stroke()
        }
        // The midnight glass cap with a high cool highlight.
        const cap = radius - 5
        const fill = ctx.createRadialGradient(c - cap * 0.35, c - cap * 0.5, cap * 0.05, c, c, cap)
        fill.addColorStop(0, t.knobTop)
        fill.addColorStop(1, t.knobBottom)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.arc(c, c, cap, 0, Math.PI * 2)
        ctx.fill()
        ctx.lineWidth = knob.activeFocus ? 1.6 : 1
        ctx.strokeStyle = knob.activeFocus ? t.aurora : t.plateEdge
        ctx.stroke()
        // The pointer: a star near the rim with a little halo.
        const px = c + Math.cos(at) * (cap - 5)
        const py = c + Math.sin(at) * (cap - 5)
        const halo = ctx.createRadialGradient(px, py, 0, px, py, 6)
        halo.addColorStop(0, Qt.rgba(t.star.r, t.star.g, t.star.b, 0.55))
        halo.addColorStop(1, Qt.rgba(t.star.r, t.star.g, t.star.b, 0))
        ctx.fillStyle = halo
        ctx.beginPath()
        ctx.arc(px, py, 6, 0, Math.PI * 2)
        ctx.fill()
        ctx.fillStyle = t.star
        ctx.beginPath()
        ctx.arc(px, py, 1.9, 0, Math.PI * 2)
        ctx.fill()
    }
}
