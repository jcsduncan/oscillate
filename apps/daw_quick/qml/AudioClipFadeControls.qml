pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: fades
    required property var clipData
    required property var bridge
    required property bool selected
    required property color accentColor
    required property color amberColor
    property real previewIn: clipData.fadeInFraction || 0
    property real previewOut: clipData.fadeOutFraction || 0

    onClipDataChanged: {
        previewIn = clipData.fadeInFraction || 0
        previewOut = clipData.fadeOutFraction || 0
    }

    Rectangle {
        visible: fades.previewIn > 0
        y: Theme.clipFadeLineY
        width: fades.width * fades.previewIn
        height: Theme.strokeBold
        color: fades.accentColor
    }
    Rectangle {
        visible: fades.previewOut > 0
        anchors.right: parent.right
        y: Theme.clipFadeLineY
        width: fades.width * fades.previewOut
        height: Theme.strokeBold
        color: fades.accentColor
    }

    Repeater {
        model: [true, false]
        delegate: Rectangle {
            id: handle
            required property bool modelData
            readonly property bool isIn: modelData
            readonly property real currentFraction: isIn ? fades.previewIn : fades.previewOut
            readonly property int currentCurve: fades.clipData.kind === "audio"
                ? (isIn ? fades.clipData.fadeInCurve : fades.clipData.fadeOutCurve) : 0
            objectName: isIn ? "audioClipFadeInHandle" : "audioClipFadeOutHandle"
            z: 12
            visible: fades.selected && fades.width >= Theme.clipFadeMinClipWidth
            x: isIn ? Math.max(Theme.clipFadeHandleMargin, Math.min(fades.width - Theme.clipFadeHandleMargin - width, fades.previewIn * fades.width - width / 2))
                    : Math.max(Theme.clipFadeHandleMargin, Math.min(fades.width - Theme.clipFadeHandleMargin - width, fades.width - Theme.clipFadeHandleMargin - width / 2 - fades.previewOut * fades.width))
            y: Theme.clipFadeHandleY
            width: Theme.clipFadeHandleWidth
            height: Theme.clipFadeHandleHeight
            radius: Theme.radiusSm
            color: currentCurve === 1 ? fades.amberColor : fades.accentColor

            MouseArea {
                anchors.fill: parent
                enabled: fades.bridge.audioEditEnabled
                cursorShape: Qt.SizeHorCursor
                activeFocusOnTab: true
                Accessible.role: Accessible.Slider
                Accessible.name: handle.isIn ? "Fade in length. Shift click to switch curve."
                                             : "Fade out length. Shift click to switch curve."
                property real startX: 0
                property real startFraction: 0
                onPressed: mouse => {
                    startX = mapToItem(fades, mouse.x, mouse.y).x
                    startFraction = handle.currentFraction
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const delta = (mapToItem(fades, mouse.x, mouse.y).x - startX) / fades.width
                    if (handle.isIn)
                        fades.previewIn = Math.max(0, Math.min(1 - fades.previewOut, startFraction + delta))
                    else
                        fades.previewOut = Math.max(0, Math.min(1 - fades.previewIn, startFraction - delta))
                }
                onReleased: mouse => {
                    const value = handle.currentFraction
                    const original = handle.isIn ? fades.clipData.fadeInFraction : fades.clipData.fadeOutFraction
                    if (handle.isIn)
                        fades.previewIn = original || 0
                    else
                        fades.previewOut = original || 0
                    if ((mouse.modifiers & Qt.ShiftModifier) !== 0)
                        fades.bridge.set_audio_clip_fade(fades.clipData.id, handle.isIn,
                            original || 0, handle.currentCurve === 1 ? 0 : 1)
                    else if (Math.abs(value - startFraction) * fades.width >= 3)
                        fades.bridge.set_audio_clip_fade(fades.clipData.id, handle.isIn,
                            value, handle.currentCurve)
                }
                onCanceled: {
                    fades.previewIn = fades.clipData.fadeInFraction || 0
                    fades.previewOut = fades.clipData.fadeOutFraction || 0
                }
                Keys.onLeftPressed: {
                    const delta = handle.isIn ? -0.01 : 0.01
                    const limit = 1 - (handle.isIn ? fades.clipData.fadeOutFraction : fades.clipData.fadeInFraction)
                    fades.bridge.set_audio_clip_fade(fades.clipData.id, handle.isIn,
                        Math.max(0, Math.min(limit, handle.currentFraction + delta)), handle.currentCurve)
                }
                Keys.onRightPressed: {
                    const delta = handle.isIn ? 0.01 : -0.01
                    const limit = 1 - (handle.isIn ? fades.clipData.fadeOutFraction : fades.clipData.fadeInFraction)
                    fades.bridge.set_audio_clip_fade(fades.clipData.id, handle.isIn,
                        Math.max(0, Math.min(limit, handle.currentFraction + delta)), handle.currentCurve)
                }
            }
        }
    }
}
