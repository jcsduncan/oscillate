pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: inspector
    objectName: "inspectorPanel"
    required property var bridge
    required property int selectedTrack
    required property string selectedClipId
    required property var currentRouteTrack
    required property color canvas
    required property color panel
    required property color panelRaised
    required property color line
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber
    // The scrolling column is laid out in absolute rows, top to bottom;
    // the heights of the variable sections push the rows below them down.
    readonly property int inset: 23
    readonly property int effectsScrollThreshold: 200
    readonly property int sourceCardExtra: 180
    readonly property int sendAutomationBlockHeight: 309
    readonly property int masterRoutingHeight: 110
    readonly property int routingBaseHeight: 352
    readonly property int routingSendRowHeight: 29
    readonly property int routingTrackExtra: 405
    readonly property int clipAnalysisHeight: 730
    readonly property int clipAnalysisGap: 20
    readonly property int baseContentHeight: 1520
    readonly property int headerRuleY: 64
    readonly property int badgeY: 88
    readonly property int badgeSize: 43
    readonly property int badgeRadius: 11
    readonly property int badgeGlyphX: 35
    readonly property int badgeGlyphY: 95
    readonly property int nameX: 78
    readonly property int nameY: 90
    readonly property int nameRightInset: 99
    readonly property int nameEditorPadX: 5
    readonly property int nameEditorPadY: 3
    readonly property int nameEditorHeight: 24
    readonly property int kindY: 112
    readonly property int sourceRuleY: 157
    readonly property int sourceLabelY: 181
    readonly property int sourceTextY: 208
    readonly property int sourceCardY: 180
    readonly property int sourceCardHeight: 275
    readonly property int routingY: 277
    readonly property int masterControlsHeight: 55
    readonly property int keyReviewY: 290
    readonly property int keyReviewHeight: 225
    readonly property int clipAutomationY: 520
    readonly property int clipAutomationHeight: 300
    readonly property int warpY: 830
    readonly property int sendAutomationY: 290
    // Send gain automation rows.
    readonly property int sendChoiceY: 19
    readonly property int sendChoiceGap: 3
    readonly property int sendControlHeight: 25
    readonly property int sendFieldHeight: 26
    readonly property int sendStatusY: 50
    readonly property int sendFieldLabelY: 84
    readonly property int sendFieldY: 99
    readonly property int sendBarFieldWidth: 44
    readonly property int sendGainFieldX: 53
    readonly property int sendGainFieldWidth: 55
    readonly property int sendAddPointX: 116
    readonly property int sendPointsLabelY: 135
    readonly property int sendPointRowY: 154
    readonly property int sendPointRowPitch: 30
    readonly property int sendPointRowHeight: 27
    readonly property int sendPointTextRight: 95
    readonly property int sendPointValueRight: 88
    readonly property int sendPointRemoveSize: 27
    readonly property int sendClearLaneY: 280
    readonly property int sendClearLaneWidth: 85
    // Quick actions, below everything else.
    readonly property int quickActionsGap: 30
    readonly property int quickActionsHeight: 1490
    readonly property int quickRuleY: 270
    readonly property int quickLabelY: 294
    readonly property int addTrackY: 326
    readonly property int importY: 374
    readonly property int saveY: 422
    readonly property int undoY: 470
    readonly property int actionHeight: 38
    readonly property int exportGap: 10
    readonly property bool fxPanelActive: inspectorFlickable.contentY > effectsScrollThreshold
    property string selectedSendId: ""
    // The export dialog lives in the shell; this quick action opens it.
    signal exportRequested()
    readonly property var currentInsertChain: {
        const index = selectedTrack === -2 ? bridge.tracks.length : selectedTrack;
        return index >= 0 && index < bridge.insertChains.length ? bridge.insertChains[index] : ({
                effects: [],
                sourceName: "",
                status: "Saved chain unavailable"
            });
    }
    readonly property string selectedPathName: selectedTrack === -2 ? "Master" : selectedTrack >= 0 && selectedTrack < bridge.tracks.length ? bridge.tracks[selectedTrack].name : "No selection"
    readonly property bool nativeSynthTrackSelected: selectedTrack >= 0
        && selectedTrack < bridge.tracks.length
        && bridge.tracks[selectedTrack].nativeSourceReady
    readonly property int sourcePanelOffset: nativeSynthTrackSelected ? sourceCardExtra : 0
    readonly property var currentSend: {
        const sends = currentRouteTrack.sends || [];
        return sends.find(send => send.id === selectedSendId) || sends[0] || null;
    }
    readonly property var currentSendAutomation: currentSend ? currentSend.automation : ({
            present: false,
            timebase: "Musical",
            pointCount: 0,
            points: []
        })
    readonly property int sendAutomationHeight: currentSend ? sendAutomationBlockHeight : 0
    readonly property int routingInspectorHeight: selectedTrack === -2 ? masterRoutingHeight : routingBaseHeight + (currentRouteTrack.sendCount || 0) * routingSendRowHeight + (currentRouteTrack.id ? routingTrackExtra : 0)
    readonly property int analysisOrAutomationHeight: inspector.selectedAudioClip() ? clipAnalysisHeight + warpEditor.implicitHeight + clipAnalysisGap : 0
    color: panel
    onSelectedTrackChanged: {
        if (inspector.bridge.vst3Editor.vst3InsertEditor.native)
            inspector.bridge.vst3Editor.apply_vst3_insert_editor();
        else
            inspector.bridge.vst3Editor.discard_vst3_insert_editor();
        selectedSendId = "";
        showSection(selectedTrack === -2);
    }
    function selectedAudioClip() {
        for (const track of bridge.tracks) {
            for (const clip of track.clips) {
                if (clip.id === selectedClipId && clip.kind === "audio")
                    return clip;
            }
        }
        return null;
    }
    function showSection(effects) {
        inspectorFlickable.contentY = effects ? Math.max(0, inspectorFlickable.contentHeight - inspectorFlickable.height) : 0;
    }
    Flickable {
        id: inspectorFlickable
        objectName: "inspectorScroll"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: inspector.baseContentHeight + inspector.sourcePanelOffset + inspector.routingInspectorHeight + inspector.analysisOrAutomationHeight + inspector.sendAutomationHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Rectangle {
            x: 0
            width: Theme.hairline
            height: parent.height
            color: inspector.line
        }
        Text {
            x: inspector.inset
            y: inspector.inset
            text: "INSPECTOR"
            color: inspector.textDim
            font.pixelSize: Theme.typeSmall
            font.bold: true
            font.letterSpacing: Theme.tracking3xl
        }
        Rectangle {
            y: inspector.headerRuleY
            width: parent.width
            height: Theme.hairline
            color: inspector.line
        }
        Rectangle {
            id: addTrackAction
            x: inspector.inset
            y: inspector.badgeY
            width: inspector.badgeSize
            height: inspector.badgeSize
            radius: inspector.badgeRadius
            color: Theme.inspectorBadgeFill
        }
        Text {
            x: inspector.badgeGlyphX
            y: inspector.badgeGlyphY
            text: "♫"
            color: inspector.accent
            font.pixelSize: Theme.inspectorBadgeGlyphSize
        }
        Text {
            id: inspectorTrackName
            objectName: "inspectorTrackName"
            x: inspector.nameX
            y: inspector.nameY
            width: parent.width - inspector.nameRightInset
            elide: Text.ElideRight
            text: inspector.selectedPathName
            color: inspector.textMain
            font.pixelSize: Theme.typeHeading
            font.bold: true
            // Double-click the selected track's name to rename it.
            MouseArea {
                anchors.fill: parent
                enabled: inspector.selectedTrack >= 0 && inspector.selectedTrack < inspector.bridge.tracks.length
                onDoubleClicked: inspectorNameEditor.begin(inspector.selectedPathName)
            }
        }
        InlineNameEditor {
            id: inspectorNameEditor
            objectName: "inspectorTrackNameEditor"
            z: 5
            x: inspectorTrackName.x - inspector.nameEditorPadX
            y: inspectorTrackName.y - inspector.nameEditorPadY
            width: inspectorTrackName.width + 2 * inspector.nameEditorPadX
            height: inspector.nameEditorHeight
            pixelSize: Theme.typeHeading
            textColor: inspector.textMain
            accentColor: inspector.accent
            onCommitted: name => {
                const track = inspector.bridge.tracks[inspector.selectedTrack]
                if (track)
                    inspector.bridge.set_track_name(track.id, name)
            }
        }
        Text {
            x: inspector.nameX
            y: inspector.kindY
            text: {
                if (inspector.selectedTrack === -2)
                    return "MASTER BUS"
                const track = inspector.selectedTrack >= 0 ? inspector.bridge.tracks[inspector.selectedTrack] : null
                if (!track)
                    return ""
                return track.kind === "instrument" ? "INSTRUMENT TRACK" : track.kind === "audio" ? "AUDIO TRACK"
                    : track.kind === "group" ? "GROUP BUS" : track.kind === "return" ? "RETURN BUS" : "TRACK"
            }
            color: inspector.textDim
            font.pixelSize: Theme.typeSmall
            font.letterSpacing: Theme.trackingMd
        }
        Rectangle {
            x: inspector.inset
            y: inspector.sourceRuleY
            width: parent.width - 2 * inspector.inset
            height: Theme.hairline
            color: inspector.line
        }
        Text {
            x: inspector.inset
            y: inspector.sourceLabelY
            visible: !inspector.nativeSynthTrackSelected
            text: "SOURCE"
            color: inspector.textDim
            font.pixelSize: Theme.typeSmall
            font.bold: true
            font.letterSpacing: Theme.tracking2xl
        }
        Text {
            x: inspector.inset
            y: inspector.sourceTextY
            visible: !inspector.nativeSynthTrackSelected
            width: parent.width - 2 * inspector.inset
            wrapMode: Text.WordWrap
            text: inspector.selectedTrack === -2 ? "Master output · no source media" : inspector.selectedTrack < 0 || inspector.selectedTrack >= inspector.bridge.tracks.length ? "Select a track" : inspector.bridge.tracks[inspector.selectedTrack].kind === "audio" ? "Audio clips on this track" : inspector.bridge.tracks[inspector.selectedTrack].kind === "instrument" ? (inspector.bridge.tracks[inspector.selectedTrack].vst3SourceReady ? inspector.currentInsertChain.sourceName : "No instrument yet: drop one from the browser") : "Bus · mixes the tracks sent to it"
            color: inspector.textMain
            font.pixelSize: Theme.typeLabel
        }
        NativeSynthSourceCard {
            visible: inspector.nativeSynthTrackSelected
            x: inspector.inset
            y: inspector.sourceCardY
            width: parent.width - 2 * inspector.inset
            height: inspector.sourceCardHeight
            trackName: inspector.selectedPathName
            engineName: inspector.nativeSynthTrackSelected
                ? inspector.bridge.tracks[inspector.selectedTrack].nativeEngineName : ""
            engineId: inspector.nativeSynthTrackSelected
                ? inspector.bridge.tracks[inspector.selectedTrack].nativeEngineId : ""
            engines: inspector.bridge.nativeSynthEngines
            onOpenRequested: inspector.bridge.open_native_synth_editor(
                inspector.bridge.tracks[inspector.selectedTrack].id)
            onChangeRequested: function(engineId) {
                inspector.bridge.select_native_instrument(
                    inspector.bridge.tracks[inspector.selectedTrack].id, engineId)
            }
        }
        RoutingInspector {
            objectName: "routingInspector"
            visible: inspector.selectedTrack !== -2
            x: inspector.inset
            y: inspector.routingY + inspector.sourcePanelOffset
            width: parent.width - 2 * inspector.inset
            height: inspector.routingInspectorHeight
            bridge: inspector.bridge
            track: inspector.currentRouteTrack
            destinations: inspector.bridge.routeTargets
            destinationCount: inspector.bridge.routeTargetCount
            panelRaised: inspector.panelRaised
            lineColor: inspector.line
            textMain: inspector.textMain
            textDim: inspector.textDim
            accent: inspector.accent
        }
        ChannelControls {
            objectName: "masterChannelControls"
            visible: inspector.selectedTrack === -2
            x: inspector.inset
            y: inspector.routingY + inspector.sourcePanelOffset
            width: parent.width - 2 * inspector.inset
            height: inspector.masterControlsHeight
            bridge: inspector.bridge
            track: ({ id: "", level: inspector.bridge.masterLevel, pan: 0 })
            masterMode: true
            panelRaised: inspector.panelRaised
            lineColor: inspector.line
            textMain: inspector.textMain
            textDim: inspector.textDim
            accent: inspector.accent
        }
KeyReviewPanel {
            objectName: "keyReviewPanel"
            visible: inspector.selectedAudioClip() !== null
            x: inspector.inset
            y: inspector.keyReviewY + inspector.sourcePanelOffset + inspector.routingInspectorHeight
            width: parent.width - 2 * inspector.inset
            height: inspector.keyReviewHeight
            review: inspector.bridge.keyReview
            textMain: inspector.textMain
            textDim: inspector.textDim
            accent: inspector.accent
            amber: inspector.amber
            lineColor: inspector.line
        }
        ChannelAutomationEditor {
            objectName: "clipChannelAutomationEditor"
            visible: inspector.selectedAudioClip() !== null
            x: inspector.inset
            y: inspector.clipAutomationY + inspector.sourcePanelOffset + inspector.routingInspectorHeight
            width: parent.width - 2 * inspector.inset
            height: inspector.clipAutomationHeight
            bridge: inspector.bridge
            track: inspector.currentRouteTrack
            clip: inspector.selectedAudioClip()
            panelRaised: inspector.panelRaised
            lineColor: inspector.line
            textMain: inspector.textMain
            textDim: inspector.textDim
            accent: inspector.accent
        }
        WarpEditor {
            id: warpEditor
            objectName: "warpEditor"
            visible: inspector.selectedAudioClip() !== null
            x: inspector.inset
            y: inspector.warpY + inspector.sourcePanelOffset + inspector.routingInspectorHeight
            width: parent.width - 2 * inspector.inset
            height: implicitHeight
            bridge: inspector.bridge
            controller: inspector.bridge.clipWarp
            lineColor: inspector.line
            textMain: inspector.textMain
            textDim: inspector.textDim
            accent: inspector.accent
            amber: inspector.amber
        }
        Item {
            id: sendAutomationEditor
            objectName: "sendAutomationEditor"
            visible: inspector.currentSend !== null
            x: inspector.inset
            y: inspector.sendAutomationY + inspector.sourcePanelOffset + inspector.routingInspectorHeight + inspector.analysisOrAutomationHeight
            width: parent.width - 2 * inspector.inset
            height: inspector.sendAutomationHeight
            Text {
                text: "SEND GAIN AUTOMATION"
                color: inspector.textDim
                font.pixelSize: Theme.typeSmall
                font.bold: true
                font.letterSpacing: Theme.trackingLg
            }
            AutomationTimebaseAction {
                objectName: "sendAutomationTimebase"
                x: parent.width - width
                bridge: inspector.bridge
                laneId: inspector.currentSendAutomation.laneId || ""
                timebase: inspector.currentSendAutomation.timebase
                accentColor: inspector.accent
            }
            Repeater {
                model: inspector.currentRouteTrack.sends || []
                delegate: Rectangle {
                    id: sendChoice
                    objectName: "sendAutomationChoice"
                    required property var modelData
                    required property int index
                    x: index * (sendAutomationEditor.width / Math.max(1, inspector.currentRouteTrack.sends.length))
                    y: inspector.sendChoiceY
                    width: sendAutomationEditor.width / Math.max(1, inspector.currentRouteTrack.sends.length) - inspector.sendChoiceGap
                    height: inspector.sendControlHeight
                    radius: Theme.controlRadius
                    color: inspector.currentSend && inspector.currentSend.id === modelData.id ? inspector.accent : inspector.panelRaised
                    border.color: activeFocus ? inspector.accent : inspector.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Edit send gain automation to " + modelData.destinationName
                    function activate() {
                        inspector.selectedSendId = modelData.id;
                    }
                    Keys.onReturnPressed: activate()
                    Keys.onSpacePressed: activate()
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - Theme.spaceMd
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        text: sendChoice.modelData.destinationName
                        color: inspector.currentSend && inspector.currentSend.id === sendChoice.modelData.id ? Theme.onAccentText : inspector.textMain
                        font.pixelSize: Theme.typeCaption
                    }
                    MouseArea {
                        Tip {}
                        anchors.fill: parent
                        onClicked: sendChoice.activate()
                    }
                }
            }
            Text {
                y: inspector.sendStatusY
                width: parent.width
                wrapMode: Text.WordWrap
                text: !inspector.bridge.playbackStatusIsDefault ? inspector.bridge.playbackStatus : "Save to keep edits. Start playback to hear this send lane."
                color: inspector.textDim
                font.pixelSize: Theme.typeCaption
            }
            Text {
                x: 0
                y: inspector.sendFieldLabelY
                text: "BAR"
                color: inspector.textDim
                font.pixelSize: Theme.typeCaption
            }
            Rectangle {
                x: 0
                y: inspector.sendFieldY
                width: inspector.sendBarFieldWidth
                height: inspector.sendFieldHeight
                radius: Theme.controlRadius
                color: inspector.panelRaised
                border.color: inspector.line
                TextInput {
                    id: sendAutomationBarInput
                    objectName: "sendAutomationBarInput"
                    anchors.fill: parent
                    anchors.margins: Theme.fieldPadding
                    text: "1"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeBody
                    validator: IntValidator {
                        bottom: 1
                        top: 16
                    }
                    activeFocusOnTab: true
                    Accessible.name: "Send automation bar, 1 to 16"
                }
            }
            Text {
                x: inspector.sendGainFieldX
                y: inspector.sendFieldLabelY
                text: "GAIN"
                color: inspector.textDim
                font.pixelSize: Theme.typeCaption
            }
            Rectangle {
                x: inspector.sendGainFieldX
                y: inspector.sendFieldY
                width: inspector.sendGainFieldWidth
                height: inspector.sendFieldHeight
                radius: Theme.controlRadius
                color: inspector.panelRaised
                border.color: inspector.line
                TextInput {
                    id: sendAutomationGainInput
                    objectName: "sendAutomationGainInput"
                    anchors.fill: parent
                    anchors.margins: Theme.fieldPadding
                    text: "1.00"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeBody
                    validator: DoubleValidator {
                        bottom: 0
                        top: 4
                        decimals: 2
                        locale: "C"
                    }
                    activeFocusOnTab: true
                    Accessible.name: "Send automation gain, 0 to 4"
                }
            }
            Rectangle {
                id: sendAutomationAddPoint
                objectName: "sendAutomationAddPoint"
                x: inspector.sendAddPointX
                y: inspector.sendFieldY
                width: parent.width - inspector.sendAddPointX
                height: inspector.sendFieldHeight
                radius: Theme.controlRadius
                color: enabled ? inspector.accent : inspector.panelRaised
                enabled: inspector.currentSend !== null && (inspector.currentSendAutomation.pointCount < 4 || inspector.currentSendAutomation.points.some(point => point.bar === Number(sendAutomationBarInput.text) - 1)) && sendAutomationBarInput.acceptableInput && sendAutomationBarInput.text.length > 0 && sendAutomationGainInput.acceptableInput && sendAutomationGainInput.text.length > 0
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: "Add send gain point"
                function activate() {
                    inspector.bridge.set_send_automation_point(inspector.currentSend.id, Number(sendAutomationBarInput.text) - 1, Number(sendAutomationGainInput.text));
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text {
                    anchors.centerIn: parent
                    text: "ADD POINT"
                    color: parent.enabled ? Theme.onAccentText : inspector.textDim
                    font.pixelSize: Theme.typeCaption
                    font.bold: true
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: sendAutomationAddPoint.activate()
                }
            }
            Text {
                y: inspector.sendPointsLabelY
                text: inspector.currentSendAutomation.pointCount > 4 ? "Showing first four of " + inspector.currentSendAutomation.pointCount + " points" : inspector.currentSendAutomation.pointCount + " POINT" + (inspector.currentSendAutomation.pointCount === 1 ? "" : "S")
                color: inspector.textDim
                font.pixelSize: Theme.typeCaption
            }
            Repeater {
                model: inspector.currentSendAutomation.points
                delegate: Item {
                    id: sendPointRow
                    objectName: "sendAutomationPointRow"
                    required property var modelData
                    required property int index
                    y: inspector.sendPointRowY + index * inspector.sendPointRowPitch
                    width: sendAutomationEditor.width
                    height: inspector.sendPointRowHeight
                    Text {
                        width: parent.width - inspector.sendPointTextRight
                        anchors.verticalCenter: parent.verticalCenter
                        text: sendPointRow.modelData.position
                        color: inspector.textMain
                        elide: Text.ElideRight
                        font.pixelSize: Theme.typeSmall
                    }
                    Rectangle {
                        x: parent.width - inspector.sendPointValueRight
                        width: inspector.sendGainFieldWidth
                        height: inspector.sendControlHeight
                        radius: Theme.controlRadius
                        color: inspector.panelRaised
                        border.color: inspector.line
                        TextInput {
                            objectName: "sendAutomationValueInput"
                            anchors.fill: parent
                            anchors.margins: Theme.fieldPadding
                            text: Number(sendPointRow.modelData.value).toFixed(2)
                            color: inspector.textMain
                            font.pixelSize: Theme.typeSmall
                            validator: DoubleValidator {
                                bottom: 0
                                top: 4
                                decimals: 2
                                locale: "C"
                            }
                            activeFocusOnTab: true
                            Accessible.name: "Send gain at " + sendPointRow.modelData.position
                            onEditingFinished: {
                                if (acceptableInput && text.length > 0)
                                    inspector.bridge.set_send_automation_value(inspector.currentSend.id, sendPointRow.modelData.id, Number(text));
                            }
                        }
                    }
                    Rectangle {
                        objectName: "sendAutomationRemovePoint"
                        x: parent.width - inspector.sendPointRemoveSize
                        width: inspector.sendPointRemoveSize
                        height: inspector.sendControlHeight
                        radius: Theme.controlRadius
                        color: inspector.panelRaised
                        border.color: activeFocus ? inspector.accent : inspector.line
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Remove send point at " + sendPointRow.modelData.position
                        function activate() {
                            inspector.bridge.remove_send_automation_point(inspector.currentSend.id, sendPointRow.modelData.id);
                        }
                        Keys.onReturnPressed: activate()
                        Keys.onSpacePressed: activate()
                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: inspector.textMain
                            font.pixelSize: Theme.typeSubtitle
                        }
                        MouseArea {
                            Tip {}
                            anchors.fill: parent
                            onClicked: parent.activate()
                        }
                    }
                }
            }
            Rectangle {
                id: sendAutomationClearLane
                objectName: "sendAutomationClearLane"
                visible: inspector.currentSendAutomation.present
                y: inspector.sendClearLaneY
                width: inspector.sendClearLaneWidth
                height: inspector.sendControlHeight
                radius: Theme.controlRadius
                color: inspector.panelRaised
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: visible
                Accessible.role: Accessible.Button
                Accessible.name: "Clear send automation lane"
                function activate() {
                    sendAutomationBarInput.forceActiveFocus();
                    inspector.bridge.remove_send_automation_lane(inspector.currentSend.id);
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text {
                    anchors.centerIn: parent
                    text: "CLEAR LANE"
                    color: inspector.textDim
                    font.pixelSize: Theme.typeCaption
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: sendAutomationClearLane.activate()
                }
            }
        }
        Item {
            x: 0
            y: inspector.sourcePanelOffset + inspector.routingInspectorHeight + inspector.quickActionsGap + inspector.analysisOrAutomationHeight + inspector.sendAutomationHeight
            width: parent.width
            height: inspector.quickActionsHeight
            Rectangle {
                x: inspector.inset
                y: inspector.quickRuleY
                width: parent.width - 2 * inspector.inset
                height: Theme.hairline
                color: inspector.line
            }
            Text {
                x: inspector.inset
                y: inspector.quickLabelY
                text: "QUICK ACTIONS"
                color: inspector.textDim
                font.pixelSize: Theme.typeSmall
                font.bold: true
                font.letterSpacing: Theme.tracking2xl
            }
            Rectangle {
                x: inspector.inset
                y: inspector.addTrackY
                width: parent.width - 2 * inspector.inset
                height: inspector.actionHeight
                radius: Theme.radiusXl
                color: inspector.accent
                activeFocusOnTab: true
                border.color: activeFocus ? Theme.focusOnAccent : inspector.accent
                Accessible.role: Accessible.Button
                Accessible.name: "Add instrument track"
                Keys.onReturnPressed: inspector.bridge.trackCommands.add_synth_track()
                Keys.onSpacePressed: inspector.bridge.trackCommands.add_synth_track()
                Text {
                    anchors.centerIn: parent
                    text: "+  ADD INSTRUMENT TRACK"
                    color: Theme.onAccentText
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: inspector.bridge.trackCommands.add_synth_track()
                }
            }
            Rectangle {
                id: importAction
                objectName: "importAudioButton"
                x: inspector.inset
                y: inspector.importY
                width: parent.width - 2 * inspector.inset
                height: inspector.actionHeight
                radius: Theme.radiusXl
                color: inspector.bridge.imports.importEnabled ? inspector.panelRaised : inspector.panel
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: inspector.bridge.imports.importEnabled
                Accessible.role: Accessible.Button
                Accessible.name: inspector.bridge.imports.encodedImportAvailable ? "Import audio" : "Import WAV audio"
                Accessible.description: inspector.bridge.imports.importStatus
                Keys.onReturnPressed: inspector.bridge.imports.choose_import_audio()
                Keys.onSpacePressed: inspector.bridge.imports.choose_import_audio()
                Text {
                    anchors.centerIn: parent
                    text: inspector.bridge.imports.importBusy ? "IMPORTING AUDIO…" : (inspector.bridge.imports.encodedImportAvailable ? "IMPORT AUDIO" : "IMPORT WAV AUDIO")
                    color: inspector.bridge.imports.importEnabled ? inspector.textMain : inspector.textDim
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    enabled: inspector.bridge.imports.importEnabled
                    onClicked: inspector.bridge.imports.choose_import_audio()
                }
            }
            Rectangle {
                id: saveAction
                x: inspector.inset
                y: inspector.saveY
                width: parent.width - 2 * inspector.inset
                height: inspector.actionHeight
                radius: Theme.radiusXl
                color: inspector.panelRaised
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Save project"
                Keys.onReturnPressed: inspector.bridge.save()
                Keys.onSpacePressed: inspector.bridge.save()
                Text {
                    anchors.centerIn: parent
                    text: "SAVE PROJECT"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: inspector.bridge.save()
                }
            }
            Rectangle {
                id: undoAction
                x: inspector.inset
                y: inspector.undoY
                width: (parent.width - 2 * inspector.inset - Theme.spaceMd) / 2
                height: inspector.actionHeight
                radius: Theme.radiusXl
                color: inspector.panelRaised
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Undo last change"
                Keys.onReturnPressed: inspector.bridge.undo()
                Keys.onSpacePressed: inspector.bridge.undo()
                Text {
                    anchors.centerIn: parent
                    text: "UNDO"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: inspector.bridge.undo()
                }
            }
            Rectangle {
                id: redoAction
                x: undoAction.x + undoAction.width + Theme.spaceMd
                y: undoAction.y
                width: undoAction.width
                height: undoAction.height
                radius: Theme.radiusXl
                color: inspector.panelRaised
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Redo last undone change"
                Keys.onReturnPressed: inspector.bridge.redo()
                Keys.onSpacePressed: inspector.bridge.redo()
                Text {
                    anchors.centerIn: parent
                    text: "REDO"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: inspector.bridge.redo()
                }
            }
            Rectangle {
                objectName: "inspectorExportButton"
                x: inspector.inset
                y: undoAction.y + undoAction.height + inspector.exportGap
                width: parent.width - 2 * inspector.inset
                height: inspector.actionHeight
                radius: Theme.radiusXl
                color: inspector.panelRaised
                border.color: activeFocus ? inspector.accent : inspector.line
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Export: choose the file, format and rate"
                Keys.onReturnPressed: inspector.exportRequested()
                Keys.onSpacePressed: inspector.exportRequested()
                Text {
                    anchors.centerIn: parent
                    text: "EXPORT…"
                    color: inspector.textMain
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                }
                MouseArea {
                    Tip {}
                    anchors.fill: parent
                    onClicked: inspector.exportRequested()
                }
            }
        }
    }
}
