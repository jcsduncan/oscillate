pragma ComponentBehavior: Bound
import QtQuick
import "shell"
import "devicekit" as Kit

// A track's MIDI effects, between its macros and its synth (ADR-0059): an
// arp, chord, note echo, random or scale card for each, in the order the
// notes pass through them. Each card switches on and off, steps presets,
// moves left or right and turns its knobs through the device kit, so every
// change is one undo step and is heard live. "+ MIDI FX" adds one.
Rectangle {
    id: strip
    required property var bridge
    required property string trackId
    required property color lineColor
    required property color textColor
    required property color dimColor
    readonly property var effects: bridge.noteEffects
    readonly property color accent: Theme.noteFxAccent
    readonly property var chain: {
        bridge.revision
        return effects && trackId !== "" ? effects.chain(trackId) : []
    }
    readonly property bool accepts: {
        bridge.revision
        return effects !== null && trackId !== "" && effects.accepts(trackId)
    }
    property bool choosing: false
    onChoosingChanged: if (choosing && Motion.enabled) chooserReveal.restart()
    // Cards are rebuilt with the chain on every revision, so arrivals and
    // moves are worked out here: slot id -> its previous index, or -1 for a
    // new card. Another track, or the first look at one, animates nothing.
    property var cardIndexes: ({})
    property string indexedTrack: ""
    property var cardMoves: ({})
    onChainChanged: {
        const next = {}
        const moves = {}
        const sameTrack = indexedTrack === trackId
        chain.forEach((effect, index) => {
            next[effect.slotId] = index
            const before = cardIndexes[effect.slotId]
            if (!sameTrack)
                return
            if (before === undefined)
                moves[effect.slotId] = -1
            else if (before !== index)
                moves[effect.slotId] = before
        })
        cardIndexes = next
        indexedTrack = trackId
        cardMoves = moves
    }

    objectName: "noteEffectStrip"
    visible: accepts
    width: accepts ? cards.width + addTab.width + Theme.spaceLg : 0
    color: Theme.noteFxGround
    // The engine list may reach over the synth's strip while choosing.
    clip: !choosing
    z: choosing ? 3 : 0
    Behavior on width { enabled: Motion.enabled; NumberAnimation { duration: Motion.normal; easing.type: Easing.OutCubic } }
    Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: strip.lineColor }

    Kit.KitTheme {
        id: fxTheme
        prefix: "noteFx"
        accent: strip.accent
        knobAccent: strip.accent
        knobDiameter: Theme.noteFxKnobDiameter
        knobMinWidth: Theme.noteFxKnobMinWidth
        knobWidthPad: Theme.noteFxKnobWidthPad
        knobLabelSize: Theme.typeCaption
        knobValueSize: Theme.typeSmall
        knobStacked: true
        knobStackedValueSize: Theme.typeSmall
        knobValueIdleInk: strip.textColor
        ink: strip.textColor
        inkDim: strip.dimColor
        well: Theme.noteFxWell
    }

    // The knobs each card shows first; the rest are one click away.
    readonly property var primary: ({
        arp: ["arp.mode", "arp.rate", "arp.gate", "arp.octaves"],
        chord: ["chord.mode", "chord.shift1", "chord.shift2", "chord.shift3"],
        echo: ["echo.time", "echo.repeats", "echo.decay", "echo.pitch"],
        random: ["random.chance", "random.range", "random.velocity", "random.timing"],
        scale: ["scale.root", "scale.scale", "scale.snap", "scale.transpose"],
        comp: ["comp.push_chance", "comp.groove", "comp.humanise", "comp.push_amount"]
    })
    readonly property var secondary: ({
        arp: ["arp.swing", "arp.steps", "arp.accent", "arp.latch",
              "arp.velocity", "arp.level", "arp.seed"],
        chord: ["chord.shift4", "chord.shift5", "chord.shift6", "chord.velocity",
                "chord.shape", "chord.inversion", "chord.root", "chord.scale"],
        echo: [],
        random: ["random.root", "random.scale", "random.seed"],
        scale: ["scale.octave"],
        comp: ["comp.octave", "comp.velocity", "comp.seed"]
    })

    Row {
        id: cards
        x: Theme.spaceMd; y: Theme.spaceMd
        height: parent.height - 2 * Theme.spaceMd
        spacing: Theme.spaceMd
        Repeater {
            model: strip.chain
            delegate: Rectangle {
                id: card
                required property var modelData
                required property int index
                readonly property string slotId: modelData.slotId
                readonly property string engineId: modelData.engineId
                readonly property bool enabled_: modelData.enabled
                property var controller: null
                property string heldSlot: ""
                property bool more: false
                readonly property var shown: more ? (strip.secondary[engineId] || [])
                                                  : (strip.primary[engineId] || [])
                function acquire() {
                    release()
                    controller = strip.effects.controller(strip.trackId, slotId)
                    heldSlot = controller ? slotId : ""
                }
                function release() {
                    if (heldSlot !== "")
                        strip.effects.release(strip.trackId, heldSlot)
                    heldSlot = ""
                    controller = null
                }
                onSlotIdChanged: acquire()
                Component.onCompleted: {
                    acquire()
                    Qt.callLater(() => card.animateArrival())
                }
                // A new card pops in; a moved one slides from its old place.
                transform: Translate { id: cardShift }
                function animateArrival() {
                    const from = strip.cardMoves[card.slotId]
                    if (from === undefined || !Motion.enabled)
                        return
                    if (from < 0) {
                        cardPop.restart()
                    } else {
                        cardShift.x = (from - card.index) * (card.width + cards.spacing)
                        cardSlide.restart()
                    }
                }
                NumberAnimation {
                    id: cardSlide
                    target: cardShift; property: "x"; to: 0
                    duration: Motion.normal; easing.type: Easing.OutCubic
                }
                ParallelAnimation {
                    id: cardPop
                    NumberAnimation { target: card; property: "scale"; from: Theme.noteFxCardPopScale; to: 1
                                      duration: Motion.normal; easing.type: Easing.OutBack }
                    NumberAnimation { target: cardShift; property: "y"; from: Theme.noteFxCardPopDrop; to: 0
                                      duration: Motion.normal; easing.type: Easing.OutCubic }
                }
                // The phrase drawing settles into its new shape.
                function redraw() {
                    glyph.requestPaint()
                    if (Motion.enabled)
                        glyphFade.restart()
                }
                Component.onDestruction: release()
                Connections {
                    target: card.controller
                    function onValues_changed() { card.redraw() }
                }
                Connections {
                    target: strip.bridge
                    function onRevisionChanged() {
                        if (card.controller && card.controller.gesture_parameter === "")
                            card.controller.refresh()
                        glyph.requestPaint()
                    }
                }

                objectName: "noteEffectCard:" + engineId
                width: Theme.noteFxCardWidth
                height: parent.height
                radius: Theme.radiusXl
                color: Theme.noteFxCardGround
                border.color: activeFocus ? strip.accent : Theme.noteFxCardEdge
                opacity: enabled_ ? 1 : Theme.noteFxBypassedOpacity
                Behavior on opacity { enabled: Motion.enabled; NumberAnimation { duration: Motion.fast } }

                // Power · name · move · remove
                Item {
                    id: head
                    x: Theme.spaceMd; y: Theme.spaceSm; width: parent.width - 2 * Theme.spaceMd; height: Theme.noteFxHeadHeight
                    Rectangle {
                        id: power
                        objectName: "noteEffectPower:" + card.engineId
                        width: Theme.noteFxPowerSize; height: Theme.noteFxPowerSize; radius: width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: card.enabled_ ? strip.accent : "transparent"
                        border.color: card.enabled_ ? strip.accent : strip.dimColor
                        activeFocusOnTab: true
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: card.modelData.name + (card.enabled_ ? " on" : " bypassed")
                        Keys.onSpacePressed: strip.effects.set_enabled(strip.trackId, card.slotId, !card.enabled_)
                        Kit.Tip {
                            theme: fxTheme
                            shown: powerArea.containsMouse
                            text: card.modelData.name + "  " + (card.enabled_ ? "On" : "Bypassed")
                            detail: card.enabled_ ? "Click to bypass: notes pass through untouched"
                                                  : "Click to switch it back on"
                        }
                        MouseArea {
                            id: powerArea
                            anchors.fill: parent; anchors.margins: -Theme.spaceXs
                            hoverEnabled: true
                            onClicked: strip.effects.set_enabled(strip.trackId, card.slotId, !card.enabled_)
                        }
                    }
                    Text {
                        x: power.width + Theme.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x - tools.width - Theme.spaceXs
                        elide: Text.ElideRight
                        text: card.modelData.name.toUpperCase()
                        color: strip.textColor
                        font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.noteFxNameLetterSpacing
                    }
                    Row {
                        id: tools
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceXxs
                        Repeater {
                            model: [
                                {glyph: "‹", tip: "Move earlier", objectName: "noteEffectLeft"},
                                {glyph: "›", tip: "Move later", objectName: "noteEffectRight"},
                                {glyph: "×", tip: "Remove", objectName: "noteEffectRemove"}
                            ]
                            delegate: Rectangle {
                                id: tool
                                required property var modelData
                                required property int index
                                objectName: modelData.objectName + ":" + card.engineId
                                width: Theme.noteFxToolSize; height: Theme.noteFxToolSize; radius: Theme.radiusMd
                                color: toolArea.containsMouse ? Theme.noteFxToolHover : "transparent"
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: modelData.tip + " " + card.modelData.name
                                function run() {
                                    if (index === 0 && card.index > 0)
                                        strip.effects.move(strip.trackId, card.slotId, card.index - 1)
                                    else if (index === 1 && card.index < strip.chain.length - 1)
                                        strip.effects.move(strip.trackId, card.slotId, card.index + 1)
                                    else if (index === 2)
                                        strip.effects.remove(strip.trackId, card.slotId)
                                }
                                Keys.onReturnPressed: run()
                                Keys.onSpacePressed: run()
                                Text {
                                    anchors.centerIn: parent
                                    text: tool.modelData.glyph
                                    color: strip.dimColor; font.pixelSize: Theme.noteFxToolGlyphSize
                                }
                                Kit.Tip {
                                    theme: fxTheme
                                    shown: toolArea.containsMouse
                                    text: tool.modelData.tip
                                    detail: tool.index === 2 ? "Undo brings it back"
                                        : "MIDI effects play left to right, into the synth"
                                }
                                MouseArea {
                                    id: toolArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: tool.run()
                                }
                            }
                        }
                    }
                }

                // Preset: name with steppers.
                Item {
                    id: presetRow
                    x: Theme.spaceMd; y: head.y + head.height + Theme.spaceXxs; width: parent.width - 2 * Theme.spaceMd; height: Theme.noteFxPresetRowHeight
                    Text {
                        anchors.left: parent.left; anchors.right: presetArrows.left
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: card.controller && card.controller.revision >= 0
                            ? (card.controller.preset_modified ? "* " : "") + card.controller.preset_name : ""
                        color: strip.accent; font.pixelSize: Theme.typeBody
                    }
                    Row {
                        id: presetArrows
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceXxs
                        Repeater {
                            model: [-1, 1]
                            delegate: Rectangle {
                                id: arrow
                                required property int modelData
                                objectName: (modelData < 0 ? "noteEffectPresetPrevious:" : "noteEffectPresetNext:")
                                            + card.engineId
                                width: Theme.noteFxToolSize; height: Theme.noteFxToolSize; radius: Theme.radiusMd
                                color: arrowArea.containsMouse ? Theme.noteFxToolHover : "transparent"
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: (modelData < 0 ? "Previous" : "Next") + " preset"
                                Keys.onReturnPressed: if (card.controller) card.controller.step_preset(modelData)
                                Text {
                                    anchors.centerIn: parent
                                    text: arrow.modelData < 0 ? "▴" : "▾"
                                    color: strip.dimColor; font.pixelSize: Theme.typeSmall
                                }
                                Kit.Tip {
                                    theme: fxTheme
                                    shown: arrowArea.containsMouse
                                    text: (arrow.modelData < 0 ? "Previous" : "Next") + " preset"
                                }
                                MouseArea {
                                    id: arrowArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: if (card.controller) card.controller.step_preset(arrow.modelData)
                                }
                            }
                        }
                    }
                }

                // What the effect does to a phrase, drawn from its values.
                Rectangle {
                    x: Theme.spaceMd; y: presetRow.y + presetRow.height + Theme.spaceXs
                    width: parent.width - 2 * Theme.spaceMd; height: Theme.noteFxGlyphHeight
                    radius: Theme.radiusLg
                    color: Theme.noteFxWell
                    Canvas {
                        id: glyph
                        anchors.fill: parent
                        anchors.margins: Theme.noteFxGlyphInset
                        // Drawing geometry: phraseSteps cells; bars barHeight tall; pitches
                        // within ±semitoneRange; the echo's notes echoBarHeight tall and
                        // echoBarFill of a step wide; scale keys as fractions of the height.
                        readonly property int phraseSteps: 16
                        readonly property real barHeight: 3
                        readonly property real semitoneRange: 24
                        readonly property real dotRadius: 4
                        readonly property real echoBarHeight: 4
                        readonly property real echoBarFill: 0.6
                        readonly property real echoTail: 1.5
                        readonly property real lateShare: 0.8
                        readonly property real blackKeyTop: 2
                        readonly property real whiteKeyTop: 0.35
                        readonly property real blackKeyLength: 0.55
                        readonly property real whiteKeyLength: 0.6
                        NumberAnimation {
                            id: glyphFade
                            target: glyph; property: "opacity"; from: Theme.noteFxGlyphFadeFrom; to: 1
                            duration: Motion.fast; easing.type: Easing.OutQuad
                        }
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const c = card.controller
                            if (!c)
                                return
                            const w = width
                            const h = height
                            const ink = strip.accent
                            ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, Theme.noteFxGlyphInkAlpha)
                            ctx.strokeStyle = Qt.rgba(ink.r, ink.g, ink.b, Theme.noteFxGlyphInkAlpha)
                            const value = id => Number(c.value(id))
                            if (card.engineId === "arp") {
                                // Sixteen steps of the pattern over a C minor chord.
                                const notes = [0, 3, 7]
                                const octaves = value("arp.octaves")
                                const mode = value("arp.mode")
                                const run = []
                                for (let o = 0; o < octaves; ++o)
                                    for (const n of notes) run.push(n + 12 * o)
                                const top = run[run.length - 1] || 1
                                const gate = Math.min(1, value("arp.gate"))
                                const cell = w / phraseSteps
                                for (let i = 0; i < phraseSteps; ++i) {
                                    let pitch = run[i % run.length]
                                    if (mode === 1) pitch = run[run.length - 1 - i % run.length]
                                    else if (mode === 2 || mode === 3) {
                                        const cycle = Math.max(1, 2 * run.length - 2)
                                        let k = i % cycle
                                        if (mode === 3) k = (k + run.length - 1) % cycle
                                        pitch = run[k < run.length ? k : cycle - k]
                                    } else if (mode === 5) pitch = run[(i * 7 + 3) % run.length]
                                    if (mode === 6) {
                                        for (const n of run.slice(0, 3))
                                            ctx.fillRect(i * cell + Theme.hairline, h - barHeight - Theme.hairline - n / Math.max(1, top) * (h - 2 * barHeight), cell * gate - Theme.hairline, barHeight)
                                    } else {
                                        ctx.fillRect(i * cell + Theme.hairline, h - barHeight - Theme.hairline - pitch / Math.max(1, top) * (h - 2 * barHeight),
                                                     Math.max(Theme.hairline, cell * gate - Theme.hairline), barHeight)
                                    }
                                }
                            } else if (card.engineId === "chord") {
                                // The chord built on C, as dots on a pitch line.
                                const shifts = [0]
                                if (value("chord.mode") === 0) {
                                    for (let s = 1; s <= 6; ++s) {
                                        const shift = value("chord.shift" + s)
                                        if (shift !== 0) shifts.push(shift)
                                    }
                                } else {
                                    shifts.push(3, 7)
                                }
                                for (const s of shifts) {
                                    const x = (s + semitoneRange) / (2 * semitoneRange) * w
                                    ctx.beginPath()
                                    ctx.arc(x, h / 2, dotRadius, 0, Math.PI * 2)
                                    ctx.fill()
                                }
                                ctx.globalAlpha = Theme.noteFxGlyphGuideAlpha
                                ctx.fillRect(0, h / 2, w, Theme.hairline)
                            } else if (card.engineId === "echo") {
                                // The note and its repeats, falling away.
                                const repeats = value("echo.repeats")
                                const decay = value("echo.decay")
                                const pitch = value("echo.pitch")
                                let level = 1
                                const step = w / (repeats + echoTail)
                                for (let k = 0; k <= repeats; ++k) {
                                    const y = h / 2 - Math.max(-1, Math.min(1, pitch * k / semitoneRange)) * (h / 2 - echoBarHeight)
                                    ctx.globalAlpha = Math.max(Theme.noteFxGlyphFaintAlpha, level)
                                    ctx.fillRect(k * step + echoBarHeight / 2, y - echoBarHeight / 2, step * echoBarFill, echoBarHeight)
                                    level *= decay
                                }
                            } else if (card.engineId === "random") {
                                // A seeded scatter: how far and how often notes move.
                                const chance = value("random.chance")
                                const range = value("random.range")
                                const timing = value("random.timing")
                                let seed = value("random.seed") * 9301 + 49297
                                const next = () => { seed = (seed * 9301 + 49297) % 233280; return seed / 233280 }
                                for (let i = 0; i < phraseSteps; ++i) {
                                    const moved = next() < chance
                                    const offset = moved ? (next() * 2 - 1) * range / semitoneRange : 0
                                    const late = next() * timing * (w / phraseSteps) * lateShare
                                    ctx.globalAlpha = moved ? 1 : Theme.noteFxGlyphDimAlpha
                                    ctx.fillRect(i * w / phraseSteps + late + Theme.hairline, h / 2 - offset * (h / 2 - barHeight) - barHeight / 2, w / phraseSteps - barHeight, barHeight)
                                }
                            } else if (card.engineId === "comp") {
                                // The pattern: the top, middle and low lanes' hits.
                                const lanes = ["top", "mid", "low"]
                                const cell = w / phraseSteps
                                lanes.forEach((lane, row) => {
                                    for (let s = 0; s < phraseSteps; ++s) {
                                        const hit = c.choice("comp." + lane + "." + (s + 1))
                                        if (hit === 0)
                                            continue
                                        ctx.globalAlpha = hit === 2 ? Theme.noteFxGlyphLitAlpha : Theme.noteFxGlyphDimAlpha
                                        ctx.fillRect(s * cell + Theme.hairline, barHeight + row * (h - 2 * barHeight) / lanes.length, cell - 2 * Theme.hairline, (h - 2 * barHeight) / lanes.length - 2 * Theme.hairline)
                                    }
                                })
                            } else if (card.engineId === "scale") {
                                // An octave of keys, the scale's notes lit.
                                const masks = [0xab5, 0x5ad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x56b, 0x9ad,
                                               0xaad, 0x295, 0x4a9, 0x4e9, 0x555, 0xfff]
                                const root = value("scale.root")
                                const mask = masks[value("scale.scale")] || 0xfff
                                const key = w / 12
                                for (let k = 0; k < 12; ++k) {
                                    const lit = ((mask >> ((k - root + 12) % 12)) & 1) !== 0
                                    const black = [1, 3, 6, 8, 10].indexOf(k) >= 0
                                    ctx.globalAlpha = lit ? Theme.noteFxGlyphLitAlpha : Theme.noteFxGlyphFaintAlpha
                                    ctx.fillRect(k * key + Theme.hairline, black ? blackKeyTop : h * whiteKeyTop, key - 2 * Theme.hairline, black ? h * blackKeyLength : h * whiteKeyLength)
                                }
                            }
                        }
                    }
                }

                Flow {
                    x: Theme.spaceSm; y: presetRow.y + presetRow.height + Theme.noteFxKnobsTop
                    width: parent.width - Theme.spaceLg
                    height: parent.height - y - Theme.noteFxKnobsBottomGap
                    spacing: Theme.spaceXxs
                    clip: true
                    Repeater {
                        model: card.controller ? card.shown : []
                        delegate: Kit.Knob {
                            required property string modelData
                            controller: card.controller
                            theme: fxTheme
                            parameterId: modelData
                            diameter: Theme.noteFxKnobDiameterSmall
                        }
                    }
                }

                // Primary or the rest of the knobs.
                Rectangle {
                    objectName: "noteEffectMore:" + card.engineId
                    visible: (strip.secondary[card.engineId] || []).length > 0
                    anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.margins: Theme.spaceSm
                    width: moreText.implicitWidth + Theme.spaceLg; height: Theme.noteFxMoreHeight; radius: Theme.radiusMd
                    color: card.more ? Theme.noteFxMoreOn : "transparent"
                    border.color: Theme.noteFxMoreEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: card.more ? "Show the main controls" : "Show more controls"
                    Keys.onReturnPressed: card.more = !card.more
                    Keys.onSpacePressed: card.more = !card.more
                    Text {
                        id: moreText
                        anchors.centerIn: parent
                        text: card.more ? "MAIN" : "MORE"
                        color: strip.dimColor; font.pixelSize: Theme.typeMicro; font.bold: true; font.letterSpacing: Theme.capsLetterSpacing
                    }
                    Kit.Tip {
                        theme: fxTheme
                        shown: moreArea.containsMouse
                        text: card.more ? "Main controls" : "More controls"
                    }
                    MouseArea { id: moreArea; anchors.fill: parent; hoverEnabled: true
                                onClicked: card.more = !card.more }
                }
            }
        }
    }

    // "+ MIDI FX": the engines to add at the end of the chain.
    Rectangle {
        id: addTab
        objectName: "noteEffectAdd"
        x: cards.width + (strip.chain.length > 0 ? Theme.spaceXl : Theme.spaceMd)
        y: Theme.spaceMd
        width: Theme.noteFxAddWidth
        height: parent.height - 2 * Theme.spaceMd
        radius: Theme.radiusLg
        color: addArea.containsMouse || strip.choosing ? Theme.noteFxAddHover : "transparent"
        border.color: Theme.noteFxCardEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Add a MIDI effect before the synth"
        Keys.onReturnPressed: strip.choosing = !strip.choosing
        Keys.onSpacePressed: strip.choosing = !strip.choosing
        Text {
            anchors.centerIn: parent
            // Reads up the tab's side.
            readonly property real quarterTurn: 90
            rotation: -quarterTurn
            text: "+ MIDI FX"
            color: strip.accent; font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.noteFxNameLetterSpacing
        }
        Kit.Tip {
            theme: fxTheme
            shown: addArea.containsMouse && !strip.choosing
            text: "Add a MIDI effect"
            detail: "Arp, chord, echo, random or scale, before the synth · drag one from the browser too"
        }
        MouseArea {
            id: addArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: strip.choosing = !strip.choosing
        }
    }

    // The engine list, over the strip while choosing.
    Rectangle {
        id: chooser
        objectName: "noteEffectChooser"
        visible: strip.choosing
        ParallelAnimation {
            id: chooserReveal
            NumberAnimation { target: chooser; property: "opacity"; from: 0; to: 1
                              duration: Motion.fast }
            NumberAnimation { target: chooser; property: "scale"; from: Theme.noteFxChooserScale; to: 1
                              duration: Motion.fast; easing.type: Easing.OutQuad }
        }
        x: Math.max(Theme.spaceXs, addTab.x - width - Theme.spaceXs); y: Theme.spaceMd
        width: Theme.noteFxChooserWidth
        height: chooserColumn.implicitHeight + Theme.spaceLg
        radius: Theme.radiusXl
        color: Theme.noteFxChooserGround
        border.color: strip.accent
        z: 5
        Column {
            id: chooserColumn
            x: Theme.spaceSm; y: Theme.spaceSm
            width: parent.width - Theme.spaceLg
            spacing: Theme.spaceXxs
            Repeater {
                model: strip.effects ? strip.effects.engines : []
                delegate: Rectangle {
                    id: choice
                    required property var modelData
                    objectName: "noteEffectChoice:" + modelData.id
                    width: parent.width; height: Theme.noteFxChoiceHeight; radius: Theme.noteFxChoiceRadius
                    color: choiceArea.containsMouse ? Theme.noteFxChoiceHover : "transparent"
                    activeFocusOnTab: strip.choosing
                    Accessible.role: Accessible.Button
                    Accessible.name: "Add " + modelData.name
                    function add() {
                        strip.effects.add(strip.trackId, modelData.id, -1, -1)
                        strip.choosing = false
                    }
                    Keys.onReturnPressed: add()
                    Keys.onEscapePressed: strip.choosing = false
                    Text {
                        x: Theme.spaceMd; anchors.verticalCenter: parent.verticalCenter
                        text: choice.modelData.name
                        color: strip.textColor; font.pixelSize: Theme.typeLabel
                    }
                    MouseArea {
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: choice.add()
                    }
                }
            }
        }
    }
}
