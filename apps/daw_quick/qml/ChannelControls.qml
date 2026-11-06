pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: channel
    required property var bridge
    required property var track
    required property color panelRaised
    required property color lineColor
    required property color textMain
    required property color textDim
    required property color accent
    property bool masterMode: false
    // Rows: level label and bar, then balance label and bar.
    readonly property int levelLabelY: 17
    readonly property int levelBarY: 34
    readonly property int panLabelY: 62
    readonly property int panBarY: 79
    readonly property int barHeight: 18
    readonly property int thumbWidth: 6
    // Shift-drag moves this fraction as far.
    readonly property real fineDragScale: 0.25
    readonly property real savedLevel: Number(track.level ?? 1)
    readonly property real savedPan: Number(track.pan ?? 0)
    property real levelPreview: -1
    property real panPreview: 2
    property real levelGesture: 0
    property real panGesture: 0
    readonly property real shownLevel: levelPreview >= 0 ? levelPreview : savedLevel
    readonly property real shownPan: panPreview <= 1 ? panPreview : savedPan

    function clamp(value) { return Math.max(0, Math.min(1, value)) }
    function levelPosition(value) {
        return value <= 0 ? 0 : clamp((20 * Math.log10(value) + 60) / 72)
    }
    function levelAt(position) {
        return position <= 0 ? 0 : Math.pow(10, (-60 + 72 * clamp(position)) / 20)
    }
    function commitLevel(value, gesture) {
        if (Math.abs(value - savedLevel) > 0.00001) {
            if (masterMode) {
                if (gesture)
                    bridge.trackCommands.set_master_level_gesture(value, gesture)
                else
                    bridge.trackCommands.set_master_level(value)
            } else if (gesture) {
                bridge.trackCommands.set_track_level_pan_gesture(track.id, value, savedPan, gesture)
            } else {
                bridge.trackCommands.set_track_level_pan(track.id, value, savedPan)
            }
        }
    }
    // Heard at once while dragging; the release saves one edit.
    function previewLevel(value) {
        if (masterMode)
            bridge.preview_master_level(value)
        else
            bridge.preview_track_level_pan(track.id, value, savedPan)
    }
    function previewPan(value) { bridge.preview_track_level_pan(track.id, savedLevel, value) }
    function commitPan(value, gesture) {
        if (Math.abs(value - savedPan) <= 0.00001)
            return
        if (gesture)
            bridge.trackCommands.set_track_level_pan_gesture(track.id, savedLevel, value, gesture)
        else
            bridge.trackCommands.set_track_level_pan(track.id, savedLevel, value)
    }

    Text {
        text: channel.masterMode ? "MASTER OUTPUT" : "CHANNEL"
        color: channel.textDim
        font.pixelSize: Theme.typeCaption
        font.bold: true
        font.letterSpacing: Theme.trackingMd
    }
    Text {
        y: channel.levelLabelY
        text: "LEVEL"
        color: channel.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: parent.width - width
        y: channel.levelLabelY
        text: channel.shownLevel === 0 ? "−∞ dB"
            : (20 * Math.log10(channel.shownLevel)).toFixed(1) + " dB"
        color: channel.textMain
        font.pixelSize: Theme.typeSmall
    }
    Item {
        id: levelBar
        objectName: channel.masterMode ? "masterLevelControl" : "trackLevelControl"
        y: channel.levelBarY
        width: parent.width
        height: channel.barHeight
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: channel.masterMode ? "Master output level in decibels" : "Track level in decibels"
        Keys.onLeftPressed: channel.commitLevel(channel.levelAt(channel.levelPosition(channel.savedLevel) - 1 / 72))
        Keys.onRightPressed: channel.commitLevel(channel.levelAt(channel.levelPosition(channel.savedLevel) + 1 / 72))
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Home) {
                channel.commitLevel(0)
                event.accepted = true
            }
        }
        Rectangle { anchors.fill: parent; radius: Theme.controlRadius; color: channel.panelRaised; border.color: levelBar.activeFocus ? channel.accent : channel.lineColor }
        Rectangle {
            x: Theme.hairline; y: Theme.hairline; height: parent.height - 2 * Theme.hairline
            width: Math.max(0, (parent.width - 2 * Theme.hairline) * channel.levelPosition(channel.shownLevel))
            radius: Theme.radiusMd; color: channel.accent
        }
        Rectangle {
            x: Math.max(0, Math.min(parent.width - width, parent.width * channel.levelPosition(channel.shownLevel) - width / 2))
            y: 0; width: channel.thumbWidth; height: parent.height; radius: Theme.radiusSm; color: channel.textMain
        }
        // Drags move the level from where it is (a click never jumps it);
        // Shift is finer; a double click resets to 0 dB.
        MouseArea {
            Tip {}
            property real pressX: 0
            property real startPosition: 0
            property bool resetting: false
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            onPressed: function(mouse) {
                pressX = mouse.x
                startPosition = channel.levelPosition(channel.shownLevel)
                resetting = false
                channel.levelGesture = channel.bridge.trackCommands.begin_channel_gesture()
                channel.levelPreview = channel.shownLevel
            }
            onPositionChanged: function(mouse) {
                if (!pressed || resetting)
                    return
                const scale = (mouse.modifiers & Qt.ShiftModifier) ? channel.fineDragScale : 1
                channel.levelPreview = channel.levelAt(
                    channel.clamp(startPosition + scale * (mouse.x - pressX) / width))
                channel.previewLevel(channel.levelPreview)
            }
            onDoubleClicked: {
                resetting = true
                channel.levelPreview = -1
                channel.commitLevel(1, channel.levelGesture) // Unity gain: 0 dB.
                channel.levelGesture = 0
            }
            onReleased: {
                if (resetting)
                    return
                const value = channel.levelPreview
                channel.levelPreview = -1
                channel.commitLevel(value, channel.levelGesture)
                channel.levelGesture = 0
            }
            onCanceled: {
                channel.levelPreview = -1
                channel.levelGesture = 0
            }
        }
    }
    Text {
        y: channel.panLabelY
        visible: !channel.masterMode
        text: "BALANCE"
        color: channel.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: parent.width - width
        y: channel.panLabelY
        visible: !channel.masterMode
        text: channel.shownPan === 0 ? "CENTER"
            : channel.shownPan < 0 ? "L " + Math.round(-channel.shownPan * 100)
            : "R " + Math.round(channel.shownPan * 100)
        color: channel.textMain
        font.pixelSize: Theme.typeSmall
    }
    Item {
        id: panBar
        objectName: "trackPanControl"
        y: channel.panBarY
        visible: !channel.masterMode
        width: parent.width
        height: channel.barHeight
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: "Track stereo balance, left to right"
        Keys.onLeftPressed: channel.commitPan(Math.max(-1, Math.round((channel.savedPan - 0.05) * 20) / 20))
        Keys.onRightPressed: channel.commitPan(Math.min(1, Math.round((channel.savedPan + 0.05) * 20) / 20))
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Home) {
                channel.commitPan(0)
                event.accepted = true
            }
        }
        Rectangle { anchors.fill: parent; radius: Theme.controlRadius; color: channel.panelRaised; border.color: panBar.activeFocus ? channel.accent : channel.lineColor }
        Rectangle {
            x: parent.width / 2; y: Theme.hairline; height: parent.height - 2 * Theme.hairline
            width: Math.abs(channel.shownPan) * (parent.width - 2 * Theme.hairline) / 2
            transform: Scale { origin.x: 0; xScale: channel.shownPan < 0 ? -1 : 1 }
            color: channel.accent; radius: Theme.radiusMd
        }
        Rectangle {
            x: Math.max(0, Math.min(parent.width - width, parent.width * (channel.shownPan + 1) / 2 - width / 2))
            y: 0; width: channel.thumbWidth; height: parent.height; radius: Theme.radiusSm; color: channel.textMain
        }
        // Drags move the balance from where it is; Shift is finer; a double
        // click centres it.
        MouseArea {
            Tip {}
            property real pressX: 0
            property real startPan: 0
            property bool resetting: false
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            onPressed: function(mouse) {
                pressX = mouse.x
                startPan = channel.shownPan
                resetting = false
                channel.panGesture = channel.bridge.trackCommands.begin_channel_gesture()
                channel.panPreview = channel.shownPan
            }
            onPositionChanged: function(mouse) {
                if (!pressed || resetting)
                    return
                const scale = (mouse.modifiers & Qt.ShiftModifier) ? channel.fineDragScale : 1
                channel.panPreview = Math.max(-1, Math.min(1,
                    startPan + scale * 2 * (mouse.x - pressX) / width))
                channel.previewPan(channel.panPreview)
            }
            onDoubleClicked: {
                resetting = true
                channel.panPreview = 2
                channel.commitPan(0, channel.panGesture)
                channel.panGesture = 0
            }
            onReleased: {
                if (resetting)
                    return
                const value = channel.panPreview
                channel.panPreview = 2
                channel.commitPan(value, channel.panGesture)
                channel.panGesture = 0
            }
            onCanceled: {
                channel.panPreview = 2
                channel.panGesture = 0
            }
        }
    }
}
