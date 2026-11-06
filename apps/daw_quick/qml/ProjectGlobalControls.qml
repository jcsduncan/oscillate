pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: controls
    required property var bridge
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor

    // Two cards side by side: tempo (with a tap button) and meter.
    readonly property int cardHeight: 42
    readonly property int tempoCardWidth: 105
    readonly property int meterCardWidth: 58
    readonly property int labelY: 5
    readonly property int tempoTextX: 10
    readonly property int tempoValueY: 18
    readonly property int tempoEntryHeight: 21
    readonly property int meterTextX: 9
    readonly property int meterEntryX: 7
    readonly property int meterEntryRightInset: 12
    readonly property int meterValueY: 19
    readonly property int meterEntryHeight: 19
    readonly property int tapWidth: 28
    readonly property int tapHeight: 30
    readonly property int tapInset: 5
    // A vertical drag: how far before it counts, and pixels per tempo step.
    readonly property int dragThreshold: 3
    readonly property int dragPixelsPerStep: 2
    readonly property real fineTempoStep: 0.1

    width: tempoCardWidth + Theme.spaceMd + meterCardWidth
    height: cardHeight

    // The project may contain later tempo events. This value is the tempo at
    // bar 1, not an estimate of the tempo at the current playhead position.
    readonly property real initialBpm: Number(bridge.projectTiming.projectBpm || 0)
    readonly property string projectMeter: String(bridge.projectTiming.projectMeter || "")
    property bool editingTempo: false
    property bool invalidTempo: false
    property bool editingMeter: false
    property bool invalidMeter: false
    function beginTempoEdit() {
        if (!bridge.projectOpen)
            return
        tempoEntry.text = initialBpm > 0 ? initialBpm.toFixed(2) : ""
        invalidTempo = false
        editingTempo = true
        tempoEntry.forceActiveFocus()
        tempoEntry.selectAll()
    }

    // leaving: focus moved away, so an invalid entry is dropped instead of
    // pulling focus back.
    function commitTempoEdit(leaving) {
        if (!editingTempo)
            return
        const bpm = Number(tempoEntry.text)
        if (tempoEntry.text.trim().length === 0 || !Number.isFinite(bpm)
                || bpm <= 0 || bpm > 1000) {
            if (leaving) {
                cancelTempoEdit()
                return
            }
            invalidTempo = true
            tempoEntry.selectAll()
            return
        }
        editingTempo = false
        bridge.set_initial_tempo_bpm(bpm)
    }

    function cancelTempoEdit() {
        editingTempo = false
        invalidTempo = false
    }

    // Dragging the readout up or down changes the tempo (1 BPM per 2 px,
    // 0.1 with Shift), heard on release; the wheel steps it the same way.
    property real dragBpm: -1
    readonly property real shownBpm: dragBpm > 0 ? dragBpm : initialBpm
    function clampBpm(bpm) { return Math.max(20, Math.min(999, Math.round(bpm * 100) / 100)) }

    // Tap tempo: the average gap of the last four taps (a pause of two
    // seconds starts again).
    property var taps: []
    function tap() {
        const now = Date.now()
        const recent = taps.length > 0 && now - taps[taps.length - 1] > 2000 ? [] : taps.slice(-3)
        recent.push(now)
        taps = recent
        if (recent.length < 4)
            return
        const gap = (recent[recent.length - 1] - recent[0]) / (recent.length - 1)
        if (gap > 0)
            bridge.set_initial_tempo_bpm(clampBpm(60000 / gap))
    }

    function beginMeterEdit() {
        if (!bridge.projectOpen)
            return
        meterEntry.text = projectMeter
        invalidMeter = false
        editingMeter = true
        meterEntry.forceActiveFocus()
        meterEntry.selectAll()
    }

    function commitMeterEdit(leaving) {
        if (!editingMeter)
            return
        const parts = /^([0-9]{1,2})\s*\/\s*([0-9]{1,2})$/.exec(meterEntry.text.trim())
        const numerator = parts ? Number(parts[1]) : 0
        const denominator = parts ? Number(parts[2]) : 0
        if (numerator < 1 || numerator > 32
                || [1, 2, 4, 8, 16, 32, 64].indexOf(denominator) < 0) {
            if (leaving) {
                cancelMeterEdit()
                return
            }
            invalidMeter = true
            meterEntry.selectAll()
            return
        }
        editingMeter = false
        bridge.set_initial_meter(numerator, denominator)
    }

    function cancelMeterEdit() {
        editingMeter = false
        invalidMeter = false
    }

    Rectangle {
        id: initialTempo
        objectName: "projectInitialTempo"
        x: 0
        anchors.verticalCenter: parent.verticalCenter
        width: controls.tempoCardWidth
        height: controls.cardHeight
        radius: Theme.radiusXl
        color: controls.raisedColor
        border.color: controls.invalidTempo ? controls.amberColor : controls.lineColor
        Accessible.role: Accessible.Button
        Accessible.name: "Project tempo at bar 1"
        Accessible.description: controls.invalidTempo ? "Enter a BPM above zero and up to one thousand"
                                : controls.initialBpm > 0
                                  ? controls.initialBpm.toFixed(2) + " beats per minute. Activate to type, drag up or down to change."
                                  : "No project tempo available"
        activeFocusOnTab: controls.bridge.projectOpen
        Keys.onReturnPressed: controls.beginTempoEdit()
        Keys.onSpacePressed: controls.beginTempoEdit()

        Text {
            x: controls.tempoTextX
            y: controls.labelY
            text: controls.invalidTempo ? "ENTER 0–1000 BPM" : "TEMPO"
            color: controls.invalidTempo ? controls.amberColor : controls.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.trackingXxs
        }
        Text {
            x: controls.tempoTextX
            y: controls.tempoValueY
            visible: !controls.editingTempo
            text: controls.shownBpm > 0 ? controls.shownBpm.toFixed(controls.dragBpm > 0 ? 1 : 2) : "—"
            color: controls.mainTextColor
            font.pixelSize: Theme.typeTitle
            font.bold: true
            font.family: Theme.monoSystem
        }
        TextInput {
            id: tempoEntry
            objectName: "projectInitialTempoEntry"
            x: controls.tempoTextX
            y: controls.tempoValueY
            width: parent.width - 2 * controls.tempoTextX
            height: controls.tempoEntryHeight
            visible: controls.editingTempo
            color: controls.mainTextColor
            selectionColor: controls.accentColor
            selectedTextColor: controls.panelColor
            font.pixelSize: Theme.typeTitle
            font.bold: true
            font.family: Theme.monoSystem
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.role: Accessible.EditableText
            Accessible.name: "Project tempo at bar 1 in beats per minute"
            Keys.onReturnPressed: controls.commitTempoEdit()
            Keys.onEnterPressed: controls.commitTempoEdit()
            Keys.onEscapePressed: controls.cancelTempoEdit()
            onActiveFocusChanged: {
                if (!activeFocus && controls.editingTempo)
                    controls.commitTempoEdit(true)
            }
        }
        // A click (or double click) types a tempo; a vertical drag or the
        // wheel changes it.
        MouseArea {
            property real pressY: 0
            property bool dragging: false
            anchors.fill: parent
            anchors.rightMargin: tapButton.width + Theme.spaceXs
            enabled: !controls.editingTempo && controls.bridge.projectOpen
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                pressY = mouse.y
                dragging = false
            }
            onPositionChanged: mouse => {
                if (!pressed || controls.initialBpm <= 0)
                    return
                const delta = pressY - mouse.y
                dragging = dragging || Math.abs(delta) >= controls.dragThreshold
                if (!dragging)
                    return
                const step = (mouse.modifiers & Qt.ShiftModifier) ? controls.fineTempoStep : 1
                controls.dragBpm = controls.clampBpm(
                    controls.initialBpm + Math.round(delta / controls.dragPixelsPerStep) * step)
            }
            onReleased: {
                if (dragging && controls.dragBpm > 0 && controls.dragBpm !== controls.initialBpm)
                    controls.bridge.set_initial_tempo_bpm(controls.dragBpm)
                const wasDragging = dragging
                controls.dragBpm = -1
                dragging = false
                if (!wasDragging)
                    controls.beginTempoEdit()
            }
            onCanceled: {
                controls.dragBpm = -1
                dragging = false
            }
            onWheel: wheel => {
                if (controls.initialBpm <= 0 || wheel.angleDelta.y === 0)
                    return
                const step = (wheel.modifiers & Qt.ShiftModifier) ? controls.fineTempoStep : 1
                controls.bridge.set_initial_tempo_bpm(
                    controls.clampBpm(controls.initialBpm + (wheel.angleDelta.y > 0 ? step : -step)))
            }
        }
        Rectangle {
            id: tapButton
            objectName: "tapTempoButton"
            visible: !controls.editingTempo
            anchors.right: parent.right
            anchors.rightMargin: controls.tapInset
            anchors.verticalCenter: parent.verticalCenter
            width: controls.tapWidth
            height: controls.tapHeight
            radius: Theme.controlRadius
            color: tapArea.pressed ? controls.accentColor : tapArea.containsMouse ? controls.panelColor : "transparent"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            border.color: controls.lineColor
            activeFocusOnTab: controls.bridge.projectOpen
            Accessible.role: Accessible.Button
            Accessible.name: "Tap tempo"
            Accessible.description: "Tap four or more times in time to set the tempo"
            Keys.onSpacePressed: controls.tap()
            Keys.onReturnPressed: controls.tap()
            Text {
                anchors.centerIn: parent
                text: "TAP"
                color: tapArea.pressed ? controls.panelColor : controls.dimTextColor
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                font.pixelSize: Theme.typeMicro
                font.bold: true
            }
            MouseArea {
                id: tapArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: controls.bridge.projectOpen
                onPressed: controls.tap()
            }
        }
    }
    Rectangle {
        id: projectMeterCard
        objectName: "projectMeter"
        x: initialTempo.width + Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        width: controls.meterCardWidth
        height: controls.cardHeight
        radius: Theme.radiusXl
        color: controls.raisedColor
        border.color: controls.invalidMeter ? controls.amberColor : controls.lineColor
        Accessible.role: Accessible.Button
        Accessible.name: "Project time signature at bar 1"
        Accessible.description: controls.invalidMeter
                                ? "Use a numerator from 1 to 32 and a denominator of 1, 2, 4, 8, 16, 32 or 64"
                                : controls.projectMeter.length > 0
                                  ? controls.projectMeter + ". Activate to edit."
                                  : "No project time signature available"
        activeFocusOnTab: controls.bridge.projectOpen
        Keys.onReturnPressed: controls.beginMeterEdit()
        Keys.onSpacePressed: controls.beginMeterEdit()
        Text {
            x: controls.meterTextX
            y: controls.labelY
            text: controls.invalidMeter ? "N / D ?" : "METER"
            color: controls.invalidMeter ? controls.amberColor : controls.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.trackingXxs
        }
        Text {
            x: controls.meterTextX
            y: controls.meterValueY
            visible: !controls.editingMeter
            text: controls.projectMeter.length > 0 ? controls.projectMeter : "—"
            color: controls.mainTextColor
            font.pixelSize: Theme.typeHeading
            font.bold: true
        }
        TextInput {
            id: meterEntry
            objectName: "projectMeterEntry"
            x: controls.meterEntryX
            y: controls.meterValueY
            width: parent.width - controls.meterEntryRightInset
            height: controls.meterEntryHeight
            visible: controls.editingMeter
            color: controls.mainTextColor
            selectionColor: controls.accentColor
            selectedTextColor: controls.panelColor
            font.pixelSize: Theme.typeSubtitle
            font.bold: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Project time signature as numerator slash denominator"
            Keys.onReturnPressed: controls.commitMeterEdit()
            Keys.onEnterPressed: controls.commitMeterEdit()
            Keys.onEscapePressed: controls.cancelMeterEdit()
            onActiveFocusChanged: {
                if (!activeFocus && controls.editingMeter)
                    controls.commitMeterEdit(true)
            }
        }
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: !controls.editingMeter && controls.bridge.projectOpen
            onClicked: controls.beginMeterEdit()
        }
    }

}
