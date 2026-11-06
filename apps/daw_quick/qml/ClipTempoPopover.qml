pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The clip strip's editors (ADR-0053), opened from its chips: the track
// tempo (type it, halve, double or tap; or go back to the detected tempo)
// and the stretch mode. Escape or a click elsewhere closes it.
Item {
    id: popover
    transformOrigin: Item.Top
    Reveal { target: popover; fromScale: Motion.popScale }
    objectName: "clipTempoPopover"
    required property var controller
    required property color amberColor
    required property color accentColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property string monoFamily
    property string clipId: ""
    property string kind: ""          // "tempo" or "mode"
    property point anchorPoint: Qt.point(0, 0)
    readonly property var tempo: clipId.length > 0 ? popover.controller.clips[clipId] : undefined
    readonly property bool open: kind.length > 0 && !!tempo

    function show(clip, what, point) {
        clipId = clip
        kind = what
        anchorPoint = point
        Qt.callLater(() => {
            if (kind === "tempo") field.forceActiveFocus()
            else panel.forceActiveFocus()
        })
    }
    function close() {
        kind = ""
        clipId = ""
    }

    visible: open
    z: 40

    // A click outside closes the editor.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: popover.close()
    }

    Rectangle {
        id: panel
        x: Math.max(Theme.spaceMd, Math.min(popover.width - width - Theme.spaceMd, popover.anchorPoint.x))
        y: Math.max(Theme.spaceMd, Math.min(popover.height - height - Theme.spaceMd, popover.anchorPoint.y))
        width: popover.kind === "tempo" ? Theme.clipTempoTempoWidth : Theme.clipTempoModeWidth
        height: content.implicitHeight + Theme.space2xl
        radius: Theme.radiusXl
        color: Theme.panelRaised
        border.color: Theme.controlEdge
        Keys.onEscapePressed: popover.close()
        MouseArea { anchors.fill: parent }

        Column {
            id: content
            x: Theme.spaceLg
            y: Theme.spaceLg
            width: parent.width - Theme.space2xl
            spacing: Theme.clipTempoSpacing

            // --- Track tempo ---
            Row {
                visible: popover.kind === "tempo"
                spacing: Theme.spaceMd
                height: Theme.clipTempoRowHeight
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Track tempo"
                    color: popover.dimColor
                    font.pixelSize: Theme.typeBody
                }
                Rectangle {
                    width: Theme.clipTempoFieldWidth
                    height: Theme.clipEditorControlHeight
                    radius: Theme.radiusLg
                    color: Theme.canvas
                    border.color: field.activeFocus ? popover.accentColor : popover.lineColor
                    TextInput {
                        id: field
                        objectName: "clipTempoField"
                        anchors.fill: parent
                        anchors.leftMargin: Theme.clipEditorTempoTextInset
                        verticalAlignment: TextInput.AlignVCenter
                        text: popover.tempo && popover.tempo.bpm > 0 ? popover.tempo.bpm.toFixed(2) : ""
                        color: popover.textColor
                        font.pixelSize: Theme.typeSubtitle
                        font.family: popover.monoFamily
                        selectByMouse: true
                        validator: DoubleValidator { bottom: 20; top: 999; decimals: 3 }
                        Accessible.role: Accessible.EditableText
                        Accessible.name: "Track tempo in BPM"
                        onAccepted: {
                            const bpm = Number(text)
                            if (bpm >= 20 && bpm <= 999)
                                popover.controller.set_bpm(popover.clipId, bpm)
                        }
                        Keys.onEscapePressed: popover.close()
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "BPM"
                    color: popover.dimColor
                    font.pixelSize: Theme.typeBody
                }
                Repeater {
                    model: [["÷2", 0.5], ["×2", 2.0]]
                    delegate: PopoverButton {
                        required property var modelData
                        label: modelData[0]
                        accessibleName: modelData[1] < 1 ? "Halve the tempo" : "Double the tempo"
                        onActivated: popover.controller.scale_bpm(popover.clipId, modelData[1])
                    }
                }
                PopoverButton {
                    id: tapButton
                    objectName: "clipTempoTap"
                    label: "TAP"
                    accessibleName: "Tap the tempo"
                    onActivated: popover.controller.tap(popover.clipId)
                }
            }
            Text {
                visible: popover.kind === "tempo"
                width: parent.width
                wrapMode: Text.Wrap
                lineHeight: Theme.clipTempoNoteLineHeight
                color: popover.dimColor
                font.pixelSize: Theme.typeBody
                text: {
                    const t = popover.tempo
                    if (!t) return ""
                    if (t.analysing) return "Analysing the track: tempo, beat grid and downbeat."
                    if (!(t.bpm > 0)) return "No steady beat found. Type its tempo, or tap along."
                    const detected = t.detected > 0 ? "Detected " + t.detected.toFixed(2) : "Not detected"
                    const fit = t.confidence >= 0.5 ? " with a steady grid. " : ", but unsure: listen and confirm, halve, double or tap. "
                    return (t.corrected ? "Set to " + t.bpm.toFixed(2) + ". " + detected + ". " : detected + fit)
                        + "Type to correct it; the grid rescales and the downbeat stays put. ÷2 and ×2 fix half- and double-time reads."
                }
            }
            PopoverButton {
                visible: popover.kind === "tempo" && !!popover.tempo && popover.tempo.corrected
                         && popover.tempo.detected > 0
                label: popover.tempo && popover.tempo.detected > 0
                    ? "Back to detected " + popover.tempo.detected.toFixed(2) : ""
                accessibleName: "Go back to the detected tempo"
                onActivated: popover.controller.reset_bpm(popover.clipId)
            }

            // --- Stretch mode ---
            Text {
                visible: popover.kind === "mode"
                text: "STRETCH MODE"
                color: popover.dimColor
                font.pixelSize: Theme.typeSmall
                font.letterSpacing: Theme.clipTempoHeadingLetterSpacing
            }
            Repeater {
                model: popover.kind === "mode" ? popover.controller.modes : []
                delegate: Rectangle {
                    id: modeRow
                    required property var modelData
                    required property int index
                    readonly property bool current: !!popover.tempo && popover.tempo.modeIndex === index
                    width: content.width
                    height: Theme.clipTempoRowHeight
                    radius: Theme.radiusLg
                    color: current ? Qt.rgba(popover.accentColor.r, popover.accentColor.g, popover.accentColor.b, Theme.clipTempoModeWashAlpha)
                         : modeArea.containsMouse ? Theme.clipEditorButtonHover : Theme.panelRaised
                    border.color: current ? popover.accentColor : activeFocus ? popover.accentColor : Theme.clipTempoModeEdge
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.name + ": " + modelData.hint
                    function pick() {
                        popover.controller.set_mode(popover.clipId, index)
                        popover.close()
                    }
                    Keys.onReturnPressed: pick()
                    Keys.onSpacePressed: pick()
                    Row {
                        x: Theme.clipTempoModeTextX
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.clipTempoSpacing
                        Text {
                            width: Theme.clipTempoModeNameWidth
                            text: modeRow.modelData.name
                            color: popover.textColor
                            font.pixelSize: Theme.typeLabel
                            font.weight: Font.DemiBold
                        }
                        Text {
                            width: modeRow.width - Theme.clipTempoModeHintReserve
                            elide: Text.ElideRight
                            text: modeRow.modelData.hint
                            color: popover.dimColor
                            font.pixelSize: Theme.typeBody
                        }
                    }
                    MouseArea {
                        id: modeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: modeRow.pick()
                    }
                }
            }
        }
    }

    component PopoverButton: Rectangle {
        id: button
        property string label
        property string accessibleName
        signal activated()
        height: Theme.clipEditorControlHeight
        width: buttonText.implicitWidth + Theme.clipTempoButtonPadding
        radius: Theme.radiusLg
        color: buttonArea.containsMouse ? Theme.clipEditorButtonHover : Theme.canvas
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        border.color: activeFocus ? Theme.accent : Theme.controlEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: Theme.chipText
            font.pixelSize: Theme.typeBody
            font.weight: button.label === "TAP" ? Font.DemiBold : Font.Normal
            font.letterSpacing: button.label === "TAP" ? 1 : 0
        }
        MouseArea {
            Tip {}
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.activated()
        }
    }
}
