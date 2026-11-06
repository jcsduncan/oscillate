pragma ComponentBehavior: Bound
import QtQuick

// The instrument seen from above with the lid up: 88 strings over their
// dampers and keys, the undamped treble bracketed, and the three pedals.
// While the synth plays (ADR-0037) each string glows with its live energy
// (sympathetic resonance included) and the dampers rise as they lift;
// otherwise it previews how long each key's samples ring.
Item {
    id: view
    required property ConcertTheme theme
    // NativeSynthDeviceController.concert_library(): the keyboard's range,
    // where the dampers end and each key's ring time.
    property var library: ({})
    property bool live: false
    property var energies: []   // 88 x log10 energy (live).
    property var dampers: []    // 88 x 0 lifted .. 1 resting (live).
    property var pedals: ({ sustain: 0, sostenuto: 0, soft: 0 })
    readonly property var sustain: library.ring_seconds !== undefined ? library.ring_seconds : []
    readonly property int firstKey: library.first_key !== undefined ? library.first_key : 0
    readonly property int keyCount: library.key_count !== undefined ? library.key_count : 0
    // Without a library every key is drawn damped.
    readonly property int lastDampedKey: library.last_damped_key > 0
        ? library.last_damped_key : firstKey + keyCount - 1
    readonly property int whiteCount: whiteIndex(firstKey + keyCount)
    readonly property real whiteWidth: width / whiteCount
    readonly property real keysHeight: 58
    readonly property real pedalHeight: 34
    readonly property real stringTop: 16
    // The gap between the strings' foot and the keys (the damper bracket's label).
    readonly property real stringFloorGap: 14
    readonly property real stringBottom: height - keysHeight - pedalHeight - stringFloorGap
    // Energy (log10) below which a string counts as silent.
    readonly property real quietEnergy: -6
    // The preview's glow: a floor plus a span scaled by ring time up to
    // longestRing seconds.
    readonly property real previewGlowFloor: 0.18
    readonly property real previewGlowSpan: 0.32
    readonly property real longestRing: 60
    // How many keys ring audibly and how many dampers are up, for the
    // description and for tests.
    readonly property int ringing: {
        let count = 0
        if (live)
            for (let i = 0; i < energies.length; ++i)
                count += energies[i] > quietEnergy ? 1 : 0
        return count
    }
    readonly property int lifted: {
        let count = 0
        if (live)
            for (let i = 0; i < dampers.length && i <= lastDampedKey - firstKey; ++i)
                count += dampers[i] < 1 ? 1 : 0
        return count
    }
    objectName: "concertStringsView"
    Accessible.role: Accessible.Graphic
    Accessible.name: "Strings, dampers and pedals"
    Accessible.description: live
        ? ringing + " keys ringing, " + lifted + " dampers lifted"
          + (pedals.sustain >= 0.5 ? ", sustain down" : "")
          + (pedals.sostenuto >= 0.5 ? ", sostenuto down" : "")
          + (pedals.soft >= 0.5 ? ", una corda" : "")
        : "Preview: how long each key's samples ring"

    function isBlack(key) {
        const pc = key % 12
        return pc === 1 || pc === 3 || pc === 6 || pc === 8 || pc === 10
    }
    // Index of the white key at or below `key`.
    function whiteIndex(key) {
        let count = 0
        for (let k = firstKey; k < key; ++k)
            count += isBlack(k) ? 0 : 1
        return count
    }
    function keyCentre(key) {
        const w = whiteIndex(key)
        return isBlack(key) ? w * whiteWidth : (w + 0.5) * whiteWidth
    }
    // A string's glow 0..1: live energy (log10, -6..0), or the preview of
    // how long its samples ring.
    function glow(index) {
        if (live)
            return energies.length > index
                ? Math.max(0, Math.min(1, (energies[index] - quietEnergy) / -quietEnergy)) : 0
        if (sustain.length <= index)
            return 0
        return previewGlowFloor + previewGlowSpan
            * Math.max(0, Math.min(1, Math.log(sustain[index]) / Math.log(longestRing)))
    }

    Canvas {
        id: canvas
        // The drawing's proportions (pixels unless noted).
        // The soundboard's reach above and below the strings.
        readonly property real boardAbove: 6
        readonly property real boardBelow: 10
        // The bridge: its rise above the strings, depth and brass cap.
        readonly property real bridgeRise: 2
        readonly property real bridgeDepth: 3
        readonly property real bridgeCap: 1
        // Keys below this MIDI note have wound strings.
        readonly property int woundBelow: 48
        // The smallest glow drawn, and the lit run and strength as fractions.
        readonly property real glowThreshold: 0.01
        readonly property real previewRunFloor: 0.25
        readonly property real previewRunSpan: 0.75
        readonly property real liveStrengthFloor: 0.3
        readonly property real liveStrengthSpan: 0.7
        // Dampers: how far they rise, the wire and the felt block.
        readonly property real damperLift: 12
        readonly property real wireTop: 16
        readonly property real wireLength: 7
        readonly property real feltTop: 9
        readonly property real feltWidth: 6
        readonly property real feltHeight: 8
        readonly property real feltSheenHeight: 1.5
        // The undamped bracket: its lead-in, right inset and leg height.
        readonly property real bracketLead: 3
        readonly property real bracketRight: 2
        readonly property real bracketLeg: 4
        // The sharps' width and length as fractions of the naturals', the
        // gap between naturals and the keyslip's depth.
        readonly property real sharpWidth: 0.62
        readonly property real sharpLength: 0.62
        readonly property real keyGap: 1
        readonly property real keyslipDepth: 3
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const keysY = height - view.keysHeight - view.pedalHeight
            const top = view.stringTop
            const bottom = view.stringBottom
            // Soundboard under the strings: dark spruce glimpsed through.
            const board = ctx.createLinearGradient(0, top, 0, bottom + boardBelow)
            board.addColorStop(0, t.soundboardTop)
            board.addColorStop(1, t.soundboardBottom)
            ctx.fillStyle = board
            ctx.fillRect(0, top - boardAbove, width, bottom - top + boardAbove + boardBelow)
            // The bridge: a brass-capped rail the strings cross.
            ctx.fillStyle = t.brassLo
            ctx.fillRect(0, top - bridgeRise, width, bridgeDepth)
            ctx.fillStyle = t.brass
            ctx.fillRect(0, top - bridgeRise, width, bridgeCap)
            // Strings.
            for (let i = 0; i < view.keyCount; ++i) {
                const key = view.firstKey + i
                const x = Math.round(view.keyCentre(key)) + t.hairline / 2
                const g = view.glow(i)
                // Bass strings are wound (thicker, copper); treble plain steel.
                const wound = key < woundBelow
                ctx.strokeStyle = wound ? t.woundString : t.plainString
                ctx.lineWidth = wound ? t.woundStringWidth : t.hairline
                ctx.beginPath()
                ctx.moveTo(x, top)
                ctx.lineTo(x, bottom)
                ctx.stroke()
                if (g > glowThreshold) {
                    // Live: the whole string lights with its energy, haloed.
                    // Preview: a gold run as long as the register rings.
                    const length = (bottom - top) * (view.live ? 1 : previewRunFloor + previewRunSpan * g)
                    const strength = view.live ? liveStrengthFloor + liveStrengthSpan * g : g
                    ctx.strokeStyle = Qt.rgba(t.gold.r, t.gold.g, t.gold.b, t.stringHaloAlpha * strength)
                    ctx.lineWidth = t.stringHaloWidth + t.stringHaloSpread * g
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, top + length)
                    ctx.stroke()
                    ctx.strokeStyle = Qt.rgba(t.gold.r, t.gold.g, t.gold.b, Math.min(1, strength))
                    ctx.lineWidth = t.hairline + t.stringLitSpread * g
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, top + length)
                    ctx.stroke()
                }
            }
            // Dampers: felt blocks resting on the strings, raised when lifted.
            for (let i = 0; i < view.keyCount; ++i) {
                const key = view.firstKey + i
                if (key > view.lastDampedKey)
                    break
                const x = view.keyCentre(key)
                const contact = view.live && view.dampers.length > i ? view.dampers[i] : 1
                const lift = (1 - contact) * damperLift
                // The wire, then the felt: resting dark on the string, or
                // raised clear and lit when lifted.
                ctx.fillStyle = t.brassLo
                ctx.fillRect(x - t.hairline / 2, bottom - wireTop - lift, t.hairline, wireLength)
                ctx.fillStyle = contact < 1 ? t.feltLifted : t.felt
                ctx.fillRect(x - feltWidth / 2, bottom - feltTop - lift, feltWidth, feltHeight)
                if (contact < 1) {
                    ctx.fillStyle = t.feltSheen
                    ctx.fillRect(x - feltWidth / 2, bottom - feltTop - lift, feltWidth, feltSheenHeight)
                }
            }
            // The undamped treble, bracketed.
            const from = view.keyCentre(view.lastDampedKey + 1) - bracketLead
            const to = width - bracketRight
            ctx.strokeStyle = t.goldDim
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(from, bottom - bracketLeg)
            ctx.lineTo(from, bottom)
            ctx.lineTo(to, bottom)
            ctx.lineTo(to, bottom - bracketLeg)
            ctx.stroke()
            // Keys.
            const blackWidth = view.whiteWidth * sharpWidth
            let w = 0
            for (let key = view.firstKey; key < view.firstKey + view.keyCount; ++key) {
                if (view.isBlack(key))
                    continue
                const x = w * view.whiteWidth
                const shade = ctx.createLinearGradient(0, keysY, 0, keysY + view.keysHeight)
                shade.addColorStop(0, t.ivory)
                shade.addColorStop(1, t.ivoryShade)
                ctx.fillStyle = shade
                ctx.fillRect(x + keyGap / 2, keysY, view.whiteWidth - keyGap, view.keysHeight)
                ++w
            }
            for (let key = view.firstKey; key < view.firstKey + view.keyCount; ++key) {
                if (!view.isBlack(key))
                    continue
                const x = view.keyCentre(key) - blackWidth / 2
                const shade = ctx.createLinearGradient(0, keysY, 0, keysY + view.keysHeight * sharpLength)
                shade.addColorStop(0, t.ebonyHi)
                shade.addColorStop(1, t.ebony)
                ctx.fillStyle = shade
                ctx.fillRect(x, keysY, blackWidth, view.keysHeight * sharpLength)
            }
            // The keyslip: a brass strip in front of the keys.
            ctx.fillStyle = t.brassLo
            ctx.fillRect(0, keysY - keyslipDepth, width, keyslipDepth)
        }
    }
    onEnergiesChanged: canvas.requestPaint()
    onDampersChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onSustainChanged: canvas.requestPaint()
    onLastDampedKeyChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()

    Text {
        x: view.keyCentre(view.lastDampedKey + 1)
        y: view.stringBottom + view.theme.spaceXxs
        text: "UNDAMPED"
        color: view.theme.goldDim
        font.family: view.theme.serif
        font.pixelSize: view.theme.typeMicro
        font.letterSpacing: view.theme.stringsLabelSpacing
    }
    Text {
        objectName: "concertStringsCaption"
        x: view.theme.spaceXs
        y: 0
        text: view.live ? "LIVE · " + view.ringing + (view.ringing === 1 ? " KEY RINGING" : " KEYS RINGING")
                        : "RING PREVIEW · NOT LIVE"
        color: view.live ? view.theme.gold : view.theme.inkFaint
        font.pixelSize: view.theme.typeMicro
        font.bold: true
        font.letterSpacing: view.theme.stringsCaptionSpacing
    }

    // The lyre: una corda, sostenuto and sustain, pressed as far as the
    // pedals are down.
    Row {
        id: lyre
        anchors.horizontalCenter: parent.horizontalCenter
        y: view.height - view.pedalHeight + view.theme.spaceXs
        spacing: view.theme.pedalSpacing
        Repeater {
            model: [{ id: "soft", name: "Una corda" }, { id: "sostenuto", name: "Sostenuto" },
                    { id: "sustain", name: "Sustain" }]
            delegate: Item {
                id: pedal
                required property var modelData
                readonly property real depth: view.live && view.pedals[modelData.id] !== undefined
                    ? view.pedals[modelData.id] : 0
                objectName: "concertPedal:" + modelData.id
                width: view.theme.pedalWidth
                height: view.pedalHeight - view.theme.pedalBottomGap
                Accessible.role: Accessible.Indicator
                Accessible.name: modelData.name + " pedal"
                Accessible.description: depth >= 0.5 ? "Down" : depth > 0 ? "Half" : "Up"
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: view.theme.pedalTravel * pedal.depth
                    width: view.theme.pedalPlateWidth
                    height: view.theme.pedalPlateHeight
                    radius: view.theme.pedalPlateRadius
                    gradient: Gradient {
                        GradientStop { position: 0; color: view.theme.brassHi }
                        GradientStop { position: 1; color: view.theme.brassLo }
                    }
                    border.color: pedal.depth >= 0.5 ? view.theme.gold : view.theme.brassLo
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -view.theme.pedalHaloSpread
                        radius: view.theme.radiusXl
                        color: "transparent"
                        border.width: view.theme.strokeBold
                        border.color: Qt.rgba(view.theme.gold.r, view.theme.gold.g, view.theme.gold.b,
                                              view.theme.pedalHaloAlpha * pedal.depth)
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: view.theme.pedalLabelY
                    text: pedal.modelData.name.toUpperCase()
                    color: pedal.depth > 0 ? view.theme.gold : view.theme.inkFaint
                    font.pixelSize: view.theme.pedalLabelSize
                    font.bold: true
                    font.letterSpacing: view.theme.captionSpacing
                }
            }
        }
    }
}
