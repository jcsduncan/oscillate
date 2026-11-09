pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract
import "NovaVoiceLanes.js" as Lanes

// The VOICES page. Left, the Analogue
// card: one Age knob over the per-part tolerances and Re-roll, drift and
// flutter, the seed, what it applies to and the number of voice cards.
// Right, Voice Lanes: one bar per voice for the chosen lane, seeded
// (violet) or painted by hand (amber), for the note voices (or cards) or
// one oscillator's unison voices; under it the note voices' cards and the
// per-voice modulation sources. Lanes are read and edited through
// NovaVoiceLanes.js; a paint drag is one document gesture. The lanes and
// the cards share any height and width the page has above its minimum.
Item {
    id: page
    required property var panel
    readonly property var controller: panel.controller
    readonly property var doc: panel.doc
    readonly property NovaTheme theme: panel.theme
    readonly property real gap: 10
    readonly property real analogueWidth: 340
    // Voice Lanes' card and its bars at the page's minimum height; the card
    // takes this share of any extra height and the voice cards the rest.
    readonly property real lanesMinimum: 452
    readonly property real lanesBarsMinimum: 340
    readonly property real lanesShare: 0.6
    // Below these the page is scaled down to fit (NovaPanel's Page): the
    // Analogue column's full height, and the lanes beside it.
    readonly property real minimumWidth: 1100
    readonly property real minimumHeight: 792
    readonly property real lanesHeight: lanesMinimum + Math.max(0, height - minimumHeight) * lanesShare

    readonly property real age: controller.revision >= 0 ? controller.normalised("nova.analogue.age") : 0
    readonly property int seed: controller.revision >= 0 ? Math.round(controller.value("nova.analogue.seed"))
                                                         : Contract.analogue.seedMin
    // Voice cards: 0 numbers note voices by slot, N deals notes over N cards.
    readonly property int cardCount: controller.revision >= 0 ? Math.round(controller.value("nova.analogue.cards")) : 0
    // The note voices the lanes and the voice cards show.
    readonly property int noteVoices: Contract.laneVoices(cardCount)
    readonly property int shownCards: Math.min(noteVoices, Contract.analogue.noteVoices)

    // The lane shown, and whose voices: 0 the note voices, 1..3 the unison
    // voices of Osc A..C.
    property string lane: Contract.voiceLanes[0].id
    property int which: 0
    readonly property string kind: which === 0 ? Lanes.notes : Lanes.unison
    readonly property int osc: Math.max(0, which - 1)
    readonly property int count: which === 0 ? noteVoices : unisonVoices(osc)
    readonly property int revision: Lanes.revision(doc, controller)
    readonly property int laneIndex: {
        for (let i = 0; i < Contract.voiceLanes.length; ++i)
            if (Contract.voiceLanes[i].id === lane)
                return i
        return 0
    }
    readonly property var laneInfo: Contract.voiceLanes[laneIndex]
    // The shown lane's voices: [{value, edited}].
    readonly property var bars: revision >= 0 ? Lanes.read(doc, kind, osc, lane, count) : []

    // The parts each voice card lists, and their values per note voice.
    readonly property var cardLanes: [{ id: "pitch", short: "pitch" }, { id: "cutoff", short: "cut" },
                                      { id: "env", short: "env" }, { id: "pan", short: "pan" }]
    readonly property var cardValues: {
        const out = {}
        if (revision >= 0)
            for (let i = 0; i < cardLanes.length; ++i)
                out[cardLanes[i].id] = Lanes.read(doc, Lanes.notes, 0, cardLanes[i].id, Contract.analogue.noteVoices)
        return out
    }

    objectName: "novaVoicesPage"

    function unisonVoices(o) {
        return controller.revision >= 0
            ? Math.max(1, Math.round(controller.value("nova.osc." + Contract.oscKeys[o] + ".unison.voices"))) : 1
    }
    function snap(x) {
        const s = Contract.analogue.paintStep
        return Math.round(Math.max(-1, Math.min(1, x)) / s) * s
    }

    // ---- The lane tools: each one undo step ----------------------------------------------

    function spread() {
        if (!controller.editable)
            return
        const values = []
        for (let v = 0; v < count; ++v)
            values.push(count > 1 ? snap((2 * v / (count - 1) - 1) * Contract.analogue.spreadReach) : 0)
        Lanes.writeAll(doc, kind, osc, lane, values)
    }
    function randomise() {
        if (!controller.editable)
            return
        const values = []
        for (let v = 0; v < count; ++v)
            values.push(snap((Math.random() * 2 - 1) * Contract.analogue.randomReach))
        Lanes.writeAll(doc, kind, osc, lane, values)
    }
    function mirror() {
        if (!controller.editable)
            return
        const values = []
        for (let v = 0; v < count; ++v)
            values.push(v < bars.length ? -Number(bars[v].value) : 0)
        Lanes.writeAll(doc, kind, osc, lane, values)
    }
    function backToSeed() {
        if (!controller.editable)
            return
        Lanes.clear(doc, kind, osc, lane)
    }
    // New synth: the next seed in the sequence, one undo step.
    function newSynth() {
        if (!controller.editable)
            return
        const id = "nova.analogue.seed"
        controller.begin_gesture(id)
        controller.set_value(id, Contract.nextSeed(seed))
        controller.end_gesture(id)
        panel.touchId(id, "SEED")
    }
    // Voice cards up or down by one, one undo step.
    function stepCards(delta) {
        if (!controller.editable)
            return
        const id = "nova.analogue.cards"
        const next = Math.max(0, Math.min(Contract.voiceCardsMax, cardCount + delta))
        if (next === cardCount)
            return
        controller.begin_gesture(id)
        controller.set_value(id, next)
        controller.end_gesture(id)
        panel.touchId(id, "VOICE CARDS")
    }

    // ---- Parts ---------------------------------------------------------------------------

    component Label: Text {
        color: page.theme.faint
        font.family: page.theme.sans
        font.pixelSize: page.theme.typeSmall
        font.weight: Font.ExtraBold
        font.letterSpacing: page.theme.voicesLabelSpacing
    }
    component Note: Text {
        color: page.theme.faint
        font.family: page.theme.sans
        font.pixelSize: page.theme.typeSmall
        lineHeight: page.theme.voicesNoteLineHeight
        wrapMode: Text.WordWrap
    }
    component Rule: Rectangle {
        height: page.theme.hairline
        color: page.theme.line
    }

    // A tolerance or drift row: name, a violet slider and the amount it
    // gives. Drag (Shift fine), wheel or arrow keys; double-click resets.
    component SliderRow: Item {
        id: row
        required property string parameterId
        required property string name
        // The amount shown for a slider position 0..1.
        required property var fmt
        readonly property real position: page.controller.revision >= 0 ? page.controller.normalised(parameterId) : 0
        readonly property real nameWidth: 82
        readonly property real valueWidth: 66

        objectName: "novaSlider:" + parameterId
        height: page.theme.voicesSliderRowHeight
        opacity: page.controller.editable ? 1 : page.theme.voicesLockedOpacity
        activeFocusOnTab: page.controller.editable
        Accessible.role: Accessible.Slider
        Accessible.name: name
        Accessible.description: amount.text
        Keys.onPressed: event => {
            if (!page.controller.editable)
                return
            const fine = (event.modifiers & Qt.ShiftModifier) !== 0
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
                page.controller.step(row.parameterId, 1, fine)
                event.accepted = true
            } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
                page.controller.step(row.parameterId, -1, fine)
                event.accepted = true
            } else if (event.key === Qt.Key_Delete) {
                page.controller.reset_to_default(row.parameterId)
                event.accepted = true
            }
        }

        Text {
            width: row.nameWidth
            anchors.verticalCenter: parent.verticalCenter
            text: row.name
            color: page.theme.dim
            font.family: page.theme.sans
            font.pixelSize: page.theme.typeBody
            elide: Text.ElideRight
        }
        Item {
            id: track
            x: row.nameWidth + page.theme.spaceMd
            width: row.width - x - row.valueWidth - page.theme.spaceMd
            height: parent.height
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: page.theme.voicesSliderTrackHeight
                radius: height / 2
                color: page.theme.knobTrack
                Rectangle {
                    width: parent.width * row.position
                    height: parent.height
                    radius: height / 2
                    color: page.theme.vi
                }
            }
            Rectangle {
                x: track.width * row.position - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: page.theme.voicesSliderThumbSize
                height: page.theme.voicesSliderThumbSize
                radius: width / 2
                color: page.theme.knobPointer
                border.color: row.activeFocus || area.pressed ? page.theme.neutral : page.theme.vi
            }
            MouseArea {
                id: area
                property real startX: 0
                property real startValue: 0
                anchors.fill: parent
                anchors.leftMargin: -page.theme.voicesSliderGrabOutset
                anchors.rightMargin: -page.theme.voicesSliderGrabOutset
                anchors.topMargin: -page.theme.hitOutset(parent.height)
                anchors.bottomMargin: -page.theme.hitOutset(parent.height)
                enabled: page.controller.editable
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                onPressed: mouse => {
                    row.forceActiveFocus()
                    startX = mouse.x
                    // A press away from the thumb jumps there first.
                    const at = Math.max(0, Math.min(1, (mouse.x - page.theme.voicesSliderGrabOutset) / track.width))
                    startValue = Math.abs(at - row.position) * track.width > page.theme.voicesSliderJumpDistance ? at : row.position
                    page.controller.begin_gesture(row.parameterId)
                    page.controller.set_normalised(row.parameterId, startValue)
                    page.panel.touchId(row.parameterId, row.name.toUpperCase())
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const scale = (mouse.modifiers & Qt.ShiftModifier) ? page.theme.voicesSliderFineDivisor : 1
                    page.controller.set_normalised(row.parameterId,
                        Math.max(0, Math.min(1, startValue + (mouse.x - startX) / track.width / scale)))
                }
                onReleased: page.controller.end_gesture(row.parameterId)
                onCanceled: page.controller.cancel_gesture(row.parameterId)
                onDoubleClicked: page.controller.reset_to_default(row.parameterId)
                onWheel: wheel => {
                    if (wheel.angleDelta.y !== 0)
                        page.controller.step(row.parameterId, wheel.angleDelta.y > 0 ? 1 : -1,
                                             (wheel.modifiers & Qt.ShiftModifier) !== 0)
                    wheel.accepted = true
                }
            }
        }
        Text {
            id: amount
            x: row.width - row.valueWidth
            width: row.valueWidth
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            text: page.controller.revision >= 0 ? row.fmt(row.position) : ""
            color: area.pressed ? page.theme.neutral : page.theme.text
            font.family: page.theme.mono
            font.pixelSize: page.theme.typeBody
        }
    }

    // A switch for an [Off, On] choice parameter.
    component Toggle: Item {
        id: toggle
        required property string parameterId
        required property string text
        readonly property bool on: page.controller.revision >= 0 && page.controller.choice(parameterId) === 1

        objectName: "novaToggle:" + parameterId
        implicitWidth: track.width + page.theme.voicesToggleGap + caption.implicitWidth
        implicitHeight: page.theme.voicesToggleHeight
        opacity: page.controller.editable ? 1 : page.theme.voicesLockedOpacity
        activeFocusOnTab: page.controller.editable
        Accessible.role: Accessible.CheckBox
        Accessible.name: text
        Accessible.checked: on
        function flip() {
            if (!page.controller.editable)
                return
            page.controller.set_choice(parameterId, on ? 0 : 1)
            page.panel.touchId(parameterId, text.toUpperCase())
        }
        Keys.onSpacePressed: flip()
        Keys.onReturnPressed: flip()

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: page.theme.voicesToggleTrackWidth
            height: page.theme.voicesToggleTrackHeight
            radius: height / 2
            color: toggle.on ? page.theme.vi : page.theme.line2
            border.color: toggle.activeFocus ? page.theme.neutral : "transparent"
            Rectangle {
                x: toggle.on ? parent.width - width - page.theme.voicesToggleKnobInset : page.theme.voicesToggleKnobInset
                y: page.theme.voicesToggleKnobInset
                width: page.theme.voicesToggleKnobSize
                height: page.theme.voicesToggleKnobSize
                radius: width / 2
                color: page.theme.text
                Behavior on x { NumberAnimation { duration: page.theme.voicesToggleDuration } }
            }
        }
        Text {
            id: caption
            x: track.width + page.theme.voicesToggleGap
            anchors.verticalCenter: parent.verticalCenter
            text: toggle.text
            color: page.theme.dim
            font.family: page.theme.sans
            font.pixelSize: page.theme.typeBody
        }
        MouseArea {
            anchors.fill: parent
            anchors.topMargin: -page.theme.hitOutset(parent.height)
            anchors.bottomMargin: -page.theme.hitOutset(parent.height)
            onClicked: toggle.flip()
        }
    }

    // ---- Analogue --------------------------------------------------------------------------

    NovaCard {
        id: analogue
        objectName: "novaAnalogueCard"
        panel: page.panel
        title: "ANALOGUE"
        titleColor: page.theme.vi
        sub: "NO TWO VOICES BUILT ALIKE"
        ledParameter: "nova.analogue.on"
        width: page.analogueWidth
        height: page.height

        Column {
            x: page.theme.voicesCardInset
            y: page.theme.voicesAnalogueTop
            width: parent.width - 2 * page.theme.voicesCardInset
            spacing: page.theme.voicesSectionSpacing

            // Age: one knob that turns every tolerance up together.
            Row {
                spacing: page.theme.voicesAgeGap
                Item {
                    id: ageKnob
                    readonly property real size: 108
                    readonly property real start: 0.75 * Math.PI
                    readonly property real sweep: 1.5 * Math.PI
                    objectName: "novaAgeKnob"
                    width: size
                    height: size
                    opacity: page.controller.editable ? 1 : page.theme.voicesLockedOpacity
                    activeFocusOnTab: page.controller.editable
                    Accessible.role: Accessible.Dial
                    Accessible.name: "Age"
                    Accessible.description: Math.round(page.age * 100) + ", " + Contract.age(page.age).name
                    Keys.onPressed: event => {
                        if (!page.controller.editable)
                            return
                        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
                            page.controller.step("nova.analogue.age", 1, fine)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
                            page.controller.step("nova.analogue.age", -1, fine)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Delete) {
                            page.controller.reset_to_default("nova.analogue.age")
                            event.accepted = true
                        }
                    }

                    Canvas {
                        id: ageFace
                        anchors.fill: parent
                        renderStrategy: Canvas.Cooperative
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const t = page.theme
                            const c = width / 2, r = c - t.voicesAgeArcInset
                            ctx.lineWidth = t.voicesAgeArcStroke
                            ctx.lineCap = "round"
                            ctx.strokeStyle = t.knobTrack
                            ctx.beginPath()
                            ctx.arc(c, c, r, ageKnob.start, ageKnob.start + ageKnob.sweep)
                            ctx.stroke()
                            if (page.age > t.voicesAgeArcMinimum) {
                                // Fresh violet ageing to amber.
                                const g = ctx.createLinearGradient(t.voicesAgeArcInset, height - t.voicesAgeArcInset,
                                                                   width - t.voicesAgeArcInset, t.voicesAgeArcInset)
                                g.addColorStop(0, t.vi)
                                g.addColorStop(1, t.am)
                                ctx.strokeStyle = g
                                ctx.beginPath()
                                ctx.arc(c, c, r, ageKnob.start, ageKnob.start + ageKnob.sweep * page.age)
                                ctx.stroke()
                            }
                            ctx.lineWidth = t.hairline
                            ctx.strokeStyle = ageKnob.activeFocus ? t.neutral : t.line2
                            ctx.beginPath()
                            ctx.arc(c, c, r - t.voicesAgeRingInset, 0, 2 * Math.PI)
                            ctx.stroke()
                        }
                        Connections {
                            target: page
                            function onAgeChanged() { ageFace.requestPaint() }
                        }
                        Connections {
                            target: ageKnob
                            function onActiveFocusChanged() { ageFace.requestPaint() }
                        }
                    }
                    Text {
                        objectName: "novaAgeValue"
                        anchors.centerIn: parent
                        text: Math.round(page.age * 100)
                        color: ageArea.pressed ? page.theme.neutral : page.theme.text
                        font.family: page.theme.mono
                        font.pixelSize: page.theme.voicesAgeValueSize
                        font.weight: Font.Bold
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.height - page.theme.voicesAgeCaptionBottom
                        text: "AGE"
                        color: page.theme.faint
                        font.family: page.theme.sans
                        font.pixelSize: page.theme.typeCaption
                        font.weight: Font.Bold
                        font.letterSpacing: page.theme.voicesAgeCaptionSpacing
                    }
                    MouseArea {
                        id: ageArea
                        property real startY: 0
                        property real startValue: 0
                        objectName: "novaAgeDial:nova.analogue.age"
                        anchors.fill: parent
                        enabled: page.controller.editable
                        preventStealing: true
                        cursorShape: Qt.SizeVerCursor
                        onPressed: mouse => {
                            ageKnob.forceActiveFocus()
                            startY = mouse.y
                            startValue = page.age
                            page.controller.begin_gesture("nova.analogue.age")
                            page.panel.touchId("nova.analogue.age", "AGE")
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return
                            const travel = (startY - mouse.y) / ((mouse.modifiers & Qt.ShiftModifier) ? page.theme.knobFinePixels
                                                                                                      : page.theme.knobDragPixels)
                            page.controller.set_normalised("nova.analogue.age", Math.max(0, Math.min(1, startValue + travel)))
                        }
                        onReleased: page.controller.end_gesture("nova.analogue.age")
                        onCanceled: page.controller.cancel_gesture("nova.analogue.age")
                        onDoubleClicked: page.controller.reset_to_default("nova.analogue.age")
                        onWheel: wheel => {
                            if (wheel.angleDelta.y !== 0)
                                page.controller.step("nova.analogue.age", wheel.angleDelta.y > 0 ? 1 : -1,
                                                     (wheel.modifiers & Qt.ShiftModifier) !== 0)
                            wheel.accepted = true
                        }
                    }
                }
                Column {
                    width: analogue.width - 2 * page.theme.voicesCardInset - ageKnob.width - page.theme.voicesAgeGap
                    spacing: page.theme.spaceSm
                    Text {
                        objectName: "novaAgeName"
                        text: Contract.age(page.age).name
                        color: page.theme.text
                        font.family: page.theme.sans
                        font.pixelSize: page.theme.typeHeading
                        font.weight: Font.DemiBold
                    }
                    Note {
                        width: parent.width
                        height: page.theme.voicesAgeHintHeight
                        text: Contract.age(page.age).hint
                        elide: Text.ElideRight
                    }
                    Row {
                        spacing: page.theme.spaceSm
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "SEED"
                        }
                        Rectangle {
                            objectName: "novaSeed:nova.analogue.seed"
                            anchors.verticalCenter: parent.verticalCenter
                            width: seedText.implicitWidth + page.theme.voicesSeedPad
                            height: page.theme.voicesFieldHeight
                            radius: page.theme.radiusLg
                            color: page.theme.wellFill
                            border.color: page.theme.line2
                            Accessible.role: Accessible.StaticText
                            Accessible.name: "Seed " + page.seed
                            Text {
                                id: seedText
                                anchors.centerIn: parent
                                text: "#" + page.seed
                                color: page.theme.vi
                                font.family: page.theme.mono
                                font.pixelSize: page.theme.typeLabel
                                font.weight: Font.DemiBold
                            }
                        }
                        NovaPill {
                            objectName: "novaNewSynth"
                            anchors.verticalCenter: parent.verticalCenter
                            panel: page.panel
                            text: "⚄ NEW SYNTH"
                            accent: page.theme.vi
                            onClicked: page.newSynth()
                        }
                    }
                }
            }

            Label { text: "TOLERANCES · HOW FAR PARTS DIFFER" }
            Column {
                width: parent.width
                spacing: page.theme.spaceXs
                Repeater {
                    model: Contract.analogueTolerances
                    delegate: SliderRow {
                        required property var modelData
                        width: parent.width
                        parameterId: "nova.analogue." + modelData.id
                        name: modelData.name
                        fmt: p => Contract.toleranceText(modelData, p, page.age)
                    }
                }
                // Re-roll: every note a fresh offset on top, like RANDOM.
                SliderRow {
                    width: parent.width
                    parameterId: "nova.analogue." + Contract.analogueReroll.id
                    name: Contract.analogueReroll.name
                    fmt: Contract.analogueReroll.fmt
                }
            }

            Rule { width: parent.width }
            Label { text: "DRIFT · FLUTTER · EACH VOICE MOVES ON ITS OWN" }
            Column {
                width: parent.width
                spacing: page.theme.spaceXs
                Repeater {
                    model: Contract.analogueDrift
                    delegate: SliderRow {
                        required property var modelData
                        width: parent.width
                        parameterId: "nova.analogue." + modelData.id
                        name: modelData.name
                        fmt: modelData.fmt
                    }
                }
                // Flutter: fast, faint pitch jitter (keep it nearly inaudible).
                Repeater {
                    model: Contract.analogueFlutter
                    delegate: SliderRow {
                        required property var modelData
                        width: parent.width
                        parameterId: "nova.analogue." + modelData.id
                        name: modelData.name
                        fmt: modelData.fmt
                    }
                }
            }

            Rule { width: parent.width }
            Toggle {
                parameterId: "nova.analogue.freerun"
                text: "Free-running oscillators: notes start mid-cycle"
            }
            Label { text: "APPLIES TO" }
            NovaSegs {
                panel: page.panel
                parameterId: "nova.analogue.scope"
                labels: Contract.analogueScopes.map(s => s.toUpperCase())
                accent: page.theme.vi
            }
            Rule { width: parent.width }
            // Voice cards: Per voice, or N cards dealt round-robin.
            Row {
                spacing: page.theme.spaceMd
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "VOICE CARDS"
                }
                Rectangle {
                    id: cardsStepper
                    objectName: "novaCards:nova.analogue.cards"
                    anchors.verticalCenter: parent.verticalCenter
                    width: page.theme.voicesStepperWidth
                    height: page.theme.voicesFieldHeight
                    radius: page.theme.radiusLg
                    color: page.theme.wellFill
                    border.color: cardsStepper.activeFocus ? page.theme.neutral : page.theme.line2
                    opacity: page.controller.editable ? 1 : page.theme.voicesLockedOpacity
                    activeFocusOnTab: page.controller.editable
                    Accessible.role: Accessible.SpinBox
                    Accessible.name: "Voice cards"
                    Accessible.description: Contract.cardsText(page.cardCount)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
                            page.stepCards(1)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
                            page.stepCards(-1)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Delete && page.controller.editable) {
                            page.controller.reset_to_default("nova.analogue.cards")
                            event.accepted = true
                        }
                    }
                    Repeater {
                        model: [{ text: "−", delta: -1, left: true }, { text: "+", delta: 1, left: false }]
                        delegate: Item {
                            id: stepButton
                            required property var modelData
                            objectName: "novaCardsStep:" + (modelData.delta > 0 ? "up" : "down")
                            x: modelData.left ? 0 : cardsStepper.width - width
                            width: page.theme.voicesStepperButtonWidth
                            height: cardsStepper.height
                            Text {
                                anchors.centerIn: parent
                                text: stepButton.modelData.text
                                color: stepArea.containsMouse ? page.theme.vi : page.theme.dim
                                font.family: page.theme.mono
                                font.pixelSize: page.theme.typeEmphasis
                            }
                            MouseArea {
                                id: stepArea
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: page.controller.editable
                                onClicked: page.stepCards(stepButton.modelData.delta)
                            }
                        }
                    }
                    Text {
                        objectName: "novaCardsValue"
                        anchors.centerIn: parent
                        text: Contract.cardsText(page.cardCount)
                        color: page.cardCount > 0 ? page.theme.vi : page.theme.text
                        font.family: page.theme.mono
                        font.pixelSize: page.theme.typeBody
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.leftMargin: page.theme.voicesStepperButtonWidth
                        anchors.rightMargin: page.theme.voicesStepperButtonWidth
                        enabled: page.controller.editable
                        onClicked: cardsStepper.forceActiveFocus()
                        onDoubleClicked: page.controller.reset_to_default("nova.analogue.cards")
                        onWheel: wheel => {
                            if (wheel.angleDelta.y !== 0)
                                page.stepCards(wheel.angleDelta.y > 0 ? 1 : -1)
                            wheel.accepted = true
                        }
                    }
                }
                Note {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "6 = a Prophet-6"
                }
            }
            Note {
                width: parent.width
                text: page.cardCount > 0
                    ? "Each new note takes the next of " + page.cardCount + " cards in turn, as on a real polysynth: "
                      + "its offsets, lanes and drift are that card's."
                    : "Every note voice has its own offsets. Pick a number of cards to deal notes round-robin over them."
            }
        }
    }

    // ---- Voice lanes -------------------------------------------------------------------------

    NovaCard {
        id: lanesCard
        objectName: "novaVoiceLanesCard"
        panel: page.panel
        title: "VOICE LANES"
        titleColor: page.theme.vi
        x: page.analogueWidth + page.gap
        width: page.width - x
        height: page.lanesHeight

        NovaSegs {
            objectName: "novaLaneTabs"
            panel: page.panel
            x: lanesCard.titleRight + page.theme.voicesTabsGap
            y: page.theme.voicesHeaderControlTop
            labels: Contract.voiceLanes.map(l => l.name.toUpperCase())
            current: page.laneIndex
            accent: page.theme.vi
            // Unison voices take only the unison lanes: another lane shows the notes'.
            onPicked: index => {
                if (page.which > 0 && !Contract.voiceLanes[index].unison)
                    page.which = 0
                page.lane = Contract.voiceLanes[index].id
            }
        }
        NovaSegs {
            objectName: "novaLaneVoices"
            panel: page.panel
            anchors.right: parent.right
            anchors.rightMargin: page.theme.spaceLg
            y: page.theme.voicesHeaderControlTop
            labels: {
                const out = [page.cardCount > 0 ? page.cardCount + (page.cardCount === 1 ? " CARD" : " CARDS")
                                            : Contract.analogue.noteVoices + " NOTE VOICES"]
                for (let o = 0; o < Contract.oscKeys.length; ++o)
                    out.push((o === 0 ? "UNISON · " : "") + "OSC " + Contract.oscKeys[o].toUpperCase()
                             + " ×" + page.unisonVoices(o))
                return out
            }
            current: page.which
            accent: page.theme.vi
            onPicked: index => {
                if (index > 0 && !page.laneInfo.unison)
                    page.lane = Contract.voiceLanes[0].id
                page.which = index
            }
        }

        Rectangle {
            id: lanes
            // The scale's gutter on the left (the plot frame's) and the room on the right.
            readonly property real pad: page.theme.plotGutterLeft
            readonly property real end: page.theme.voicesLaneEnd
            readonly property int count: page.count
            readonly property var bars: page.bars
            readonly property real cellWidth: (width - pad - end) / Math.max(1, count)
            readonly property real barWidth: Math.max(page.theme.voicesBarMinWidth,
                                                   Math.min(cellWidth * page.theme.voicesBarWidthRatio, page.theme.voicesBarMaxWidth))
            readonly property real mid: height / 2 - page.theme.voicesLaneMidLift
            readonly property real amp: height / 2 - page.theme.voicesLaneAmpInset
            // The scale: a lane's reach each way, in percent of it.
            readonly property real reachPercent: 100
            // Keys: Left and Right pick a voice, Up and Down move its value
            // a paint step (Shift: this many steps). Each key is one edit.
            property int keyVoice: 0
            readonly property int keyCoarseSteps: 10

            objectName: "novaVoiceLanes"
            x: page.theme.spaceLg
            y: page.theme.voicesBodyTop
            width: parent.width - 2 * page.theme.spaceLg
            height: page.lanesBarsMinimum + page.lanesHeight - page.lanesMinimum
            radius: page.theme.radiusXl
            color: page.theme.wellFill
            border.color: activeFocus ? page.theme.vi : page.theme.line
            border.width: activeFocus ? page.theme.strokeBold : page.theme.hairline
            clip: true
            activeFocusOnTab: page.controller.editable
            Accessible.role: Accessible.Graphic
            Accessible.name: "Per-voice " + page.laneInfo.name + " offsets, paint to edit"
            Accessible.description: "Left and Right pick a voice, Up and Down move its value; voice "
                + (keyVoice + 1) + " is " + (keyVoice < bars.length ? Contract.laneText(page.lane, Number(bars[keyVoice].value)) : "")

            function voiceAt(x) {
                return Math.floor((x - pad) / cellWidth)
            }
            function valueAt(y) {
                return page.snap((mid - y) / amp)
            }
            function centreX(voice) {
                return pad + (voice + 0.5) * cellWidth
            }
            // A value's place on the scale, 0 at the plot's foot to 1 at its top.
            function scaleAt(value) {
                return height > 0 ? 1 - (mid - value * amp) / height : 0
            }

            onCountChanged: {
                keyVoice = Math.max(0, Math.min(count - 1, keyVoice))
                laneCanvas.requestPaint()
            }
            onKeyVoiceChanged: laneCanvas.requestPaint()
            onActiveFocusChanged: laneCanvas.requestPaint()
            Keys.onPressed: event => {
                if (!page.controller.editable || count === 0)
                    return
                if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                    keyVoice = Math.max(0, Math.min(count - 1, keyVoice + (event.key === Qt.Key_Right ? 1 : -1)))
                } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    const steps = (event.modifiers & Qt.ShiftModifier) !== 0 ? keyCoarseSteps : 1
                    const value = keyVoice < bars.length ? Number(bars[keyVoice].value) : 0
                    const direction = event.key === Qt.Key_Up ? 1 : -1
                    Lanes.write(page.doc, page.kind, page.osc, page.lane, keyVoice,
                                page.snap(value + direction * steps * Contract.analogue.paintStep))
                } else {
                    return
                }
                event.accepted = true
            }

            // The offsets' frame: the scale in percent of the lane's reach
            // (each bar's caption gives its value in the lane's unit). The
            // lanes are the settings, never the running voices: PREVIEW.
            Kit.PlotFrame {
                anchors.fill: parent
                theme: page.theme
                live: false
                yUnit: "%"
                yTicks: [{ at: lanes.scaleAt(-1), label: "−" + lanes.reachPercent },
                         { at: lanes.scaleAt(0), label: "0" },
                         { at: lanes.scaleAt(1), label: "+" + lanes.reachPercent }]

                Canvas {
                    id: laneCanvas
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        // Drawn in the lanes' coordinates: the plot starts past the gutter.
                        ctx.translate(-lanes.pad, 0)
                        const t = page.theme
                        const W = lanes.width, H = lanes.height
                        const mid = lanes.mid, amp = lanes.amp
                        ctx.fillStyle = t.gridLine
                        ctx.fillRect(lanes.pad, mid - amp, W - lanes.pad - lanes.end, t.hairline)
                        ctx.fillRect(lanes.pad, mid + amp, W - lanes.pad - lanes.end, t.hairline)
                        ctx.fillStyle = t.line2
                        ctx.fillRect(lanes.pad, mid, W - lanes.pad - lanes.end, t.hairline)
                        ctx.textAlign = "center"
                        for (let v = 0; v < lanes.count; ++v) {
                            const bar = v < lanes.bars.length ? lanes.bars[v] : null
                            const value = bar !== null ? Number(bar.value) : 0
                            const edited = bar !== null && bar.edited === true
                            const c = edited ? t.am : t.vi
                            const bx = lanes.centreX(v) - lanes.barWidth / 2
                            const y = mid - value * amp
                            // The voice the keys move: its column outlined.
                            if (lanes.activeFocus && v === lanes.keyVoice) {
                                ctx.strokeStyle = t.vi
                                ctx.lineWidth = t.hairline
                                ctx.strokeRect(lanes.centreX(v) - lanes.cellWidth / 2, mid - amp,
                                               lanes.cellWidth, 2 * amp)
                            }
                            ctx.fillStyle = t.soft(c, t.voicesBarFillAlpha)
                            ctx.fillRect(bx, Math.min(mid, y), lanes.barWidth, Math.abs(y - mid))
                            ctx.fillStyle = c
                            ctx.fillRect(bx, y - t.voicesBarCapHeight / 2, lanes.barWidth, t.voicesBarCapHeight)
                            ctx.fillStyle = t.dim
                            ctx.font = t.typeBody + "px \"" + t.mono + "\""
                            ctx.fillText(String(v + 1), lanes.centreX(v), H - t.voicesVoiceNumberBottom)
                            ctx.fillStyle = edited ? t.am : t.faint
                            ctx.font = t.voicesLaneValueSize + "px \"" + t.mono + "\""
                            ctx.fillText(Contract.laneText(page.lane, value), lanes.centreX(v), H - t.voicesLaneValueBottom)
                        }
                    }
                }

                // Paint: press and drag across the bars, one gesture. Voices a
                // fast drag skips over take the values in between. Positions
                // are the lanes' (past the frame's gutter).
                MouseArea {
                    id: paint
                    property bool painting: false
                    property int last: -1
                    property real lastValue: 0
                    anchors.fill: parent
                    enabled: page.controller.editable
                    preventStealing: true
                    cursorShape: Qt.CrossCursor

                    function put(voice, value) {
                        Lanes.write(page.doc, page.kind, page.osc, page.lane, voice, value)
                    }
                    function paintAt(x, y) {
                        const voice = Math.max(0, Math.min(lanes.count - 1, lanes.voiceAt(x)))
                        const value = lanes.valueAt(y)
                        if (voice === last && value === lastValue)
                            return
                        if (last >= 0 && Math.abs(voice - last) > 1) {
                            const step = voice > last ? 1 : -1
                            for (let v = last + step; v !== voice; v += step)
                                put(v, page.snap(lastValue + (value - lastValue) * (v - last) / (voice - last)))
                        }
                        put(voice, value)
                        last = voice
                        lastValue = value
                        lanes.keyVoice = voice
                    }
                    onPressed: mouse => {
                        const voice = lanes.voiceAt(mouse.x + lanes.pad)
                        if (voice < 0 || voice >= lanes.count) {
                            mouse.accepted = false
                            return
                        }
                        lanes.forceActiveFocus()
                        Lanes.begin(page.doc)
                        painting = true
                        last = -1
                        paintAt(mouse.x + lanes.pad, mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (painting)
                            paintAt(mouse.x + lanes.pad, mouse.y)
                    }
                    onReleased: {
                        if (painting)
                            Lanes.commit(page.doc)
                        painting = false
                    }
                    onCanceled: {
                        if (painting)
                            Lanes.cancel(page.doc)
                        painting = false
                    }
                }
            }
            onBarsChanged: laneCanvas.requestPaint()
            onWidthChanged: laneCanvas.requestPaint()
            onHeightChanged: laneCanvas.requestPaint()
        }

        // Legend and tools.
        Row {
            x: page.theme.voicesCardInset
            y: lanes.y + lanes.height + page.theme.voicesLegendTop
            spacing: page.theme.voicesLegendSpacing
            Repeater {
                model: [{ colour: page.theme.vi, text: "From the seed (scaled by Age)" },
                        { colour: page.theme.am, text: "Painted by you" }]
                delegate: Row {
                    id: key
                    required property var modelData
                    spacing: page.theme.spaceSm
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: page.theme.voicesSwatchSize
                        height: page.theme.voicesSwatchSize
                        radius: page.theme.radiusSm
                        color: key.modelData.colour
                    }
                    Text {
                        text: key.modelData.text
                        color: page.theme.dim
                        font.family: page.theme.sans
                        font.pixelSize: page.theme.typeBody
                    }
                }
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: page.theme.spaceLg
            y: lanes.y + lanes.height + page.theme.voicesToolsTop
            spacing: page.theme.spaceSm
            NovaPill {
                objectName: "novaLaneSpread"
                panel: page.panel
                text: "SPREAD"
                accent: page.theme.vi
                onClicked: page.spread()
            }
            NovaPill {
                objectName: "novaLaneRandomise"
                panel: page.panel
                text: "RANDOMISE"
                accent: page.theme.vi
                onClicked: page.randomise()
            }
            NovaPill {
                objectName: "novaLaneMirror"
                panel: page.panel
                text: "MIRROR"
                accent: page.theme.vi
                onClicked: page.mirror()
            }
            NovaPill {
                objectName: "novaLaneBackToSeed"
                panel: page.panel
                text: "BACK TO SEED"
                accent: page.theme.vi
                onClicked: page.backToSeed()
            }
        }
        Note {
            objectName: "novaLaneHint"
            x: page.theme.voicesCardInset
            y: lanes.y + lanes.height + page.theme.voicesHintTop
            width: parent.width - 2 * page.theme.voicesCardInset
            text: page.laneInfo.hint + (page.which > 0 ? " Unison voices take Pitch, Level, Pan and Phase." : "")
                + " Drag across the bars to paint."
        }
    }

    // ---- Voice cards -------------------------------------------------------------------------

    NovaCard {
        id: cardsCard
        objectName: "novaVoiceCardsCard"
        panel: page.panel
        title: "VOICE CARDS"
        sub: "EACH NOTE VOICE'S OFFSETS"
        x: lanesCard.x
        y: page.lanesHeight + page.gap
        width: lanesCard.width
        height: page.height - y

        // The per-voice modulation sources: drag ✥ onto any knob.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: page.theme.spaceLg
            y: page.theme.voicesHeaderControlTop
            spacing: page.theme.spaceXs
            Repeater {
                model: Contract.voiceSources
                delegate: Rectangle {
                    id: chip
                    required property string modelData
                    readonly property int routes: page.doc.routes_revision >= 0 ? page.doc.routes_from(modelData) : 0
                    objectName: "novaSource:" + modelData
                    width: chipRow.implicitWidth + page.theme.voicesSourceChipPad
                    height: page.theme.voicesSourceChipHeight
                    radius: page.theme.radiusLg
                    color: page.theme.wellFill
                    border.color: routes > 0 ? page.theme.viBorder : page.theme.line2
                    Row {
                        id: chipRow
                        x: page.theme.spaceXs
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: page.theme.voicesSourceChipSpacing
                        NovaHandle {
                            panel: page.panel
                            source: chip.modelData
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.panel.sourceName(chip.modelData)
                            color: chip.routes > 0 ? page.theme.text : page.theme.dim
                            font.family: page.theme.sans
                            font.pixelSize: page.theme.typeSmall
                            font.weight: Font.Bold
                        }
                        Text {
                            visible: chip.routes > 0
                            anchors.verticalCenter: parent.verticalCenter
                            text: chip.routes
                            color: page.theme.vi
                            font.family: page.theme.mono
                            font.pixelSize: page.theme.typeSmall
                        }
                    }
                }
            }
        }

        Row {
            id: cards
            objectName: "novaVoiceCards"
            readonly property real cardWidth: (cardsCard.width - 2 * page.theme.spaceLg - (Contract.analogue.noteVoices - 1) * spacing)
                                              / Contract.analogue.noteVoices
            readonly property real cardHeight: 124
            x: page.theme.spaceLg
            y: page.theme.voicesBodyTop
            spacing: page.theme.spaceMd
            Repeater {
                // The cards in use (up to eight), or the first eight voices.
                model: page.shownCards
                delegate: Rectangle {
                    id: voiceCard
                    required property int index
                    objectName: "novaVoiceCard:" + (index + 1)
                    width: cards.cardWidth
                    height: cards.cardHeight
                    radius: page.theme.radiusXl
                    color: page.theme.card2
                    border.color: page.theme.line
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "Voice " + (index + 1)
                    Text {
                        x: page.theme.voicesVoiceCardInset
                        y: page.theme.spaceMd
                        text: "V" + (voiceCard.index + 1)
                        color: page.theme.text
                        font.family: page.theme.mono
                        font.pixelSize: page.theme.typeLabel
                        font.weight: Font.Bold
                    }
                    Column {
                        x: page.theme.voicesVoiceCardInset
                        y: page.theme.voicesVoiceCardPartsTop
                        width: parent.width - 2 * page.theme.voicesVoiceCardInset
                        spacing: page.theme.voicesVoiceCardPartSpacing
                        Repeater {
                            model: page.cardLanes
                            delegate: Item {
                                id: part
                                required property var modelData
                                readonly property var values: page.cardValues[modelData.id] !== undefined
                                    ? page.cardValues[modelData.id] : []
                                readonly property var bar: voiceCard.index < values.length ? values[voiceCard.index] : null
                                readonly property real value: bar !== null ? Number(bar.value) : 0
                                readonly property bool edited: bar !== null && bar.edited === true
                                width: parent ? parent.width : 0
                                height: page.theme.voicesVoiceCardPartHeight
                                Text {
                                    text: part.modelData.short
                                    color: page.theme.faint
                                    font.family: page.theme.mono
                                    font.pixelSize: page.theme.typeSmall
                                }
                                Text {
                                    anchors.right: parent.right
                                    text: Contract.laneText(part.modelData.id, part.value)
                                    color: part.edited ? page.theme.am : page.theme.text
                                    font.family: page.theme.mono
                                    font.pixelSize: page.theme.typeSmall
                                }
                                // A small bipolar bar under the value.
                                Rectangle {
                                    y: parent.height - page.theme.voicesPartBarBottom
                                    width: parent.width
                                    height: page.theme.voicesPartBarHeight
                                    color: page.theme.line
                                    Rectangle {
                                        x: parent.width / 2 + Math.min(0, part.value) * parent.width / 2
                                        width: Math.abs(part.value) * parent.width / 2
                                        height: parent.height
                                        color: part.edited ? page.theme.am : page.theme.vi
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // How it works.
        Row {
            x: page.theme.spaceLg
            y: cards.y + cards.cardHeight + page.theme.spaceXl
            spacing: page.theme.spaceXl
            Repeater {
                model: [{ title: "FIXED PER VOICE, REPEATABLE",
                          text: "Offsets come from the seed and the voice's number, so playback and export match. New synth rolls a new set." },
                        { title: "AGE SCALES, LANES REFINE",
                          text: "Tolerances set how far parts can differ and Age scales them together. Painted voices keep their values." },
                        { title: "DRIFT MOVES, TOLERANCES STAY",
                          text: "Drift is a slow, separate wander per voice on pitch and cutoff. Free-running oscillators never start chords in phase." }]
                delegate: Column {
                    id: how
                    required property var modelData
                    width: (cardsCard.width - 2 * page.theme.spaceLg - 2 * page.theme.spaceXl) / 3
                    spacing: page.theme.spaceXs
                    Label { text: how.modelData.title }
                    Note {
                        width: parent.width
                        text: how.modelData.text
                    }
                }
            }
        }
    }
}
