pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: editor
    required property var bridge
    required property var track
    property var clip: null
    required property color panelRaised
    required property color lineColor
    required property color textMain
    required property color textDim
    required property color accent
    readonly property bool clipMode: clip !== null && Boolean(clip.id)
    readonly property var lane: (clipMode ? clip.channelAutomation : track.channelAutomation) || ({ present: false, pointCount: 0, points: [], timebase: "Musical", laneId: "" })
    property bool graphMode: true
    // Layout, top to bottom: title and timebase, note, the bar / level / add
    // form, the count and view toggle, the graph or point list, then status.
    readonly property int timebaseRise: 4
    readonly property int noteY: 20
    readonly property int formY: 50
    readonly property int fieldHeight: 26
    readonly property int barFieldWidth: 43
    readonly property int levelFieldX: 49
    readonly property int levelFieldWidth: 53
    readonly property int addX: 108
    readonly property int countY: 83
    readonly property int toggleY: 82
    readonly property int bodyY: 101
    readonly property int bodyHeight: 151
    readonly property int segmentRowExtra: 32
    readonly property int pointRowHeight: 27
    readonly property int pointTextReserve: 91
    readonly property int pointValueRight: 86
    readonly property int pointValueWidth: 52
    readonly property int pointFieldHeight: 24
    readonly property int pointDeleteWidth: 29
    readonly property int segmentY: 29
    readonly property int footerY: 264
    readonly property int statusY: 282

    Text {
        text: editor.clipMode ? "CLIP LEVEL ENVELOPE" : "CHANNEL LEVEL AUTOMATION"
        color: editor.textDim
        font.pixelSize: Theme.typeCaption
        font.bold: true
        font.letterSpacing: Theme.capsLetterSpacing
    }
    AutomationTimebaseAction {
        objectName: "channelAutomationTimebase"
        x: parent.width - width
        y: -editor.timebaseRise
        bridge: editor.bridge
        laneId: editor.lane.laneId || ""
        timebase: editor.lane.timebase || "Musical"
        accentColor: editor.accent
    }
    Text {
        y: editor.noteY
        width: parent.width
        text: editor.clipMode ? "Set this clip's output level by project bar." : "Set output level by bar. Clip envelopes can override it."
        wrapMode: Text.WordWrap
        color: editor.textDim
        font.pixelSize: Theme.typeCaption
    }
    Rectangle {
        x: 0; y: editor.formY; width: editor.barFieldWidth; height: editor.fieldHeight
        radius: Theme.automationFieldRadius; color: editor.panelRaised; border.color: editor.lineColor
        TextInput {
            id: barInput
            objectName: "channelAutomationBarInput"
            anchors.fill: parent; anchors.margins: Theme.automationFieldInset
            text: "1"
            color: editor.textMain
            font.pixelSize: Theme.typeSmall
            validator: IntValidator { bottom: 1; top: 65536 }
            activeFocusOnTab: true
            Accessible.name: "Channel automation bar, 1 to 65536"
        }
    }
    Rectangle {
        x: editor.levelFieldX; y: editor.formY; width: editor.levelFieldWidth; height: editor.fieldHeight
        radius: Theme.automationFieldRadius; color: editor.panelRaised; border.color: editor.lineColor
        TextInput {
            id: levelInput
            objectName: "channelAutomationLevelInput"
            anchors.fill: parent; anchors.margins: Theme.automationFieldInset
            text: Number(editor.track.level ?? 1).toFixed(2)
            color: editor.textMain
            font.pixelSize: Theme.typeSmall
            validator: DoubleValidator { bottom: 0; top: 4; decimals: 3 }
            activeFocusOnTab: true
            Accessible.name: "Channel automation linear level, 0 to 4"
        }
    }
    Rectangle {
        id: addPoint
        objectName: "channelAutomationAddPoint"
        x: editor.addX; y: editor.formY; width: parent.width - editor.addX; height: editor.fieldHeight
        radius: Theme.automationFieldRadius
        color: enabled ? Theme.automationApplyFill : Theme.automationActionDisabled
        border.color: enabled ? editor.accent : editor.lineColor
        enabled: editor.bridge.audioEditEnabled && (editor.clipMode ? editor.clip.id : editor.track.id) &&
                 barInput.acceptableInput &&
                 levelInput.acceptableInput && barInput.text.length > 0 && levelInput.text.length > 0
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: "Add channel level automation point"
        function activate() {
            if (editor.clipMode)
                editor.bridge.clipAutomation.set_point(editor.clip.id, Number(barInput.text) - 1, Number(levelInput.text))
            else
                editor.bridge.trackAutomation.set_point(editor.track.id, Number(barInput.text) - 1, Number(levelInput.text))
        }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text {
            anchors.centerIn: parent
            text: "+ POINT"
            color: addPoint.enabled ? editor.accent : editor.textDim
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            enabled: addPoint.enabled
            onClicked: addPoint.activate()
            Tip {}
        }
    }
    Text {
        y: editor.countY
        text: editor.lane.pointCount + " POINT" + (editor.lane.pointCount === 1 ? "" : "S")
        color: editor.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        id: viewToggle
        objectName: "channelAutomationViewToggle"
        x: editor.width - width; y: editor.toggleY
        visible: !editor.clipMode
        text: editor.graphMode ? "POINT LIST" : "GRAPH"
        color: editor.accent
        font.pixelSize: Theme.typeCaption
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: editor.graphMode
                         ? "Show channel automation point list" : "Show channel automation graph"
        function activate() { editor.graphMode = !editor.graphMode }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        MouseArea {
            anchors.fill: parent
            onClicked: viewToggle.activate()
            Tip {}
        }
    }
    ChannelAutomationGraph {
        objectName: "channelAutomationGraph"
        x: 0; y: editor.bodyY; width: editor.width; height: editor.bodyHeight
        visible: editor.graphMode && !editor.clipMode
        bridge: editor.bridge
        trackId: editor.track.id || ""
        lane: editor.lane
        accentColor: editor.accent
        lineColor: editor.lineColor
        textColor: editor.textDim
    }
    ListView {
        id: pointList
        objectName: "channelAutomationPointList"
        x: 0; y: editor.bodyY; width: editor.width; height: editor.bodyHeight
        visible: !editor.graphMode || editor.clipMode
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: editor.lane.points || []
        delegate: Item {
            id: pointRow
            required property var modelData
            required property int index
            objectName: "channelAutomationPointRow"
            width: pointList.width
            height: index < editor.lane.pointCount - 1 ? segmentControls.height + editor.segmentRowExtra : editor.pointRowHeight
            Text {
                y: Theme.spaceSm
                width: parent.width - editor.pointTextReserve
                elide: Text.ElideRight
                text: pointRow.modelData.position
                color: editor.textMain
                font.pixelSize: Theme.typeSmall
            }
            Rectangle {
                x: parent.width - editor.pointValueRight; width: editor.pointValueWidth; height: editor.pointFieldHeight
                radius: Theme.automationFieldRadius; color: editor.panelRaised; border.color: editor.lineColor
                TextInput {
                    objectName: "channelAutomationPointValue"
                    anchors.fill: parent; anchors.margins: Theme.spaceXs
                    text: Number(pointRow.modelData.value).toFixed(2)
                    color: editor.textMain
                    font.pixelSize: Theme.typeSmall
                    validator: DoubleValidator { bottom: 0; top: 4; decimals: 3 }
                    activeFocusOnTab: true
                    Accessible.name: "Channel level at " + pointRow.modelData.position
                    onEditingFinished: {
                        if (acceptableInput && Math.abs(Number(text) - Number(pointRow.modelData.value)) > 0.000001) {
                            if (editor.clipMode)
                                editor.bridge.clipAutomation.set_value(editor.clip.id, pointRow.modelData.id, Number(text))
                            else
                                editor.bridge.trackAutomation.set_value(editor.track.id, pointRow.modelData.id, Number(text))
                        }
                    }
                }
            }
            Rectangle {
                id: removePoint
                objectName: "channelAutomationRemovePoint"
                x: parent.width - width; width: editor.pointDeleteWidth; height: editor.pointFieldHeight
                radius: Theme.automationFieldRadius; color: editor.panelRaised; border.color: editor.lineColor
                activeFocusOnTab: editor.bridge.audioEditEnabled
                Accessible.role: Accessible.Button
                Accessible.name: "Remove channel point at " + pointRow.modelData.position
                function activate() {
                    if (editor.clipMode)
                        editor.bridge.clipAutomation.remove_point(editor.clip.id, pointRow.modelData.id)
                    else
                        editor.bridge.trackAutomation.remove_point(editor.track.id, pointRow.modelData.id)
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text { anchors.centerIn: parent; text: "×"; color: editor.textMain; font.pixelSize: Theme.typeSubtitle }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    enabled: editor.bridge.audioEditEnabled
                    onClicked: removePoint.activate()
                }
            }
            AutomationSegmentControls {
                id: segmentControls
                objectName: "channelAutomationSegmentControls"
                y: editor.segmentY
                width: pointRow.width
                visible: pointRow.index < editor.lane.pointCount - 1
                bridge: editor.bridge
                laneId: editor.lane.laneId || ""
                pointId: pointRow.modelData.id
                panelColor: editor.panelRaised
                lineColor: editor.lineColor
                textColor: editor.textMain
                dimColor: editor.textDim
                accentColor: editor.accent
            }
        }
    }
    Text {
        y: editor.footerY
        visible: editor.lane.present
        text: "CLEAR LANE"
        color: editor.textDim
        font.pixelSize: Theme.typeCaption
        activeFocusOnTab: editor.bridge.audioEditEnabled
        Accessible.role: Accessible.Button
        Accessible.name: editor.clipMode ? "Remove clip level envelope" : "Remove channel level automation lane"
        function activate() {
            if (editor.clipMode)
                editor.bridge.clipAutomation.remove_lane(editor.clip.id)
            else
                editor.bridge.trackAutomation.remove_lane(editor.track.id)
        }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        MouseArea {
            Tip {}
            anchors.fill: parent
            enabled: editor.bridge.audioEditEnabled
            onClicked: parent.activate()
        }
    }
    Text {
        y: editor.statusY
        width: parent.width
        visible: editor.lane.present && editor.bridge.playbackActive
        text: editor.bridge.playbackRevisionInactive
              ? "Saved curve pending · Stop and Play to apply"
              : "Curve active in playback"
        color: editor.bridge.playbackRevisionInactive ? Theme.automationInactiveWarning : editor.accent
        font.pixelSize: Theme.typeCaption
        elide: Text.ElideRight
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
