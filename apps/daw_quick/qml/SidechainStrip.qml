pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The key of one effect's sidechain input, chosen on the effect itself: the
// source track, the key gain, a live key meter and, for built-in effects
// while playing, Listen to hear the key in place of the effect's output.
// Under the hood it is the source track's sidechain send (ADR-0048 era).
Item {
    id: strip
    objectName: "sidechainStrip"
    required property var bridge
    required property string trackId
    required property string slotId
    // Built-in effects can audition their key; VST3 effects cannot.
    property bool listenable: true
    property color accentColor: Theme.deviceChromeAccent
    property color raisedColor: Theme.deviceChromeButton
    property color lineColor: Theme.deviceChromeButtonEdge
    property color textColor: Theme.deviceChromeText
    property color dimColor: Theme.textDim
    // Re-read whenever the project's tracks change.
    readonly property var state: {
        strip.bridge.tracks
        return strip.bridge.sidechain_state(strip.trackId, strip.slotId)
    }
    readonly property bool available: state.available === true
    readonly property bool keyed: state.keyed === true
    readonly property real gain: keyed ? Number(state.gain) : 1
    property bool menuOpen: false
    property bool listening: false
    property real level: 0
    property real dragGain: -1
    readonly property real shownGain: dragGain >= 0 ? dragGain : gain
    visible: available
    implicitHeight: stripHeight
    // A row: label, source button, key gain, key meter, listen.
    readonly property int stripHeight: 30
    readonly property int controlHeight: 26
    readonly property int sourceGap: 10
    readonly property int sourceWidth: 170
    readonly property int sourceTextX: 9
    readonly property int sourceArrowRoom: 26
    readonly property int gainWidth: 104
    readonly property int meterWidth: 72
    readonly property int meterHeight: 8
    readonly property int listenWidth: 62
    readonly property int menuWidth: 220
    readonly property int menuMaxRows: 8
    readonly property int menuRowHeight: 27
    // Key gain runs 0..4 (−∞ to +12 dB); arrows step about 1 dB; a drag
    // across this many pixels sweeps the whole range.
    readonly property real maxGain: 4
    readonly property real gainStep: 1.122
    readonly property real gainDragPixels: 200
    // The key meter: three decades (−60 to 0 dBFS), red near full scale.
    readonly property real meterDecades: 3
    readonly property real meterClipLevel: 0.99
    readonly property int meterMs: 33

    function gainText(value) {
        return value <= 0.0001 ? "−∞ dB" : (20 * Math.log(value) / Math.LN10).toFixed(1) + " dB"
    }
    function choose(sourceId) {
        menuOpen = false
        if (sourceId === "" && listening)
            setListening(false)
        bridge.routeCommands.set_sidechain_source(trackId, slotId, sourceId, keyed ? gain : 1)
    }
    function setListening(on) {
        listening = on
        bridge.set_effect_key_listen(trackId, slotId, on)
    }
    onKeyedChanged: if (!keyed && listening) setListening(false)
    Connections {
        target: strip.bridge
        function onPlaybackChanged() {
            if (!strip.bridge.playbackActive && strip.listening)
                strip.setListening(false)
        }
    }
    Component.onDestruction: if (listening) bridge.set_effect_key_listen(trackId, slotId, false)

    Timer {
        interval: strip.meterMs
        repeat: true
        running: strip.visible && strip.keyed && strip.bridge.playbackActive
        onRunningChanged: if (!running) strip.level = 0
        onTriggered: strip.level = strip.bridge.effect_key_level(strip.trackId, strip.slotId)
    }

    Text {
        id: label
        anchors.verticalCenter: parent.verticalCenter
        text: "SIDECHAIN"
        color: strip.keyed ? strip.accentColor : strip.dimColor
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.trackingMd
    }

    Rectangle {
        id: sourceButton
        objectName: "sidechainSourceButton"
        anchors.left: label.right
        anchors.leftMargin: strip.sourceGap
        anchors.verticalCenter: parent.verticalCenter
        width: strip.sourceWidth; height: strip.controlHeight; radius: Theme.controlRadius
        color: sourceArea.containsMouse ? Qt.lighter(strip.raisedColor, Theme.hoverLighten) : strip.raisedColor
        border.color: activeFocus || strip.menuOpen ? strip.accentColor : strip.lineColor
        activeFocusOnTab: strip.visible
        Accessible.role: Accessible.ComboBox
        Accessible.name: strip.keyed ? "Sidechain keyed from " + strip.state.keyName
                                     : "Sidechain source: none"
        Keys.onReturnPressed: strip.menuOpen = !strip.menuOpen
        Keys.onSpacePressed: strip.menuOpen = !strip.menuOpen
        Text {
            x: strip.sourceTextX; width: parent.width - strip.sourceArrowRoom
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: strip.keyed ? strip.state.keyName : "None (self)"
            color: strip.keyed ? strip.textColor : strip.dimColor
            font.pixelSize: Theme.typeBody
        }
        Text {
            anchors.right: parent.right; anchors.rightMargin: Theme.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"; color: strip.dimColor; font.pixelSize: Theme.typeSmall
        }
        MouseArea {
            id: sourceArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: strip.menuOpen = !strip.menuOpen
        }
    }

    Rectangle {
        id: gainField
        objectName: "sidechainGain"
        anchors.left: sourceButton.right
        anchors.leftMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        width: strip.gainWidth; height: strip.controlHeight; radius: Theme.controlRadius
        opacity: strip.keyed ? 1 : Theme.inactiveOpacity
        color: strip.raisedColor
        border.color: activeFocus ? strip.accentColor : strip.lineColor
        activeFocusOnTab: strip.keyed
        Accessible.role: Accessible.Slider
        Accessible.name: "Sidechain key gain " + strip.gainText(strip.shownGain)
        function commit(value) {
            strip.bridge.routeCommands.set_sidechain_source(strip.trackId, strip.slotId,
                                                            strip.state.sourceId,
                                                            Math.max(0, Math.min(strip.maxGain, value)))
        }
        Keys.onUpPressed: commit(strip.gain * strip.gainStep)
        Keys.onDownPressed: commit(strip.gain / strip.gainStep)
        // The fill shows the gain on a 0..4 (−∞ to +12 dB) scale.
        Rectangle {
            x: Theme.hairline; y: Theme.hairline; height: parent.height - 2 * Theme.hairline
            width: (parent.width - 2 * Theme.hairline) * Math.sqrt(strip.shownGain / strip.maxGain)
            radius: Theme.radiusMd
            color: Qt.rgba(strip.accentColor.r, strip.accentColor.g, strip.accentColor.b, Theme.keyGainFillAlpha)
        }
        Text {
            anchors.centerIn: parent
            text: "KEY " + strip.gainText(strip.shownGain)
            color: strip.textColor
            font.pixelSize: Theme.typeSmall
        }
        MouseArea {
            anchors.fill: parent
            enabled: strip.keyed
            cursorShape: Qt.SizeHorCursor
            property real startX: 0
            property real startGain: 1
            onPressed: mouse => { startX = mouse.x; startGain = strip.gain; strip.dragGain = startGain }
            onPositionChanged: mouse => {
                if (!pressed) return
                // Drag in the square-root domain so unity sits mid-way.
                const root = Math.max(0, Math.min(1, Math.sqrt(startGain / strip.maxGain) + (mouse.x - startX) / strip.gainDragPixels))
                strip.dragGain = strip.maxGain * root * root
            }
            onReleased: {
                const value = strip.dragGain
                strip.dragGain = -1
                if (Math.abs(value - strip.gain) > 0.0001) gainField.commit(value)
            }
            onCanceled: strip.dragGain = -1
            onDoubleClicked: gainField.commit(1)
        }
    }

    // The key arriving at the effect, while playing.
    Rectangle {
        id: keyMeter
        objectName: "sidechainKeyMeter"
        anchors.left: gainField.right
        anchors.leftMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        width: strip.meterWidth; height: strip.meterHeight; radius: Theme.radiusMd
        color: Theme.keyMeterTrack
        border.color: strip.lineColor
        opacity: strip.keyed ? 1 : Theme.inactiveOpacity
        Rectangle {
            x: Theme.hairline; y: Theme.hairline; height: parent.height - 2 * Theme.hairline; radius: Theme.radiusSm
            // −60 dBFS to 0 dBFS.
            width: (parent.width - 2 * Theme.hairline) * Math.max(0, Math.min(1,
                strip.level > 0 ? 1 + Math.log(strip.level) / Math.LN10 / strip.meterDecades : 0))
            color: strip.level >= strip.meterClipLevel ? Theme.meterPeakHot : strip.accentColor
        }
    }

    Rectangle {
        id: listenButton
        objectName: "sidechainListen"
        visible: strip.listenable
        anchors.left: keyMeter.right
        anchors.leftMargin: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        width: strip.listenWidth; height: strip.controlHeight; radius: Theme.controlRadius
        readonly property bool usable: strip.keyed && strip.bridge.playbackActive
        opacity: usable ? 1 : Theme.inactiveOpacity
        color: strip.listening ? strip.accentColor : strip.raisedColor
        border.color: activeFocus ? strip.accentColor : strip.lineColor
        activeFocusOnTab: usable
        Accessible.role: Accessible.Button
        Accessible.name: strip.listening ? "Stop listening to the sidechain key"
                                         : "Listen to the sidechain key"
        Keys.onReturnPressed: if (usable) strip.setListening(!strip.listening)
        Keys.onSpacePressed: if (usable) strip.setListening(!strip.listening)
        Text {
            anchors.centerIn: parent
            text: "LISTEN"
            color: strip.listening ? Theme.listenOnText : strip.textColor
            font.pixelSize: Theme.typeSmall
            font.bold: strip.listening
        }
        MouseArea {
            anchors.fill: parent
            enabled: listenButton.usable
            onClicked: strip.setListening(!strip.listening)
        }
    }

    Rectangle {
        objectName: "sidechainSourceMenu"
        visible: strip.menuOpen
        x: sourceButton.x
        y: -height - Theme.spaceXs
        z: 40
        width: strip.menuWidth
        height: Math.min(strip.menuMaxRows, menuList.count) * strip.menuRowHeight + 2 * Theme.spaceXxs
        radius: Theme.radiusLg
        color: Theme.sourceMenuFill
        border.color: strip.accentColor
        ListView {
            id: menuList
            anchors.fill: parent
            anchors.margins: Theme.spaceXxs
            clip: true
            model: [{ id: "", name: "None (self)" }].concat(strip.state.sources || [])
            delegate: Rectangle {
                id: choice
                required property var modelData
                objectName: "sidechainSourceChoice"
                width: ListView.view.width
                height: strip.menuRowHeight
                radius: Theme.radiusMd
                readonly property bool current: strip.keyed ? modelData.id === strip.state.sourceId
                                                            : modelData.id === ""
                color: choiceArea.containsMouse ? Theme.sourceMenuHover : "transparent"
                Text {
                    x: strip.sourceTextX; width: parent.width - 2 * strip.sourceTextX
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: (choice.current ? "● " : "   ") + choice.modelData.name
                    color: choice.current ? strip.accentColor : strip.textColor
                    font.pixelSize: Theme.typeBody
                }
                MouseArea {
                    id: choiceArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: strip.choose(choice.modelData.id)
                }
            }
        }
    }
}
