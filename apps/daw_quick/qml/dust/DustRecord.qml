pragma ComponentBehavior: Bound
import QtQuick

// The record on Dust's platter: it turns where the engine's record is
// (telemetry rotation, one turn per revolution at the chosen speed), its
// grooves worn by dust.vinyl.wear in scratches placed by the seed, and
// sparks flash at the needle as the crackle's pops are counted.
Item {
    id: record
    required property var controller
    required property DustTheme theme
    property var live: ({ live: false })
    readonly property bool playing: live.live === true && live.rotation !== undefined
    readonly property int revision: controller.revision
    readonly property bool vinylOn: revision >= 0 && controller.choice("dust.vinyl.on") === 1
    readonly property real wear: revision >= 0 ? controller.normalised("dust.vinyl.wear") : 0
    readonly property int seed: revision >= 0 ? Math.round(controller.value("dust.mix.seed")) : 0
    readonly property var speeds: controller.parameter("dust.vinyl.speed").choices
    readonly property string speedText: revision >= 0 && speeds !== undefined
        ? speeds[controller.choice("dust.vinyl.speed")] : ""
    // Where the record stands, 0..1 of a turn.
    readonly property real turn: playing ? Number(live.rotation) : 0
    readonly property real fullTurnDegrees: 360
    // Pops counted so far by this view, and the last count the engine gave.
    property int sparkTotal: 0
    property real lastPops: -1
    property int sparkSeed: 0
    property real flash: 0
    // The most scratches a fully worn record shows.
    readonly property int maxScratches: 26
    readonly property int grooveRings: 22
    // Every so many grooves, a band of the stronger grid colour.
    readonly property int grooveBandEvery: 5
    // Specks of dust per scratch at full wear.
    readonly property int specksPerScratch: 4
    // The flash's fade after a run of pops (milliseconds).
    readonly property int flashDuration: 220
    // The disc's proportions: its widest share of the view, the label's
    // share of its radius, the grooves' inset from the label and the rim,
    // the scratches' and specks' inset, the sheen's inset from the rim.
    readonly property real discWidthShare: 0.62
    readonly property real labelShare: 0.34
    readonly property real grooveInset: 4
    readonly property real scratchInset: 6
    readonly property real speckInset: 4
    readonly property real sheenInset: 2
    // The sheen's wedge (radians, mirrored across the spindle).
    readonly property real sheenStart: -0.5
    readonly property real sheenEnd: 0.15
    // A scratch's arc (radians): its shortest, the most it adds; the share
    // of scratches cut deep.
    readonly property real scratchMinArc: 0.05
    readonly property real scratchArcRange: 0.35
    readonly property real deepScratchShare: 0.2
    // The label: its ring, its title's and side line's size and place, as
    // shares of its radius.
    readonly property real labelRingShare: 0.8
    readonly property real labelTitleShare: 0.28
    readonly property real labelTitleRise: 0.32
    readonly property real labelSideShare: 0.16
    readonly property real labelSideDrop: 0.5
    // The tonearm: its pivot beside and below the platter's corner, the
    // needle's place as shares of the disc, the arm's elbow, and the
    // headshell's ends, from the needle.
    readonly property real pivotGap: 4
    readonly property real pivotDrop: 10
    readonly property real needleShareX: 0.80
    readonly property real needleShareY: 0.66
    readonly property real elbowX: -6
    readonly property real elbowY: -28
    readonly property real armEndX: 6
    readonly property real armEndY: -4
    readonly property real headshellBackX: 9
    readonly property real headshellBackY: -6
    readonly property real headshellTipX: 1
    readonly property real headshellTipY: -1
    // The sparks: how many, and how far they fly from the needle.
    readonly property int sparkCount: 6
    // Each scatter's seeds, a stride apart so runs of pops differ.
    readonly property int sparkSeedStride: 13
    readonly property real sparkReachMin: 4
    readonly property real sparkReachRange: 12

    objectName: "dustRecord"
    readonly property string plotExemptReason: "a picture of the record turning on its platter, worn and sparking; no axes to scale"
    implicitWidth: theme.recordWidth
    implicitHeight: theme.recordHeight

    // A small seeded generator (mulberry32), so the same seed wears the same
    // record.
    function seeded(s) {
        let state = (s * 2654435761) >>> 0
        return function () {
            state = (state + 0x6D2B79F5) >>> 0
            let t = state
            t = Math.imul(t ^ (t >>> 15), t | 1)
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296
        }
    }

    onLiveChanged: {
        if (live.live !== true || live.rotation === undefined) {
            lastPops = -1
            return
        }
        const pops = Number(live.pops)
        if (lastPops >= 0) {
            // The count wraps; a smaller count is a fresh run of pops.
            const added = pops >= lastPops ? pops - lastPops : pops
            if (added > 0) {
                sparkTotal += Math.round(added)
                sparkSeed += 1
                flashAnimation.restart()
            }
        }
        lastPops = pops
    }
    NumberAnimation {
        id: flashAnimation
        target: record
        property: "flash"
        from: 1
        to: 0
        duration: record.flashDuration
        easing.type: Easing.OutQuad
    }

    readonly property real discSize: Math.min(height - 2 * theme.spaceMd, width * discWidthShare)
    readonly property real discX: theme.spaceLg
    readonly property real discY: (height - discSize) / 2

    // The platter's shadow.
    Rectangle {
        x: record.discX - record.theme.spaceXs
        y: record.discY - record.theme.spaceXs
        width: record.discSize + 2 * record.theme.spaceXs
        height: width
        radius: width / 2
        color: record.theme.well
        border.color: record.theme.plateEdge
    }

    Item {
        id: disc
        objectName: "dustDisc"
        x: record.discX
        y: record.discY
        width: record.discSize
        height: record.discSize
        rotation: record.turn * record.fullTurnDegrees
        opacity: record.vinylOn ? 1 : record.theme.discOffOpacity

        Canvas {
            id: grooves
            anchors.fill: parent
            readonly property real wear: record.wear
            readonly property int seed: record.seed
            onWearChanged: requestPaint()
            onSeedChanged: requestPaint()
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                const t = record.theme
                const w = width
                const c = w / 2
                const r = w / 2
                ctx.clearRect(0, 0, w, w)
                ctx.fillStyle = t.vinyl
                ctx.beginPath(); ctx.arc(c, c, r, 0, 2 * Math.PI); ctx.fill()
                // The grooves, from the lead-in to the label.
                const labelR = r * record.labelShare
                const inset = record.grooveInset
                ctx.lineWidth = t.hairline
                for (let i = 0; i < record.grooveRings; ++i) {
                    const gr = labelR + inset + (r - labelR - 2 * inset) * i / (record.grooveRings - 1)
                    ctx.strokeStyle = i % record.grooveBandEvery === 0 ? t.gridStrong : t.vinylGroove
                    ctx.beginPath(); ctx.arc(c, c, gr, 0, 2 * Math.PI); ctx.stroke()
                }
                // The sheen, a wedge that turns with the record.
                ctx.fillStyle = t.vinylSheen
                const sheenR = r - record.sheenInset
                ctx.beginPath(); ctx.moveTo(c, c); ctx.arc(c, c, sheenR, record.sheenStart, record.sheenEnd); ctx.closePath(); ctx.fill()
                ctx.beginPath(); ctx.moveTo(c, c)
                ctx.arc(c, c, sheenR, Math.PI + record.sheenStart, Math.PI + record.sheenEnd); ctx.closePath(); ctx.fill()
                // Wear: scratches across the grooves and a haze of dust.
                const random = record.seeded(seed)
                const scratches = Math.round(wear * record.maxScratches)
                ctx.strokeStyle = Qt.rgba(t.ink.r, t.ink.g, t.ink.b, t.scratchAlpha + t.scratchWearAlpha * wear)
                for (let i = 0; i < scratches; ++i) {
                    const a = random() * 2 * Math.PI
                    const rr = labelR + record.scratchInset + random() * (r - labelR - 2 * record.scratchInset)
                    const len = record.scratchMinArc + random() * record.scratchArcRange
                    ctx.lineWidth = random() < record.deepScratchShare ? t.deepScratchWidth : t.scratchWidth
                    ctx.beginPath(); ctx.arc(c, c, rr, a, a + len); ctx.stroke()
                }
                const specks = Math.round(wear * record.maxScratches * record.specksPerScratch)
                ctx.fillStyle = Qt.rgba(t.ink.r, t.ink.g, t.ink.b, t.speckAlpha)
                for (let i = 0; i < specks; ++i) {
                    const a = random() * 2 * Math.PI
                    const rr = labelR + random() * (r - labelR - record.speckInset)
                    ctx.fillRect(c + Math.cos(a) * rr, c + Math.sin(a) * rr, t.hairline, t.hairline)
                }
                // The label and the spindle.
                ctx.fillStyle = t.label
                ctx.beginPath(); ctx.arc(c, c, labelR, 0, 2 * Math.PI); ctx.fill()
                ctx.strokeStyle = t.amberHot
                ctx.lineWidth = t.labelRingWidth
                ctx.beginPath(); ctx.arc(c, c, labelR * record.labelRingShare, 0, 2 * Math.PI); ctx.stroke()
                ctx.fillStyle = t.labelInk
                ctx.font = "bold " + Math.max(t.labelTitleMinSize, Math.round(labelR * record.labelTitleShare))
                    + "px '" + t.display + "'"
                ctx.textAlign = "center"
                ctx.fillText("DUST", c, c - labelR * record.labelTitleRise)
                ctx.font = Math.max(t.labelSideMinSize, Math.round(labelR * record.labelSideShare)) + "px '" + t.sans + "'"
                ctx.fillText("SIDE A · " + seed, c, c + labelR * record.labelSideDrop)
                ctx.fillStyle = t.well
                ctx.beginPath(); ctx.arc(c, c, t.spindleRadius, 0, 2 * Math.PI); ctx.fill()
            }
        }
    }

    // The tonearm: from its pivot at the platter's corner to the needle in
    // the outer grooves (where the sparks fly).
    readonly property real pivotX: discX + discSize + pivotGap
    readonly property real pivotY: discY + pivotDrop
    readonly property real needleX: discX + discSize * needleShareX
    readonly property real needleY: discY + discSize * needleShareY
    Canvas {
        id: arm
        anchors.fill: parent
        readonly property bool lit: record.vinylOn
        onLitChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            const t = record.theme
            ctx.clearRect(0, 0, width, height)
            ctx.lineCap = "round"
            ctx.strokeStyle = t.plateHi
            ctx.lineWidth = t.armWidth
            ctx.beginPath()
            ctx.moveTo(record.pivotX, record.pivotY)
            ctx.lineTo(record.pivotX + record.elbowX, record.needleY + record.elbowY)
            ctx.lineTo(record.needleX + record.armEndX, record.needleY + record.armEndY)
            ctx.stroke()
            ctx.strokeStyle = arm.lit ? t.amber : t.inkFaint
            ctx.lineWidth = t.headshellWidth
            ctx.beginPath()
            ctx.moveTo(record.needleX + record.headshellBackX, record.needleY + record.headshellBackY)
            ctx.lineTo(record.needleX + record.headshellTipX, record.needleY + record.headshellTipY)
            ctx.stroke()
            ctx.fillStyle = t.knobTop
            ctx.beginPath(); ctx.arc(record.pivotX, record.pivotY, t.pivotRadius, 0, 2 * Math.PI); ctx.fill()
            ctx.strokeStyle = t.plateHi
            ctx.lineWidth = t.hairline
            ctx.stroke()
        }
    }

    // Sparks at the needle, a fresh scatter for each run of pops.
    Item {
        id: sparks
        objectName: "dustSparks"
        readonly property int total: record.sparkTotal
        x: record.needleX
        y: record.needleY
        opacity: record.flash
        Repeater {
            model: record.sparkCount
            delegate: Rectangle {
                id: spark
                required property int index
                readonly property real angle: record.seeded(record.sparkSeed * record.sparkSeedStride + index * 2 + 1)() * 2 * Math.PI
                readonly property real reach: record.sparkReachMin
                    + record.seeded(record.sparkSeed * record.sparkSeedStride + index * 2 + 2)() * record.sparkReachRange
                x: Math.cos(angle) * reach - width / 2
                y: Math.sin(angle) * reach - height / 2
                width: index === 0 ? record.theme.sparkLeadSize : record.theme.sparkSize
                height: width
                radius: width / 2
                color: record.theme.spark
            }
        }
    }

    // What it is playing.
    Column {
        x: record.discX + record.discSize + record.theme.recordInfoGap
        anchors.bottom: parent.bottom
        anchors.bottomMargin: record.theme.spaceMd
        spacing: record.theme.spaceXxs
        Text {
            objectName: "dustRecordSpeed"
            text: record.speedText + " RPM"
            color: record.vinylOn ? record.theme.amber : record.theme.inkFaint
            font.family: record.theme.mono
            font.pixelSize: record.theme.recordSpeedSize
            font.bold: true
        }
        Text {
            objectName: "dustRecordState"
            text: !record.vinylOn ? "NEEDLE UP" : record.playing ? "PLAYING" : "STOPPED"
            color: record.theme.inkFaint
            font.pixelSize: record.theme.typeMicro
            font.bold: true
            font.letterSpacing: record.theme.tagSpacing
        }
    }
}
