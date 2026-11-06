pragma ComponentBehavior: Bound
import QtQuick

// The Constellation (ADR-0077): every
// preset a star placed by how it measured, brightness across and length up.
// Click or drag between stars and the patch becomes a blend of the three
// nearest, weighted by inverse square distance, heard live while notes are
// held; a drag is one undo step. Click a star itself to load it exactly.
// KEEP MINE holds the FX, envelopes or routes; MUTATE scatters six measured
// variations; KEEP THIS saves the blend as a user preset, which becomes a
// star. Everything comes from panel.controller.constellation
// (NovaConstellation).
Item {
    id: view
    required property var panel
    readonly property var constellation: panel.controller.constellation
    readonly property var theme: panel.theme

    // Category colours, as the preset browser's; anything else (User) dim.
    readonly property var categoryColours: theme.categoryColours
    // The sky: a deep graphite centre with the faintest violet breath, and a
    // neutral grid (canvas CSS colours).
    readonly property string skyCentre: theme.constellationSkyCentre
    readonly property string gridInk: theme.constellationGridInk
    readonly property string cursorInk: theme.constellationCursorInk

    // The sky's geometry and drawing, in pixels and seconds.
    readonly property real skyPad: 40
    readonly property int nearest: 3
    readonly property int gridDivisions: 10
    readonly property real starHit: 9
    readonly property real starRadius: 3
    readonly property real starWidthRadius: 2
    readonly property real variationRadius: 2.5
    readonly property real glowScale: 5
    readonly property real glowAlpha: theme.constellationGlowAlpha
    readonly property real twinkleRate: 1.7
    readonly property real twinklePhase: 2.3
    readonly property real twinkleFloor: 0.6
    readonly property real twinkleMovement: 0.3
    readonly property real lineWidth: theme.hairline
    readonly property real lineWeightWidth: 4
    readonly property real cursorRing: 8
    readonly property real cursorPulse: 3
    readonly property real cursorPulseRate: 5
    readonly property real cursorGlow: 30
    readonly property real labelGap: 5
    readonly property int labelSize: theme.typeSmall
    readonly property int labelSelectedSize: theme.typeLabel
    // A label sits this fraction of its size (as a divisor) below its star's centre.
    readonly property real labelDrop: 3
    // Wider than this, every star is labelled.
    readonly property real labelAllWidth: 900
    readonly property int frameMs: 33
    readonly property int barHeight: theme.constellationBarHeight
    // The backdrop's light: its centre's height and its reach past the
    // longer side, as fractions.
    readonly property real skyLightY: 0.4
    readonly property real skyLightReach: 0.75
    // The blend readout keeps at least this width, and leaves this much for
    // the MORPHING label beside it.
    readonly property real mixMinimumWidth: 80
    readonly property real mixReserve: 70
    readonly property int toastMs: 2600

    // The cursor in sky pixels, or null; the stars it blends
    // [{key, weight, x, y, category}].
    property var cursor: null
    property var near: []
    property bool dragging: false
    property real clock: 0
    // What the last paint drew, for tests.
    property int drawnStars: 0
    property int labelledStars: 0
    property string toast: ""

    readonly property var allStars: constellation ? constellation.stars.concat(constellation.variations) : []

    objectName: "novaConstellation"

    function colourOf(category) {
        const colour = categoryColours[category]
        return String(colour !== undefined ? colour : theme.dim)
    }
    // A "#rrggbb" colour at alpha `a`, as canvas CSS.
    function rgba(hex, a) {
        const v = parseInt(String(hex).slice(1, 7), 16)
        return "rgba(" + ((v >> 16) & 255) + "," + ((v >> 8) & 255) + "," + (v & 255) + "," + a + ")"
    }
    function skyPosition(star, w, h) {
        return [skyPad + star.x * (w - 2 * skyPad), h - skyPad - star.y * (h - 2 * skyPad)]
    }
    // The three nearest stars to (px, py) weighted by inverse square
    // distance (one pixel added, so a star under the cursor is not infinite).
    function blendAt(px, py) {
        const w = sky.width
        const h = sky.height
        const ranked = allStars.map(s => {
            const p = skyPosition(s, w, h)
            return { star: s, x: p[0], y: p[1], d: Math.hypot(p[0] - px, p[1] - py) + 1 }
        }).sort((a, b) => a.d - b.d).slice(0, nearest)
        const weights = ranked.map(r => 1 / (r.d * r.d))
        const sum = weights.reduce((a, b) => a + b, 0)
        return ranked.map((r, i) => ({ key: r.star.key, weight: weights[i] / sum, x: r.x, y: r.y,
                                       category: r.star.category }))
    }
    function starAt(px, py) {
        let best = null
        let bestDistance = starHit
        for (const s of allStars) {
            const p = skyPosition(s, sky.width, sky.height)
            const d = Math.hypot(p[0] - px, p[1] - py)
            if (d <= bestDistance) {
                best = s
                bestDistance = d
            }
        }
        return best
    }
    function morphTo(px, py) {
        cursor = [px, py]
        near = blendAt(px, py)
        constellation.morph(near.map(n => ({ key: n.key, weight: n.weight })))
        sky.requestPaint()
    }
    function mixText() {
        const parts = constellation ? constellation.blend : []
        if (parts.length === 0)
            return "Click or drag anywhere. The nearest presets blend into one sound."
        return parts.map(p => "<b><font color=\"" + colourOf(p.category) + "\">" + Math.round(p.weight * 100)
                         + "%</font></b> " + p.name).join(" · ")
    }

    function paintSky(ctx) {
        const w = sky.width
        const h = sky.height
        ctx.reset()
        const backdrop = ctx.createRadialGradient(w / 2, h * skyLightY, 0, w / 2, h * skyLightY,
                                                  Math.max(w, h) * skyLightReach)
        backdrop.addColorStop(0, skyCentre)
        backdrop.addColorStop(1, String(theme.bg))
        ctx.fillStyle = backdrop
        ctx.fillRect(0, 0, w, h)

        ctx.strokeStyle = gridInk
        ctx.lineWidth = lineWidth
        for (let i = 1; i < gridDivisions; ++i) {
            ctx.beginPath()
            ctx.moveTo(w * i / gridDivisions, 0)
            ctx.lineTo(w * i / gridDivisions, h)
            ctx.moveTo(0, h * i / gridDivisions)
            ctx.lineTo(w, h * i / gridDivisions)
            ctx.stroke()
        }

        if (cursor) {
            for (const n of near) {
                const g = ctx.createLinearGradient(cursor[0], cursor[1], n.x, n.y)
                g.addColorStop(0, theme.constellationLinkInk)
                g.addColorStop(1, rgba(colourOf(n.category), 0))
                ctx.strokeStyle = g
                ctx.lineWidth = lineWidth + n.weight * lineWeightWidth
                ctx.beginPath()
                ctx.moveTo(cursor[0], cursor[1])
                ctx.lineTo(n.x, n.y)
                ctx.stroke()
            }
        }

        const selected = constellation ? constellation.selected : ""
        let drawn = 0
        let labelled = 0
        allStars.forEach((s, i) => {
            const p = skyPosition(s, w, h)
            const wave = 0.5 + 0.5 * Math.sin(clock * twinkleRate + i * twinklePhase)
            // Moving sounds twinkle deeper.
            const twinkle = twinkleFloor + (1 - twinkleFloor) * (1 - twinkleMovement + twinkleMovement * s.movement) * wave
            const colour = colourOf(s.category)
            const r = s.variation ? variationRadius : starRadius + s.width * starWidthRadius
            const glow = ctx.createRadialGradient(p[0], p[1], 0, p[0], p[1], r * glowScale)
            glow.addColorStop(0, rgba(colour, 1))
            glow.addColorStop(1, rgba(colour, 0))
            ctx.globalAlpha = glowAlpha * twinkle
            ctx.fillStyle = glow
            ctx.beginPath()
            ctx.arc(p[0], p[1], r * glowScale, 0, 2 * Math.PI)
            ctx.fill()
            ctx.globalAlpha = 1
            ctx.fillStyle = s.variation ? cursorInk : colour
            ctx.beginPath()
            ctx.arc(p[0], p[1], r, 0, 2 * Math.PI)
            ctx.fill()
            ++drawn
            const isSelected = s.key === selected
            if (isSelected || s.variation || near.some(n => n.key === s.key) || w > labelAllWidth) {
                ctx.font = "500 " + (isSelected ? labelSelectedSize : labelSize) + "px \"" + theme.mono + "\""
                ctx.fillStyle = isSelected ? cursorInk : rgba(String(theme.text), theme.constellationLabelAlpha)
                // Names that would run off the right edge sit left of their star.
                const labelWidth = ctx.measureText(s.name).width
                const right = p[0] + r + labelGap + labelWidth <= w
                ctx.fillText(s.name, right ? p[0] + r + labelGap : p[0] - r - labelGap - labelWidth,
                             p[1] + labelSize / labelDrop)
                ++labelled
            }
        })

        if (cursor) {
            const pulse = cursorRing + cursorPulse * Math.sin(clock * cursorPulseRate)
            ctx.strokeStyle = cursorInk
            ctx.lineWidth = theme.constellationCursorStroke
            ctx.beginPath()
            ctx.arc(cursor[0], cursor[1], pulse, 0, 2 * Math.PI)
            ctx.stroke()
            const g = ctx.createRadialGradient(cursor[0], cursor[1], 0, cursor[0], cursor[1], cursorGlow)
            g.addColorStop(0, theme.constellationCursorGlowInk)
            g.addColorStop(1, rgba(String(theme.vi), 0))
            ctx.fillStyle = g
            ctx.beginPath()
            ctx.arc(cursor[0], cursor[1], cursorGlow, 0, 2 * Math.PI)
            ctx.fill()
        }
        drawnStars = drawn
        labelledStars = labelled
    }

    Component.onCompleted: if (constellation && visible) constellation.start()
    onVisibleChanged: if (constellation && visible) constellation.start()
    onAllStarsChanged: sky.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: view.theme.bg
    }

    Canvas {
        id: sky
        objectName: "novaConstellationSky"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: bar.top
        renderStrategy: Canvas.Cooperative
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: view.paintSky(getContext("2d"))

        MouseArea {
            objectName: "novaConstellationArea"
            anchors.fill: parent
            enabled: view.constellation !== null && view.allStars.length > 0
            cursorShape: Qt.CrossCursor
            onPressed: mouse => {
                const hit = view.starAt(mouse.x, mouse.y)
                if (hit) {
                    view.constellation.apply_star(hit.key)
                    view.cursor = null
                    view.near = []
                    sky.requestPaint()
                    return
                }
                view.dragging = true
                view.constellation.begin_morph()
                view.morphTo(mouse.x, mouse.y)
            }
            onPositionChanged: mouse => {
                if (view.dragging)
                    view.morphTo(Math.max(0, Math.min(sky.width, mouse.x)), Math.max(0, Math.min(sky.height, mouse.y)))
            }
            onReleased: {
                if (!view.dragging)
                    return
                view.dragging = false
                view.constellation.end_morph()
            }
            onCanceled: {
                if (!view.dragging)
                    return
                view.dragging = false
                view.constellation.end_morph()
            }
        }
    }

    component Axis: Text {
        color: view.theme.faint
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeCaption
        font.weight: Font.DemiBold
        font.letterSpacing: view.theme.constellationAxisSpacing
    }
    Axis {
        objectName: "novaConstellationLengthAxis"
        text: "SHORT ⟵ ENVELOPE ⟶ LONG"
        rotation: -90
        x: view.theme.spaceLg - width / 2 + height / 2
        y: sky.height / 2 - height / 2
    }
    Axis {
        objectName: "novaConstellationBrightnessAxis"
        text: "DARK ⟵ BRIGHTNESS ⟶ BRIGHT"
        anchors.horizontalCenter: sky.horizontalCenter
        y: sky.height - height - view.theme.constellationAxisInset
    }

    // Listening: until every preset has been measured once.
    Column {
        objectName: "novaConstellationProgress"
        visible: view.constellation !== null && !view.constellation.ready
        anchors.centerIn: sky
        spacing: view.theme.spaceMd
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: view.theme.text
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeLabel
            font.letterSpacing: view.theme.constellationProgressSpacing
            text: view.constellation ? "LISTENING TO PRESETS… " + view.constellation.analysed + "/" + view.constellation.total : ""
        }
        Rectangle {
            width: view.theme.constellationProgressWidth
            height: view.theme.constellationProgressHeight
            radius: height / 2
            color: view.theme.line
            Rectangle {
                width: view.constellation && view.constellation.total > 0
                       ? parent.width * view.constellation.analysed / view.constellation.total : 0
                height: parent.height
                radius: parent.radius
                color: view.theme.cy
            }
        }
    }

    component Chip: Rectangle {
        id: chip
        property string label: ""
        property bool active: false
        property color tone: view.theme.am
        signal clicked()
        implicitWidth: caption.implicitWidth + view.theme.spaceXl
        implicitHeight: view.theme.constellationChipHeight
        radius: view.theme.radiusMd
        color: active ? view.theme.soft(tone, view.theme.constellationChipFillAlpha) : "transparent"
        border.color: active ? tone : view.theme.line
        Text {
            id: caption
            anchors.centerIn: parent
            text: chip.label
            color: chip.active ? chip.tone : view.theme.dim
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeCaption
            font.weight: Font.DemiBold
            font.letterSpacing: view.theme.constellationChipSpacing
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }
    component Label: Text {
        color: view.theme.faint
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeCaption
        font.weight: Font.DemiBold
        font.letterSpacing: view.theme.constellationLabelSpacing
    }

    Rectangle {
        id: bar
        objectName: "novaConstellationBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: view.barHeight
        color: view.theme.soft(view.theme.rail, view.theme.constellationBarAlpha)
        Rectangle {
            width: parent.width
            height: view.theme.hairline
            color: view.theme.line
        }
        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: view.theme.spaceLg
            anchors.rightMargin: view.theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: view.theme.constellationBarSpacing
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: "MORPHING"
            }
            Text {
                objectName: "novaConstellationMix"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(view.mixMinimumWidth, parent.width - locksRow.width - 3 * parent.spacing - view.mixReserve)
                elide: Text.ElideRight
                textFormat: Text.StyledText
                color: view.theme.dim
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeSmall
                text: view.toast !== "" ? view.toast
                      : view.constellation && view.constellation.message !== "" ? view.constellation.message
                      : view.mixText()
            }
            Row {
                id: locksRow
                anchors.verticalCenter: parent.verticalCenter
                spacing: view.theme.spaceSm
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "KEEP MINE"
                }
                Repeater {
                    model: [{ lock: "fx", label: "FX" }, { lock: "env", label: "ENVELOPES" },
                            { lock: "mod", label: "MOD ROUTES" }]
                    delegate: Chip {
                        required property var modelData
                        objectName: "novaConstellationLock:" + modelData.lock
                        label: modelData.label
                        active: view.constellation !== null && view.constellation.locks.indexOf(modelData.lock) >= 0
                        onClicked: view.constellation.set_lock(modelData.lock, !active)
                    }
                }
                Chip {
                    objectName: "novaConstellationMutate"
                    label: view.constellation && view.constellation.mutating ? "⟡ LISTENING…" : "⟡ MUTATE"
                    tone: view.theme.vi
                    active: view.constellation !== null && view.constellation.mutating
                    onClicked: {
                        view.constellation.mutate()
                        view.toast = "Six variations around your sound. Click one to hear it."
                        toastTimer.restart()
                    }
                }
                Chip {
                    objectName: "novaConstellationKeep"
                    label: "★ KEEP THIS"
                    tone: view.theme.cy
                    onClicked: {
                        const name = view.constellation.keep("")
                        if (name !== "") {
                            view.toast = "★ Saved “" + name + "” as a user preset; it becomes a star."
                            toastTimer.restart()
                        }
                    }
                }
            }
        }
    }

    Timer {
        id: toastTimer
        interval: view.toastMs
        onTriggered: view.toast = ""
    }

    // The sky twinkles only while it shows.
    Timer {
        objectName: "novaConstellationClock"
        interval: view.frameMs
        repeat: true
        running: view.visible
        onTriggered: {
            view.clock += interval / 1000
            sky.requestPaint()
        }
    }

    Connections {
        target: view.constellation
        function onSelected_changed() { sky.requestPaint() }
    }
}
