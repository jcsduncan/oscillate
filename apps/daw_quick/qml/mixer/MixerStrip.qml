pragma ComponentBehavior: Bound
import QtQuick
import ".."
import "../shell"

// One mixer channel: colour cap, name and kind, inserts, sends (one knob per
// return), pan, fader with stereo peak meters, M/S and its output routing.
// `strip` is MixerView's projection of a bridge track (or the master).
Rectangle {
    id: strip
    required property var bridge
    required property var channel
    required property var routeOptions
    property color ink: Theme.textMain
    property color inkDim: Theme.textDim
    property color line: Theme.line
    property color raised: Theme.panelRaised
    property real levelGesture: 0
    property real panGesture: 0
    property bool routeMenuOpen: false
    readonly property bool master: strip.channel.master === true
    readonly property bool returnBus: strip.channel.returnBus === true
    readonly property var data_: strip.channel
    // Live level: the master's output meter, otherwise this track's routed
    // post-fader meter. Absent (no fill) until the routed graph publishes one.
    readonly property var meter: bridge.meters.channel(master ? "" : data_.id)
    readonly property bool meterValid: !!meter && meter.valid

    // The strip's own sizes and colours.
    readonly property int stripWidth: 172
    readonly property int masterWidth: 220
    readonly property real stripRadius: 12
    readonly property color stripFill: Theme.mixerStripFill
    readonly property color masterFill: Theme.mixerMasterFill
    readonly property int capHeight: 6
    // The rename field reaches this far past the name block.
    readonly property real editorOutsetX: 5
    readonly property real editorOutsetY: 3
    readonly property int editorHeight: 22
    // Source and insert slots: height, gap, hover edge, the colour dot and
    // the name after it.
    readonly property int slotHeight: 24
    readonly property int slotSpacing: 3
    readonly property color slotHoverEdge: Theme.mixerSlotHoverEdge
    readonly property color addSlotHover: Theme.mixerAddSlotHover
    readonly property real slotOffOpacity: 0.5
    readonly property real slotDotSize: 6
    readonly property real slotTextX: 20
    readonly property real slotTextRight: 28
    readonly property int knobWidth: 52
    // A pan this near the centre reads C.
    readonly property real panCentreTolerance: 0.005
    // M and S keys.
    readonly property real footerInset: 10
    readonly property int keyHeight: 30
    readonly property real keyRadius: 7
    readonly property real keyHoverLift: 1.3
    readonly property color keyIdleInk: Theme.mixerKeyIdleInk
    // The output row and the menu it opens above it.
    readonly property int routeHeight: 30
    readonly property real routeMenuInset: 6
    readonly property real routeMenuPad: 2
    readonly property real routeMenuGap: 2
    readonly property color routeMenuFill: Theme.mixerRouteMenuFill
    readonly property color routeMenuEdge: Theme.mixerRouteMenuEdge
    readonly property int routeChoiceHeight: 28
    readonly property color routeChoiceHover: Theme.mixerRouteChoiceHover
    readonly property real routeChoiceX: 10
    // Sends: the dB floor and span of a send knob, the position that turns a
    // send off, and the smallest change worth committing.
    readonly property real sendFloorDb: -60
    readonly property real sendSpanDb: 72
    readonly property real sendOffBelow: 0.0005
    readonly property real unchangedEpsilon: 0.00001

    signal insertActivated(string pathId, var insert)
    signal addInsertRequested(string pathId)
    signal sourceActivated(string trackId)

    objectName: "mixerStrip:" + data_.id
    width: master ? masterWidth : stripWidth
    radius: stripRadius
    color: master ? masterFill : stripFill
    border.color: master ? Theme.lineStrong : line
    clip: true
    Accessible.role: Accessible.Grouping
    Accessible.name: data_.name + " channel"

    function db(level) {
        return level <= 0 ? "−∞" : (20 * Math.log10(level) >= 0 ? "+" : "")
            + (20 * Math.log10(level)).toFixed(1).replace("-", "−")
    }
    function sendPosition(gain) {
        return gain <= 0 ? 0 : Math.max(0, Math.min(1, (20 * Math.log10(gain) - sendFloorDb) / sendSpanDb))
    }
    function sendGainAt(position) {
        return position <= sendOffBelow ? 0
            : Math.pow(10, (sendFloorDb + sendSpanDb * Math.max(0, Math.min(1, position))) / 20)
    }
    function sendText(gain) {
        return gain <= 0 ? "off" : db(gain) + " dB"
    }
    function commitLevel(value, gesture) {
        if (Math.abs(value - data_.level) <= unchangedEpsilon)
            return
        if (master)
            bridge.trackCommands.set_master_level_gesture(value, gesture)
        else
            bridge.trackCommands.set_track_level_pan_gesture(data_.id, value, data_.pan, gesture)
    }
    function commitPan(value, gesture) {
        if (Math.abs(value - data_.pan) <= unchangedEpsilon)
            return
        bridge.trackCommands.set_track_level_pan_gesture(data_.id, data_.level, value, gesture)
    }
    function commitSend(send, gain) {
        if (send.sendId)
            bridge.routeCommands.set_send_gain(send.sendId, gain)
        else if (gain > 0)
            bridge.routeCommands.add_send_route(data_.id, send.returnId, gain)
    }

    Rectangle {
        id: capBar
        width: parent.width
        height: strip.capHeight
        color: strip.data_.color
    }

    Column {
        id: head
        y: capBar.height + Theme.spaceMd
        x: Theme.spaceLg
        width: parent.width - 2 * Theme.spaceLg
        spacing: 1
        Text {
            id: stripName
            objectName: "mixerStripName"
            width: parent.width
            text: strip.data_.name
            color: strip.ink
            font.pixelSize: Theme.typeEmphasis
            font.bold: true
            elide: Text.ElideRight
            // Double-click a track's name to rename it (not the master's).
            MouseArea {
                anchors.fill: parent
                enabled: !strip.master
                onDoubleClicked: stripNameEditor.begin(strip.data_.name)
            }
        }
        Text {
            width: parent.width
            text: strip.data_.kindLabel
            color: strip.inkDim
            font.pixelSize: Theme.typeBody
            elide: Text.ElideRight
        }
    }

    InlineNameEditor {
        id: stripNameEditor
        objectName: "mixerStripNameEditor"
        z: 5
        x: head.x - strip.editorOutsetX
        y: head.y - strip.editorOutsetY
        width: head.width + 2 * strip.editorOutsetX
        height: strip.editorHeight
        textColor: strip.ink
        onCommitted: name => strip.bridge.set_track_name(strip.data_.id, name)
    }

    // Instrument source, then inserts, in chain order.
    Column {
        id: inserts
        y: head.y + head.height + Theme.spaceMd
        x: Theme.spaceMd
        width: parent.width - 2 * Theme.spaceMd
        spacing: strip.slotSpacing
        Rectangle {
            visible: strip.data_.sourceName !== ""
            width: parent.width
            height: visible ? strip.slotHeight : 0
            radius: Theme.radiusLg
            color: Theme.well
            border.color: sourceArea.containsMouse || sourceButton.activeFocus ? strip.slotHoverEdge : strip.line
            Behavior on border.color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Text {
                x: Theme.spaceMd
                width: parent.width - 2 * Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                text: "◆ " + strip.data_.sourceName
                color: strip.ink
                font.pixelSize: Theme.typeBody
                elide: Text.ElideRight
            }
            Item {
                id: sourceButton
                anchors.fill: parent
                activeFocusOnTab: parent.visible
                Accessible.role: Accessible.Button
                Accessible.name: "Open " + strip.data_.sourceName
                Keys.onReturnPressed: strip.sourceActivated(strip.data_.id)
                Keys.onSpacePressed: strip.sourceActivated(strip.data_.id)
            }
            MouseArea {
                id: sourceArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: strip.sourceActivated(strip.data_.id)
                Tip { text: "Open " + strip.data_.sourceName + " in the device chain" }
            }
        }
        Repeater {
            model: strip.data_.inserts
            delegate: Rectangle {
                id: insertRow
                required property var modelData
                objectName: "mixerInsert:" + modelData.slotId
                width: inserts.width
                height: strip.slotHeight
                radius: Theme.radiusLg
                color: Theme.well
                opacity: modelData.enabled ? 1 : strip.slotOffOpacity
                border.color: insertArea.containsMouse || insertFocus.activeFocus ? strip.slotHoverEdge : strip.line
                Behavior on border.color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                Rectangle {
                    x: Theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    width: strip.slotDotSize
                    height: width
                    radius: width / 2
                    color: insertRow.modelData.color
                }
                Text {
                    x: strip.slotTextX
                    width: parent.width - strip.slotTextRight
                    anchors.verticalCenter: parent.verticalCenter
                    text: insertRow.modelData.name
                    color: strip.ink
                    font.pixelSize: Theme.typeBody
                    elide: Text.ElideRight
                }
                Item {
                    id: insertFocus
                    anchors.fill: parent
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Open " + insertRow.modelData.name
                    Keys.onReturnPressed: strip.insertActivated(strip.data_.pathId, insertRow.modelData)
                    Keys.onSpacePressed: strip.insertActivated(strip.data_.pathId, insertRow.modelData)
                }
                MouseArea {
                    id: insertArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: strip.insertActivated(strip.data_.pathId, insertRow.modelData)
                    Tip {
                        text: "Open " + insertRow.modelData.name
                              + (insertRow.modelData.enabled ? "" : " (switched off)")
                    }
                }
            }
        }
        Rectangle {
            objectName: "mixerAddInsert:" + strip.data_.id
            width: parent.width
            height: strip.slotHeight
            radius: Theme.radiusLg
            color: addTip.hovered ? strip.addSlotHover : "transparent"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            border.color: addFocus.activeFocus ? Theme.accent : Theme.fieldEdge
            Text {
                anchors.centerIn: parent
                text: "+ Insert"
                color: Theme.textFaint
                font.pixelSize: Theme.typeBody
            }
            Item {
                id: addFocus
                anchors.fill: parent
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Add an insert to " + strip.data_.name
                Keys.onReturnPressed: strip.addInsertRequested(strip.data_.pathId)
                Keys.onSpacePressed: strip.addInsertRequested(strip.data_.pathId)
            }
            MouseArea {
                anchors.fill: parent
                onClicked: strip.addInsertRequested(strip.data_.pathId)
                Tip { id: addTip; text: "Add an effect to " + strip.data_.name + " from the browser" }
            }
        }
    }

    // One row of knobs: pan, then a send per return, each labelled with its
    // return's name and drawn in its colour.
    Row {
        id: knobRow
        y: inserts.y + inserts.height + Theme.spaceLg
        anchors.horizontalCenter: parent.horizontalCenter
        visible: !strip.master
        height: visible ? implicitHeight : 0
        spacing: 0
        MixerKnob {
            id: pan
            objectName: "mixerPan:" + strip.data_.id
            width: strip.knobWidth
            value: (strip.data_.pan + 1) / 2
            origin: 0.5
            accent: strip.data_.color
            label: "PAN"
            valueText: {
                const value = pan.preview >= 0 ? pan.preview * 2 - 1 : strip.data_.pan
                return Math.abs(value) < strip.panCentreTolerance ? "C" : (value < 0 ? "L" : "R") + Math.round(Math.abs(value) * 100)
            }
            accessibleName: strip.data_.name + " pan"
            onBegun: strip.panGesture = strip.bridge.trackCommands.begin_channel_gesture()
            // Heard at once while dragging; the release saves one edit.
            onMoved: function(value) { strip.bridge.preview_track_level_pan(strip.data_.id, strip.data_.level, value * 2 - 1) }
            onCommitted: function(value) { strip.commitPan(value * 2 - 1, strip.panGesture); strip.panGesture = 0 }
            onResetRequested: strip.commitPan(0, strip.bridge.trackCommands.begin_channel_gesture())
        }
        Row {
            id: sends
            objectName: "mixerSends:" + strip.data_.id
            visible: !strip.returnBus && strip.data_.sends.length > 0
            Repeater {
                model: (strip.data_.sends || []).length
                delegate: MixerKnob {
                    id: sendKnob
                    required property int index
                    readonly property var modelData: strip.data_.sends[index] || ({})
                    // "A · Reverb" reads REVERB; an unnamed return keeps its letter.
                    readonly property string returnName: {
                        const parts = String(sendKnob.modelData.name || "").split("·")
                        const name = parts[parts.length - 1].trim()
                        return name.length > 0 ? name.toUpperCase() : String(sendKnob.modelData.letter)
                    }
                    objectName: "mixerSend:" + strip.data_.id + ":" + sendKnob.modelData.letter
                    width: strip.knobWidth
                    value: strip.sendPosition(sendKnob.modelData.gain)
                    origin: 0
                    accent: sendKnob.modelData.color
                    label: sendKnob.returnName
                    valueText: strip.sendText(sendKnob.preview >= 0 ? strip.sendGainAt(sendKnob.preview)
                                                                   : sendKnob.modelData.gain)
                    accessibleName: "Send " + sendKnob.modelData.letter + " to " + sendKnob.modelData.name
                    // Heard at once while dragging; one commit per gesture.
                    onMoved: function(value) {
                        if (sendKnob.modelData.sendId)
                            strip.bridge.preview_send_gain(sendKnob.modelData.sendId, strip.sendGainAt(value))
                    }
                    onCommitted: function(value) {
                        strip.commitSend(sendKnob.modelData, strip.sendGainAt(value))
                    }
                    onResetRequested: strip.commitSend(sendKnob.modelData, 0)
                }
            }
        }
    }

    // Fader and meters.
    Item {
        id: faderArea
        y: strip.master ? inserts.y + inserts.height + Theme.spaceLg : knobRow.y + knobRow.height + Theme.spaceMd
        width: parent.width
        height: footer.y - y - Theme.spaceMd
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            height: parent.height
            spacing: Theme.spaceMd
            MixerMeter {
                objectName: "mixerMeterL:" + strip.data_.id
                height: parent.height
                inset: fader.travelInset
                peak: strip.meterValid ? strip.meter.leftPeak : 0
                hold: strip.meterValid ? strip.meter.leftHold : 0
                valid: strip.meterValid
                clipped: strip.meterValid && strip.meter.clipped
            }
            MixerMeter {
                objectName: "mixerMeterR:" + strip.data_.id
                height: parent.height
                inset: fader.travelInset
                peak: strip.meterValid ? strip.meter.rightPeak : 0
                hold: strip.meterValid ? strip.meter.rightHold : 0
                valid: strip.meterValid
                clipped: strip.meterValid && strip.meter.clipped
            }
            MixerScale {
                objectName: "mixerScale:" + strip.data_.id
                height: parent.height
                inset: fader.travelInset
            }
            MixerFader {
                id: fader
                objectName: "mixerFader:" + strip.data_.id
                height: parent.height
                level: strip.data_.level
                accent: strip.data_.color
                accessibleName: strip.data_.name + " level"
                onBegun: strip.levelGesture = strip.bridge.trackCommands.begin_channel_gesture()
                // Heard at once while dragging; the release saves one edit.
                onMoved: function(value) {
                    if (strip.master)
                        strip.bridge.preview_master_level(value)
                    else
                        strip.bridge.preview_track_level_pan(strip.data_.id, value, strip.data_.pan)
                }
                onCommitted: function(value) { strip.commitLevel(value, strip.levelGesture); strip.levelGesture = 0 }
                onResetRequested: strip.commitLevel(1, strip.bridge.trackCommands.begin_channel_gesture())
            }
        }
    }

    Column {
        id: footer
        y: strip.height - height
        width: parent.width
        spacing: Theme.spaceSm
        Text {
            objectName: "mixerLevelText:" + strip.data_.id
            anchors.horizontalCenter: parent.horizontalCenter
            text: strip.db(fader.shown) + " dB"
            color: strip.ink
            font.pixelSize: Theme.typeLabel
            font.family: Theme.monoFamily
        }
        Text {
            visible: strip.master
            anchors.horizontalCenter: parent.horizontalCenter
            text: strip.data_.truePeakText
            color: strip.inkDim
            font.pixelSize: Theme.typeBody
            font.family: Theme.monoFamily
        }
        Row {
            visible: !strip.master
            x: strip.footerInset
            width: parent.width - 2 * strip.footerInset
            spacing: Theme.spaceSm
            Repeater {
                model: [
                    { key: "M", on: strip.data_.muted, onColor: Theme.amber, name: "Mute" },
                    { key: "S", on: strip.data_.soloed, onColor: Theme.accent, name: "Solo" }
                ]
                delegate: Rectangle {
                    id: toggle
                    required property var modelData
                    objectName: "mixer" + modelData.name + ":" + strip.data_.id
                    width: parent ? (parent.width - Theme.spaceSm) / 2 : 0
                    height: strip.keyHeight
                    radius: strip.keyRadius
                    color: modelData.on ? modelData.onColor
                        : toggleTip.hovered ? Qt.lighter(strip.raised, strip.keyHoverLift) : strip.raised
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
                    border.color: toggleFocus.activeFocus ? Theme.accent : Theme.fieldEdge
                    function activate() {
                        if (modelData.key === "M")
                            strip.bridge.trackCommands.set_track_muted(strip.data_.id, !strip.data_.muted)
                        else
                            strip.bridge.trackCommands.set_track_soloed(strip.data_.id, !strip.data_.soloed)
                    }
                    Text {
                        anchors.centerIn: parent
                        text: toggle.modelData.key
                        color: toggle.modelData.on ? Theme.accentInk : strip.keyIdleInk
                        font.pixelSize: Theme.typeLabel
                        font.bold: true
                    }
                    Item {
                        id: toggleFocus
                        anchors.fill: parent
                        activeFocusOnTab: true
                        Accessible.role: Accessible.CheckBox
                        Accessible.checkable: true
                        Accessible.checked: toggle.modelData.on
                        Accessible.name: toggle.modelData.name + " " + strip.data_.name
                        Keys.onReturnPressed: toggle.activate()
                        Keys.onSpacePressed: toggle.activate()
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: toggle.activate()
                        Tip {
                            id: toggleTip
                            text: (toggle.modelData.on ? "Un" + toggle.modelData.name.toLowerCase()
                                                       : toggle.modelData.name) + " " + strip.data_.name
                            shortcut: toggle.modelData.key
                        }
                    }
                }
            }
        }
        Rectangle {
            width: parent.width
            height: strip.routeHeight
            color: "transparent"
            Rectangle { width: parent.width; height: Theme.hairline; color: strip.line }
            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.spaceMd
                horizontalAlignment: Text.AlignHCenter
                text: strip.master ? "Speakers" : "→ " + strip.data_.mainName + (strip.routeOptions.length > 1 ? "  ▾" : "")
                color: routeFocus.activeFocus ? Theme.textMain : strip.inkDim
                font.pixelSize: Theme.typeBody
                elide: Text.ElideRight
            }
            Item {
                id: routeFocus
                objectName: "mixerRoute:" + strip.data_.id
                anchors.fill: parent
                activeFocusOnTab: !strip.master && strip.routeOptions.length > 1
                Accessible.role: Accessible.ButtonMenu
                Accessible.name: strip.data_.name + " output: " + strip.data_.mainName
                Keys.onReturnPressed: strip.routeMenuOpen = !strip.routeMenuOpen
                Keys.onSpacePressed: strip.routeMenuOpen = !strip.routeMenuOpen
                Keys.onEscapePressed: strip.routeMenuOpen = false
            }
            MouseArea {
                anchors.fill: parent
                enabled: !strip.master && strip.routeOptions.length > 1
                onClicked: strip.routeMenuOpen = !strip.routeMenuOpen
                Tip { text: "Where " + strip.data_.name + " goes: the master or a group" }
            }
        }
    }

    // The output menu opens upward over the strip.
    Rectangle {
        id: routeMenu
        objectName: "mixerRouteMenu:" + strip.data_.id
        visible: strip.routeMenuOpen
        transformOrigin: Item.Bottom
        Reveal { target: routeMenu; fromScale: Motion.popScale; duration: Motion.fast }
        z: 10
        x: strip.routeMenuInset
        width: strip.width - 2 * strip.routeMenuInset
        height: routeList.contentHeight + 2 * strip.routeMenuPad
        y: strip.height - strip.routeHeight - height - strip.routeMenuGap
        radius: Theme.radiusXl
        color: strip.routeMenuFill
        border.color: strip.routeMenuEdge
        ListView {
            id: routeList
            anchors.fill: parent
            anchors.margins: strip.routeMenuPad
            interactive: false
            model: strip.routeOptions
            delegate: Rectangle {
                id: choice
                required property var modelData
                objectName: "mixerRouteChoice:" + modelData.name
                width: ListView.view.width
                height: strip.routeChoiceHeight
                radius: Theme.radiusLg
                color: choiceArea.containsMouse ? strip.routeChoiceHover : "transparent"
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                Text {
                    x: strip.routeChoiceX
                    anchors.verticalCenter: parent.verticalCenter
                    text: "→ " + choice.modelData.name
                    color: Theme.textMain
                    font.pixelSize: Theme.typeLabel
                }
                MouseArea {
                    id: choiceArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        strip.routeMenuOpen = false
                        strip.bridge.routeCommands.set_main_route(strip.data_.id, choice.modelData.id)
                    }
                }
            }
        }
    }
}
